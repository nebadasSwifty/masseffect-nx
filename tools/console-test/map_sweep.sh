#!/usr/bin/env bash
# Location sweep: start straight in each map (switch_cycle.sh --map), walk and turn for 45 s, and put four of the
# screenshots into one contact sheet per map. Judge the image of every location from the sheets, and look for
# `fatal` hits in the live log.
#
#   tools/console-test/map_sweep.sh PREFIX [MAP ...]
#
#   PREFIX   run-name prefix; the runs are called PREFIX-<map>
#   MAP      map names (default: the nine levels below). Starting in a map does not always load the level;
#            real per-location tests need saves.
# Needs ffmpeg for the contact sheets (optional). Settings: SETTINGS (default app/masseffect.toml); SECONDS_PER_MAP (45).
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PREFIX="${1:?usage: map_sweep.sh PREFIX [MAP ...]}"; shift
if [[ $# -gt 0 ]]; then MAPS=("$@"); else
  MAPS=(BIOA_END00 BIOA_ICE00 BIOA_JUG00 BIOA_LAV00 BIOA_NOR00 BIOA_PRO00 BIOA_PRO10 BIOA_STA00 BIOA_WAR00)
fi
SETTINGS="${SETTINGS:-$ROOT/app/masseffect.toml}"
for map in "${MAPS[@]}"; do
  name="$PREFIX-${map#BIOA_}"
  mkdir -p "$OUT_ROOT/$name"
  "$CT_DIR/switch_cycle.sh" "$name" "${SECONDS_PER_MAP:-45}" --toml "$SETTINGS" --map "$map" > "$OUT_ROOT/$name/cycle.log" 2>&1
  shots="$OUT_ROOT/$name/shots"
  if command -v ffmpeg >/dev/null 2>&1 && [[ -f "$shots/start.jpg" && -f "$shots/leg-1.jpg" && -f "$shots/leg-2.jpg" && -f "$shots/leg-3.jpg" ]]; then
    ffmpeg -loglevel error -y -i "$shots/start.jpg" -i "$shots/leg-1.jpg" -i "$shots/leg-2.jpg" -i "$shots/leg-3.jpg" \
      -filter_complex "[0][1]hstack[a];[2][3]hstack[b];[a][b]vstack,scale=1600:-1" "$OUT_ROOT/$name/sheet.jpg"
  fi
  fatal=$(grep -c -E 'DEVICE_LOST|VK_ERROR|FATAL' "$OUT_ROOT/$name/live.log" 2>/dev/null || true)
  echo "$map: $(python3 "$CT_DIR/cpu_per_frame.py" "$OUT_ROOT/$name" | cut -c1-90) | fatal: ${fatal:-0}"
done
