// Mass Effect - decryption mini-game: the face-button diamond in the Nintendo Switch layout (Russian edition, XEX 0.0.0.5).
//
// Russian-edition copy of app/src/native/me_switch_minigame.cpp: same code, Russian addresses. Keep both copies
// identical below the edition block. See docs/switch-branding.md ("Decryption mini-game").
//
// WHAT THE GAME DOES
//   The bypass / decryption mini-game (control panels, lockers, crates) is the Scaleform movie
//   GUI_SF_SkillGame.MiniGame, a UBioSWF object in Layer0/MEInit/BIOC_Materials.xxx. Its sprite 11 draws the Xbox
//   diamond (Y top, X left, B right, A bottom). UnrealScript calls the movie's SetButtonVisible(0..3 = A/B/X/Y) to
//   light one named clip ("AButton" ... "YButton"); the movie's onKeyDown compares the pad key (Key codes 65/66/88/89
//   = A/B/X/Y) with the visible clip and reports SG_ButtonPress(id) back. Everything is by label, so with our default
//   input mapping (Switch A = Xbox A) the correct button is pressed, but the lit glyph sits where the Xbox button
//   is, not where the Switch button is (a reaction game: the player reacts to the position).
//
// WHAT THIS DOES
//   Hooks UObject::Serialize = sub_8230BD80 (EN) / sub_8230BCF8 (RU). UBioSWF does not override Serialize (vtable
//   slot 13), and its tagged "Data" byte array (the whole .swf) is read inside it. After the original returns, for an
//   object whose vtable is UBioSWF's (0x820C6840 EN / 0x820C6810 RU) the hook looks for the {data, num, max} array
//   that holds exactly the original compressed mini-game movie (size + FNV-1a 64), inflates it, rewrites the 8
//   PlaceObject2 tags of sprite 11 (me_switch_minigame_patch.h) and stores it back uncompressed ("FWS", which GFx
//   reads like the stored-uncompressed GUI_SF_Inventory movie) through the game's own appRealloc = sub_822100F8, so
//   the game frees it normally. Any other object or movie is left untouched; a patched movie no longer matches, so a
//   second Serialize of the same object does nothing.
//
//   input_xbox_layout = false (default): glyphs AND clip names move (A right, B bottom, X top, Y left): the lit glyph
//     is the label and the position of the Switch button to press.
//   input_xbox_layout = true (Switch buttons mapped by position): positions already match; only the letters change
//     (bottom shows B, right A, left Y, top X), so the lit letter is the Switch button that sends the wanted Xbox one.
//
// masseffect_switch_minigame_layout = true by default (false = the original Xbox diamond). No game file is changed.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "me_switch_minigame_patch.h"

REXCVAR_DEFINE_BOOL(masseffect_switch_minigame_layout, true, "Mass Effect",
                    "Decryption mini-game: draw the face-button diamond in the Nintendo Switch layout (X top, Y left, "
                    "A right, B bottom); false = the original Xbox diamond")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, input_xbox_layout);  // sdk/src/input/switch/switch_input_driver.cpp

// zlib inflate of the SDK's stb_image (sdk/src/ui/image_decode.cpp, linked into every build through rexui).
// Returns the number of bytes written, or -1.
extern "C" int stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

// ---- edition ------------------------------------------------------------------------------------------------
#define ME_SWITCH_MINIGAME_RU 1
#define ME_OBJECT_SERIALIZE sub_8230BCF8   // UObject::Serialize(UObject* this, FArchive& ar)
#define ME_APP_REALLOC sub_822100F8        // appRealloc(ptr, size, align) -> r3 (GMalloc->Realloc)
inline constexpr uint32_t kBioSwfVtable = 0x820C6810u;  // stored by the UBioSWF constructor sub_82241790

#define ME_CONCAT2(a, b) a##b
#define ME_CONCAT(a, b) ME_CONCAT2(a, b)
REX_EXTERN(ME_CONCAT(__imp__, ME_OBJECT_SERIALIZE));
REX_EXTERN(ME_APP_REALLOC);

