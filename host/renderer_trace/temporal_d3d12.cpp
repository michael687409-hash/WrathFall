#include "temporal_d3d12.h"

#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <sl.h>
#include <sl_dlss.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace asura::temporal {
namespace {

struct Module {
  HMODULE handle = nullptr;
  ~Module() {
    if (handle) FreeLibrary(handle);
  }

  void Load(const std::filesystem::path& directory, const wchar_t* file) {
    const auto path = std::filesystem::absolute(directory / file);
    handle = LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle) {
      throw std::runtime_error("Unable to load " + path.string() +
                               " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
  }

  template <class Function>
  Function* Get(const char* name) const {
    auto function = reinterpret_cast<Function*>(GetProcAddress(handle, name));
    if (!function) throw std::runtime_error(std::string("Missing SDK export ") + name);
    return function;
  }
};

template <class Result>
void Check(Result result, const char* operation) {
  if (static_cast<uint32_t>(result) != 0) {
    throw std::runtime_error(std::string(operation) + " returned " +
                             std::to_string(static_cast<uint32_t>(result)));
  }
}

bool IsNvidia(Algorithm algorithm) {
  return algorithm == Algorithm::Dlaa || algorithm == Algorithm::Dlss;
}

bool HasMajorVersion(std::string_view version, char major) {
  const auto first_digit = version.find_first_of("0123456789");
  return first_digit != std::string_view::npos && first_digit + 1 < version.size() &&
         version[first_digit] == major && version[first_digit + 1] == '.';
}

struct Mode {
  sl::DLSSMode nvidia;
  uint32_t amd;
  sl::DLSSPreset sl::DLSSOptions::*preset;
};

Mode GetMode(Quality quality) {
  using D = sl::DLSSMode;
  using O = sl::DLSSOptions;
  switch (quality) {
    case Quality::Native: return {D::eDLAA, FFX_UPSCALE_QUALITY_MODE_NATIVEAA, &O::dlaaPreset};
    case Quality::Quality: return {D::eMaxQuality, FFX_UPSCALE_QUALITY_MODE_QUALITY, &O::qualityPreset};
    case Quality::Balanced: return {D::eBalanced, FFX_UPSCALE_QUALITY_MODE_BALANCED, &O::balancedPreset};
    case Quality::Performance:
      return {D::eMaxPerformance, FFX_UPSCALE_QUALITY_MODE_PERFORMANCE, &O::performancePreset};
    case Quality::UltraPerformance:
      return {D::eUltraPerformance, FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE,
              &O::ultraPerformancePreset};
  }
  throw std::invalid_argument("Invalid upscaler quality");
}

sl::DLSSPreset GetPreset(DlssPreset preset) {
  using P = sl::DLSSPreset;
  switch (preset) {
    case DlssPreset::Default: return P::eDefault;
    case DlssPreset::J: return P::ePresetJ;
    case DlssPreset::K: return P::ePresetK;
    case DlssPreset::L: return P::ePresetL;
    case DlssPreset::M: return P::ePresetM;
  }
  throw std::invalid_argument("Invalid DLSS preset");
}

sl::Boolean Flag(bool value) {
  return value ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

sl::float3 Vector(const std::array<float, 3>& values) {
  return {values[0], values[1], values[2]};
}

void SetMatrix(sl::float4x4& target, const std::array<float, 16>& values) {
  static_assert(sizeof(target) == sizeof(values));
  std::memcpy(&target, values.data(), sizeof(target));
}

void CheckTexture(ID3D12Resource* resource, Extent extent, bool output = false) {
  if (!resource) throw std::invalid_argument("Missing temporal texture");
  const auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
      desc.Width != extent.width || desc.Height != extent.height ||
      (output && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
    throw std::invalid_argument("Temporal texture extent or usage does not match the frame");
  }
}

}  // namespace

struct Upscaler::State {
  Settings settings;
  Extent render{};
  Module module;
  ffxContext ffx_context = nullptr;
  sl::ViewportHandle viewport{0};
  bool streamline_initialized = false;
  bool streamline_configured = false;
  bool dispatched = false;

