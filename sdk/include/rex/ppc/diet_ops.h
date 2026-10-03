/**
 * @file        ppc/diet_ops.h
 * @brief       Mass Effect "diet" helpers: cheaper but bit-identical lowerings of single PPC instructions.
 *
 * Included from context.h. Each helper reproduces exactly the value the previous generated expression produced
 * (including the upper word of the 64-bit register image), so it can be switched on per option.
 */

#pragma once

#include <cstdint>

namespace rex::ppc {

/// fctiwz: truncate toward zero to int32 with PPC saturation, NaN -> 0x80000000. Returned as the s64 register image
/// that the old expression produced: NaN -> 0x0000000080000000 (zero extended), everything else sign extended.
/// The old expression was `isnan ? 0x80000000U : v >= INT_MAX ? INT_MAX : simde_mm_cvttsd_si32(v)`, where simde's
/// portable version returns INT32_MIN for v <= INT32_MIN: that is exactly the saturating AArch64 fcvtzs (positive
/// overflow -> INT32_MAX, negative overflow -> INT32_MIN); only NaN (fcvtzs gives 0) needs the select.
inline int64_t fctiwz(double v) noexcept {
#if defined(__aarch64__)
  int32_t r;
  asm("fcvtzs %w0, %d1" : "=r"(r) : "w"(v));
  return __builtin_isnan(v) ? int64_t(0x80000000U) : int64_t(r);
#else
  if (__builtin_isnan(v))
    return int64_t(0x80000000U);
  if (v >= 2147483647.0)
    return INT32_MAX;
  return (v > -2147483648.0) ? int64_t(int32_t(v)) : int64_t(INT32_MIN);
#endif
}

}  // namespace rex::ppc
