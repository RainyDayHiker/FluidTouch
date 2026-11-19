# LowRider Touch — specs

Specs for the new touch-screen firmware built on this FluidTouch fork: a workflow-ordered
interface (Setup, Job, Run, Macros, Settings) for one FluidNC machine, with on-device G-code
preview, transforms, and outline check.

**Status:** M0 draft, 2026-11-16. New here? Read [HANDOFF.md](HANDOFF.md) first. Nothing here is built yet. Lines marked **Q** are open
questions; the list is in [07-decisions.md](07-decisions.md#open-questions).

| # | Spec | State |
|---|------|-------|
| 01 | [Requirements](01-requirements.md) | Draft, mostly settled |
| 02 | [Architecture](02-architecture.md) | Draft; memory budget measured (S2) |
| 03 | [Screens](03-screens.md) | First draft; control lists need review screen by screen |
| 04 | [G-code library](04-gcode-library.md) | Built in CNC-Tools (D67); its spec is in [outgoing/](outgoing/cnc-tools-15-cpp-core.md) until it moves there |
| 05 | [File workflow](05-file-workflow.md) | Draft; inbox is OneDrive direct, pending spike S5 |
| 06 | [Testing and spikes](06-test-and-spikes.md) | S1 (partly), S2, S3 and S4 run; S5 not yet |
| 07 | [Decisions and open questions](07-decisions.md) | Living log |
| 08 | [Milestones](08-milestones.md) | Draft |
| 09 | [Git workflow](09-git-workflow.md) | Draft |
|    | [Handoff](HANDOFF.md) | Current state and next steps; read first |

Related specs live in the CNC-Tools repo (`gcode-editor/specs/`). Its specs 04 (G-code
model), 06 (Rotate 90), 12 (Flip) and 13 (Outline check) define the behaviour the on-device
G-code library must reproduce; this folder does not repeat them.

Names: the working name is "LowRider Touch". The existing interface is called the
**classic UI** throughout.
