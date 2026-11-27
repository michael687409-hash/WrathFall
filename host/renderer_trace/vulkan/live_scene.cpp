#include "gpu/xenos/host/vulkan/vulkan_objects.h"
#include "gpu/xenos/vulkan/command_processor.h"
#include "live_scene.h"
#include "dlss_vulkan.h"
#include "fsr_vulkan.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace wrathfall::vulkan_live {
namespace {

struct SceneEvidence {
  uint64_t frame = UINT64_MAX;
  unsigned world_draws = 0;
  unsigned motion_draws = 0;
  unsigned screen_draws = 0;
  unsigned hud_blends = 0;
  unsigned stencil_draws = 0;
  unsigned depth_samples = 0;
  unsigned depth_format = 0, depth_pitch_tiles = 0, depth_tile_base = 0, depth_size = 0;
  unsigned color_format = 0, color_pitch_pixels = 0, color_samples = 0;
  uint64_t first_overlay_vs = 0, first_overlay_ps = 0;
  bool host_compositor = false;
  bool compositor = false;
  bool host_depth_image = false;
  bool depth_resolved = false;
  bool camera_found = false;
  float near_depth = 0;
  float far_depth = 0;
  std::array<float, 16> camera{};
};

std::filesystem::path EnvironmentPath(const wchar_t* name) {
  const auto size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring path(size, L'\0');
  const auto used = GetEnvironmentVariableW(name, path.data(), size);
  if (!used || used >= size) return {};
  path.resize(used);
  return path;
}

std::ofstream& Log() {
  const auto open = [] {
    const auto path = EnvironmentPath(L"WRATHFALL_VK_SCENE_TRACE");
    return path.empty() ? std::ofstream{} : std::ofstream(path, std::ios::trunc);
  };
  static auto stream = open();
  return stream;
}

// Lives on the Vulkan command processor thread and is released after GPU idle.
thread_local std::unique_ptr<FsrVulkan> fsr_probe;
std::unique_ptr<DlssVulkan> dlss_probe;
VkDevice dlss_device = VK_NULL_HANDLE;

// A 1280x720 single-sample Xenos target occupies 16x45 80x16 tiles.
constexpr VkDeviceSize kTileBytes = 80 * 16 * sizeof(uint32_t);
constexpr VkDeviceSize kInputBytes = 16 * 45 * kTileBytes;
constexpr VkDeviceSize kColorBase = 720 * kTileBytes;
struct Readback {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint64_t frame = UINT64_MAX;
  uint64_t next_frame = 0;
  uint32_t memory_type = UINT32_MAX;
  bool color = false, depth = false, done = false;
};
thread_local Readback capture;

bool CaptureRequested(uint64_t frame) {
  static const uint64_t first_frame = [] {
    const auto value = EnvironmentPath(L"WRATHFALL_VK_CAPTURE_FRAME").native();
    if (value.empty()) return UINT64_MAX;
    wchar_t* end = nullptr;
    const auto number = std::wcstoull(value.c_str(), &end, 10);
    return *end ? UINT64_MAX : static_cast<uint64_t>(number);
  }();
  if (capture.done || frame < first_frame || frame < capture.next_frame) return false;
  if (capture.frame != frame) {
    capture.frame = frame;
    capture.color = capture.depth = false;
  }
  return true;
}

void RequireExtensions(const std::vector<const char*>& required,
                       std::span<const VkExtensionProperties> available) {
  for (const char* name : required) {
    if (std::none_of(available.begin(), available.end(), [name](const auto& extension) {
          return std::strcmp(name, extension.extensionName) == 0;
        })) {
      throw std::runtime_error(std::string("Missing Vulkan extension ") + name);
    }
  }
}

void AppendExtensions(const std::vector<const char*>& required, std::vector<const char*>& enabled) {
  for (const char* name : required) {
    if (std::none_of(enabled.begin(), enabled.end(), [name](const char* extension) {
          return std::strcmp(name, extension) == 0;
        })) enabled.push_back(name);
  }
}

SceneEvidence& Evidence(uint64_t frame) {
  // Draws and resolves for one title are delivered on its GPU command thread.
  static thread_local SceneEvidence state;
  if (state.frame != frame) state = SceneEvidence{frame};
  return state;
}

}  // namespace

bool Enabled() {
  return Log().is_open();
}

