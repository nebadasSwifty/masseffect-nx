// ORIG: sub_826EAF70
// NATIVE: n_826EAF70.h
// Object hash (see n_826EAF70.h): the CRC table is a constant in guest memory at 0x82E97C98, so the window is placed at
// 0x82E90000 (table + objects inside; the stack scratch is the top 0x100 bytes and is never read). The generator fills the
// table with random words (some entries all-ones / zero to provoke borrows), plants objects with random / zero / 0xFF
// bytes, objects that overlap the table, unaligned pointers and garbage in the upper half of r3. The function has no
// branches besides the fixed 16-iteration loop, so every path is covered by every call.
// The harness only extracts DEFINE_REX_FUNC blocks; this function's body lives in the generated __fast_ entry, so a verbatim
// copy of that generated function (app/generated/default/masseffect_recomp.137.cpp) is included here (the case is
// compiled in the harness TU together with the generated pch).
#include "masseffect_pch.h"
extern "C" uint64_t __fast_sub_826EAF70(PPCContext& __restrict ctx, uint8_t* base, uint64_t p_r3) {
	REX_FUNC_PROLOGUE();
	asm volatile("" ::: "memory");
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	// lis r9,-32023
	PPCRegister r3{};
	PPCRegister r5{};
	PPCRegister r6{};
	PPCRegister r7{};
	PPCRegister r8{};
	PPCRegister r9{};
	PPCRegister r10{};
	r3.u64 = p_r3;
	r9.s64 = -2098659328;
	// addi r8,r3,12
	r8.s64 = r3.s64 + 12;
	// li r11,-1
	r11.s64 = -1;
	// li r10,0
	r10.s64 = 0;
	// addi r9,r9,31896
	r9.s64 = r9.s64 + 31896;
loc_826EAF84:
	// lbzx r7,r10,r8
	r7.u64 = REX_LOAD_U8(r10.u32 + r8.u32);
	// rlwinm r6,r11,8,24,31
	r6.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 8) & 0xFF;
	// addi r10,r10,1
	r10.s64 = r10.s64 + 1;
	// xor r7,r7,r6
	r7.u64 = r7.u64 ^ r6.u64;
	// rlwinm r11,r11,8,0,23
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 8) & 0xFFFFFF00;
	// rlwinm r7,r7,2,0,29
	r7.u64 = __builtin_rotateleft64(r7.u32 | (r7.u64 << 32), 2) & 0xFFFFFFFC;
	// cmpwi cr6,r10,16
	cr6.compare<int32_t>(r10.s32, 16, xer);
	// lwzx r7,r7,r9
	r7.u64 = REX_LOAD_U32(r7.u32 + r9.u32);
	// xor r11,r7,r11
	r11.u64 = r7.u64 ^ r11.u64;
	// blt cr6,0x826eaf84
	if (cr6.lt) goto loc_826EAF84;
	// not r10,r11
	r10.u64 = ~r11.u64;
	// lwz r9,8(r3)
	r9.u64 = REX_LOAD_D_U32(r3.u32, 8);
	// lis r11,-25033
	r11.s64 = -1640562688;
	// lwz r7,4(r3)
	r7.u64 = REX_LOAD_D_U32(r3.u32, 4);
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// lwz r6,0(r3)
	r6.u64 = REX_LOAD_D_U32(r3.u32, 0);
	// rlwinm r8,r10,19,13,31
	r8.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 19) & 0x7FFFF;
	// ori r11,r11,31161
	r11.u64 = r11.u64 | 31161;
	// xor r9,r9,r8
	r9.u64 = r9.u64 ^ r8.u64;
	// subf r8,r9,r11
	r8.u64 = r11.u64 - r9.u64;
	// rlwinm r5,r9,8,0,23
	r5.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 8) & 0xFFFFFF00;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r5
	r8.u64 = r8.u64 ^ r5.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r5,r8,19,13,31
	r5.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 19) & 0x7FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r8,r9
	r9.u64 = r9.u64 - r8.u64;
	// xor r10,r10,r5
	r10.u64 = r10.u64 ^ r5.u64;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// rlwinm r5,r10,20,12,31
	r5.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 20) & 0xFFFFF;
	// xor r9,r9,r5
	r9.u64 = r9.u64 ^ r5.u64;
	// subf r8,r9,r8
	r8.u64 = r8.u64 - r9.u64;
	// rlwinm r5,r9,16,0,15
	r5.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 16) & 0xFFFF0000;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r5
	r8.u64 = r8.u64 ^ r5.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r5,r8,27,5,31
	r5.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 27) & 0x7FFFFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r8,r9
	r9.u64 = r9.u64 - r8.u64;
	// xor r10,r10,r5
	r10.u64 = r10.u64 ^ r5.u64;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// rlwinm r5,r10,29,3,31
	r5.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 29) & 0x1FFFFFFF;
	// xor r9,r9,r5
	r9.u64 = r9.u64 ^ r5.u64;
	// subf r8,r9,r8
	r8.u64 = r8.u64 - r9.u64;
	// rlwinm r5,r9,10,0,21
	r5.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 10) & 0xFFFFFC00;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r5
	r8.u64 = r8.u64 ^ r5.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r8,r8,17,15,31
	r8.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 17) & 0x1FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// xor r10,r10,r8
	r10.u64 = r10.u64 ^ r8.u64;
	// subf r9,r10,r7
	r9.u64 = r7.u64 - r10.u64;
	// rlwinm r8,r10,19,13,31
	r8.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 19) & 0x7FFFF;
	// xor r9,r9,r8
	r9.u64 = r9.u64 ^ r8.u64;
	// subf r8,r9,r11
	r8.u64 = r11.u64 - r9.u64;
	// rlwinm r7,r9,8,0,23
	r7.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 8) & 0xFFFFFF00;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r7
	r8.u64 = r8.u64 ^ r7.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r7,r8,19,13,31
	r7.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 19) & 0x7FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r8,r9
	r9.u64 = r9.u64 - r8.u64;
	// xor r10,r10,r7
	r10.u64 = r10.u64 ^ r7.u64;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// rlwinm r7,r10,20,12,31
	r7.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 20) & 0xFFFFF;
	// xor r9,r9,r7
	r9.u64 = r9.u64 ^ r7.u64;
	// subf r8,r9,r8
	r8.u64 = r8.u64 - r9.u64;
	// rlwinm r7,r9,16,0,15
	r7.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 16) & 0xFFFF0000;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r7
	r8.u64 = r8.u64 ^ r7.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r7,r8,27,5,31
	r7.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 27) & 0x7FFFFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r8,r9
	r9.u64 = r9.u64 - r8.u64;
	// xor r10,r10,r7
	r10.u64 = r10.u64 ^ r7.u64;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// rlwinm r7,r10,29,3,31
	r7.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 29) & 0x1FFFFFFF;
	// xor r9,r9,r7
	r9.u64 = r9.u64 ^ r7.u64;
	// subf r8,r9,r8
	r8.u64 = r8.u64 - r9.u64;
	// rlwinm r7,r9,10,0,21
	r7.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 10) & 0xFFFFFC00;
	// subf r8,r10,r8
	r8.u64 = r8.u64 - r10.u64;
	// xor r8,r8,r7
	r8.u64 = r8.u64 ^ r7.u64;
	// subf r10,r8,r10
	r10.u64 = r10.u64 - r8.u64;
	// rlwinm r8,r8,17,15,31
	r8.u64 = __builtin_rotateleft64(r8.u32 | (r8.u64 << 32), 17) & 0x1FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// xor r10,r10,r8
	r10.u64 = r10.u64 ^ r8.u64;
	// rlwinm r8,r10,19,13,31
	r8.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 19) & 0x7FFFF;
	// subf r9,r10,r6
	r9.u64 = r6.u64 - r10.u64;
	// xor r9,r9,r8
	r9.u64 = r9.u64 ^ r8.u64;
	// rlwinm r8,r9,8,0,23
	r8.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 8) & 0xFFFFFF00;
	// subf r11,r9,r11
	r11.u64 = r11.u64 - r9.u64;
	// subf r11,r10,r11
	r11.u64 = r11.u64 - r10.u64;
	// xor r11,r11,r8
	r11.u64 = r11.u64 ^ r8.u64;
	// subf r10,r11,r10
	r10.u64 = r10.u64 - r11.u64;
	// rlwinm r8,r11,19,13,31
	r8.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 19) & 0x7FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r11,r9
	r9.u64 = r9.u64 - r11.u64;
	// xor r10,r10,r8
	r10.u64 = r10.u64 ^ r8.u64;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// rlwinm r8,r10,20,12,31
	r8.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 20) & 0xFFFFF;
	// xor r9,r9,r8
	r9.u64 = r9.u64 ^ r8.u64;
	// subf r11,r9,r11
	r11.u64 = r11.u64 - r9.u64;
	// rlwinm r8,r9,16,0,15
	r8.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 16) & 0xFFFF0000;
	// subf r11,r10,r11
	r11.u64 = r11.u64 - r10.u64;
	// xor r11,r11,r8
	r11.u64 = r11.u64 ^ r8.u64;
	// subf r10,r11,r10
	r10.u64 = r10.u64 - r11.u64;
	// rlwinm r8,r11,27,5,31
	r8.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 27) & 0x7FFFFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// subf r9,r11,r9
	r9.u64 = r9.u64 - r11.u64;
	// xor r10,r10,r8
	r10.u64 = r10.u64 ^ r8.u64;
	// rlwinm r8,r10,29,3,31
	r8.u64 = __builtin_rotateleft64(r10.u32 | (r10.u64 << 32), 29) & 0x1FFFFFFF;
	// subf r9,r10,r9
	r9.u64 = r9.u64 - r10.u64;
	// xor r9,r9,r8
	r9.u64 = r9.u64 ^ r8.u64;
	// subf r11,r9,r11
	r11.u64 = r11.u64 - r9.u64;
	// rlwinm r8,r9,10,0,21
	r8.u64 = __builtin_rotateleft64(r9.u32 | (r9.u64 << 32), 10) & 0xFFFFFC00;
	// subf r11,r10,r11
	r11.u64 = r11.u64 - r10.u64;
	// xor r11,r11,r8
	r11.u64 = r11.u64 ^ r8.u64;
	// subf r10,r11,r10
	r10.u64 = r10.u64 - r11.u64;
	// rlwinm r11,r11,17,15,31
	r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 17) & 0x1FFFF;
	// subf r10,r9,r10
	r10.u64 = r10.u64 - r9.u64;
	// xor r3,r10,r11
	r3.u64 = r10.u64 ^ r11.u64;
	// blr 
	ctx.r5.u64 = r5.u64;
	ctx.r6.u64 = r6.u64;
	ctx.r7.u64 = r7.u64;
	ctx.r8.u64 = r8.u64;
	ctx.r10.u64 = r10.u64;
	return r3.u64;
}

