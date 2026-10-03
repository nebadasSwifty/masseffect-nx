# Testing on a real console

All performance numbers of this port were measured on a Switch, never on a computer (see
[../../docs/measuring.md](../../docs/measuring.md) for the method and the mistakes to avoid). These scripts upload a
build, run the game, collect the logs and screenshots, and compute the stable metrics. They need no personal paths:
everything comes from environment variables or from one file you create, `credentials.env`.

## What the console needs

- Custom firmware with the **sys-ftpd** sysmodule (the scripts upload and download over FTP; the stock port is 5000)
  and, for the unattended cycle only, **sys-botbase** (screenshots and button presses on port 6000).
- The port installed as `sdmc:/switch/masseffect-nx/masseffect-nx.nro` with `masseffect.toml` and `game_root/`
  beside it, started from the Homebrew Menu or from a forwarder tile.
- Both computer and console on the same network.

## Configuration

    cp tools/console-test/credentials.env.example tools/console-test/credentials.env
    $EDITOR tools/console-test/credentials.env

`credentials.env` holds `SWITCH_IP` and, if you changed them, the FTP port, user and password, the sys-botbase port,
the SD card folder and the address the console should send its live log to. It is ignored by git. Any variable you
set in the shell wins over the file, so `SWITCH_IP=10.0.0.7 tools/console-test/deploy.sh` works too. Other variables:
`OUT_ROOT` (results, default `out/console-test`), `GAME_ROOT` (your extracted disc, default `assets/game_root`),
`CONSOLE_ENV` (another credentials file), `REF_DIR` (reference screenshots).

## Scripts

| File | What it does |
|---|---|
| `deploy.sh` | Uploads `out/nx/masseffect-nx.nro` and `app/masseffect.toml` over FTP. `--live NAME` also asks the game to stream its log to this computer and receives it in `out/console-test/NAME/live.log`. For a manual test: you start the game yourself. |
| `switch_cycle.sh` | The unattended cycle: optional build, upload, start from the HOME menu, title, Resume, play window (a recorded route replayed by the game, or a walk in place), close, download the newest log and the profiler log, summary. Options: `--cold` (cold start, the acceptance condition), `--route FILE`, `--map BIOA_xxx`, `--no-movies`, `--toml FILE`, `--hold`. Results in `out/console-test/NAME/`: `game.log`, `profile.log`, `live.log`, `uploaded.toml`, `shots/`. |
| `switch_bot.py` | The sys-botbase client the cycle uses: `shot`, `press`, `hold`, `stick`, `run "..."`, `state`, `until`. Screens are recognised against reference screenshots that you make yourself (see below). |
| `map_sweep.sh` | A location sweep: starts in each map, plays 45 s, writes a contact sheet per map (needs ffmpeg for the sheets). |
| `cpu_per_frame.py` | Core-milliseconds per frame (total, main thread, ring thread) from the profiler logs of one or more runs: the stable metric for CPU changes. |
| `live_log.py` | Receives the live log over TCP (a portable `nc -l`). Started by the scripts above. |
| `common.sh` | Shared configuration loading and FTP helpers. |

## Reference screenshots for the bot

`switch_bot.py` finds out which screen the console shows by comparing a region of a screenshot with a reference image
of the same name. The references are screenshots of your own console, so they are not distributed. Make them once:

    python3 tools/console-test/switch_bot.py shot tools/console-test/ref/home.jpg    # HOME menu, forwarder tile selected
    python3 tools/console-test/switch_bot.py shot tools/console-test/ref/title.jpg   # "press START" screen of the game
    python3 tools/console-test/switch_bot.py shot tools/console-test/ref/menu.jpg    # main menu with Resume selected
    python3 tools/console-test/switch_bot.py shot tools/console-test/ref/game.jpg    # in play, HUD visible

Regions and thresholds are in the source and can be overridden with `ref/screens.json`
(`{"home": [[0, 600, 1280, 720], 6], ...}`: the region of the 1280x720 capture and the largest mean pixel distance
accepted). The cycle starts the game by pressing A on the selected tile of the HOME menu.

## Routes

A route is the pad input of a play session recorded by the game itself: set `input_record = "sdmc:/switch/masseffect-nx/route.tsv"` in
`masseffect.toml`, press MINUS to start the recording, play, press MINUS to stop, then download the file. Replaying it
(`switch_cycle.sh --route route.tsv`, which sets `input_play`) makes every run walk the same path from the same
save, which is what makes A/B comparisons meaningful. Only compare runs of the same route.

## Rules of thumb

Read [../../docs/measuring.md](../../docs/measuring.md) before trusting a number. In short: stock clocks, one change per
run, compare two builds back to back, judge a picture from many captures, and acceptance runs are cold (`--cold`).
