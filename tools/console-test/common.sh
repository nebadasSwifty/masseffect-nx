# Shared by the scripts of tools/console-test: loads the configuration and defines the FTP helpers. Source it.
# Works with the bash 3.2 that ships with macOS and with bash on Linux.

CT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$CT_DIR/../.." && pwd)"

# credentials.env is yours and is not distributed (see credentials.env.example). Variables already set in the
# environment win over the file.
CONSOLE_ENV="${CONSOLE_ENV:-$CT_DIR/credentials.env}"
if [[ -f "$CONSOLE_ENV" ]]; then
  while IFS='=' read -r key value; do
    case "$key" in ''|\#*) continue ;; esac
    [[ -z "${!key:-}" ]] && export "$key=$value"
  done < "$CONSOLE_ENV"
fi

: "${SWITCH_IP:?SWITCH_IP is not set: copy tools/console-test/credentials.env.example to credentials.env and edit it}"
FTP_PORT="${FTP_PORT:-5000}"
FTP_USER="${FTP_USER:-anonymous}"
FTP_PASS="${FTP_PASS:-}"
SD_DIR="${SD_DIR:-/switch/masseffect-nx}"            # folder of the NRO on the SD card
LIVE_LOG_PORT="${LIVE_LOG_PORT:-28771}"
OUT_ROOT="${OUT_ROOT:-$ROOT/out/console-test}"
FTP_BASE="ftp://$SWITCH_IP:$FTP_PORT/"               # a second slash after the port makes the path absolute

# curl against the console's FTP server. The server is slow and drops connections: retry.
ftp_curl() { curl -sS --retry 6 --retry-delay 5 --retry-all-errors --user "$FTP_USER:$FTP_PASS" "$@"; }
ftp_put()  { ftp_curl --ftp-create-dirs -T "$1" "${FTP_BASE}${SD_DIR}/$2"; }                 # local file, remote name
ftp_get()  { ftp_curl --max-time 300 "${FTP_BASE}${SD_DIR}/$1" -o "$2"; }                    # remote name, local file
ftp_list() { ftp_curl "${FTP_BASE}${SD_DIR}/$1/" -l | tr -d '\r'; }                          # remote folder
ftp_del()  { ftp_curl "${FTP_BASE}" -Q "DELE ${SD_DIR}/$1" -o /dev/null 2>/dev/null || true; }

# Address of this computer as the console sees it, for the live log (override with LIVE_LOG_HOST).
host_ip() {
  if [[ -n "${LIVE_LOG_HOST:-}" ]]; then echo "$LIVE_LOG_HOST"; return; fi
  ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null \
    || hostname -I 2>/dev/null | awk '{print $1}' || true
}

# The toml of a run: the base file without the keys this script manages, plus the given "key = value" lines.
make_toml() {  # base-toml output-toml key... (keys to drop; the lines to add are read from stdin)
  local base="$1" out="$2"; shift 2
  local pattern="^($(IFS='|'; echo "$*"))[[:space:]]*="
  grep -Ev "$pattern" "$base" > "$out" || true
  cat >> "$out"
}

sha1_of() { if command -v shasum >/dev/null 2>&1; then shasum "$1" | cut -d' ' -f1; else sha1sum "$1" | cut -d' ' -f1; fi; }
