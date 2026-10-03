# Contributing

Thanks for your interest in the Mass Effect port. Bug reports, measurements and patches are welcome.

## Before you open an issue

- Say which console model you use (Erista, Mariko, Lite, OLED), the firmware, the custom firmware, and whether the game
  ran handheld or docked.
- Attach the newest log from `sdmc:/switch/masseffect-nx/logs/` and your `masseffect.toml`.
- Do **not** attach game files, `default.xex`, shader packages made from your disc, or screenshots of copyrighted
  assets other than what is needed to show a rendering bug.

## Sending a patch

1. Read [docs/README.md](docs/README.md) and [docs/building.md](docs/building.md).
2. Keep a change to one idea. If it affects speed, measure it on the console as described in
   [docs/measuring.md](docs/measuring.md) and put the numbers in the pull request.
3. Do not change the image of the default configuration without saying so; changes that alter the image must be a
   setting that is off by default.
4. Keep the licence headers of third-party files.

By contributing you agree that your work is distributed under the licences stated in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): GPL-3.0 for the port, and the licence of the component for changes
inside `sdk/`, `shaders/` and `mesa/`.
