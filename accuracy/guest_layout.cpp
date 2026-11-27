#include <array>
#include <bit>
#include <cstdio>
#include <cstring>

#include <rex/kernel/heap.h>
#include <rex/memory/endian.h>

#include "generated/default/asuras_wrath_pch.h"

namespace {

bool CheckScalarLayout(rex::kernel::GuestAddressSpace& memory) {
  auto* base = memory.GetVirtualMembase();
  const u32 address = memory.SystemHeapAlloc(24);
  if (!address) {
    return false;
  }

  REX_STORE_U8(address, 0xA5);
  REX_STORE_U16(address + 2, 0x1234);
  REX_STORE_U32(address + 4, 0x89ABCDEF);
  REX_STACK_STORE_U64(address + 8, 0x0123456789ABCDEF);
  REX_STORE_U32(address + 16, std::bit_cast<u32>(-0.0f));
  REX_STORE_U32(address + 20, 0x7FC12345);

  const std::array<u8, 24> expected = {
      0xA5, 0x00, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
      0x80, 0x00, 0x00, 0x00, 0x7F, 0xC1, 0x23, 0x45,
  };
  const auto* bytes = memory.TranslateVirtual(address);
  const bool correct =
      std::memcmp(bytes, expected.data(), expected.size()) == 0 &&
      REX_LOAD_U8(address) == 0xA5 && REX_LOAD_U16(address + 2) == 0x1234 &&
      REX_STACK_LOAD_U32(address + 4) == 0x89ABCDEF &&
      REX_LOAD_U64(address + 8) == 0x0123456789ABCDEF &&
      std::bit_cast<u32>(rex::memory::load_and_swap<float>(bytes + 16)) == 0x80000000 &&
      std::bit_cast<u32>(rex::memory::load_and_swap<float>(bytes + 20)) == 0x7FC12345;
  memory.SystemHeapFree(address);
  return correct;
}

bool CheckPhysicalView(rex::kernel::GuestAddressSpace& memory) {
  auto* base = memory.GetVirtualMembase();
  auto* heap = memory.LookupHeapByType(true, 0x1000);
  u32 guest = 0;
  if (!heap || !heap->Alloc(0x1000, 0x1000,
                            rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                            rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite,
                            false, &guest)) {
    return false;
  }
  bool correct = false;
  if (guest >= 0xE0000000) {
    const u32 physical = (guest - 0xE0000000) + 0x1000;
    auto* bytes = memory.TranslatePhysical(physical);
    REX_STORE_U32(guest, 0xFEDCBA98);
    const std::array<u8, 4> expected = {0xFE, 0xDC, 0xBA, 0x98};
    correct = memory.GetPhysicalAddress(guest) == physical &&
              std::memcmp(bytes, expected.data(), expected.size()) == 0;
    rex::memory::store_and_swap<u32>(bytes, 0x6543210F);
    correct = correct && REX_LOAD_U32(guest) == 0x6543210F;
  }
  return heap->Release(guest) && correct;
}

}  // namespace

int main() {
  rex::kernel::GuestAddressSpace memory;
  if (!memory.Initialize()) {
    std::fputs("Could not map the Xbox 360 guest address space.\n", stderr);
    return 1;
  }
  if (!CheckScalarLayout(memory)) {
    std::fputs("Xbox 360 big-endian scalar or stack layout check failed.\n", stderr);
    return 1;
  }
  if (!CheckPhysicalView(memory)) {
    std::fputs("Xbox 360 physical memory view check failed.\n", stderr);
    return 1;
  }
  std::puts("Passed Xbox 360 guest layout checks: 8/16/32/64-bit access, stack, float bits, and physical view.");
}
