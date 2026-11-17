# 08 — Milestones

Each milestone ends with a build that runs in the simulator and on the CrowPanel, and a
short device checklist. M4 can proceed alongside M2 and M3.

| # | Milestone | Depends on | Delivers | Done when |
|---|-----------|-----------|----------|-----------|
| M0 | Specs and spike plans | | This folder | Specs reviewed; open questions that block M1 and M2 answered (done 2026-11-16) |
| M1 | Chassis | M0 | Branch `lrt/main`; listener interface in `FluidNCClient`; UI mode switch with classic UI intact; services skeleton; frame (tabs, status bar, banners) with empty screens; SettingsService on classic slot 0; fake FluidNC job playback and HTTP; CI; spikes S1 and S2 run and recorded | Device boots into the new frame, connects, shows state and position; can restart into classic and back; S1 and S2 results in [06](06-test-and-spikes.md). **Done 2026-11-16**, apart from confirming the reconnect fix on the device |
| M2 | Setup | M1 | Setup screen complete, including laser move and alarm/offline banners | A real job is prepared without leaving Setup |
| M3 | Run and basic Job | M2 | Job list from Controller SD and Display SD (no preview); start panel; Run with Prompt state, pause, abort, overrides, progress bar, times; Macros; Settings pages; upload from display SD; position log and spikes S3 and S4 | A full real job including bit prompts runs from the new UI; you stop needing classic for normal work |
| M4 | G-code library, read side | M0 | `lib/gcode` tokenizer, fixed, simulator, analysis, segments; PC test runner; `export-golden.mjs` in CNC-Tools; CI | All read-side golden cases pass; the reference job analyses on the device within budget |
| M5 | Preview | M3, M4 | Working folder on display SD; fetch from sources; preview and info line on Job; "fits from here" on Setup; progress layer and tool marker on Run; operation N of M | Preview of real jobs matches the G-code Editor's top view; progress follows a real cut |
| M6 | Transforms | M5 | classify, rotate, flip (flip only), move; blocked dialog; undo; upload-and-select; Move spec and implementation in CNC-Tools | Byte-identical to the G-code Editor on the golden set; a rotated job cuts correctly |
| M7 | Outline | M6 | Outline view; generated outline file; Run outline | Loops match the editor's within tolerance; outline run checked on the machine with Z raised |
| M8 | Inbox and polish | M5 | Spike S5 first; inbox source: OneDrive direct, or the PC folder server if S5 failed (D17); "Save to CNC inbox" in the G-code Editor; flip with reverse cuts; settings backup; user doc | A file saved at the PC is cut later with no PC in the workshop |

## M0 exit checklist

- [x] Recommendation agreed: fork as chassis, new UI (D1)
- [x] Specs 01 to 08 drafted
- [ ] Screens reviewed one by one (Setup first)
- [x] Inbox direction decided (D17)
- [x] S1 tried from a browser (works; during a job still to try)
