#include "dlss_vulkan.h"

#include <Windows.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace wrathfall {
namespace {

using asura::temporal::DlssPreset;

void Check(sl::Result result, const char* operation) {
  if (result != sl::Result::eOk) {
    throw std::runtime_error(std::string(operation) + " failed (" +
                             std::to_string(static_cast<int>(result)) + ")");
  }
}

sl::DLSSPreset ToStreamline(DlssPreset preset) {
  using P = sl::DLSSPreset;
  switch (preset) {
    case DlssPreset::Default: return P::eDefault;
    case DlssPreset::J: return P::ePresetJ;
    case DlssPreset::K: return P::ePresetK;
    case DlssPreset::L: return P::ePresetL;
    case DlssPreset::M: return P::ePresetM;
  }
  throw std::invalid_argument("Invalid DLSS model preset");
}

sl::Resource TagResource(const VulkanUpscaleImage& texture, VkImageLayout layout,
                         VkExtent2D extent, VkImageUsageFlags usage) {
  const auto& image = texture.description;
  if (!texture.image || !texture.view || image.imageType != VK_IMAGE_TYPE_2D ||
      image.extent.width != extent.width || image.extent.height != extent.height ||
      image.extent.depth != 1 || image.arrayLayers != 1 || !image.mipLevels ||
      image.samples != VK_SAMPLE_COUNT_1_BIT || (image.usage & usage) != usage) {
    throw std::invalid_argument("Invalid Vulkan DLSS image");
  }
  sl::Resource resource(sl::ResourceType::eTex2d, texture.image, nullptr, texture.view, layout);
  resource.width = image.extent.width;
  resource.height = image.extent.height;
  resource.nativeFormat = image.format;
  resource.mipLevels = image.mipLevels;
  resource.arrayLayers = image.arrayLayers;
  resource.flags = image.flags;
  resource.usage = image.usage;
  return resource;
}

sl::Boolean Flag(bool value) {
  return value ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

sl::float3 ToVector(const std::array<float, 3>& vector) {
  return {vector[0], vector[1], vector[2]};
}

void CopyMatrix(sl::float4x4& output, const std::array<float, 16>& input) {
  static_assert(sizeof(output) == sizeof(input));
  std::memcpy(&output, input.data(), sizeof(output));
}

}  // namespace

struct DlssVulkan::State {
  ~State() {
    if (configured) free_resources(sl::kFeatureDLSS, viewport);
    if (initialized) shutdown();
    if (interposer) FreeLibrary(interposer);
  }

  template <typename T> T* Import(const char* name) const {
    auto function = reinterpret_cast<T*>(GetProcAddress(interposer, name));
    if (!function) throw std::runtime_error(std::string("Missing Streamline function ") + name);
    return function;
  }

  template <typename T> T* Feature(const char* name) const {
    void* function = nullptr;
    Check(Import<PFun_slGetFeatureFunction>("slGetFeatureFunction")(sl::kFeatureDLSS, name, function),
          name);
    return reinterpret_cast<T*>(function);
  }

