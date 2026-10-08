// sub_8245FF18 - TFieldIterator<UProperty>::IterateToNext(): advance a property iterator to the next field whose class is
// UProperty or a subclass, walking up the struct's super chain when a struct runs out.
// English copy of editions/ru/overlay/app/src/native/hot/n_8245FF18.h (Russian build, same function): the English
// function and its StaticClass getter are instruction-for-instruction the Russian ones except the singleton's data
// offset (-3896 here, -3864 in the Russian build). Fuzzed against the English generated code on 2026-10-08
// (tests/hot_fuzz/cases/case_8245FF18.inc).
//
//   r3 = iterator: [+0] current struct (UStruct*), [+4] current field (UField*).
//   loop:  while (it.struct) {
//            while (it.field) {
//              cls = UProperty::StaticClass();        // lazy singleton, see kPropertyClass
//              f = it.field;                          // re-read after the call (r10)
//              for (c = f->Class [+52]; c; c = c->SuperField [+60]) if (c == cls) return;   // IsA
//              if (!cls) return;
//              it.field = f->Next [+64];
//            }
//            it.struct = it.struct->GetSuperStruct();  // virtual, vtable byte offset 304 (r3 = struct)
//            if (it.struct) it.field = it.struct->Children [+76];
//          }
// Profile (Feros firefight, main thread): ~1.1 % leaf (2 % of the busy hitch samples); called by
// UObject::GetInterpPropertyNames (4 call sites), which a BioWare actor tick runs every frame.
// Note: every 8-hex-digit 82xxxxxx number in app sources marks that guest function as hooked for
// tools/direct_calls.py, so this header names no other function addresses.
//
// Exactness:
//  * The StaticClass getter is inlined when the singleton is set (it is then a pure load; its own stack frame is
//    callee-owned scratch). If the singleton is 0 at entry the native version declines (the original runs: it
//    constructs the class) and Writes() reports overflow. If the singleton became 0 later (only possible through the
//    virtual callee) the guest getter itself is called, exactly as the original does.
//  * The virtual GetSuperStruct call is made as the generated REX_CALL_INDIRECT_FUNC does (me_hot_call.h), with the
//    state the callee can observe identical to the original: r1 = frame (back chain, saved LR word and the r31 spill
//    slot written), r3 = struct, r10 = last field read (or the caller's r10), lr = caller's lr (truncated to 32 bits
//    once the getter has "run", as the getter's epilogue does), r4-r9 / r11 / r12 untouched (r11 / r12 are locals in
//    the generated code).
//  * Returned registers: r3 exactly as the original (getter result on the IsA / null-class exits, GetSuperStruct
//    result or the caller's r3 on the end-of-chain exit), r10 too; r1 restored as `r1 += 96` (64-bit add), lr =
//    the saved 32-bit word. No FPU / vector use. Memory written: the iterator's 8 bytes (+ stack scratch).
#pragma once

#include "../me_hot_common.h"
#include "../me_hot_call.h"

extern "C" void __imp__sub_82267B38(PPCContext&, uint8_t*);  // UProperty::StaticClass()

namespace me::hot::n_8245FF18 {

inline constexpr Cmp kCmp = {R(3) | R(10), 0, 0};

inline constexpr uint32_t kPropertyClass = 0x82EB0000u - 3896u;  // 0x82EAF0C8: lis r31,-32021; lwz r3,-3896(r31)

inline bool Native(PPCContext& ctx, uint8_t* base) {
  if (Ld32(base, kPropertyClass) == 0) [[unlikely]] return false;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = sp - 96u;
  const uint32_t it = ctx.r3.u32;
  St32(base, sp - 8, uint32_t(ctx.lr));  // stw r12,-8(r1)
  St64(base, sp - 16, 0);                // std r31,-16(r1): r31 is a zeroed local in the generated code
  St32(base, frame, sp);                 // stwu r1,-96(r1)
  ctx.r1.u32 = frame;
  while (Ld32(base, it + 0) != 0) {  // loc_8245FF9C
    while (Ld32(base, it + 4) != 0) {  // loc_8245FF68 -> loc_8245FF30
      uint32_t cls = Ld32(base, kPropertyClass);
      if (cls != 0) [[likely]] {
        ctx.r3.u64 = cls;
        ctx.lr = uint32_t(ctx.lr);  // the getter's epilogue reloads the 32-bit LR word it saved
      } else {
        __imp__sub_82267B38(ctx, base);
        cls = ctx.r3.u32;
      }
      const uint32_t f = Ld32(base, it + 4);
      ctx.r10.u64 = f;
      for (uint32_t c = Ld32(base, f + 52); c != 0; c = Ld32(base, c + 60)) {
        if (c == cls) goto done;
      }
      if (cls == 0) goto done;
      St32(base, it + 4, Ld32(base, f + 64));
    }
    const uint32_t s = Ld32(base, it + 0);
    ctx.r3.u64 = s;
    CallIndirect(ctx, base, Ld32(base, Ld32(base, s + 0) + 304));
    const uint32_t sup = ctx.r3.u32;
    St32(base, it + 0, sup);
    if (sup != 0) St32(base, it + 4, Ld32(base, sup + 76));
  }
done:
  ctx.r1.s64 = ctx.r1.s64 + 96;
  ctx.lr = Ld32(base, ctx.r1.u32 - 8);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  if (Ld32(base, kPropertyClass) == 0) {  // declined: the original constructs the class
    w.overflow = true;
    return;
  }
  w.Add(ctx.r3.u32, 8);
}

}  // namespace me::hot::n_8245FF18
