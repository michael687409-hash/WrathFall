#pragma once

#include <cstdint>

namespace wrathfall::vulkan_live {

inline bool ShouldResetHistory(uint64_t frame, uint64_t previous_frame, bool camera_found,
                               bool had_camera, float camera_direction) {
  return previous_frame == UINT64_MAX || frame != previous_frame + 1 ||
      (camera_found && (!had_camera || camera_direction < 0.92f));
}

}  // namespace wrathfall::vulkan_live