void PrepareInstance(uint32_t api_version, std::span<const VkExtensionProperties> available,
                     std::vector<const char*>& enabled) {
  if (!Enabled()) return;
  const auto runtime = EnvironmentPath(L"WRATHFALL_VK_DLSS_PROBE_RUNTIME");
  if (runtime.empty()) return;
  try {
    if (api_version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 is unavailable");
    auto probe = std::make_unique<DlssVulkan>(runtime);
    RequireExtensions(probe->InstanceExtensions(), available);
    AppendExtensions(probe->InstanceExtensions(), enabled);
    dlss_probe = std::move(probe);
    Log() << "dlss-instance-extensions\t" << dlss_probe->InstanceExtensions().size() << std::endl;
  } catch (const std::exception& failure) {
    Log() << "dlss-instance-error\t" << failure.what() << std::endl;
  }
}

bool PrepareDevice(VkPhysicalDevice physical, VkPhysicalDeviceProperties properties,
                   std::span<const VkExtensionProperties> available,
                   const VkPhysicalDeviceVulkan12Features& supported12,
                   const VkPhysicalDeviceVulkan13Features& supported13,
                   VkPhysicalDeviceVulkan12Features& enabled12,
                   VkPhysicalDeviceVulkan13Features& enabled13,
                   std::vector<const char*>& extensions) {
  if (!dlss_probe || dlss_device) return false;
  try {
    if (properties.apiVersion < VK_API_VERSION_1_3)
      throw std::runtime_error("Adapter lacks Vulkan 1.3");
    RequireExtensions(dlss_probe->DeviceExtensions(), available);
    dlss_probe->EnableDeviceFeatures(physical, supported12, supported13, enabled12, enabled13);
    AppendExtensions(dlss_probe->DeviceExtensions(), extensions);
    Log() << "dlss-device-extensions\t" << properties.deviceName << '\t'
          << dlss_probe->DeviceExtensions().size() << std::endl;
    return true;
  } catch (const std::exception& failure) {
    Log() << "dlss-device-error\t" << properties.deviceName << '\t' << failure.what() << std::endl;
    return false;
  }
}

void BindDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                uint32_t graphics_family) {
  dlss_device = device;
  try {
    dlss_probe->Bind(instance, physical, device, graphics_family, {1280, 720}, {2560, 1440},
                     asura::temporal::DlssPreset::K, false);
    Log() << "dlss-context\tK\t1280x720\t2560x1440" << std::endl;
  } catch (const std::exception& failure) {
    Log() << "dlss-context-error\t" << failure.what() << std::endl;
  }
}

void ReleaseDevice(VkDevice device) {
  if (!dlss_device || dlss_device != device) return;
  dlss_probe.reset();
  dlss_device = VK_NULL_HANDLE;
}

void ReleaseInstance() {
  dlss_probe.reset();
}

void Device(VkPhysicalDevice physical, VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc,
            uint32_t graphics_family) {
  if (!Enabled()) return;
  Log() << "vulkan-device\t" << (physical != VK_NULL_HANDLE)
        << "\tgraphics_family=" << graphics_family << std::endl;
  const auto runtime = EnvironmentPath(L"WRATHFALL_VK_FSR_PROBE_RUNTIME");
  if (runtime.empty()) return;
  try {
    fsr_probe = std::make_unique<FsrVulkan>(runtime, physical, device, get_device_proc,
                                            VkExtent2D{1280, 720}, VkExtent2D{2560, 1440}, false);
    Log() << "fsr-context\t" << fsr_probe->Provider() << "\t1280x720\t2560x1440" << std::endl;
  } catch (const std::exception& failure) {
    Log() << "fsr-context-error\t" << failure.what() << std::endl;
  }
}

void Shutdown(const rex::gpu::vulkan::VulkanCommandProcessor& processor) {
  fsr_probe.reset();
  if (!capture.memory) return;
  const auto* vulkan = processor.GetVulkanDevice();
  const auto& f = vulkan->GetFunctions();
  f.vkDestroyBuffer(vulkan->GetDevice(), capture.buffer, nullptr);
  f.vkFreeMemory(vulkan->GetDevice(), capture.memory, nullptr);
  capture = {};
}

