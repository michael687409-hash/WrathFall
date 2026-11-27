#pragma once

#include <cstdint>

namespace wrathfall::vulkan_trace {

inline constexpr uint64_t kNoId = UINT64_MAX;
inline constexpr int32_t kNoResult = INT32_MIN;

int64_t Timestamp() noexcept;
void Write(const char* stage, uint64_t id, uint64_t related, int64_t begin,
           int64_t end, int32_t result = kNoResult) noexcept;
void Point(const char* stage, uint64_t id, uint64_t related = kNoId) noexcept;

class Span {
 public:
  Span(const char* stage, uint64_t id, uint64_t related = kNoId) noexcept
      : stage_(stage), id_(id), related_(related), begin_(Timestamp()) {}
  ~Span() { Finish(); }
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  void Finish(int32_t result = kNoResult) noexcept;

 private:
  const char* stage_;
  uint64_t id_, related_;
  int64_t begin_;
};

// The presenter refresh callback runs synchronously on the guest command thread.
extern thread_local uint64_t guest_swap_frame;

class GuestSwap {
 public:
  explicit GuestSwap(uint64_t frame) noexcept
      : timing_("guest-swap", frame), previous_(guest_swap_frame) { guest_swap_frame = frame; }
  ~GuestSwap() { guest_swap_frame = previous_; }

 private:
  Span timing_;
  uint64_t previous_;
};

}  // namespace wrathfall::vulkan_trace
