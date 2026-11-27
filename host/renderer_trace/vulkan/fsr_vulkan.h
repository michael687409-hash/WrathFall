#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "temporal_vulkan_frame.h"

namespace wrathfall {

// Record on a graphics/compute command buffer outside rendering. Transition inputs
// to SHADER_READ_ONLY_OPTIMAL and output to GENERAL before calling Dispatch.
// Finish outstanding GPU work before destroying the context.
class FsrVulkan {
 public:
  FsrVulkan(const std::filesystem::path& runtime_dir, VkPhysicalDevice physical,
            VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc, VkExtent2D render,
            VkExtent2D output, bool inverted_depth);
  ~FsrVulkan();
  std::string_view Provider() const;
  void Dispatch(VkCommandBuffer command, const VulkanUpscaleFrame& frame);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace wrathfall