namespace {

namespace mg = me::switch_minigame;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

// UObject ends at +0x3C; UBioSWF is 96 bytes. The Data array is one of the {data, num, max} triples after it.
constexpr uint32_t kScanFirst = 0x3C;
constexpr uint32_t kScanLast = 96 - 12;

std::atomic<bool> g_done{false};
std::atomic<bool> g_warned{false};

void WarnOnce(const char* what, uint32_t obj) {
  if (!g_warned.exchange(true)) REXLOG_WARN("[switch_minigame] {} (BioSWF object {:08X}); diamond left as is", what, obj);
}

// Inflates the original compressed movie, patches it, and moves it into a reallocated guest array. True if done.
bool PatchCompressed(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t field, mg::Mode mode) {
  const uint32_t data = Load32(base, field);
  const uint8_t* cws = base + data;
  std::vector<uint8_t> swf(mg::kSwfBytes);
  swf[0] = 'F';
  swf[1] = 'W';
  swf[2] = 'S';
  std::memcpy(swf.data() + 3, cws + 3, 5);  // version + length
  const int out = stbi_zlib_decode_buffer(reinterpret_cast<char*>(swf.data() + 8), int(mg::kSwfBytes - 8),
                                          reinterpret_cast<const char*>(cws + 8), int(mg::kCwsBytes - 8));
  if (out != int(mg::kSwfBytes - 8)) {
    WarnOnce("inflate failed", obj);
    return false;
  }
  if (!mg::PatchDiamond(swf.data(), swf.size(), mode)) {
    WarnOnce("inflated movie is not the expected one", obj);
    return false;
  }
  PPCContext saved = ctx;
  ctx.r3.u64 = data;
  ctx.r4.u64 = mg::kSwfBytes;
  ctx.r5.u64 = 8;  // alignment used by the game's own TArray growth
  ME_APP_REALLOC(ctx, base);
  const uint32_t grown = ctx.r3.u32;
  ctx = saved;
  if (!grown) {
    WarnOnce("appRealloc failed", obj);
    return false;
  }
  std::memcpy(base + grown, swf.data(), swf.size());
  Store32(base, field + 0, grown);
  Store32(base, field + 4, mg::kSwfBytes);
  Store32(base, field + 8, mg::kSwfBytes);
  return true;
}

}  // namespace

REX_HOOK_RAW(ME_OBJECT_SERIALIZE) {
  static const bool enabled = REXCVAR_GET(masseffect_switch_minigame_layout);
  const uint32_t obj = ctx.r3.u32;
  ME_CONCAT(__imp__, ME_OBJECT_SERIALIZE)(ctx, base);
  if (!enabled || obj == 0 || Load32(base, obj) != kBioSwfVtable) return;

  static const mg::Mode mode =
      REXCVAR_GET(input_xbox_layout) ? mg::Mode::kRelabel : mg::Mode::kMoveByLabel;
  for (uint32_t off = kScanFirst; off <= kScanLast; off += 4) {
    const uint32_t field = obj + off;
    const uint32_t data = Load32(base, field);
    const uint32_t num = Load32(base, field + 4);
    const uint32_t max = Load32(base, field + 8);
    if (data == 0 || max < num) continue;
    bool patched = false;
    if (num == mg::kCwsBytes) {
      if (!mg::IsOriginalCws(base + data, num)) continue;
      patched = PatchCompressed(ctx, base, obj, field, mode);
    } else if (num == mg::kSwfBytes) {
      // Already uncompressed: the original stored that way, or this very object serialized again after our patch
      // (then the sprite no longer matches and nothing is written).
      patched = mg::PatchDiamond(base + data, num, mode);
    } else {
      continue;
    }
    if (patched && !g_done.exchange(true))
      REXLOG_INFO("[switch_minigame] GUI_SF_SkillGame.MiniGame: diamond set to the Switch layout ({}), BioSWF {:08X}",
                  mode == mg::Mode::kRelabel ? "letters relabeled, input_xbox_layout" : "glyphs moved", obj);
    return;
  }
}
