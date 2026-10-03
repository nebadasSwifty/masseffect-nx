// Mass Effect - the guest memory macros of the generated code (masseffect_pch.h), for natives that were derived
// mechanically from generated functions (tests/hot_fuzz/autonative.py) and so use REX_LOAD_U32(...) / REX_STORE_U32(...) /
// REX_RAW_ADDR(...) with a variable called `base`. In translation units that include the generated pch (the fuzz harness)
// REX_CONFIG_H_INCLUDED is defined and the pch's own definitions are used; in the app's sources (no pch) these are
// identical copies (same semantics: big-endian, no volatile, +0x1000 on Windows / Apple-silicon hosts above 0xE0000000).
#pragma once

#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#ifndef REX_CONFIG_H_INCLUDED
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)
#else
#define REX_PHYS_HOST_OFFSET(addr) 0u
#endif
#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#define REX_LOAD_U8(x) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U8(x, y) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
// The generated ppc_trap only logs (no state change); the hooked functions' traps are covered by the guard.
inline void ppc_trap(PPCContext&, u8*, u16) {}
#endif
