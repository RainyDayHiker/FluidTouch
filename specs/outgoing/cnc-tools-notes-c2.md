# Notes for CNC-Tools after C2 (0.1.0-build.70)

> **Answered** in spec 15 revision 11 (R69 to R72): `applyChecked()` with `Status::FileChanged`,
> and the manifest's `phase` set per phase. Kept as sent.

From LowRider Touch, 2026-11-20. The release passes the device suite on the CrowPanel
(162 of 162 in 6:03, display SD at 20 MHz), the host program built with MinGW GCC 16.2 with
no warnings, and the simulator. The C1 notes (runner statics, yield docs, `LineSplitter`,
the GCC 16 warning, "content bytes") are all resolved in this release. Two notes:

## 1. A request: `apply()` without its second check

Spec 15 §5.3 has `apply()` run `check()` again "(the file may have changed since)". On the
device that second check is about 40% of a transform's time. Rotate CW, measured on the
device (`apply()` includes its check):

| Job | `check()` | `apply()` |
|-----|-----------|-----------|
| `Spoilboard_DogHoles.gcode` (789 KB) | 8.0 s | 11.5 s |
| `CNC_EscapeWheel.gcode` (157 KB) | 1.6 s | 2.3 s |
| 5 MB perf fixture | 45.5 s | 64.8 s |

The editor doesn't do this second check. `engine.ts` validates once, keeps the session's
revision with the pending report, and refuses with "The file changed since the panel showed
it" if the document changed. `rotate90.ts`'s `apply()` doesn't validate, and keeps only the
overflow guard.

The display has the same guarantee as that revision check: the job is in its own working
folder on the display SD, which only the display writes.

**Asked:** an `apply()` variant that takes the result of the `check()` the caller just ran,
with the caller vouching that the file is unchanged (as the editor's revision check does),
and skips the second check. Keep the existing `apply()` as the safe default.

How much the variant saves is yours to design. The rewrite pass needs what the check found
(the job extents, the stock, the overflow guard), so the check may have to hand over a
small state block, sized by the caller or held in the arena, rather than nothing. If the
saving turns out small, say so and we'll keep the current `apply()`. Device suite: a case
that runs the variant and compares it byte for byte with `apply()`.

## 2. A slip: the manifest's phase

`manifest.json` in `gcode-core-0.1.0-build.70.zip` says `"phase": "C1"`; the release is C2
(its title, CHANGELOG, `transform` group). Nothing here reads the field yet, but
`tools/gcode_core.py` prints it, and HANDOFF records it.
