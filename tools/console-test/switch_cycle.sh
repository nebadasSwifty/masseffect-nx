#!/usr/bin/env bash
# Unattended test cycle on a real Switch (sys-ftpd + sys-botbase, see README.md):
#   [build] -> upload -> start the game from the HOME menu -> title -> Resume -> play for a while -> close ->
#   download the newest game log and the profiler log -> short summary.
#
#   tools/console-test/switch_cycle.sh NAME [SECONDS] [options]
#
#   NAME           label of the run; results go to out/console-test/NAME/
#   SECONDS        length of the play window (default 60; with --route: the length of the route plus 8)
#   --build        run tools/build_nro.sh first
#   --nro FILE     NRO to upload (default out/nx/masseffect-nx.nro)
#   --toml FILE    settings to upload as masseffect.toml (default app/masseffect.toml)
#   --cold         cold start: `masseffect_cold_startup = true` and the saved pipeline list is deleted first.
#                  Acceptance runs are cold; use warm runs only for quick A/B of one change.
#   --route FILE   play a route recorded in the game (cvar input_record: press MINUS, play, press MINUS) back with
#                  the game's own input_play. Without it the script walks and turns in place.
#   --map NAME     start straight in this map (BIOA_STA00, ...): uploads a Coalesced.ini whose [URL] LocalMap is
#                  changed (tools/coalesced.py) and gives its SHA-1 to the game. The original is restored afterwards.
#   --no-movies    upload a Coalesced.ini without the logo movies (faster start); restored afterwards
#   --hold         stop once in game and leave it running
#   --shots N      seconds between screenshots during the route (default 15)
#
# Before you start: the console is at the HOME menu with the tile that launches this port selected, sys-botbase has
# been given reference screenshots (switch_bot.py --help), and tools/console-test/credentials.env exists.
# Inputs are held, not clicked: at a few fps the game polls the pad too rarely to see a click.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:?usage: switch_cycle.sh NAME [SECONDS] [options]}"; shift
SECS=60; BUILD=0; NRO="$ROOT/out/nx/masseffect-nx.nro"; TOML="$ROOT/app/masseffect.toml"; COLD=0; ROUTE=""
MAP=""; NO_MOVIES=0; HOLD=0; SHOT_EVERY=15
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD=1 ;;
    --nro) NRO="$2"; shift ;;
    --toml) TOML="$2"; shift ;;
    --cold) COLD=1 ;;
    --route) ROUTE="$2"; shift ;;
    --map) MAP="$2"; shift ;;
    --no-movies) NO_MOVIES=1 ;;
    --hold) HOLD=1 ;;
    --shots) SHOT_EVERY="$2"; shift ;;
    -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
    [0-9]*) SECS="$1" ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

OUT="$OUT_ROOT/$NAME"
mkdir -p "$OUT/shots"
GAME_ROOT_COALESCED="${GAME_ROOT:-$ROOT/assets/game_root}/Layer0/MEInit/Coalesced.ini"
bot() { python3 "$CT_DIR/switch_bot.py" "$@"; }
FATAL='XamShowDirtyDiscErrorUI|FATAL|Unhandled exception|Abort|panic|DEVICE_LOST|VK_ERROR'
LIVE="$OUT/live.log"
LISTENER=""
cleanup() { [[ -n "$LISTENER" ]] && kill "$LISTENER" 2>/dev/null || true; }
trap cleanup EXIT

[[ $BUILD -eq 1 ]] && "$ROOT/tools/build_nro.sh"
[[ -f "$NRO" ]] || { echo "error: $NRO not found" >&2; exit 1; }
[[ -f "$TOML" ]] || { echo "error: $TOML not found" >&2; exit 1; }

# ---- upload ---------------------------------------------------------------------------------------------------------
echo "== upload"
ftp_put "$NRO" masseffect-nx.nro

# Coalesced.ini: a modified one (start map, no movies) needs its SHA-1 in the settings; the game checks it.
COALESCED_MODIFIED=0
extra_toml="$(mktemp)"; : > "$extra_toml"
if [[ -n "$MAP" || $NO_MOVIES -eq 1 ]]; then
  [[ -f "$GAME_ROOT_COALESCED" ]] || { echo "error: $GAME_ROOT_COALESCED not found (set GAME_ROOT)" >&2; exit 1; }
  modified="$OUT/Coalesced.ini"
  cp "$GAME_ROOT_COALESCED" "$modified"
  [[ $NO_MOVIES -eq 1 ]] && python3 "$ROOT/tools/coalesced.py" no-startup-movies "$modified" "$modified" >/dev/null
  [[ -n "$MAP" ]] && python3 "$ROOT/tools/coalesced.py" set-map "$modified" "$modified" "$MAP" >/dev/null
  sha="$(sha1_of "$modified")"
  ftp_put "$modified" game_root/Layer0/MEInit/Coalesced.ini
  echo "masseffect_coalesced_sha1 = \"$sha\"" >> "$extra_toml"
  COALESCED_MODIFIED=1
fi
[[ $COLD -eq 1 ]] && echo "masseffect_cold_startup = true" >> "$extra_toml"
if [[ -n "$ROUTE" ]]; then
  [[ -f "$ROUTE" ]] || { echo "error: route $ROUTE not found" >&2; exit 1; }
  ftp_put "$ROUTE" route.tsv
  echo "input_play = \"sdmc:$SD_DIR/route.tsv\"" >> "$extra_toml"
  SECS=$(( $(tail -1 "$ROUTE" | cut -f1) / 1000 + 8 ))
