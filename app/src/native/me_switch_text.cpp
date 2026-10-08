// Mass Effect - talk-table strings say "Nintendo Switch" instead of "Xbox 360" (English edition).
//
// PROPOSAL, NOT BUILT: this file is not in app/CMakeLists.txt yet. See docs/switch-branding.md.
// The Russian edition has its own copy with Russian addresses:
// editions/ru/overlay/app/src/native/me_switch_text.cpp.
//
// WHAT THE GAME DOES
//   Every localized string the game shows (menus, tutorials, message boxes, HUD prompts, subtitles) comes from a
//   BioTlkFile talk table (GlobalTlk.xxx plus per-map *_tlk exports). The lookup is the virtual
//
//     bool UBioTlkFile::GetString(UBioTlkFile* this /* r3 */, int32 strref /* r4 */, FString* out /* r5 */)
//
//   = sub_82963EB8 (EN) / sub_82963D18 (RU), vtable slot 71 (+284) of UBioTlkFile (vtable 0x82065EE8 in RU).
//   It hashes the strref into the in-memory hash table (12-byte buckets {id, flags, index}), and either copies
//   an already decoded string (flags & 0x40) with FString::operator=(const TCHAR*) = sub_822437F0 (EN) /
//   sub_822D8708 (RU), or Huffman-decodes it with sub_82963CD8 (EN) / sub_82963B80 (RU). Returns 1 when found.
//   It is only reached through the vtable (no direct bl callers), so a REX_HOOK_RAW hook sees every call.
//
//   FString on the guest: { u32 data (TCHAR*), i32 num (characters incl. the terminating 0), i32 max }.
//   TCHAR is 16-bit, stored big-endian (UTF-16BE).
//
// WHAT THIS DOES
//   After the original returns 1, the strref is looked up in the tables of me_switch_text_tables.h:
//   whole-string replacements, phrase substitutions for controller wording, and (only with
//   input_xbox_layout = true) a swap of the face-button letters so the text names the button the player
//   presses. If the new text fits in the FString's allocation it is written in place, else the game's own
//   FString::operator= is called with a copy in a guest system-heap buffer.
//
// On by default (masseffect_switch_text = true; false shows the original text). No game file is changed.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "me_switch_text_tables.h"

REXCVAR_DEFINE_BOOL(masseffect_switch_text, true, "Mass Effect",
                    "Talk-table strings name the Nintendo Switch (storage, users, HOME button, eShop, L/R/ZL/ZR "
                    "and +/- buttons) instead of the Xbox 360")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_switch_text_log, false, "Mass Effect",
                    "Log every talk-table string the Switch text hook changes (first time per strref)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, input_xbox_layout);  // sdk/src/input/switch/switch_input_driver.cpp

// ---- edition ------------------------------------------------------------------------------------------------
#ifndef ME_SWITCH_TEXT_RU
#define ME_TLK_GET_STRING sub_82963EB8   // UBioTlkFile::GetString(this, strref, FString* out) -> bool
#define ME_FSTRING_ASSIGN sub_822437F0   // FString::operator=(FString* this, const TCHAR* src)
#define ME_TEXT_TABLE(name) name##En
#endif

#define ME_CONCAT2(a, b) a##b
#define ME_CONCAT(a, b) ME_CONCAT2(a, b)
REX_EXTERN(ME_CONCAT(__imp__, ME_TLK_GET_STRING));
REX_EXTERN(ME_CONCAT(__imp__, ME_FSTRING_ASSIGN));

namespace {

using me::switch_text::PhraseEntry;
using me::switch_text::WholeEntry;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
uint16_t Load16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}
void Store16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(base + a, &v, 2);
}

std::u16string Utf8To16(const char* s) {
  std::u16string out;
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xF0) {
      c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if (c >= 0xE0) {
      c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if (c >= 0xC0) {
      c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
      p += 1;
    }
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(char16_t(0xD800 + (c >> 10)));
      out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(char16_t(c));
    }
  }
  return out;
}

