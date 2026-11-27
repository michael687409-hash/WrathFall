#pragma once

#include "dlss_vulkan.h"
#include "fsr_vulkan.h"

#include <array>
#include <iosfwd>
#include <memory>

namespace rex::gpu::vulkan {
class VulkanCommandProcessor;
}

namespace wrathfall::vulkan_live {

class Temporal {
 public:
  explicit Temporal(const rex::gpu::vulkan::VulkanCommandProcessor& processor);
  ~Temporal();

  VkBuffer Inputs() const;
  bool Schedule(uint64_t frame, VkImage image, VkImageView output, bool written, VkImageView guest,
                uint32_t guest_width, uint32_t guest_height, bool swap_red_blue,
                const std::array<uint32_t, 256>& gamma, std::ostream& log);
  bool Record(VkCommandBuffer command, DlssVulkan* dlss, FsrVulkan* fsr,
              const std::array<float, 16>& projection, bool camera_found,
              std::array<float, 2> jitter, uint32_t motion_draws,
              uint32_t hud_draws, uint32_t hud_blends, uint32_t stencil_draws, std::ostream& log);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace wrathfall::vulkan_live
