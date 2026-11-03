# Improvements

Features beyond a working Klipper install. The rest of the repo is bring-up:
flash, boot, link, a config that prints. This folder is where the AD4 gets
better than stock and, where it can, better than anything else on the market.

- [ROADMAP.md](ROADMAP.md): every idea, grouped by area, with its status.
- One file per feature once work starts on it (`<id>-<name>.md`): the design,
  what was measured, what is left. The roadmap row links to it.

Status words: **idea** (not checked), **ready** (approach known, nothing
blocking), **blocked** (says on what), **wip**, **done** (commit).

Rules that carry over from bring-up:

- Motion, heating and pin tests only with Caleb at the printer and his go for
  that test.
- Never point `SET_PIN` at a heater, fan or stepper pin.
- Speeds stay at or under 60 mm/s (travel included) until Caleb lifts it.
- The SoC has about 55 MB of RAM and runs busybox: anything on the printer is
  a small static C binary like `ad4-bridge`, not Python or ffmpeg.