  decltype(ffxDestroyContext)* destroy_fsr = nullptr;
  decltype(ffxDispatch)* dispatch_fsr = nullptr;
  PFun_slShutdown* shutdown_streamline = nullptr;
  PFun_slFreeResources* free_dlss = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
  PFun_slGetNewFrameToken* new_frame_token = nullptr;
  PFun_slSetTagForFrame* set_tags = nullptr;
  PFun_slEvaluateFeature* evaluate_dlss = nullptr;

  explicit State(Settings initial) : settings(initial) {}
  ~State() {
    if (ffx_context) destroy_fsr(&ffx_context, nullptr);
    if (streamline_configured) free_dlss(sl::kFeatureDLSS, viewport);
    if (streamline_initialized) shutdown_streamline();
  }

  void LoadNvidia(const std::filesystem::path& runtime);
  void InitializeNvidia(ID3D12Device* device);
  void InitializeAmd(ID3D12Device* device);
  void DispatchNvidia(ID3D12GraphicsCommandList* commands, const Frame& frame);
  void DispatchAmd(ID3D12GraphicsCommandList* commands, const Frame& frame);
};

void Upscaler::State::LoadNvidia(const std::filesystem::path& runtime) {
  module.Load(runtime, L"sl.interposer.dll");
  shutdown_streamline = module.Get<PFun_slShutdown>("slShutdown");
  free_dlss = module.Get<PFun_slFreeResources>("slFreeResources");
  set_constants = module.Get<PFun_slSetConstants>("slSetConstants");
  new_frame_token = module.Get<PFun_slGetNewFrameToken>("slGetNewFrameToken");
  set_tags = module.Get<PFun_slSetTagForFrame>("slSetTagForFrame");
  evaluate_dlss = module.Get<PFun_slEvaluateFeature>("slEvaluateFeature");

  const auto plugins_directory = std::filesystem::absolute(runtime);
  const wchar_t* plugin_paths[] = {plugins_directory.c_str()};
  const sl::Feature dlss = sl::kFeatureDLSS;
  sl::Preferences preferences{};
  preferences.featuresToLoad = &dlss;
  preferences.numFeaturesToLoad = 1;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  preferences.engineVersion = "0.11.0";
  preferences.projectId = "9019940f-5aa0-42fd-92a5-194efc801e78";
  preferences.flags = sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.logMessageCallback = [](sl::LogType level, const char* message) {
    if (level == sl::LogType::eError) std::fputs(message, stderr);
  };
  Check(module.Get<PFun_slInit>("slInit")(preferences, sl::kSDKVersion), "slInit");
  streamline_initialized = true;
}

void Upscaler::State::InitializeNvidia(ID3D12Device* device) {
  Check(module.Get<PFun_slSetD3DDevice>("slSetD3DDevice")(device), "slSetD3DDevice");
  LUID luid = device->GetAdapterLuid();
  sl::AdapterInfo adapter{};
  adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
  adapter.deviceLUIDSizeInBytes = sizeof(luid);
  Check(module.Get<PFun_slIsFeatureSupported>("slIsFeatureSupported")(sl::kFeatureDLSS, adapter),
        "slIsFeatureSupported");

  sl::DLSSOptions options{};
  options.outputWidth = settings.output.width;
  options.outputHeight = settings.output.height;
  options.colorBuffersHDR = Flag(settings.hdr);
  options.useAutoExposure = sl::Boolean::eTrue;

  const auto get_function = module.Get<PFun_slGetFeatureFunction>("slGetFeatureFunction");
  void* function = nullptr;
  Check(get_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function),
        "Getting slDLSSGetOptimalSettings");
  const auto get_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(function);
  Extent selected_render{};
  const auto options_for = [&](Quality quality) {
    const auto mode = GetMode(quality);
    auto candidate = options;
    candidate.mode = mode.nvidia;
    candidate.*(mode.preset) = GetPreset(settings.preset);
    return candidate;
  };
  if (settings.fixed_render && settings.algorithm == Algorithm::Dlss) {
    const Extent fixed = *settings.fixed_render;
    double best_distance = std::numeric_limits<double>::infinity();
    for (const auto quality : {Quality::Quality, Quality::Balanced, Quality::Performance,
                               Quality::UltraPerformance}) {
      const auto candidate = options_for(quality);
      sl::DLSSOptimalSettings optimal{};
      if (get_optimal(candidate, optimal) != sl::Result::eOk) continue;
      const bool exact = optimal.optimalRenderWidth == fixed.width &&
                         optimal.optimalRenderHeight == fixed.height;
      const bool in_range = fixed.width >= optimal.renderWidthMin &&
                            fixed.width <= optimal.renderWidthMax &&
                            fixed.height >= optimal.renderHeightMin &&
                            fixed.height <= optimal.renderHeightMax;
      if (!exact && !in_range) continue;
      const double dx = (static_cast<double>(optimal.optimalRenderWidth) - fixed.width) / fixed.width;
      const double dy =
          (static_cast<double>(optimal.optimalRenderHeight) - fixed.height) / fixed.height;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        options = candidate;
        best_distance = distance;
      }
    }
    if (!std::isfinite(best_distance)) {
      throw std::runtime_error("No DLSS mode accepts the fixed input for this output resolution");
    }
    selected_render = fixed;
  } else {
    options = options_for(settings.algorithm == Algorithm::Dlaa ? Quality::Native : settings.quality);
    sl::DLSSOptimalSettings optimal{};
    Check(get_optimal(options, optimal), "slDLSSGetOptimalSettings");
    if (!optimal.optimalRenderWidth || !optimal.optimalRenderHeight) {
      throw std::runtime_error("DLSS returned an empty render extent");
    }
    selected_render = {optimal.optimalRenderWidth, optimal.optimalRenderHeight};
  }
  Check(get_function(sl::kFeatureDLSS, "slDLSSSetOptions", function), "Getting slDLSSSetOptions");
  Check(reinterpret_cast<PFun_slDLSSSetOptions*>(function)(viewport, options), "slDLSSSetOptions");
  streamline_configured = true;
  render = selected_render;
}

