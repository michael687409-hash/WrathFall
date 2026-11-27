#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "temporal_vulkan_frame.h"
#include "temporal_d3d12.h"

namespace wrathfall {

// Construct before the Vulkan instance so its extensions can be enabled at creation.
// Keep the device alive and finish submitted GPU work before releasing the context.
class DlssVulkan {
 public:
  explicit DlssVulkan(const std::filesystem::path& runtime_dir);
  ~DlssVulkan();

  const std::vector<const char*>& InstanceExtensions() const;
  const std::vector<const char*>& DeviceExtensions() const;
  void CheckDevice(VkPhysicalDevice physical, const VkPhysicalDeviceVulkan12Features& features12,
                   const VkPhysicalDeviceVulkan13Features& features13) const;
  void EnableDeviceFeatures(VkPhysicalDevice physical,
                            const VkPhysicalDeviceVulkan12Features& supported12,
                            const VkPhysicalDeviceVulkan13Features& supported13,
                            VkPhysicalDeviceVulkan12Features& enabled12,
                            VkPhysicalDeviceVulkan13Features& enabled13) const;
  void Bind(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family,
            VkExtent2D render, VkExtent2D output, asura::temporal::DlssPreset preset,
            bool inverted_depth);
  void Dispatch(VkCommandBuffer command, const VulkanUpscaleFrame& frame,
                const asura::temporal::Camera& camera);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace wrathfall
