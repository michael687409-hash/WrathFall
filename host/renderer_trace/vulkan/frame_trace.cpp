#include "frame_trace.h"

#include "../../frame_clock.h"

#include <fstream>
#include <mutex>
#include <utility>

namespace wrathfall::vulkan_trace {
namespace {

struct FrameLog {
  FrameLog() {
    try {
      constexpr auto variable = L"WRATHFALL_VK_FRAME_TRACE_DIR";
      const DWORD size = GetEnvironmentVariableW(variable, nullptr, 0);
      if (size == 0) return;
      std::wstring directory(size, L'\0');
      const DWORD length = GetEnvironmentVariableW(variable, directory.data(), size);
      if (length == 0 || length >= size) return;
      directory.resize(length);
      const auto file = asura::frame_clock::VulkanTraceFile(
          directory, GetCurrentProcessId(), GetCurrentProcess());
      if (file.empty()) return;
      output.open(file, std::ios::trunc);
      if (output) output << "stage\tid\trelated_id\tbegin_qpc_100ns\tend_qpc_100ns\tvk_result\n";
      enabled = output.is_open();
    } catch (...) {
      // Diagnostics must never prevent renderer initialization or presentation.
    }
  }

  bool enabled = false;
  std::mutex mutex;
  std::ofstream output;
  int64_t next_flush = 0;
};

FrameLog& Log() {
  static FrameLog log;
  return log;
}

}  // namespace

thread_local uint64_t guest_swap_frame = kNoId;

int64_t Timestamp() noexcept {
  return Log().enabled ? asura::frame_clock::Now100ns() : 0;
}

void Write(const char* stage, uint64_t id, uint64_t related, int64_t begin,
           int64_t end, int32_t result) noexcept {
  if (!begin) return;
  auto& log = Log();
  std::lock_guard lock(log.mutex);
  log.output << stage << '\t';
  if (id != kNoId) log.output << id;
  log.output << '\t';
  if (related != kNoId) log.output << related;
  log.output << '\t' << begin << '\t' << end << '\t';
  if (result != kNoResult) log.output << result;
  log.output << '\n';
  if (end >= log.next_flush) {
    log.next_flush = end + 2'500'000;  // 250 ms.
    log.output.flush();
  }
}

void Point(const char* stage, uint64_t id, uint64_t related) noexcept {
  const auto time = Timestamp();
  Write(stage, id, related, time, time);
}

void Span::Finish(int32_t result) noexcept {
  if (begin_) Write(stage_, id_, related_, std::exchange(begin_, 0), Timestamp(), result);
}

}  // namespace wrathfall::vulkan_trace
