# Notes for CNC-Tools: generators on the display (after spec 15 revision 15)

From LowRider Touch, 2026-11-21. Spec 15 §20 (New Surfacing Job) is agreed as it stands;
the display's design for it is this repo's spec 10. One slip, then a request for the next
generators.

## 1. A slip in §20.2

"**The golden export:**" is an empty bullet, with its list (`surfacing.json` …) set below it
as a separate list.

## 2. The display's form: two texts still in the editor's dialog

The display builds its form only from core's data. Two texts the editor's dialog shows are
still written in the dialog:

- the **set-up instructions**: "Zero X/Y at the area's lower-left corner; the bit hangs
  6.35 mm past every edge", which the dialog builds with `DOVETAIL_BIT.diameter / 2`;
- the generator's **title and a one-line description** for a list of generators ("New
  Surfacing Job"; "Flatten the spoilboard or a blank: probes the surface, then skims an
  area").

**Asked:** put both in core with the form's data (`SURFACING_FORM.title`, `.description`,
`.instructions`), with the number in the instructions written by core. The dialog reads them
from there too.

## 3. A request: one shape for every generator

The display offers generators as a list (its spec 10): one form, one flow, and per generator
only a small adapter over its typed calls. Every generator needs the same parts:

| Part | Surfacing's |
|------|-------------|
| Title, description, instructions, the tool to fit | `SURFACING_FORM`, `surfacingBit()` |
| Fields, in order: name, label, unit as shown, group, limits, default text | `surfacingField()` |
| A field's text to its value | `surfacingFieldValue()` (D70's grammar) |
| Each field's message | `surfacingErrors()` |
| Name and summary without writing | `planSurfacing()` |
| The file through a `ByteWriter` and a small arena | `writeSurfacingJob()` |
| No progress or cancel when the work is small; `InvalidParameters` when a field has an error | as §20.4 |

**Asked:**

1. Add to §12 (new features) that a future generator gives these same parts, so the display's
   work for it is only the adapter.
2. If a second generator is planned, consider one shared descriptor in core, for example
   `gcode/generators.h` with `generatorCount()`, `generator(i)` giving the texts, a field table
   and calls that take the field values as an array of doubles. The display would then list
   and run every generator a release carries with no code of its own. The typed surfacing API
   can stay as it is beside it. Not needed for surfacing alone.
3. A field that is a choice rather than a number (a bit from a list, a direction) comes as
   field data too (a kind and its options), when a generator first needs it.
