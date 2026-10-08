// masseffect_native_bias_life_probe bookkeeping (me_bias_life.h): producer chains and resolve lives on synthetic
// event sequences, including the negative cases each class must not swallow.
#include <cstdio>
#include <cstdlib>

#include "me_bias_life.h"

using namespace me::native;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);              \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

namespace {

constexpr uint64_t kScene = 0x1000;   // the 7e3 host image (source)
constexpr uint64_t kLdr = 0x2000;     // another view of the same base (RGBA8)
constexpr uint64_t kQuarter = 0x3000; // a view elsewhere
constexpr uint32_t kBase = 0x2D0;
constexpr uint32_t kTexture = 0x14B38000;
const BiasLifeSpan kSceneSpan{0x2D0, 720};
const BiasLifeSpan kQuarterSpan{0x5A0, 40};
const BiasLifeRect kFull{0, 0, 1280, 720};

struct Images {
  uint64_t writes[4] = {};
  uint64_t& Of(uint64_t image) { return writes[image >> 12]; }
};

BiasLifeWrite Write(Images& im, uint64_t image, bool direct, bool full_mask, bool proven,
                    BiasLifeRect rect = kFull, BiasLifeSpan span = kSceneSpan, bool no_kill = true) {
  BiasLifeWrite w;
  w.image = image;
  w.base = span.first;
  w.span = span;
  w.direct = direct;
  w.full_mask = full_mask;
  w.no_kill = no_kill;
  w.has_proven = proven;
  w.proven = rect;
  w.writes_before = im.Of(image);
  w.writes_after = ++im.Of(image);
  return w;
}

BiasLifeDraw DrawOf(uint32_t ps, std::vector<BiasLifeWrite> writes, std::vector<BiasLifeRead> reads = {}) {
  BiasLifeDraw d;
  d.vs = 1;
  d.ps = ps;
  d.writes = std::move(writes);
  d.reads = std::move(reads);
  return d;
}

BiasLifeRead ReadOf(uint32_t address, bool point) {
  BiasLifeRead r;
  r.address = address;
  r.point = point;
  return r;
}

uint32_t ResolveFull(BiasLifeProbe& p, Images& im) {
  return p.Resolve(kScene, im.Of(kScene), kBase, kSceneSpan, kFull, kTexture, true);
}

}  // namespace

