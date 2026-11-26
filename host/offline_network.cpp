#include <mutex>

#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/logger.h>
#include <rex/ppc/hook.h>

// Report a disconnected Xbox network before ReXGlue creates a host socket.
REX_HOOK_RAW(__imp__NetDll_socket) {
  constexpr u32 kNetworkDown = 10050;  // WSAENETDOWN.
  constexpr u32 kInvalidSocket = 0xFFFFFFFFu;
  if (auto* thread = REX_RUNTIME()->GetThreadRegistry().GetCallingThread()) {
    thread->SetLastError(kNetworkDown);
  }
  static std::once_flag offline_notice;
  std::call_once(offline_notice, [] { REXLOG_INFO("Guest socket requests are disabled in this build."); });
  ctx.r3.u64 = kInvalidSocket;
}
