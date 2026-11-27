#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "temporal_vulkan_frame.h"

namespace rex::gpu::vulkan {
class VulkanCommandProcessor;
}

namespace wrathfall::vulkan_live {

bool Enabled();
bool TemporalRequested();
void PrepareInstance(uint32_t api_version, std::span<const VkExtensionProperties> available,
                     std::vector<const char*>& enabled);
bool PrepareDevice(VkPhysicalDevice physical, VkPhysicalDeviceProperties properties,
                   std::span<const VkExtensionProperties> available,
                   const VkPhysicalDeviceVulkan12Features& supported12,
                   const VkPhysicalDeviceVulkan13Features& supported13,
                   VkPhysicalDeviceVulkan12Features& enabled12,
                   VkPhysicalDeviceVulkan13Features& enabled13,
                   std::vector<const char*>& extensions);
void BindDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                uint32_t graphics_family);
void ReleaseDevice(VkDevice device);
void ReleaseInstance();
void Device(VkPhysicalDevice physical, VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc,
            uint32_t graphics_family);
void Shutdown(const rex::gpu::vulkan::VulkanCommandProcessor& processor);
bool DepthResolve(uint64_t frame, bool interlock, bool host_image, uint32_t samples, uint32_t format,
                  uint32_t pitch_tiles, uint32_t tile_base, uint32_t extent_length);
bool Draw(uint64_t frame, uint64_t vertex, uint64_t pixel, uint32_t width, uint32_t height,
          uint32_t color_base, uint32_t depth_base, bool host_targets, bool depth, bool stencil,
          const std::array<float, 16>& camera, float near_depth, float far_depth,
          uint32_t color_format, uint32_t pitch_pixels, uint32_t samples);
bool NeedsColorBeforeResolve(uint64_t frame);
bool SuppressMotionBlur(uint64_t frame);
void MarkMotionBlurSuppressed(uint64_t frame);
void CopyInterlockInput(rex::gpu::vulkan::VulkanCommandProcessor& processor, uint64_t frame,
                        VkBuffer edram, bool depth);
std::array<float, 2> Jitter(uint64_t frame);
bool CanPresentTemporal(uint64_t frame, uint32_t width, uint32_t height);
bool ScheduleTemporal(uint64_t frame,
                      VkImage image, VkImageView output, bool written, VkImageView guest,
                      uint32_t guest_width, uint32_t guest_height, bool swap_red_blue,
                      const std::array<uint32_t, 256>& gamma);
void RecordTemporal(VkCommandBuffer command);
bool CaptureReady();
void ReadInterlockInputs(const rex::gpu::vulkan::VulkanCommandProcessor& processor);
void Present(uint64_t frame, uint32_t frontbuffer, uint32_t width, uint32_t height);

}  // namespace wrathfall::vulkan_live
