#include "fsr_vulkan.h"

#include <Windows.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace wrathfall {
namespace {

void Check(ffxReturnCode_t result, const char* operation) {
  if (result != FFX_API_RETURN_OK) {
    throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
  }
}

bool IsFsr3(std::string_view name) {
  return name.starts_with("3.") || name.find(" 3.") != std::string_view::npos;
}

FfxApiResource Wrap(const VulkanUpscaleImage& texture, uint32_t state) {
  if (!texture.image) return {};
  auto description = ffxApiGetImageResourceDescriptionVK(texture.image, texture.description, 0);
  if (description.format == FFX_API_SURFACE_FORMAT_UNKNOWN) {
    throw std::invalid_argument("Unsupported Vulkan image format for FSR");
  }
  return ffxApiGetResourceVK(reinterpret_cast<void*>(texture.image), description, state);
}

bool Matches(const VulkanUpscaleImage& texture, VkExtent2D extent, VkImageUsageFlags usage) {
  return texture.image && texture.description.imageType == VK_IMAGE_TYPE_2D &&
         texture.description.extent.width == extent.width &&
         texture.description.extent.height == extent.height &&
         texture.description.extent.depth == 1 && texture.description.arrayLayers == 1 &&
         texture.description.mipLevels &&
         texture.description.samples == VK_SAMPLE_COUNT_1_BIT &&
         (texture.description.usage & usage) == usage;
}

}  // namespace

struct FsrVulkan::State {
  ~State() {
    if (context) destroy(&context, nullptr);
    if (loader) FreeLibrary(loader);
  }

  template <typename T> T Import(const char* name) const {
    auto function = reinterpret_cast<T>(GetProcAddress(loader, name));
    if (!function) throw std::runtime_error(std::string("Missing FidelityFX function ") + name);
    return function;
  }

  HMODULE loader = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  ffxContext context = nullptr;
  ffxOverrideVersion version{};
  ffxCreateBackendVKDesc backend{};
  ffxCreateContextDescUpscale descriptor{};
  VkExtent2D render{}, output{};
  std::string provider;
  bool dispatched = false;
};

FsrVulkan::FsrVulkan(const std::filesystem::path& runtime_dir, VkPhysicalDevice physical,
                     VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc, VkExtent2D render,
                     VkExtent2D output, bool inverted_depth)
    : state_(std::make_unique<State>()) {
  if (!physical || !device || !get_device_proc || !render.width || !render.height ||
      output.width < render.width || output.height < render.height) {
    throw std::invalid_argument("Invalid Vulkan upscaler device or extent");
  }
  auto& state = *state_;
  state.render = render;
  state.output = output;
  const auto loader_path = std::filesystem::absolute(runtime_dir / L"amd_fidelityfx_loader_vk.dll");
  state.loader = LoadLibraryExW(loader_path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!state.loader) {
    throw std::runtime_error("Could not load the Vulkan FidelityFX loader (" +
                             std::to_string(GetLastError()) + ")");
  }
  auto query = state.Import<PfnFfxQuery>("ffxQuery");
  auto create = state.Import<PfnFfxCreateContext>("ffxCreateContext");
  state.destroy = state.Import<PfnFfxDestroyContext>("ffxDestroyContext");
  state.dispatch = state.Import<PfnFfxDispatch>("ffxDispatch");
  state.backend.deviceContext = {device, physical, get_device_proc};

  uint64_t count = 0;
  ffxQueryDescGetVersions versions{};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = &state.backend.deviceContext;
  versions.outputCount = &count;
  Check(query(nullptr, &versions.header), "Enumerate Vulkan FSR providers");
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  Check(query(nullptr, &versions.header), "Read Vulkan FSR providers");
  for (size_t index = 0; index < count && index < ids.size(); ++index) {
    if (names[index] && IsFsr3(names[index])) {
      state.version = {{FFX_API_DESC_TYPE_OVERRIDE_VERSION, nullptr}, ids[index]};
      break;
    }
  }
  if (!state.version.versionId) throw std::runtime_error("Vulkan FSR 3 is unavailable");

  state.backend.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &state.version.header};
  state.descriptor.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &state.backend.header};
  state.descriptor.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  if (inverted_depth) state.descriptor.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  state.descriptor.maxRenderSize = {render.width, render.height};
  state.descriptor.maxUpscaleSize = {output.width, output.height};
  Check(create(&state.context, &state.descriptor.header, nullptr), "Create Vulkan FSR 3 context");
  ffxQueryGetProviderVersion actual{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, nullptr}};
  Check(query(&state.context, &actual.header), "Check active Vulkan FSR provider");
  if (!actual.versionName || !IsFsr3(actual.versionName)) {
    throw std::runtime_error("Vulkan FidelityFX returned a different provider");
  }
  state.provider = actual.versionName;
}

FsrVulkan::~FsrVulkan() = default;

std::string_view FsrVulkan::Provider() const {
  return state_->provider;
}

void FsrVulkan::Dispatch(VkCommandBuffer command, const VulkanUpscaleFrame& frame) {
  auto& state = *state_;
  const auto sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!command || !Matches(frame.color, state.render, sampled) ||
      !Matches(frame.depth, state.render, sampled) || !Matches(frame.motion, state.render, sampled) ||
      (frame.reactive.image && !Matches(frame.reactive, state.render, sampled)) ||
      !Matches(frame.output, state.output, VK_IMAGE_USAGE_STORAGE_BIT)) {
    throw std::invalid_argument("Invalid Vulkan FSR frame resources");
  }
  constexpr auto read = FFX_API_RESOURCE_STATE_COMPUTE_READ;
  ffxDispatchDescUpscale dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = command;
  dispatch.color = Wrap(frame.color, read);
  dispatch.depth = Wrap(frame.depth, read);
  dispatch.motionVectors = Wrap(frame.motion, read);
  dispatch.reactive = Wrap(frame.reactive, read);
  dispatch.output = Wrap(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch.jitterOffset = {frame.jitter_x, frame.jitter_y};
  dispatch.motionVectorScale = {1, 1};
  dispatch.renderSize = {state.render.width, state.render.height};
  dispatch.upscaleSize = {state.output.width, state.output.height};
  dispatch.frameTimeDelta = frame.elapsed_ms;
  dispatch.preExposure = 1;
  dispatch.reset = !state.dispatched || frame.reset_history;
  dispatch.cameraNear = frame.near_plane;
  dispatch.cameraFar = frame.far_plane;
  dispatch.cameraFovAngleVertical = frame.vertical_fov;
  dispatch.viewSpaceToMetersFactor = frame.meters_per_unit;
  Check(state.dispatch(&state.context, &dispatch.header), "Dispatch Vulkan FSR 3");
  state.dispatched = true;
}

}  // namespace wrathfall