fi
host="$(host_ip)"
if [[ -n "$host" ]]; then          # live log: the game streams its log to this computer while it runs
  : > "$LIVE"
  echo "log_network = \"$host:$LIVE_LOG_PORT\"" >> "$extra_toml"
  python3 "$CT_DIR/live_log.py" "$LIVE_LOG_PORT" "$LIVE" & LISTENER=$!
fi
make_toml "$TOML" "$OUT/uploaded.toml" masseffect_coalesced_sha1 masseffect_cold_startup input_play input_record log_network < "$extra_toml"
rm -f "$extra_toml"
ftp_put "$OUT/uploaded.toml" masseffect.toml
[[ $COLD -eq 1 ]] && ftp_del cache/cold.bin

log_pattern='[A-Za-z_-]+_[0-9]+\.log'
newest_log() { ftp_list logs | grep -Eo "$log_pattern" | sort | tail -1; }
before="$(newest_log || true)"

restore_game() {
  if [[ $COALESCED_MODIFIED -eq 1 && -f "$GAME_ROOT_COALESCED" ]]; then
    ftp_put "$GAME_ROOT_COALESCED" game_root/Layer0/MEInit/Coalesced.ini && echo "original Coalesced.ini restored"
    echo "(masseffect.toml still holds the SHA-1 of the modified one: upload your normal settings with deploy.sh)"
  fi
}
check_fatal() { [[ -s "$LIVE" ]] && grep -m1 -aE "$FATAL" "$LIVE" || true; }

# ---- start the game and reach gameplay -----------------------------------------------------------------------------
echo "== start"
bot until home 20 || { echo "not at the HOME menu" >&2; exit 1; }
bot press A
if [[ -n "$MAP" ]]; then
  bot until game 120 || true         # the classifier may call some scenes (cutscenes, bright interiors) "other"
  sleep 40                           # let the level load and settle
else
  bot until title 150 || { bot shot "$OUT/shots/fail.jpg"; exit 1; }
  bot until menu 90 PLUS 5 || { bot shot "$OUT/shots/fail.jpg"; exit 1; }
  bot until other 40 A 6             # Resume was pressed: the menu is gone (loading)
  bot until game 300 || { bot shot "$OUT/shots/fail.jpg"; exit 1; }
fi
bot shot "$OUT/shots/start.jpg"
if [[ $HOLD -eq 1 ]]; then
  echo "in game, left running (close it: switch_bot.py run 'press HOME; wait 2; press X; wait 2; press A')"
  exit 0
fi

# ---- play window ---------------------------------------------------------------------------------------------------
echo "== play for $SECS s"
t0=$SECONDS; aborted=0
if [[ -n "$ROUTE" ]]; then
  bot hold MINUS 0.5                 # held, not clicked: the game polls the pad once per frame
  shot=0
  while (( SECONDS - t0 < SECS )); do
    sleep "$SHOT_EVERY"
    shot=$((shot + 1)); bot shot "$OUT/shots/route-$shot.jpg" || true
    if f="$(check_fatal)" && [[ -n "$f" ]]; then echo "ABORT (live log): $f" >&2; aborted=1; break; fi
  done
else
  # Keep moving for the whole window (a static view in front of a rock measures nothing): walk forward, back, then
  # turn the camera in place, a screenshot per leg. Judge the picture from many views, never from two or three.
  leg=0
  while (( SECONDS - t0 < SECS )); do
    if (( (leg / 2) % 2 )); then turn=-24000; else turn=24000; fi
    bot run "stick LEFT 0 32000 3; stick LEFT 0 -32000 3; stick RIGHT $turn 0 1.5"
    bot shot "$OUT/shots/leg-$leg.jpg" || true
    leg=$((leg + 1))
    if f="$(check_fatal)" && [[ -n "$f" ]]; then echo "ABORT (live log): $f" >&2; aborted=1; break; fi
  done
fi
bot shot "$OUT/shots/end.jpg" || true
echo "play window: $((SECONDS - t0)) s"

# ---- close, collect ------------------------------------------------------------------------------------------------
echo "== close and collect"
bot run "press HOME; wait 2; press X; wait 2; press A" || true
bot until home 30 || true
restore_game

after="$(newest_log || true)"
[[ -n "$after" && "$after" != "$before" ]] || echo "warning: no new log on the SD card (still ${after:-none})" >&2
[[ -n "$after" ]] && ftp_get "logs/$after" "$OUT/game.log"
ftp_get logs/rex/rex_profile.log "$OUT/profile.log" || true
elf="${NRO%.nro}"; [[ "$elf" == *-nx ]] && elf="${elf%-nx}"
[[ -f "$elf" ]] && cp "$elf" "$OUT/masseffect.elf" 2>/dev/null || true   # for symbolizing the profile

# ---- summary -------------------------------------------------------------------------------------------------------
echo "== $NAME (${after:-no log})"
if [[ -s "$OUT/profile.log" ]]; then
  echo "game fps per 10 s (profiler, last 6 intervals):"
  grep '^====' "$OUT/profile.log" | tail -6 | grep -Eo 'game [0-9.]+ fps' | tr '\n' ' '; echo
  python3 "$CT_DIR/cpu_per_frame.py" "$OUT" || true
fi
[[ -s "$OUT/game.log" ]] && grep "GPU per Swap" "$OUT/game.log" | tail -1 | cut -c1-300 || true
[[ -s "$LIVE" ]] && echo "live log: $LIVE ($(wc -l < "$LIVE") lines)"
echo "results: $OUT"
[[ $aborted -eq 0 ]] || exit 3
