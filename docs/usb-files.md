# USB file channel (developer only)

Reads logs, `tour_status.txt`, stack profiles and caches (and writes small files) over the USB-C cable **while
the game runs**, as a faster and more reliable replacement for sys-ftpd during test runs. It is off by default and
meant for development builds only.

- Game side: `app/src/me_usb_files.cpp` (started from `MassEffectApp::OnPostInitLogging`), plus the current-log read
  hook `RexLogReadCurrentFile` in `sdk/src/core/log_nonblocking.cpp`.
- Host side: `tools/switch_usb.py` in mass-effect-recomp (pyusb + Homebrew libusb, run with `run/venv/bin/python3`).
- Tour integration: `USB=1 zsh tools/me1_tour.sh ...` (mass-effect-recomp).

## Enabling

```toml
dev_usb_files = true            # init only
dev_usb_files_priority = 0x3F   # optional: Horizon priority of the server thread (0x2C..0x3F, default 0x3F = lowest)
```

With `dev_usb_files = false` nothing runs: no thread, no `usb:ds` session, no memory (the 1 MB transfer buffer is
allocated only when the channel starts).

When it is on, the log gets `[usb_files] thread started ...` and `[usb_files] listening: USB 057E:3000 ...`, then one
line per LIST/GET/PUT (path, bytes, time, MB/s). PING and STAT are silent. Errors are `[usb_files]` warnings.

## Device identification

| Field | Value |
|---|---|
| VID:PID | `057E:3000` (libnx usbComms default; strings "Nintendo" / "Nintendo Switch") |
| Interface | class `FF` (vendor), subclass `4D` ('M'), protocol `45` ('E') |
| Endpoints | one bulk IN, one bulk OUT (max packet 512 at High Speed, 1024 at SuperSpeed) |

The host matches VID:PID **and** the interface triple, so other homebrew that uses `057E:3000` (nxlink-style tools,
Goldleaf, ...) is not mistaken for the game. Close other USB tools on the Mac (NS-USBloader etc.) while using it.
Outside the game the console shows up with its own descriptor (`057E:2000` here); `switch_usb.py` reports that case
as "the Switch is connected, but the game's channel is not up".

## Thread and safety

- One libnx thread (`threadCreate`, never `std::thread`), 64 KB stack, priority `dev_usb_files_priority`, created
  on core 2 with core mask 0x7 (the ring thread lives on cores 0-1; the exclusive-core logic may later drop core 2
  from its mask like any other registered thread). Shown as `usb files` in `rex_profile.log`.
- All `usbComms*` calls happen on that thread only. They block: `usbCommsRead` waits for the next request (and,
  while the cable is out, inside `usbDsWaitReady` until it is plugged back in); `usbCommsWrite` waits until the host
  reads. The game never waits for the thread.
- A failed or zero-length transfer counts as a link error; after 4 in a row the thread calls `usbCommsExit`, sleeps
  2 s and initializes again. If `usbCommsInitializeEx` fails (another program holds `usb:ds`), it retries every 5 s
  (warning logged once a minute).
- One 1 MB page-aligned buffer (file data in both directions, transferred by usbComms without a copy), two 4 KB
  page-aligned buffers (request, response header) and a 32-entry directory buffer. Allocated once at start.

## Paths

