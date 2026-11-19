# LowRider Touch (fork of FluidTouch)

This repo is Jeffrey's fork of FluidTouch (ESP32-S3 CrowPanel touch controller for FluidNC).
A new workflow-ordered interface is being built on top of the existing platform code.

**Start here, in this order:**

1. `specs/HANDOFF.md` — current state, what to do next, things not yet verified.
2. `specs/README.md` — index of the specs. `specs/07-decisions.md` is the decision log and open-question list.
3. `specs/09-git-workflow.md` — branches, commits, syncing with upstream.

## Rules for working in this repo

- **Specs lead.** Behaviour changes go into `specs/` (and a row in `07-decisions.md`) in the same commit as the code. If a spec is unclear or missing, ask Jeffrey rather than assume; he prefers clarifying questions to assumptions.
- **Keep upstream-owned code mergeable.** `src/core`, `src/network`, `src/ui`, `sim/`, `platformio.ini`, `main.cpp` belong to upstream FluidTouch. Change them only as listed in `specs/02-architecture.md` §2, in small commits separate from new code (`platform:` prefix).
- **New code lives in** `src/app/` and `src/services/`. `lib/gcode/` is copied from a CNC-Tools release (`library/` of `gcode-core-<version>.zip`, D67, D69): never edit it here; changes go into CNC-Tools and come back with a later release.
- **The classic UI must keep working** (boot-selectable). Don't delete or restructure `src/ui`.
- **Simulator first.** Build and exercise every change in the desktop simulator before flashing.
- **Never claim device behaviour that was only run in the simulator.** Say which was tested.
- The G-code behaviour is defined by the CNC-Tools repo (`D:\Source Code\Repos\CNC-Tools\gcode-editor\specs`). The C++ library is built and tested there too ([specs/04](specs/04-gcode-library.md)); don't invent behaviour here.

## Build

```powershell
# Simulator (needs MSYS2 with gcc, SDL2, cmake, ninja; see sim/README.md)
.\sim\build.ps1 -Run
python sim\tools\fake_fluidnc.py --port 81      # fake controller; machine URL 127.0.0.1, port 81

# Firmware for the CrowPanel Advance v1.3/v1.4/v1.5
pio run -e elecrow-crowpanel-7-advance-v13
pio run -e elecrow-crowpanel-7-advance-v13 -t upload
```

The checkout path contains a space, which ESP-IDF can't build from. `platformio_local.ini`
(gitignored, already present on Jeffrey's PC) redirects packages and build output to
`D:/pio/FluidTouch/`. Keep its `custom_sdkconfig` lines in sync with `platformio.ini`.

Upstream's own notes for AI assistants are in `.github/copilot-instructions.md` (classic
architecture, LVGL conventions, theme usage). Still valid for the platform and classic UI.
