// Mass Effect - decryption mini-game (GUI_SF_SkillGame.MiniGame): face-button diamond in the Nintendo Switch layout.
// Pure byte logic shared by both editions (no guest access); the hook is me_switch_minigame.cpp.
// See docs/switch-branding.md, section "Decryption mini-game".
//
// The movie is a UBioSWF in Layer0/MEInit/BIOC_Materials.xxx (byte-identical in the EN and RU discs): a zlib "CWS"
// Flash 8 movie of 253,964 bytes, 279,796 bytes once inflated. Sprite 11 ("buttonCluster" in the timeline) holds
// the diamond: four dim glyphs (depths 1..7) and four named glyph clips (depths 9..15) that the ActionScript shows
// one at a time with SetButtonVisible(0..3 = A, B, X, Y) and checks in onKeyDown (Key codes 65/66/88/89 = A/B/X/Y):
//
//   char 4 = A glyph at (-0.6, 47.8)    bottom   name "AButton"
//   char 6 = B glyph at (54.95, -1.55)  right    name "BButton"
//   char 8 = X glyph at (-55.4, -1.55)  left     name "XButton"
//   char 10 = Y glyph at (-0.6, -48.35) top      name "YButton"
//
// Every PlaceObject2 tag of the sprite keeps its matrix; the patch only rewrites the 2-byte character id and,
// for kMoveByLabel, the first letter of the instance name (same size, so no tag length changes):
//
//   kMoveByLabel (default input mapping, Switch A = Xbox A by label): A<->B and X<->Y glyphs AND names are swapped,
//     so "BButton" with the B glyph sits at the bottom, A at the right, X at the top, Y at the left: the Switch
//     layout. The clip the script lights for "A" is now at the right, where the Switch A button is.
//   kRelabel (input_xbox_layout = true, mapping by position): only the glyphs are swapped; the names stay where they
//     are, so the bottom clip is still "AButton" (lit when the game wants Xbox A, which is the Switch B button in that
//     mapping) but now shows the letter B.
//
// The patch runs only on the exact original: the size, an FNV-1a 64 of the compressed movie (before inflating) and
// an FNV-1a 64 of the 230-byte sprite 11 tag (before writing) must match. A patched or different movie is left alone.
#pragma once

#include <cstddef>
#include <cstdint>

namespace me::switch_minigame {

inline constexpr uint32_t kCwsBytes = 253964;   // compressed movie as stored in the package (Data.Num())
inline constexpr uint32_t kSwfBytes = 279796;   // header length field = inflated movie including the 8-byte header
inline constexpr uint64_t kCwsFnv = 0x36a776ba56672406ull;     // FNV-1a 64 of the kCwsBytes compressed bytes
inline constexpr uint32_t kSpriteOffset = 7642;                // sprite 11 tag (long header) in the inflated movie
inline constexpr uint32_t kSpriteBytes = 230;                  // 6-byte header + 224-byte body
inline constexpr uint64_t kSpriteFnv = 0xe3572844573ca721ull;  // FNV-1a 64 of the original sprite 11 tag

struct Site {
  uint16_t char_offset;  // low byte of the PlaceObject2 character id (u16 little endian; high byte is 0)
  uint16_t name_offset;  // first letter of the instance name, 0 = no name
};
// Offsets from the start of the sprite 11 tag, in tag order (depths 1, 3, 5, 7 dim; 9, 11, 13, 15 named).
inline constexpr Site kSites[8] = {
    {0x0F, 0},    {0x29, 0},    {0x43, 0},    {0x5D, 0},
    {0x7B, 0x86}, {0x97, 0xA2}, {0xB3, 0xBE}, {0xCF, 0xDA},
};

enum class Mode { kMoveByLabel, kRelabel };

inline uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

inline uint32_t LoadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// True when `p` (n bytes) is the original compressed movie.
inline bool IsOriginalCws(const uint8_t* p, size_t n) {
  return n == kCwsBytes && p[0] == 'C' && p[1] == 'W' && p[2] == 'S' && LoadLe32(p + 4) == kSwfBytes &&
         Fnv1a64(p, n) == kCwsFnv;
}

// `swf` is an uncompressed movie ("FWS" header) of n bytes. Rewrites sprite 11 in place; false (nothing written)
// unless the movie and the sprite are exactly the original ones.
inline bool PatchDiamond(uint8_t* swf, size_t n, Mode mode) {
  if (n != kSwfBytes || swf[0] != 'F' || swf[1] != 'W' || swf[2] != 'S' || LoadLe32(swf + 4) != kSwfBytes) return false;
  uint8_t* s = swf + kSpriteOffset;
  if (Fnv1a64(s, kSpriteBytes) != kSpriteFnv) return false;
  for (const Site& site : kSites) {
    uint8_t& ch = s[site.char_offset];
    ch = ch == 4 ? 6 : ch == 6 ? 4 : ch == 8 ? 10 : ch == 10 ? 8 : ch;  // A<->B, X<->Y glyph
    if (mode == Mode::kMoveByLabel && site.name_offset) {
      uint8_t& c = s[site.name_offset];
      c = c == 'A' ? 'B' : c == 'B' ? 'A' : c == 'X' ? 'Y' : c == 'Y' ? 'X' : c;  // "AButton" <-> "BButton", ...
    }
  }
  return true;
}

}  // namespace me::switch_minigame
