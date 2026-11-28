# Notes for CNC-Tools: operations in a file with line numbers (after 0.4.0-build.100)

From LowRider Touch, 2026-11-27. Low priority: nothing here uses line numbers today. It is
sent so the gap is known, for whenever someone turns line numbers on in the post-processor.

## 1. What was found

The display follows a running job by FluidNC's `Ln:` field when the file has N words (its
D93). To try it, the reference job `Spoilboard_DogHoles.gcode` was given an N word on every
line (`N10`, `N20`, ...). Then `analyse()` found no operations: `Structure::kind` was `None`.
The display therefore shows no "op N of M" for such a file. The moves were read as before:
the display's host test found all 13,301 of them by their N word.

The cause is in `ops/structure.ts` `findOperationStructure()`, and so in core's port of it.
Every marker is matched at the start of the trimmed line:

| Match | Line it misses with an N word |
|-------|-------------------------------|
| `t.startsWith(BEGIN_OP)`, `FINISH_OP`, `BEGIN_TC` | `N100 (Begin operation: Pocket1)` |
| `M6_RE` (`^\(?[ \t]*(?:M0*6...`) | `N120 M6 T3` |
| `FIXTURE_RE` (`^(?:[ \t]*G0*5[4-9]...)+[ \t]*$`) | `N130 G54` |
| `COMMENT_ONLY_RE`, between two blocks | `N140 (MSG Tool change: ...)` reads as G-code between two operations |
| `^[ \t]*\(MSG ` (the first block's leading lines) | `N90 (MSG ...)` |
| `PROBE_RE`, `LABEL_RE` | not anchored; they still match |

Posts that number lines differ. Some number only lines with a command, others number every
line, comments included. Either kind breaks at least the M6 and fixture lines.

## 2. Asked

When it suits, skip one leading N word before matching: `[ \t]*N\d+[ \t]*`, any case.
That is, the same rule in each match above, or a helper the scan calls once per line before
the matches.

- **The editor's own operations (Arrange, `;OFF`):** they move or comment out whole lines,
  so an N word moves with its line. A rearranged file then has its N words out of order;
  FluidNC only reports them, it doesn't check their order. Whether Arrange should renumber, or warn, is yours to decide. The display only
  needs the structure to be found.
- **Golden cases:** a numbered copy of a reference job, whose structure is the unnumbered
  one's with the same line indices.

Nothing on the display changes when this lands: the structure comes with a later release.
If you'd rather not support numbered files, say so and the display's spec will note that
"op N of M" needs a file without line numbers.