std::string Utf16To8(const std::u16string& s) {
  std::string out;
  for (char16_t ch : s) {
    const uint32_t c = ch;
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

struct Tables {
  std::unordered_map<int32_t, std::u16string> whole;
  std::unordered_set<int32_t> phrase_ids;
  std::vector<std::pair<std::u16string, std::u16string>> phrases;
  std::unordered_set<int32_t> letter_ids;
  std::vector<std::u16string> letter_before, letter_after;
};

const Tables& GetTables() {
  static const Tables t = [] {
    Tables r;
    for (const WholeEntry& e : me::switch_text::ME_TEXT_TABLE(kWhole)) r.whole[e.id] = Utf8To16(e.text);
    for (int32_t id : me::switch_text::ME_TEXT_TABLE(kPhraseIds)) r.phrase_ids.insert(id);
    for (const PhraseEntry& e : me::switch_text::ME_TEXT_TABLE(kPhrase))
      r.phrases.emplace_back(Utf8To16(e.from), Utf8To16(e.to));
    for (int32_t id : me::switch_text::ME_TEXT_TABLE(kLetterIds)) r.letter_ids.insert(id);
    for (const char* w : me::switch_text::ME_TEXT_TABLE(kLetterBefore)) r.letter_before.push_back(Utf8To16(w));
    for (const char* w : me::switch_text::ME_TEXT_TABLE(kLetterAfter))
      if (*w) r.letter_after.push_back(Utf8To16(w));
    return r;
  }();
  return t;
}

bool IsWordChar(char16_t c) {
  return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') ||
         (c >= 0x0400 && c <= 0x04FF) || c == u'-' || c == u'\'';
}

// A<->B and X<->Y for a standalone letter preceded by a "press"-like word or followed by "Button"/"to"/...
// The Cyrillic capital A (U+0410), which the Russian text uses in two strings, counts as A.
bool SwapFaceLetters(std::u16string& s, const Tables& t) {
  bool changed = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    char16_t to = 0;
    if (c == u'A' || c == 0x0410) to = u'B';
    else if (c == u'B') to = u'A';
    else if (c == u'X') to = u'Y';
    else if (c == u'Y') to = u'X';
    if (!to) continue;
    if ((i > 0 && IsWordChar(s[i - 1])) || (i + 1 < s.size() && IsWordChar(s[i + 1]))) continue;
    // previous word
    size_t e = i;
    while (e > 0 && s[e - 1] == u' ') --e;
    size_t b = e;
    while (b > 0 && IsWordChar(s[b - 1])) --b;
    const std::u16string prev = s.substr(b, e - b);
    // next word
    size_t nb = i + 1;
    while (nb < s.size() && s[nb] == u' ') ++nb;
    size_t ne = nb;
    while (ne < s.size() && IsWordChar(s[ne])) ++ne;
    const std::u16string next = s.substr(nb, ne - nb);
    const bool hit = std::find(t.letter_before.begin(), t.letter_before.end(), prev) != t.letter_before.end() ||
                     std::find(t.letter_after.begin(), t.letter_after.end(), next) != t.letter_after.end();
    if (hit) {
      s[i] = to;
      changed = true;
    }
  }
  return changed;
}

bool Rewrite(int32_t id, std::u16string& s) {
  const Tables& t = GetTables();
  bool changed = false;
  if (auto it = t.whole.find(id); it != t.whole.end()) {
    s = it->second;
    changed = true;
  }
  if (t.phrase_ids.count(id)) {
    for (const auto& [from, to] : t.phrases) {
      size_t pos = 0;
      while ((pos = s.find(from, pos)) != std::u16string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
      }
    }
  }
  static const bool per_position = REXCVAR_GET(input_xbox_layout);
  if (per_position && t.letter_ids.count(id)) changed |= SwapFaceLetters(s, t);
  return changed;
}

std::mutex g_log_mutex;
std::unordered_set<int32_t> g_logged;

}  // namespace

REX_HOOK_RAW(ME_TLK_GET_STRING) {
  static const bool enabled = REXCVAR_GET(masseffect_switch_text);
  const int32_t strref = ctx.r4.s32;
  const uint32_t out = ctx.r5.u32;
  ME_CONCAT(__imp__, ME_TLK_GET_STRING)(ctx, base);
  if (!enabled || ctx.r3.u32 == 0 || out == 0) return;

  const uint32_t data = Load32(base, out + 0);
  const int32_t num = int32_t(Load32(base, out + 4));
  const int32_t max = int32_t(Load32(base, out + 8));
  if (!data || num <= 0 || num > 65536) return;

  std::u16string text;
  text.reserve(size_t(num));
  for (int32_t i = 0; i + 1 < num; ++i) text.push_back(char16_t(Load16(base, data + 2u * uint32_t(i))));
  const std::u16string before = text;
  if (!Rewrite(strref, text)) return;

  static const bool log = REXCVAR_GET(masseffect_switch_text_log);
  if (log) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_logged.insert(strref).second)
      REXLOG_INFO("[switch_text] {}: \"{}\" -> \"{}\"", strref, Utf16To8(before), Utf16To8(text));
  }

  const int32_t new_num = int32_t(text.size()) + 1;
  if (new_num <= max) {
    for (size_t i = 0; i < text.size(); ++i) Store16(base, data + 2u * uint32_t(i), uint16_t(text[i]));
    Store16(base, data + 2u * uint32_t(text.size()), 0);
    Store32(base, out + 4, uint32_t(new_num));
    return;
  }

  // Longer than the allocation: let the game's FString::operator= reallocate (guest heap), from a copy of the
  // new text in a guest system-heap buffer.
  auto* memory = rex::system::kernel_state()->memory();
  const uint32_t bytes = uint32_t(new_num) * 2u;
  const uint32_t src = memory->SystemHeapAlloc(bytes);
  if (!src) return;
  for (size_t i = 0; i < text.size(); ++i) Store16(base, src + 2u * uint32_t(i), uint16_t(text[i]));
  Store16(base, src + 2u * uint32_t(text.size()), 0);
  PPCContext saved = ctx;
  ctx.r3.u64 = out;
  ctx.r4.u64 = src;
  ME_CONCAT(__imp__, ME_FSTRING_ASSIGN)(ctx, base);
  ctx = saved;  // GetString's result (r3 = 1) and the caller's registers
  memory->SystemHeapFree(src);
}