void Upscaler::State::InitializeAmd(ID3D12Device* device) {
  const auto query = module.Get<decltype(ffxQuery)>("ffxQuery");
  destroy_fsr = module.Get<decltype(ffxDestroyContext)>("ffxDestroyContext");
  dispatch_fsr = module.Get<decltype(ffxDispatch)>("ffxDispatch");
  const auto create = module.Get<decltype(ffxCreateContext)>("ffxCreateContext");

  uint64_t count = 0;
  ffxQueryDescGetVersions versions{};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = device;
  versions.outputCount = &count;
  Check(query(nullptr, &versions.header), "Querying FSR version count");
  if (!count) throw std::runtime_error("No FSR upscaling providers on this adapter");
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  Check(query(nullptr, &versions.header), "Querying FSR versions");

  const char major = settings.algorithm == Algorithm::Fsr4 ? '4' : '3';
  std::string offered;
  uint64_t selected = 0;
  for (size_t index = 0; index < count && index < ids.size(); ++index) {
    if (!names[index]) continue;
    const std::string_view name = names[index];
    if (!offered.empty()) offered += ", ";
    offered += name;
    if (!selected && HasMajorVersion(name, major)) {
      selected = ids[index];
    }
  }
  if (!selected) {
    throw std::runtime_error(std::string("FSR ") + major +
                             " is unavailable on this adapter. Offered providers: " + offered);
  }
  ffxOverrideVersion override_version{{FFX_API_DESC_TYPE_OVERRIDE_VERSION, nullptr}, selected};
  Extent selected_render{};
  if (settings.fixed_render) {
    selected_render = *settings.fixed_render;
  } else {
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode resolution{};
    resolution.header = {FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE,
                         &override_version.header};
    resolution.displayWidth = settings.output.width;
    resolution.displayHeight = settings.output.height;
    resolution.qualityMode = GetMode(settings.quality).amd;
    resolution.pOutRenderWidth = &selected_render.width;
    resolution.pOutRenderHeight = &selected_render.height;
    Check(query(nullptr, &resolution.header), "Querying FSR render resolution");
  }

  ffxCreateBackendDX12Desc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
                                   &override_version.header}, device};
  ffxCreateContextDescUpscale context{};
  context.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &backend.header};
  context.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  if (settings.hdr) context.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
  if (settings.inverted_depth) context.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  context.maxRenderSize = {selected_render.width, selected_render.height};
  context.maxUpscaleSize = {settings.output.width, settings.output.height};
  Check(create(&ffx_context, &context.header, nullptr), "Creating FSR upscaler context");
  ffxQueryGetProviderVersion actual{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, nullptr}};
  Check(query(&ffx_context, &actual.header), "Checking active FSR provider");
  if (!actual.versionName || !HasMajorVersion(actual.versionName, major)) {
    throw std::runtime_error("FSR returned a different provider than the requested generation");
  }
  std::printf("Active provider %s\n", actual.versionName);
  render = selected_render;
}