extern "C" void __imp__sub_826EAF70(PPCContext&, uint8_t*);
namespace {
void gen_826EAF70(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x82E90000;
  auto st32 = [&](uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  };
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t v = r.u32();
    if (r.chance(0.03)) v = 0xFFFFFFFFu;
    else if (r.chance(0.03)) v = 0;
    st32(win + 0x7C98 + i * 4, v);
  }
  uint32_t o;
  switch (r.below(6)) {
    case 0: o = win + 0x7C98 - 40 + r.below(0x400 + 40); break;  // overlaps the table
    case 1: o = win + 0x100 + r.below(0x1000); break;
    default: o = win + 0x9000 + r.below(0x6000); break;
  }
  const uint32_t mode = r.below(5);
  for (uint32_t i = 0; i < 40; ++i) {
    uint32_t a = o + i;
    if (a >= win + 0x7C98 && a < win + 0x8098) continue;  // keep the table intact
    uint8_t b = uint8_t(r.u32());
    if (mode == 1) b = 0xFF;
    else if (mode == 2) b = 0;
    else if (mode == 3 && r.chance(0.5)) b = r.chance(0.5) ? 0xFF : 0;
    base[a] = b;
  }
  ctx.r3.u64 = o;
  if (r.chance(0.3)) ctx.r3.u64 |= r.u64() & 0xFFFFFFFF00000000ull;
}
fuzz::Registrar reg_826EAF70({"sub_826EAF70", __imp__sub_826EAF70, me::hot::n_826EAF70::Native, me::hot::n_826EAF70::Writes,
                              me::hot::n_826EAF70::kCmp, 0x82E90000, 0x20000, gen_826EAF70});
}  // namespace
