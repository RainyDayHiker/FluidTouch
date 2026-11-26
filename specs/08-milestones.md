# 08 — Milestones

Each milestone ends with a build that runs in the simulator and on the CrowPanel, and a
short device checklist. M4 can proceed alongside M2 and M3. Features added to the G-code Editor
later get a milestone of their own, or a place in one, when Jeffrey wants them on the display
([04 §4](04-gcode-library.md#4-new-features-from-the-editor), D68).

| # | Milestone | Depends on | Delivers | Done when |
|---|-----------|-----------|----------|-----------|
| M0 | Specs and spike plans | | This folder | Specs reviewed; open questions that block M1 and M2 answered (done 2026-11-16) |
| M1 | Chassis | M0 | Branch `lrt/main`; listener interface in `FluidNCClient`; UI mode switch with classic UI intact; services skeleton; frame (tabs, status bar, banners) with empty screens; SettingsService on classic slot 0; fake FluidNC job playback and HTTP; CI; spikes S1 and S2 run and recorded | Device boots into the new frame, connects, shows state and position; can restart into classic and back; S1 and S2 results in [06](06-test-and-spikes.md). **Done 2026-11-16**; reconnect fix confirmed on the device 2026-11-17 |
| M2 | Setup | M1 | Setup screen complete, including laser move and alarm/offline banners | A real job is prepared without leaving Setup. **Setup done and checked on the device 2026-11-17** (D33 to D45). Choosing and starting the job moved to the Job tab (D33), so the job part of this check is met with M3's Job screen |
| M3 | Run and basic Job | M2 | Job list from Controller SD and Display SD (no preview); start panel; Run with Prompt state, pause, abort, overrides, progress bar, times; Macros; Settings pages; upload from display SD; position log and spikes S3 and S4 | A full real job including bit prompts runs from the new UI; you stop needing classic for normal work |
| M4 | G-code library, read side | M0 | In CNC-Tools (D67): phases C0 and C1 of its spec 15, ending in a promoted release. Here (D69): the copy script from a release with the checksum check and `.gitattributes`; the device suite (`float`, `parse`, `self`, `analyse`, `perf`) run from Settings > Tools (D73) with its data on the SD card; the SD reader and the arena; `lib/gcode` in the firmware and simulator builds; the fallback font (done early, D71) | The device suite passes on the device for the C1 release; its `perf` results (SD speed, analysis time and memory of the reference jobs) recorded in 06 and the display's budgets set from them |
| M5 | Preview | M3, M4 | Working folder on display SD; fetch from the Controller SD; preview and info line (with the display's time estimate, D80) on Job; progress layer, tool marker and the tool's line (D79) on Run; operation N of M; time left from the estimate. Built in parts 5a to 5c ([11](11-preview.md)); no "fits from here" (D81) | Preview of real jobs matches the G-code Editor's top view; progress follows a real cut |
| M6 | Transforms | M5 | Rotate, Flip ("Flip only" with the W-FLIP confirm) from phase C2 in CNC-Tools; device suite `transform` group passes; blocked dialog; undo; upload-and-select. Move later, once it is an editor feature (D70) | Byte-identical to the G-code Editor on the golden set; a rotated job cuts correctly |
| M7 | Outline | M6 | Outline view; generated outline file (phase C3 in CNC-Tools; its design note reviewed, 15 §18, D77), built in the preview's analysis; device suite `outline` group passes; Run outline | Loops and file byte-identical to the editor's; outline run checked on the machine with Z raised |
| M8 | New jobs | M7 | Library 0.4.0 (New Surfacing Job, CNC-Tools 15 §20) integrated, device suite `surfacing` group passes; the New job card in the picker; the generator form from the library's data with the text keypad; Create into the working folder, then the analysis, transforms, outline and upload as for any job; last values on the display SD; the Generators service, so later generators need only an adapter ([10](10-new-jobs.md), D78) | A surfacing job made on the display is byte-identical to the editor's for the same field texts, and is cut on the machine through its probe and prompts |
| M9 | Inbox and polish | M5 | Spike S5 first; inbox source: OneDrive direct, or the PC folder server if S5 failed (D17); "Save to CNC inbox" in the G-code Editor; flip with reverse cuts; settings backup; user doc | A file saved at the PC is cut later with no PC in the workshop |
| M10 | Battery gauge | M1; the gauge fitted and the hardware check passed ([12 §6](12-battery.md#6-hardware-check-jeffrey-before-building)) | The gauge model in the simulator's `Wire` stub; BatteryService; the status bar's battery icon and the About line ([12](12-battery.md), D95 to D97) | Every level of 12 §4 shown in the simulator; on the device, About's voltage matches a multimeter and the icon shows charging and the right level |

## M0 exit checklist

- [x] Recommendation agreed: fork as chassis, new UI (D1)
- [x] Specs 01 to 08 drafted
- [ ] Screens reviewed one by one (Setup first). Setup done 2026-11-17 (D33 to D45)
- [x] Inbox direction decided (D17)
- [x] S1 tried from a browser (works, also during a job, 2026-11-18)