void Upscaler::State::DispatchNvidia(ID3D12GraphicsCommandList* commands, const Frame& frame) {
  sl::FrameToken* token = nullptr;
  Check(new_frame_token(token, &frame.index), "slGetNewFrameToken");
  if (!token) throw std::runtime_error("Streamline returned an empty frame token");

  const float width = static_cast<float>(frame.render.width);
  const float height = static_cast<float>(frame.render.height);
  sl::Constants constants{};
  SetMatrix(constants.cameraViewToClip, frame.camera.view_to_clip);
  SetMatrix(constants.clipToCameraView, frame.camera.clip_to_view);
  SetMatrix(constants.clipToPrevClip, frame.camera.clip_to_previous_clip);
  SetMatrix(constants.prevClipToClip, frame.camera.previous_clip_to_clip);
  constants.jitterOffset = {frame.jitter_x, frame.jitter_y};
  constants.mvecScale = {1.0f / width, 1.0f / height};
  constants.cameraPos = Vector(frame.camera.position);
  constants.cameraUp = Vector(frame.camera.up);
  constants.cameraRight = Vector(frame.camera.right);
  constants.cameraFwd = Vector(frame.camera.forward);
  constants.cameraNear = frame.camera.near_plane;
  constants.cameraFar = frame.camera.far_plane;
  constants.cameraFOV = frame.camera.vertical_fov;
  constants.cameraAspectRatio = width / height;
  constants.depthInverted = Flag(settings.inverted_depth);
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = Flag(!dispatched || frame.reset_history);
  Check(set_constants(constants, *token, viewport), "slSetConstants");

  constexpr auto texture = sl::ResourceType::eTex2d;
  constexpr auto compute_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  sl::Resource color(texture, frame.color, compute_read);
  sl::Resource depth(texture, frame.depth, compute_read);
  sl::Resource motion(texture, frame.motion, compute_read);
  sl::Resource output(texture, frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const sl::Extent source{0, 0, frame.render.width, frame.render.height};
  const sl::Extent destination{0, 0, settings.output.width, settings.output.height};
  constexpr auto during_dispatch = sl::ResourceLifecycle::eOnlyValidNow;
  const sl::ResourceTag resources[] = {
      {&color, sl::kBufferTypeScalingInputColor, during_dispatch, &source},
      {&depth, sl::kBufferTypeDepth, during_dispatch, &source},
      {&motion, sl::kBufferTypeMotionVectors, during_dispatch, &source},
      {&output, sl::kBufferTypeScalingOutputColor, during_dispatch, &destination},
  };
  Check(set_tags(*token, viewport, resources, static_cast<uint32_t>(std::size(resources)), commands),
        "slSetTagForFrame");
  const sl::BaseStructure* inputs[] = {&viewport};
  Check(evaluate_dlss(sl::kFeatureDLSS, *token, inputs, 1, commands), "slEvaluateFeature");
}

void Upscaler::State::DispatchAmd(ID3D12GraphicsCommandList* commands, const Frame& frame) {
  ffxDispatchDescUpscale dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = commands;
  dispatch.color = ffxApiGetResourceDX12(frame.color);
  dispatch.depth = ffxApiGetResourceDX12(frame.depth);
  dispatch.motionVectors = ffxApiGetResourceDX12(frame.motion);
  dispatch.output = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch.reactive = ffxApiGetResourceDX12(frame.reactive);
  dispatch.jitterOffset = {frame.jitter_x, frame.jitter_y};
  dispatch.motionVectorScale = {1.0f, 1.0f};
  dispatch.renderSize = {frame.render.width, frame.render.height};
  dispatch.upscaleSize = {settings.output.width, settings.output.height};
  dispatch.frameTimeDelta = frame.elapsed_ms;
  dispatch.preExposure = 1.0f;
  dispatch.reset = !dispatched || frame.reset_history;
  dispatch.cameraNear = frame.camera.near_plane;
  dispatch.cameraFar = frame.camera.far_plane;
  dispatch.cameraFovAngleVertical = frame.camera.vertical_fov;
  dispatch.viewSpaceToMetersFactor = frame.camera.view_space_to_meters;
  Check(dispatch_fsr(&ffx_context, &dispatch.header), "ffxDispatch upscaling");
}

Upscaler::Upscaler(Settings settings, const std::filesystem::path& runtime)
    : state_(std::make_unique<State>(settings)) {
  if (!settings.output.width || !settings.output.height) {
    throw std::invalid_argument("Empty temporal output extent");
  }
  if (settings.fixed_render &&
      (!settings.fixed_render->width || !settings.fixed_render->height ||
       settings.fixed_render->width > settings.output.width ||
       settings.fixed_render->height > settings.output.height ||
       settings.algorithm == Algorithm::Dlaa)) {
    throw std::invalid_argument("Fixed input extent is incompatible with the upscaler output");
  }
  if (IsNvidia(settings.algorithm)) {
    state_->LoadNvidia(runtime);
  } else {
    state_->module.Load(runtime, L"amd_fidelityfx_loader_dx12.dll");
  }
}

Upscaler::~Upscaler() = default;

Extent Upscaler::Initialize(ID3D12Device* device) {
  if (!device || state_->render.width || state_->ffx_context || state_->streamline_configured) {
    throw std::logic_error("Expected an uninitialized upscaler and a valid D3D12 device");
  }
  if (IsNvidia(state_->settings.algorithm)) {
    state_->InitializeNvidia(device);
  } else {
    state_->InitializeAmd(device);
  }
  return state_->render;
}

void Upscaler::Dispatch(ID3D12GraphicsCommandList* commands, const Frame& frame) {
  if (!commands || !state_->render.width) throw std::logic_error("Upscaler is uninitialized");
  if (frame.render.width != state_->render.width || frame.render.height != state_->render.height ||
      !std::isfinite(frame.elapsed_ms) || frame.elapsed_ms <= 0.0f) {
    throw std::invalid_argument("Temporal frame extent or elapsed time is invalid");
  }
  CheckTexture(frame.color, frame.render);
  CheckTexture(frame.depth, frame.render);
  CheckTexture(frame.motion, frame.render);
  CheckTexture(frame.output, state_->settings.output, true);
  if (frame.reactive) CheckTexture(frame.reactive, frame.render);
  if (frame.output == frame.color || frame.output == frame.depth ||
      frame.output == frame.motion || frame.output == frame.reactive) {
    throw std::invalid_argument("Temporal output aliases a sampled texture");
  }
  if (IsNvidia(state_->settings.algorithm)) {
    state_->DispatchNvidia(commands, frame);
  } else {
    state_->DispatchAmd(commands, frame);
  }
  state_->dispatched = true;
}

}  // namespace asura::temporal