bool DepthResolve(uint64_t frame, bool interlock, bool host_image, uint32_t samples, uint32_t format,
                  uint32_t pitch_tiles, uint32_t tile_base, uint32_t extent_length) {
  if (!Enabled()) return false;
  auto& evidence = Evidence(frame);
  evidence.depth_resolved = extent_length != 0;
  evidence.host_depth_image = host_image;
  evidence.depth_samples = samples;
  evidence.depth_format = format;
  evidence.depth_pitch_tiles = pitch_tiles;
  evidence.depth_tile_base = tile_base;
  evidence.depth_size = extent_length;
  return interlock && evidence.depth_resolved && samples == 1 && format == 1 && pitch_tiles == 16 &&
         tile_base == 0 && CaptureRequested(frame) && !capture.depth;
}

bool Draw(uint64_t frame, uint64_t vertex, uint64_t pixel, uint32_t width, uint32_t height,
          uint32_t color_base, uint32_t depth_base, bool host_targets, bool depth, bool stencil,
          const std::array<float, 16>& camera, float near_depth, float far_depth,
          uint32_t color_format, uint32_t pitch_pixels, uint32_t samples) {
  if (!Enabled()) return false;
  auto& evidence = Evidence(frame);
  if (width == 640 && height == 360 && color_base == 1624 &&
      (pixel == 0xa9e9e8cbe344da0d || pixel == 0xcfc6d41016f07ac2 ||
       pixel == 0xe7d0056ff4d559ee)) ++evidence.motion_draws;
  if (width != 1280 || height != 720) return false;
  if (depth && depth_base == 0) {
    ++evidence.world_draws;
    if (vertex == 0x05253eaa754b90a5 || vertex == 0x121a96d38a21bfdb) {
      evidence.camera_found = near_depth < far_depth &&
          std::all_of(camera.begin(), camera.end(), [](float value) { return std::isfinite(value); });
      if (evidence.camera_found) {
        evidence.camera = camera;
        evidence.near_depth = near_depth;
        evidence.far_depth = far_depth;
      }
    }
  }
  if (color_base != 720) return false;
  if (vertex == 0x9ac7fba7d9196e93 && pixel == 0x2e571939e5c4f114) {
    evidence.compositor = true;
    evidence.host_compositor = host_targets;
    evidence.color_format = color_format;
    evidence.color_pitch_pixels = pitch_pixels;
    evidence.color_samples = samples;
    evidence.screen_draws = evidence.hud_blends = evidence.stencil_draws = 0;
    return false;
  }
  if (evidence.compositor) {
    if (++evidence.screen_draws == 1) {
      evidence.first_overlay_vs = vertex;
      evidence.first_overlay_ps = pixel;
    }
    if (vertex == 0x5bf4e342d00f1593 && pixel == 0x2c1e4e099e31e037) ++evidence.hud_blends;
    if (stencil) ++evidence.stencil_draws;
    return !host_targets && evidence.screen_draws == 1 && evidence.color_format == 0 &&
           evidence.color_pitch_pixels == 1280 && evidence.color_samples == 1 &&
           CaptureRequested(frame) && capture.depth && !capture.color;
  }
  return false;
}

