// ReXGlue 0.11.0 binds XAM to its initial workspace before resizing it from the XEX.
// Resolve the live workspace when the game uses the affected allocation exports.
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <windows.h>

#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/ppc/hook.h>

namespace {

constexpr u32 kWorkspace = 0x10000000;
constexpr u32 kSystemHeap = 0x04000000;
constexpr u32 kNoZero = 0x00100000;
constexpr u32 kPhysical = 0x40000000;

std::mutex allocations_mutex;
std::unordered_set<u32> workspace_allocations;

bool UsesWorkspace(u32 flags) {
  return flags == 0 || ((flags & kWorkspace) && !(flags & kSystemHeap));
}

PPCFunc* KernelExport(const char* name) {
  for (const auto library : {L"rexkernel.dll", L"rexkernelrd.dll", L"rexkerneld.dll"}) {
    if (const HMODULE module = GetModuleHandleW(library)) {
      if (const auto entry = GetProcAddress(module, name)) {
        return reinterpret_cast<PPCFunc*>(entry);
      }
    }
  }
  throw std::runtime_error(std::string("Could not find original ReXGlue export ") + name);
}

u32 AllocWorkspace(u32 flags, u32 size, MappedU32 out) {
  if (!out) {
    return rex::kernel::X_E_INVALIDARG;
  }
  *out = 0;
  auto& workspace = REX_RUNTIME()->GetGuestWorkspace();
  const auto address = workspace.Allocate(size, !(flags & kNoZero));
  if (!address) {
    return rex::kernel::kXEOutOfMemory;
  }
  {
    const std::lock_guard lock(allocations_mutex);
    workspace_allocations.insert(*address);
  }
  *out = *address;
  return rex::kernel::X_E_SUCCESS;
}

u32 AllocWorkspaceEx(u32, u32 flags, u32 size, MappedU32 out) {
  return AllocWorkspace(flags, size, out);
}

}  // namespace

REX_HOOK_RAW(__imp__XamAlloc) {
  if (UsesWorkspace(ctx.r3.u32)) {
    rex::ppc::GuestToHostThunk<AllocWorkspace>(ctx, base);
  } else {
    static const auto original = KernelExport("__imp__XamAlloc");
    original(ctx, base);
  }
}

REX_HOOK_RAW(__imp__XamAllocEx) {
  if (!(ctx.r4.u32 & kPhysical) && UsesWorkspace(ctx.r4.u32)) {
    rex::ppc::GuestToHostThunk<AllocWorkspaceEx>(ctx, base);
  } else {
    static const auto original = KernelExport("__imp__XamAllocEx");
    original(ctx, base);
  }
}

REX_HOOK_RAW(__imp__XamFree) {
  const u32 address = ctx.r3.u32;
  {
    const std::lock_guard lock(allocations_mutex);
    if (workspace_allocations.erase(address)) {
      REX_RUNTIME()->GetGuestWorkspace().Free(address);
      return;
    }
  }
  static const auto original = KernelExport("__imp__XamFree");
  original(ctx, base);
}

void ResetXamWorkspaceTracking() {
  const std::lock_guard lock(allocations_mutex);
  workspace_allocations.clear();
}