Remote paths are relative to the **NRO folder** (`sdmc:/switch/masseffect-nx/`, taken from argv[0] like
`RexSwitchLogDir()`), e.g. `logs/rex/tour_status.txt`, `cache/masseffect_native_pipelines.bin`. A leading `/` is
ignored; `.` and empty components are dropped; `..`, `\`, `:` and control characters are rejected (status 2). An
empty path is the NRO folder itself (for LIST).

## Protocol (version 1, little endian)

Every request is exactly **1000 bytes** and every response header exactly **80 bytes**. Neither is a multiple of any
USB max packet size (64/512/1024), so each always ends in a short packet: the game reads requests into a 4 KB buffer
and the transfer completes at the request boundary, and the host can read headers with a 1 MB read that ends at the
header. Payloads are read by the host with exact sizes (no zero-length packets are needed or sent).

Request (host -> game):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"MEUQ"` |
| 4 | 2 | version (1) |
| 6 | 2 | command: 1 PING, 2 LIST, 3 STAT, 4 GET, 5 PUT |
| 8 | 4 | sequence number (echoed in the answer) |
| 12 | 4 | flags (GET: bit 0 = tail) |
| 16 | 8 | arg0 (GET: offset, or byte count from the end with the tail flag; PUT: file size) |
| 24 | 8 | arg1 (GET: length, 0 = to the end) |
| 32 | 2 | path length (max 768) |
| 34 | n | path (UTF-8, no terminator); the rest is zero |

