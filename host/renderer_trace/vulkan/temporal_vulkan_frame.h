#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace wrathfall {

struct VulkanUpscaleImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageCreateInfo description{};
};

struct VulkanUpscaleFrame {
  VulkanUpscaleImage color, depth, motion, reactive, output;
  float jitter_x = 0, jitter_y = 0;
  float elapsed_ms = 1000.0f / 30.0f;
  float near_plane = 0.1f, far_plane = 1000.0f;
  float vertical_fov = 1.0f, meters_per_unit = 1.0f;
  bool reset_history = false;
};

}  // namespace wrathfall