int main() {
  // Span overlap on the 2048-tile ring.
  CHECK(BiasLifeOverlap({2000, 100}, {10, 5}));
  CHECK(!BiasLifeOverlap({2000, 40}, {10, 5}));
  CHECK(BiasLifeOverlap({10, 5}, {2000, 100}));
  CHECK(!BiasLifeOverlap({0x2D0, 720}, {0x5A0, 40}));  // 0x2D0 + 720 = 0x5A0: adjacent, not overlapping
  CHECK(BiasLifeOverlap({0x2D0, 721}, {0x5A0, 40}));
  CHECK(!BiasLifeOverlap({0, 0}, {0, 2048}));
  CHECK(BiasLifeOverlap({5, 2048}, {100, 1}));

  // 1. Strict: one full start, then the resolve; the reader writes another image elsewhere: hand-off, point.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    CHECK(ResolveFull(p, im) == kBiasProducerStrict);
    p.Draw(DrawOf(24056, {Write(im, kQuarter, true, true, false, kFull, kQuarterSpan)},
                  {ReadOf(kTexture, true)}));
    CHECK(p.Retire(kTexture) == kBiasEndHandOff);
    CHECK(p.Counts().hand_off_point == 1);
    CHECK(p.Retire(kTexture) == kBiasEndCount);  // already ended
  }
  // 2. Chain: full start, then a mask-7 non-blended draw that also kills.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    p.Draw(DrawOf(7992, {Write(im, kScene, true, false, false, kFull, kSceneSpan, false)}));
    CHECK(ResolveFull(p, im) == kBiasProducerChain);
  }
  // 3. Blended after the full start.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    p.Draw(DrawOf(14702, {Write(im, kScene, false, true, false)}));
    CHECK(ResolveFull(p, im) == kBiasProducerBlended);
    // A new full start clears the blend.
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    CHECK(ResolveFull(p, im) == kBiasProducerStrict);
  }
  // 4. Another view writes the same tiles after the chain: other.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    p.Draw(DrawOf(21267, {Write(im, kLdr, true, true, false)}));
    CHECK(ResolveFull(p, im) == kBiasProducerOther);
  }
  // 5. A write the probe did not see (the count moved): other, both at the next draw and at the resolve.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    ++im.Of(kScene);
    CHECK(ResolveFull(p, im) == kBiasProducerOther);
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    ++im.Of(kScene);
    p.Draw(DrawOf(7992, {Write(im, kScene, true, false, false)}));
    CHECK(ResolveFull(p, im) == kBiasProducerOther);
  }
  // 5b. An unknown "before" count skips the check.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    BiasLifeWrite w = Write(im, kScene, true, false, false);
    w.writes_before = kBiasLifeUnknown;
    p.Draw(DrawOf(7992, {w}));
    CHECK(ResolveFull(p, im) == kBiasProducerChain);
  }
  // 6. The start's rectangle does not cover the resolve; a start without proof is no start.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true, BiasLifeRect{0, 0, 1280, 512})}));
    CHECK(ResolveFull(p, im) == kBiasProducerSmall);
    BiasLifeProbe q;
    Images jm;
    q.Draw(DrawOf(3474, {Write(jm, kScene, true, true, false)}));
    CHECK(ResolveFull(q, jm) == kBiasProducerNoStart);
    BiasLifeProbe r;
    Images km;
    r.Draw(DrawOf(3474, {Write(km, kScene, true, true, true, kFull, kSceneSpan, false)}));  // kill: no start
    CHECK(ResolveFull(r, km) == kBiasProducerNoStart);
  }
  // 7. Lives: unread, reader writes the source, reader after the source changed, import.
  {
    BiasLifeProbe p;
    Images im;
    ResolveFull(p, im);
    CHECK(p.Retire(kTexture) == kBiasEndUnread);
    ResolveFull(p, im);
    p.Draw(DrawOf(7992, {Write(im, kScene, true, false, false)}, {ReadOf(kTexture, false)}));
    CHECK(p.Retire(kTexture) == kBiasEndCopyWriter);
    ResolveFull(p, im);
    p.Draw(DrawOf(14702, {Write(im, kScene, false, true, false)}));
    p.Draw(DrawOf(15729, {Write(im, kLdr, true, true, true)}, {ReadOf(kTexture, false)}));
    CHECK(p.Retire(kTexture) == kBiasEndCopyChanged);
    ResolveFull(p, im);
    p.Imported(kScene);
    p.Draw(DrawOf(15729, {Write(im, kLdr, true, true, true)}, {ReadOf(kTexture, true)}));
    CHECK(p.Retire(kTexture) == kBiasEndCopyChanged);
    // A write of the same tiles through another view does not change the source image: still a hand-off.
    ResolveFull(p, im);
    p.Draw(DrawOf(24056, {Write(im, kQuarter, true, true, false, kFull, kQuarterSpan)}, {ReadOf(kTexture, false)}));
    p.Draw(DrawOf(15729, {Write(im, kLdr, true, true, true)}, {ReadOf(kTexture, false)}));
    p.Draw(DrawOf(16272, {Write(im, kLdr, true, false, false)}));
    CHECK(p.Retire(kTexture) == kBiasEndHandOff);
    CHECK(p.Counts().hand_off_point == 0);  // linear reads
    CHECK(p.Counts().end[kBiasEndUnread] == 1);
    CHECK(p.Counts().end[kBiasEndCopyWriter] == 1);
    CHECK(p.Counts().end[kBiasEndCopyChanged] == 2);
    CHECK(p.Counts().end[kBiasEndHandOff] == 1);
    CHECK(p.Counts().reads == 1 + 1 + 1 + 2);
  }
  // 8. Dead: the first later write of the tiles is a full overwrite of the rect (any view) or a clear of it.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    ResolveFull(p, im);
    p.Draw(DrawOf(24056, {Write(im, kQuarter, true, true, true, kFull, kQuarterSpan)}));  // elsewhere
    p.Draw(DrawOf(15729, {Write(im, kLdr, true, true, true)}, {ReadOf(kTexture, true)}));
    p.Draw(DrawOf(14702, {Write(im, kScene, false, true, false)}));  // after: does not matter
    p.Retire(kTexture);
    CHECK(p.Counts().dead == 1 && p.Counts().not_dead == 0 && p.Counts().dual_and_dead == 1);
    ResolveFull(p, im);
    p.Draw(DrawOf(14702, {Write(im, kScene, false, true, false)}));
    p.Retire(kTexture);
    CHECK(p.Counts().not_dead == 1);
    ResolveFull(p, im);
    p.Clear(kLdr, kSceneSpan, kBase, &kFull);
    p.Retire(kTexture);
    CHECK(p.Counts().dead == 2);
    ResolveFull(p, im);
    const BiasLifeRect half{0, 0, 640, 720};
    p.Clear(kLdr, kSceneSpan, kBase, &half);
    p.Retire(kTexture);
    CHECK(p.Counts().not_dead == 2);
    // A clear of the source image itself changes it.
    ResolveFull(p, im);
    p.Clear(kScene, kSceneSpan, kBase, &kFull);
    p.Draw(DrawOf(15729, {}, {ReadOf(kTexture, true)}));
    CHECK(p.Retire(kTexture) == kBiasEndCopyChanged);
  }
  // 9. A depth write over the source tiles breaks the chain.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    BiasLifeDraw d = DrawOf(9828, {});
    d.depth_writes.push_back({0x2D0 + 100, 10});
    p.Draw(d);
    CHECK(ResolveFull(p, im) == kBiasProducerOther);
  }
  // 10. Producer sites and reader shaders are counted.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    ResolveFull(p, im);
    p.Draw(DrawOf(15729, {}, {ReadOf(kTexture, true), ReadOf(kTexture, false)}));
    p.Retire(kTexture);
    CHECK(p.ProducerSites().count((uint64_t(1) << 32) | 3474) == 1);
    CHECK(p.ProducerSites().at((uint64_t(1) << 32) | 3474)[kBiasProducerStrict] == 1);
    CHECK(p.ReaderShaders().at(15729)[kBiasEndHandOff] == 1);
    CHECK(p.Counts().hand_off_point == 0);  // one of its two fetches is linear
    p.ClearCounts();
    CHECK(p.Counts().resolves == 0 && p.ProducerSites().empty());
  }
  // 11. Round trip: the full start sampled the texture that the resolve then writes.
  {
    BiasLifeProbe p;
    Images im;
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}));
    ResolveFull(p, im);  // a first life of the texture, so its reads are tracked
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}, {ReadOf(kTexture, true)}));
    CHECK(ResolveFull(p, im) == kBiasProducerStrict);
    CHECK(p.Counts().round_trip == 1);
    p.Draw(DrawOf(7992, {Write(im, kScene, true, false, false)}, {ReadOf(kTexture, false)}));
    CHECK(ResolveFull(p, im) == kBiasProducerChain);
    CHECK(p.Counts().round_trip == 1);  // a chain is not a round trip
    // The start read an older content of the texture (another resolve came between): not a round trip.
    p.Draw(DrawOf(3474, {Write(im, kScene, true, true, true)}, {ReadOf(kTexture, true)}));
    p.Resolve(kScene, im.Of(kScene), kBase, kSceneSpan, kFull, kTexture, true);  // a resolve of something else...
    CHECK(p.Counts().round_trip == 2);  // ...which is itself a round trip of the start
    CHECK(p.Resolve(kScene, im.Of(kScene), kBase, kSceneSpan, kFull, kTexture, true) == kBiasProducerStrict);
    CHECK(p.Counts().round_trip == 2);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("bias life probe: all checks passed\n");
  return 0;
}
