// The float32 -> float16 conversion models of the ROP self-test (me_half_rop_test.h): the IEEE model against the
// host's own conversion, every half value round-trips under every model, and the other models differ exactly where
// they should.
#include <cstdio>
#include <cstring>

#include "me_half_rop_test.h"

using namespace me::native;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);              \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 31u, mantissa = h & 0x3FFu;
  double v;
  if (exponent == 31) return HalfRopFloat(sign | 0x7F800000u | (mantissa << 13));
  v = exponent ? std::ldexp(1.0 + mantissa / 1024.0, int(exponent) - 15) : std::ldexp(double(mantissa), -24);
  const float f = float(v);
  return sign ? -f : f;
}

static uint16_t HostHalf(float f) {
#if defined(__FLT16_MANT_DIG__)
  const _Float16 h = static_cast<_Float16>(f);
  uint16_t bits;
  std::memcpy(&bits, &h, 2);
  return bits;
#else
  (void)f;
  return 0;
#endif
}

int main() {
  // Every finite half round-trips under every model.
  for (uint32_t h = 0; h < 0x10000; ++h) {
    if (((h >> 10) & 31u) == 31u) continue;
    const float f = HalfToFloat(uint16_t(h));
    for (uint32_t m = 0; m < kHalfModelCount; ++m) {
      const uint16_t got = HalfRopConvert(f, m);
      const bool denormal = (h & 0x7C00u) == 0 && (h & 0x3FFu);
      const bool flush = m == kHalfRneFlush || m == kHalfRtzFlush;
      if (denormal && flush) CHECK(got == (h & 0x8000u));
      else if (got != h) {
        CHECK(got == h);
        std::printf("  half %04X model %u -> %04X\n", h, m, got);
        if (failures > 10) return 1;
      }
    }
  }
#if defined(__FLT16_MANT_DIG__)
  // The IEEE model equals the host conversion on the test inputs and on 4 M random float32 values.
  const auto inputs = HalfRopTestInputs();
  CHECK(inputs.size() == size_t(kHalfRopTestWidth) * kHalfRopTestHeight * 4);
  uint64_t state = 12345;
  std::vector<uint32_t> all = inputs;
  for (uint32_t i = 0; i < (1u << 22); ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    all.push_back(uint32_t(state >> 32));
  }
  uint64_t bad = 0;
  for (const uint32_t bits : all) {
    const float f = HalfRopFloat(bits);
    const uint16_t host = HostHalf(f), model = HalfRopConvert(f, kHalfRneDenormals);
    const bool equal = HalfRopIsNaN(host) ? HalfRopIsNaN(model) : host == model;
    if (!equal && ++bad <= 5) std::printf("  float %08X: host %04X model %04X\n", bits, host, model);
  }
  CHECK(bad == 0);
  // The evaluator picks the IEEE model for an IEEE "GPU" and reports zero mismatches for it, for both biases.
  for (const int bias : {0, -3}) {
    std::vector<uint16_t> gpu(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
      gpu[i] = HostHalf(float(double(HalfRopFloat(inputs[i])) * std::ldexp(1.0, bias)));
    const HalfRopVerdict v = HalfRopEvaluate(inputs, gpu.data(), bias);
    CHECK(v.best == kHalfRneDenormals);
    CHECK(v.mismatches[kHalfRneDenormals] == 0);
    CHECK(v.mismatches[kHalfRtzDenormals] > 0);
    CHECK(v.mismatches[kHalfRneFlush] > 0);
    CHECK(v.examples.empty());
    // A flushing, truncating "GPU" is recognised as such.
    for (size_t i = 0; i < inputs.size(); ++i)
      gpu[i] = HalfRopConvert(float(double(HalfRopFloat(inputs[i])) * std::ldexp(1.0, bias)), kHalfRtzFlush);
    const HalfRopVerdict w = HalfRopEvaluate(inputs, gpu.data(), bias);
    CHECK(w.best == kHalfRtzFlush && w.mismatches[kHalfRtzFlush] == 0);
    CHECK(!w.examples.size());
  }
#else
  std::printf("no _Float16 on this host: the host comparison is skipped\n");
#endif
  // Hand cases.
  CHECK(HalfRopConvert(65519.0f, kHalfRneDenormals) == 0x7BFF);
  CHECK(HalfRopConvert(65520.0f, kHalfRneDenormals) == 0x7C00);
  CHECK(HalfRopConvert(65520.0f, kHalfRneSaturate) == 0x7BFF);
  CHECK(HalfRopConvert(1.0e6f, kHalfRtzDenormals) == 0x7BFF);
  CHECK(HalfRopConvert(-1.0e6f, kHalfRneDenormals) == 0xFC00);
  CHECK(HalfRopConvert(std::ldexp(1.0f, -25), kHalfRneDenormals) == 0x0000);  // tie to even (0)
  CHECK(HalfRopConvert(std::ldexp(1.5f, -25), kHalfRneDenormals) == 0x0001);
  CHECK(HalfRopConvert(std::ldexp(1.5f, -25), kHalfRtzDenormals) == 0x0000);
  CHECK(HalfRopConvert(std::ldexp(3.0f, -25), kHalfRneDenormals) == 0x0002);  // 1.5 quanta: tie to even 2
  CHECK(HalfRopConvert(std::ldexp(1.0f, -14), kHalfRneFlush) == 0x0400);
  CHECK(HalfRopConvert(std::ldexp(1.0f, -15), kHalfRneFlush) == 0x0000);
  CHECK(HalfRopConvert(-std::ldexp(1.0f, -15), kHalfRtzFlush) == 0x8000);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("half ROP models: all checks passed\n");
  return 0;
}