  HMODULE interposer = nullptr;
  sl::FeatureRequirements requirements{};
  std::vector<const char*> instance_extensions, device_extensions;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slFreeResources* free_resources = nullptr;
  PFun_slGetNewFrameToken* new_token = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
  PFun_slSetTagForFrame* set_tags = nullptr;
  PFun_slEvaluateFeature* evaluate = nullptr;
  VkExtent2D render{}, output{};
  sl::ViewportHandle viewport{0};
  uint32_t frame_index = 0;
  bool initialized = false;
  bool configured = false;
  bool inverted_depth = false;
};

DlssVulkan::DlssVulkan(const std::filesystem::path& runtime_dir) : state_(std::make_unique<State>()) {
  auto& state = *state_;
  const auto directory = std::filesystem::absolute(runtime_dir);
  const auto filename = directory / L"sl.interposer.dll";
  state.interposer = LoadLibraryExW(filename.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!state.interposer) {
    throw std::runtime_error("Could not load Streamline (" + std::to_string(GetLastError()) + ")");
  }
  state.shutdown = state.Import<PFun_slShutdown>("slShutdown");
  state.free_resources = state.Import<PFun_slFreeResources>("slFreeResources");
  state.new_token = state.Import<PFun_slGetNewFrameToken>("slGetNewFrameToken");
  state.set_constants = state.Import<PFun_slSetConstants>("slSetConstants");
  state.set_tags = state.Import<PFun_slSetTagForFrame>("slSetTagForFrame");
  state.evaluate = state.Import<PFun_slEvaluateFeature>("slEvaluateFeature");

  const wchar_t* plugin_paths[] = {directory.c_str()};
  const sl::Feature dlss = sl::kFeatureDLSS;
  sl::Preferences preferences{};
  preferences.featuresToLoad = &dlss;
  preferences.numFeaturesToLoad = 1;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.renderAPI = sl::RenderAPI::eVulkan;
  preferences.engineVersion = "0.11.0";
  preferences.projectId = "9019940f-5aa0-42fd-92a5-194efc801e78";
  preferences.flags = sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.logMessageCallback = [](sl::LogType level, const char* message) {
    if (level == sl::LogType::eError) std::fputs(message, stderr);
  };
  Check(state.Import<PFun_slInit>("slInit")(preferences, sl::kSDKVersion), "Initialize Vulkan Streamline");
  state.initialized = true;
  Check(state.Import<PFun_slGetFeatureRequirements>("slGetFeatureRequirements")(dlss, state.requirements),
        "Query Vulkan DLSS requirements");
  const auto& requirements = state.requirements;
  for (uint32_t index = 0; index < requirements.vkNumInstanceExtensions; ++index) {
    state.instance_extensions.push_back(requirements.vkInstanceExtensions[index]);
  }
  for (uint32_t index = 0; index < requirements.vkNumDeviceExtensions; ++index) {
    state.device_extensions.push_back(requirements.vkDeviceExtensions[index]);
  }
  if (requirements.vkNumGraphicsQueuesRequired || requirements.vkNumComputeQueuesRequired ||
      requirements.vkNumOpticalFlowQueuesRequired) {
    throw std::runtime_error("Vulkan DLSS requested dedicated queues");
  }
}

DlssVulkan::~DlssVulkan() = default;

const std::vector<const char*>& DlssVulkan::InstanceExtensions() const {
  return state_->instance_extensions;
}

const std::vector<const char*>& DlssVulkan::DeviceExtensions() const {
  return state_->device_extensions;
}

void DlssVulkan::CheckDevice(VkPhysicalDevice physical,
                             const VkPhysicalDeviceVulkan12Features& enabled12,
                             const VkPhysicalDeviceVulkan13Features& enabled13) const {
  const auto& state = *state_;
  sl::AdapterInfo adapter{};
  adapter.vkPhysicalDevice = physical;
  Check(state.Import<PFun_slIsFeatureSupported>("slIsFeatureSupported")(sl::kFeatureDLSS, adapter),
        "Check Vulkan DLSS adapter");

  const auto& requested = state.requirements;
  auto required12 = sl::getVkPhysicalDeviceVulkan12Features(requested.vkNumFeatures12,
                                                           requested.vkFeatures12);
  auto required13 = sl::getVkPhysicalDeviceVulkan13Features(requested.vkNumFeatures13,
                                                           requested.vkFeatures13);
  const auto supports = [](auto required, const auto& enabled, size_t begin, size_t end) {
    auto merged = required;
    sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures(
        reinterpret_cast<VkBaseOutStructure*>(&merged),
        reinterpret_cast<const VkBaseOutStructure*>(&required),
        reinterpret_cast<const VkBaseOutStructure*>(&enabled));
    const auto* expected = reinterpret_cast<const char*>(&required) + begin;
    const auto* verified = reinterpret_cast<const char*>(&merged) + begin;
    return std::memcmp(expected, verified, end - begin) == 0;
  };
  using Features12 = VkPhysicalDeviceVulkan12Features;
  using Features13 = VkPhysicalDeviceVulkan13Features;
  if (!supports(required12, enabled12, offsetof(Features12, samplerMirrorClampToEdge),
                offsetof(Features12, subgroupBroadcastDynamicId) + sizeof(VkBool32)) ||
      !supports(required13, enabled13, offsetof(Features13, robustImageAccess),
                offsetof(Features13, maintenance4) + sizeof(VkBool32))) {
    throw std::runtime_error("Vulkan DLSS requires device features that are unavailable");
  }
}

void DlssVulkan::EnableDeviceFeatures(VkPhysicalDevice physical,
                                     const VkPhysicalDeviceVulkan12Features& supported12,
                                     const VkPhysicalDeviceVulkan13Features& supported13,
                                     VkPhysicalDeviceVulkan12Features& enabled12,
                                     VkPhysicalDeviceVulkan13Features& enabled13) const {
  CheckDevice(physical, supported12, supported13);
  const auto& requirements = state_->requirements;
  const auto required12 = sl::getVkPhysicalDeviceVulkan12Features(requirements.vkNumFeatures12,
                                                                  requirements.vkFeatures12);
  const auto required13 = sl::getVkPhysicalDeviceVulkan13Features(requirements.vkNumFeatures13,
                                                                  requirements.vkFeatures13);
  auto enable = [](auto& enabled, const auto& required, const auto& supported) {
    sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures(
        reinterpret_cast<VkBaseOutStructure*>(&enabled),
        reinterpret_cast<const VkBaseOutStructure*>(&required),
        reinterpret_cast<const VkBaseOutStructure*>(&supported));
  };
  enable(enabled12, required12, supported12);
  enable(enabled13, required13, supported13);
}

void DlssVulkan::Bind(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                      uint32_t queue_family, VkExtent2D render, VkExtent2D output,
                      DlssPreset preset, bool inverted_depth) {
  auto& state = *state_;
  if (state.configured || !instance || !physical || !device || !render.width || !render.height ||
      output.width < render.width || output.height < render.height) {
    throw std::invalid_argument("Invalid Vulkan DLSS device or extent");
  }
  sl::VulkanInfo info{};
  info.instance = instance;
  info.physicalDevice = physical;
  info.device = device;
  info.computeQueueFamily = queue_family;
  info.graphicsQueueFamily = queue_family;
  Check(state.Import<PFun_slSetVulkanInfo>("slSetVulkanInfo")(info), "Bind Streamline Vulkan device");

  auto best_distance = std::numeric_limits<double>::infinity();
  const auto get_optimal = state.Feature<PFun_slDLSSGetOptimalSettings>("slDLSSGetOptimalSettings");
  sl::DLSSOptions options{};
  options.outputWidth = output.width;
  options.outputHeight = output.height;
  options.colorBuffersHDR = sl::Boolean::eFalse;
  options.useAutoExposure = sl::Boolean::eTrue;
  const auto model = ToStreamline(preset);
  options.qualityPreset = model;
  options.balancedPreset = model;
  options.performancePreset = model;
  options.ultraPerformancePreset = model;
  using Mode = sl::DLSSMode;
  for (auto mode : {Mode::eMaxQuality, Mode::eBalanced, Mode::eMaxPerformance, Mode::eUltraPerformance}) {
    auto candidate = options;
    candidate.mode = mode;
    sl::DLSSOptimalSettings optimal{};
    if (get_optimal(candidate, optimal) != sl::Result::eOk) continue;
    const bool exact = optimal.optimalRenderWidth == render.width &&
                       optimal.optimalRenderHeight == render.height;
    const bool covers = render.width >= optimal.renderWidthMin && render.width <= optimal.renderWidthMax &&
                        render.height >= optimal.renderHeightMin && render.height <= optimal.renderHeightMax;
    if (!exact && !covers) continue;
    const double dx = (static_cast<double>(optimal.optimalRenderWidth) - render.width) / render.width;
    const double dy = (static_cast<double>(optimal.optimalRenderHeight) - render.height) / render.height;
    const double distance = dx * dx + dy * dy;
    if (distance < best_distance) {
      options.mode = mode;
      best_distance = distance;
    }
  }
  if (options.mode == Mode::eOff) throw std::runtime_error("Vulkan DLSS rejected the fixed input extent");
  Check(state.Feature<PFun_slDLSSSetOptions>("slDLSSSetOptions")(state.viewport, options),
        "Set Vulkan DLSS output and model");
  state.render = render;
  state.output = output;
  state.inverted_depth = inverted_depth;
  state.configured = true;
}

void DlssVulkan::Dispatch(VkCommandBuffer command, const VulkanUpscaleFrame& frame,
                          const asura::temporal::Camera& camera) {
  auto& state = *state_;
  if (!state.configured || !command) throw std::invalid_argument("Vulkan DLSS is unconfigured");
  const auto source = state.render;
  const auto output = state.output;
  constexpr auto read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  constexpr auto sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
  auto color = TagResource(frame.color, read, source, sampled);
  auto depth = TagResource(frame.depth, read, source, sampled);
  auto motion = TagResource(frame.motion, read, source, sampled);
  auto target = TagResource(frame.output, VK_IMAGE_LAYOUT_GENERAL, output, VK_IMAGE_USAGE_STORAGE_BIT);
  const sl::Extent render_extent{0, 0, source.width, source.height};
  const sl::Extent output_extent{0, 0, output.width, output.height};
  constexpr auto now = sl::ResourceLifecycle::eOnlyValidNow;
  const sl::ResourceTag tags[] = {
      {&color, sl::kBufferTypeScalingInputColor, now, &render_extent},
      {&depth, sl::kBufferTypeDepth, now, &render_extent},
      {&motion, sl::kBufferTypeMotionVectors, now, &render_extent},
      {&target, sl::kBufferTypeScalingOutputColor, now, &output_extent},
  };

  sl::FrameToken* token = nullptr;
  Check(state.new_token(token, &state.frame_index), "Acquire Vulkan DLSS frame token");
  if (!token) throw std::runtime_error("Streamline returned an empty frame token");
  sl::Constants constants{};
  CopyMatrix(constants.cameraViewToClip, camera.view_to_clip);
  CopyMatrix(constants.clipToCameraView, camera.clip_to_view);
  CopyMatrix(constants.clipToPrevClip, camera.clip_to_previous_clip);
  CopyMatrix(constants.prevClipToClip, camera.previous_clip_to_clip);
  const auto width = static_cast<float>(source.width);
  const auto height = static_cast<float>(source.height);
  constants.jitterOffset = {frame.jitter_x, frame.jitter_y};
  constants.mvecScale = {1.0f / width, 1.0f / height};
  constants.cameraPos = ToVector(camera.position);
  constants.cameraUp = ToVector(camera.up);
  constants.cameraRight = ToVector(camera.right);
  constants.cameraFwd = ToVector(camera.forward);
  constants.cameraNear = camera.near_plane;
  constants.cameraFar = camera.far_plane;
  constants.cameraFOV = camera.vertical_fov;
  constants.cameraAspectRatio = width / height;
  constants.depthInverted = Flag(state.inverted_depth);
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = Flag(!state.frame_index || frame.reset_history);
  Check(state.set_constants(constants, *token, state.viewport), "Record Vulkan DLSS camera");
  Check(state.set_tags(*token, state.viewport, tags, static_cast<uint32_t>(std::size(tags)), command),
        "Tag Vulkan DLSS images");
  const sl::BaseStructure* inputs[] = {&state.viewport};
  Check(state.evaluate(sl::kFeatureDLSS, *token, inputs, 1, command), "Dispatch Vulkan DLSS");
  ++state.frame_index;
}

}  // namespace wrathfall