void CopyInterlockInput(rex::gpu::vulkan::VulkanCommandProcessor& processor, VkBuffer edram,
                        bool depth) {
  using namespace rex::gpu::vulkan;
  if (!capture.buffer && !objects::CreateDedicatedAllocationBuffer(
          processor.GetVulkanDevice(), 2 * kInputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          objects::MemoryPurpose::kReadback, capture.buffer, capture.memory, &capture.memory_type)) {
    capture.done = true;
    Log() << "input-capture-error\tCould not allocate readback buffer" << std::endl;
    return;
  }
  const VkBufferCopy region{depth ? 0 : kColorBase, depth ? 0 : kInputBytes, kInputBytes};
  // The capture does not change the render target cache's tracked use of guest EDRAM.
  constexpr VkPipelineStageFlags writers = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  constexpr VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  processor.PushBufferMemoryBarrier(edram, region.srcOffset, region.size, writers,
      VK_PIPELINE_STAGE_TRANSFER_BIT, writes, VK_ACCESS_TRANSFER_READ_BIT);
  processor.PushBufferMemoryBarrier(capture.buffer, region.dstOffset, region.size,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, false);
  processor.SubmitBarriers(true);
  processor.GetDeferredCommandBuffer().CmdVkCopyBuffer(edram, capture.buffer, 1, &region);
  processor.PushBufferMemoryBarrier(edram, region.srcOffset, region.size,
      VK_PIPELINE_STAGE_TRANSFER_BIT, writers, VK_ACCESS_TRANSFER_READ_BIT,
      writes | VK_ACCESS_SHADER_READ_BIT);
  processor.PushBufferMemoryBarrier(capture.buffer, region.dstOffset, region.size,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  if (depth) capture.depth = true;
  else capture.color = true;
}

bool CaptureReady() {
  return !capture.done && capture.color && capture.depth;
}

void ReadInterlockInputs(const rex::gpu::vulkan::VulkanCommandProcessor& processor) {
  capture.done = true;
  const auto* vulkan = processor.GetVulkanDevice();
  const auto& f = vulkan->GetFunctions();
  const VkDevice device = vulkan->GetDevice();
  void* mapped = nullptr;
  if (f.vkMapMemory(device, capture.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    Log() << "input-capture-error\tCould not map readback buffer" << std::endl;
    return;
  }
  VkResult valid = VK_SUCCESS;
  if (!(vulkan->GetMemoryTypes().host_coherent & (1u << capture.memory_type))) {
    const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
                                    capture.memory, 0, VK_WHOLE_SIZE};
    valid = f.vkInvalidateMappedMemoryRanges(device, 1, &range);
  }
  if (valid == VK_SUCCESS) {
    const auto* depth = static_cast<const uint32_t*>(mapped);
    const auto* color = depth + kInputBytes / sizeof(uint32_t);
    const size_t count = kInputBytes / sizeof(uint32_t);
    const size_t depth_changes = std::count_if(depth, depth + count, [first = depth[0]](uint32_t d) {
      return (d >> 8) != (first >> 8);
    });
    const size_t color_changes = std::count_if(color, color + count, [first = color[0]](uint32_t c) {
      return (c & 0xffffff) != (first & 0xffffff);
    });
    Log() << "input-capture\t" << capture.frame << "\t1280x720\tcolor_changes=" << color_changes
          << "\tdepth_changes=" << depth_changes << "\tpixels=" << count << std::endl;
    // Menu and transition frames often carry flat depth. Wait for a frame with scene data.
    capture.done = depth_changes > 0 && color_changes > 0;
    capture.next_frame = capture.frame + 120;
    capture.color = capture.depth = false;
  } else {
    Log() << "input-capture-error\tCould not invalidate readback buffer" << std::endl;
  }
  f.vkUnmapMemory(device, capture.memory);
}

void Present(uint64_t frame, uint32_t frontbuffer, uint32_t width, uint32_t height) {
  if (!Enabled()) return;
  auto& evidence = Evidence(frame);
  static thread_local unsigned scene_frames;
  if (!evidence.compositor && !evidence.depth_resolved && !evidence.motion_draws) return;
  if (++scene_frames > 10 && scene_frames % 120 != 0) return;
  auto& log = Log();
  log << "scene\t" << frame << '\t' << width << 'x' << height
      << "\tfrontbuffer=" << std::hex << frontbuffer << std::dec
      << "\tcompositor=" << evidence.compositor
      << "\tcompositor_path=" << (evidence.host_compositor ? "host" : "interlock")
      << "\tdepth=" << evidence.depth_resolved << "\thost_depth_image=" << evidence.host_depth_image
      << "\tsamples=" << evidence.depth_samples << "\tworld=" << evidence.world_draws
      << "\tmotion=" << evidence.motion_draws << "\tcamera=" << evidence.camera_found
      << "\thud=" << evidence.screen_draws << "\tblend=" << evidence.hud_blends
      << "\tstencil=" << evidence.stencil_draws;
  if (evidence.compositor) {
    log << "\tcolor_layout=" << evidence.color_format << ',' << evidence.color_pitch_pixels
        << ',' << evidence.color_samples;
  }
  if (evidence.screen_draws) {
    log << "\toverlay=" << std::hex << evidence.first_overlay_vs << ',' << evidence.first_overlay_ps
        << std::dec;
  }
  if (evidence.depth_resolved) {
    log << "\tdepth_layout=" << evidence.depth_format << ',' << evidence.depth_pitch_tiles
        << ',' << evidence.depth_tile_base << ',' << evidence.depth_size;
  }
  if (evidence.camera_found) {
    log << "\tdepth_range=" << evidence.near_depth << ',' << evidence.far_depth
        << "\tprojection=";
    for (float value : evidence.camera) log << value << ',';
  }
  log << std::endl;
}

}  // namespace wrathfall::vulkan_live
