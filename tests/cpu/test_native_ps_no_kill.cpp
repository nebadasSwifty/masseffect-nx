#include "me_native_ps_no_kill.h"
#include <cassert>
#include <vector>

using me::native::ProveStraightLinePixelNoKill;

// Actual host-order PS10527 utility microcode from v20 container, independent
// of its package index: ALLOC colors; EXEC_END MAX c0,c0 to color0; padding.
static std::vector<uint32_t> Utility() {
  return {0, 0x1001C400, 0x22000000, 0xC80F8000, 0, 0xC2000000, 0, 0, 0};
}

int main() {
  assert(ProveStraightLinePixelNoKill(Utility()));
  assert(!ProveStraightLinePixelNoKill({}));
  auto bad = Utility(); bad.pop_back(); assert(!ProveStraightLinePixelNoKill(bad));
  bad.assign(129, 0); assert(!ProveStraightLinePixelNoKill(bad));
  for (uint32_t vector = 24; vector <= 27; ++vector) {
    for (bool predicated : {false, true}) {
      bad = Utility(); bad[5] = (bad[5] & ~(31u << 24)) | vector << 24;
      bad[3] &= ~(0xFFu << 16); // no write masks does NOT remove side effects
      if (predicated) bad[4] |= 1u << 28;
      assert(!ProveStraightLinePixelNoKill(bad));
    }
  }
  for (uint32_t scalar = 35; scalar <= 39; ++scalar) {
    bad = Utility(); bad[3] = (bad[3] & ~(63u << 26)) | scalar << 26;
    assert(!ProveStraightLinePixelNoKill(bad));
  }
  for (uint32_t scalar : {41u, 51u, 63u}) {
    bad = Utility(); bad[3] = (bad[3] & ~(63u << 26)) | scalar << 26;
    assert(!ProveStraightLinePixelNoKill(bad));
  }
  for (uint32_t vector : {30u,31u}) {
    bad = Utility(); bad[5] = (bad[5] & ~(31u << 24)) | vector << 24;
    assert(!ProveStraightLinePixelNoKill(bad));
  }
  bad = Utility(); bad[1] |= 1u << 9; // ALLOC memory
  assert(!ProveStraightLinePixelNoKill(bad));
  bad = Utility(); bad[3] = (bad[3] & ~63u) | 32; // memexport address
  assert(!ProveStraightLinePixelNoKill(bad));
  for (uint32_t opcode : {3u,4u,5u,6u,7u,8u,9u,10u,11u,13u,14u,15u}) {
    bad = Utility(); bad[2] = (bad[2] & 0x0FFFFFFF) | opcode << 28;
    assert(!ProveStraightLinePixelNoKill(bad));
  }
  bad = Utility(); bad[2] |= 1; // fetch sequence in terminal exec
  assert(!ProveStraightLinePixelNoKill(bad));
  bad = Utility(); bad[1] = (bad[1] & 0xFFFF) | (uint32_t(0x1FFF) << 16); // address4095
  assert(!ProveStraightLinePixelNoKill(bad));
  bad = Utility(); bad[2] = 0x12000000; // no terminal end
  assert(!ProveStraightLinePixelNoKill(bad));
  // Add an unreachable malicious CF pair before the body; reject without any
  // attempt to read its enormous instruction address.
  bad = {0,0x1002C400,0x22000000,0x00001FFF,0x00002000,0,
         0xC80F8000,0,0xC2000000};
  assert(!ProveStraightLinePixelNoKill(bad));
  // Same enlarged structure with inert padding is valid.
  bad[3] = bad[4] = bad[5] = 0;
  assert(ProveStraightLinePixelNoKill(bad));
}