Response header (game -> host), followed by `payload` bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"MEUR"` |
| 4 | 2 | version |
| 6 | 2 | command (echo) |
| 8 | 4 | sequence number (echo) |
| 12 | 4 | status: 0 ok, 1 bad request, 2 bad path, 3 not found, 4 I/O error, 5 locked, 6 is a directory |
| 16 | 8 | payload length |
| 24 | 8 | arg0: file size (GET/STAT), entry count (LIST), chunk size (PING), file size (PUT) |
| 32 | 8 | arg1: mtime, POSIX seconds, 0 = unknown (GET/STAT) |
| 40 | 8 | arg2: first byte served (GET); 0 file, 1 directory, 2 file held open for writing (STAT); 0 ready, 1 done (PUT) |
| 48 | 4 | Horizon Result of the failure, 0 if none |
| 52 | 28 | reserved (0) |

With a non-zero status the payload is an error message (text).

Commands:

- **PING**: payload `masseffect-nx usb_files v1 base=sdmc:/switch/masseffect-nx`.
- **LIST** `dir`: payload is one line per entry, `d|f TAB size TAB mtime TAB name LF` (sizes come from the directory
  read, so files held open for writing are listed too).
- **STAT** `path`: header only.
- **GET** `path`: header (payload = bytes that follow), then the data in 1 MB writes. The game always sends exactly
  `payload` bytes; if the file shrinks during the transfer or a read fails, the rest is zero-filled (logged).
- **PUT** `path`: the game creates `path.usbtmp` (and missing parent folders) and answers with arg2 = 0 (ready); the
  host then sends exactly `arg0` bytes; the game flushes, deletes the old file, renames the temporary file and
  answers again with arg2 = 1 (done). A transfer that stops early deletes the temporary file.

Resynchronization: a new host session may find the pipe dirty (an earlier `get` was interrupted with Ctrl-C, or the
game is still in the middle of a transfer). `switch_usb.py` drains the IN pipe, then sends PINGs until it gets an
answer with its own sequence number, discarding everything else; it also discards answers with a wrong sequence
number before every command. On the game side, anything that is not a 1000-byte `MEUQ` request is ignored, and a PUT
whose data stops short (e.g. a new host's request arrived instead) is abandoned; a whole request received that way is
executed next.

## Files that are open for writing

Horizon's FS refuses a second open of a file that is open for writing (this is also why sys-ftpd cannot read the
current log). GET and STAT first try a normal read-only open; if it fails:

- **The current log** (`logs/masseffect_N.log`, with `log_nonblocking = true`): read through
  `RexLogReadCurrentFile`, which takes the log writer's file lock, writes what is queued, closes the file, reads the
  requested range through a read-only handle and reopens the file for appending (as rotation does). Done per 1 MB
  chunk; the writer waits at most for that one chunk. A rotation between two chunks of one GET can mix two files
  (the rotated parts `masseffect_N.<k>.log` are closed files and read normally). The match is by path
  (`sdmc:` prefix and doubled `/` ignored) against the sink's path, `<exe folder>/logs/...` from `rex_app.cpp`.
- **Any other file held open for writing** (`logs/rex/rex_stderr.log`, the process's stderr): GET answers status 5
  ("locked"); STAT and LIST still give the size from the directory entry.

`tour_status.txt`, `rex_profile.log` and `cache/masseffect_native_pipelines.bin` are written and closed (the pipeline
cache through a temporary file and a rename), so they are read normally.

## Host tool

```sh
cd mass-effect-recomp
run/venv/bin/python3 tools/switch_usb.py ping
run/venv/bin/python3 tools/switch_usb.py ls logs
run/venv/bin/python3 tools/switch_usb.py stat logs/rex/rex_profile.log
run/venv/bin/python3 tools/switch_usb.py get logs/masseffect_12.log out.log --tail 200000
run/venv/bin/python3 tools/switch_usb.py cat logs/masseffect_12.log --tail 4000
run/venv/bin/python3 tools/switch_usb.py put local.flag logs/rex/hang_dump.flag
run/venv/bin/python3 tools/switch_usb.py cat-status        # logs/rex/tour_status.txt
```

Options: `--wait S` (seconds to wait for the device, default 5), `--timeout S` (per transfer, default 10), `-q` (no
throughput line on stderr). `get` writes `LOCAL.part` and renames it when complete. Exit codes: 0 ok, 1 the game
answered with an error, 2 no channel (game not running, cvar off, cable, timeout).

Tour (`tools/me1_tour.sh`): `USB=1` adds `dev_usb_files = true` to each session's toml, polls `tour_status.txt`
through USB (FTP if that fails), requests the hang stack dump through USB, and fetches the session's log parts,
`rex_profile.log` and `pipelines.bin` through USB **before** the game is closed (FTP after closing if any of them
failed). The USB copy of the current log lacks the lines written while the game closes. Errors of the tool go to
`run/me1/tour_<stamp>/usb_errors.txt`. Uploading the toml, deleting stale files and everything else done while the
game is not running stays on FTP.

## Limits

- Only while the game runs and after logging is initialized (the channel starts in `OnPostInitLogging`, before the
  guest image loads); a crash, HOME exit or hang that kills the process ends it.
- One host at a time; requests are served one after the other. A huge GET keeps the thread busy until the host
  has read all of it (an interrupted GET is drained by the next session).
- Throughput depends on the thread's priority: at 0x3F it only runs when the cores have idle time, so transfers can
  slow down in heavy scenes. That is by design (the game is never delayed); raise `dev_usb_files_priority` for a
  faster but more intrusive channel.
- NRO builds under hbloader can open `usb:ds`. An installed NSP whose NPDM does not list `usb:ds` cannot (the init
  fails and is retried every 5 s with a warning).
- Not for release builds: no authentication, writes anywhere under the NRO folder.

## To verify on the console

1. `dev_usb_files = false`: no `[usb_files]` line, nothing changes (fps, start-up).
2. `dev_usb_files = true`: `listening` line; `ping`, `ls logs`, `stat`, `cat-status`, GET of a closed log part, GET of
   the current log (also `--tail`), GET of `cache/masseffect_native_pipelines.bin` (16 MB: note the MB/s), PUT of a
   small file and of a multi-MB file (SHA compare via FTP).
3. GET of `logs/rex/rex_stderr.log`: expected status 5 (locked) unless FS allows the second open.
4. Unplug the cable during idle, plug it back: `ping` works again without restarting the game. Unplug during a large
   GET: the game keeps running; the next `ping` resynchronizes (look for `[usb_files] repeated USB errors` and the
   re-init in the log).
5. Ctrl-C `switch_usb.py` in the middle of a 16 MB GET, then `ping` at once: it must drain and answer.
6. fps with the channel idle and during a 16 MB GET (expected: no change at 0x3F).
7. `USB=1 zsh tools/me1_tour.sh` with `LOCS=eden`: status polled over USB, `.usb_fetched` in the session folder.
