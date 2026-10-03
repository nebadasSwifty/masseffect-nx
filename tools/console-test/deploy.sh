#!/usr/bin/env bash
# Copies a build to the console's SD card over FTP and, optionally, listens for the game's live log.
# For a manual test: you start the game from the Homebrew Menu yourself.
#
#   tools/console-test/deploy.sh [--nro FILE] [--toml FILE] [--live NAME]
#
#   --nro FILE    NRO to upload (default out/nx/masseffect-nx.nro)
#   --toml FILE   settings to upload as masseffect.toml (default app/masseffect.toml)
#   --live NAME   add `log_network` to the uploaded settings and receive the log in out/console-test/NAME/live.log
#                 until you press Ctrl-C (start the game meanwhile)
#   --no-nro      upload only the settings
#
# Configuration: tools/console-test/credentials.env (see README.md).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NRO="$ROOT/out/nx/masseffect-nx.nro"
TOML="$ROOT/app/masseffect.toml"
LIVE=""
UPLOAD_NRO=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --nro) NRO="$2"; shift ;;
    --toml) TOML="$2"; shift ;;
    --live) LIVE="$2"; shift ;;
    --no-nro) UPLOAD_NRO=0 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
[[ -f "$TOML" ]] || { echo "error: $TOML not found" >&2; exit 1; }

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
if [[ -n "$LIVE" ]]; then
  host="$(host_ip)"
  [[ -n "$host" ]] || { echo "error: cannot detect this computer's address; set LIVE_LOG_HOST" >&2; exit 1; }
  echo "log_network = \"$host:$LIVE_LOG_PORT\"" | make_toml "$TOML" "$tmp" log_network
else
  cp "$TOML" "$tmp"
fi

if [[ $UPLOAD_NRO -eq 1 ]]; then
  [[ -f "$NRO" ]] || { echo "error: $NRO not found; build it with tools/build_nro.sh" >&2; exit 1; }
  echo "== uploading $(basename "$NRO") to $SD_DIR/masseffect-nx.nro"
  ftp_put "$NRO" masseffect-nx.nro
fi
echo "== uploading the settings as $SD_DIR/masseffect.toml"
ftp_put "$tmp" masseffect.toml

if [[ -n "$LIVE" ]]; then
  mkdir -p "$OUT_ROOT/$LIVE"
  echo "Uploaded. Start Mass Effect from the Homebrew Menu; the log goes to $OUT_ROOT/$LIVE/live.log (Ctrl-C to stop)."
  exec python3 "$CT_DIR/live_log.py" "$LIVE_LOG_PORT" "$OUT_ROOT/$LIVE/live.log"
fi
echo "Uploaded. Start Mass Effect from the Homebrew Menu."
