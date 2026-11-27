#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace asura::frame_clock {

// Windows Graphics Capture reports SystemRelativeTime on this same QPC clock.
inline int64_t Now100ns() noexcept {
  static const int64_t frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
  }();
  LARGE_INTEGER ticks;
  QueryPerformanceCounter(&ticks);
  constexpr int64_t units_per_second = 10'000'000;
  return (ticks.QuadPart / frequency) * units_per_second +
      (ticks.QuadPart % frequency) * units_per_second / frequency;
}

inline std::filesystem::path VulkanTraceFile(const std::filesystem::path& logs,
                                             DWORD pid, HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (logs.empty() || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
  const auto start = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  return logs / ("vulkan-frame-" + std::to_string(pid) + "-" + std::to_string(start) + ".tsv");
}

}  // namespace asura::frame_clock
