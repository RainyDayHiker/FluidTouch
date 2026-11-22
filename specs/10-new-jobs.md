# 10 — New jobs (generators)

A **generator** makes a complete job from a few values, with no file to start from. The first
is the G-code Editor's New Surfacing Job (CNC-Tools spec 14; its port, CNC-Tools 15 §20). This
spec is how the display offers generators, so that more can be added later with little work:
one form, one flow and one service for all of them, and per generator only what its library
release provides. Milestone M8 ([08](08-milestones.md)); decision D78.

## 1. What a generator is on the display

| Part | Where it comes from |
|------|---------------------|
| Title ("New Surfacing Job"), a one-line description, the set-up line (where to zero), the bit line (what to fit), the advanced group's title | The library, as finished text (`surfacingForm()`, CNC-Tools 15 §20.4, revision 16), and the tool's data (`surfacingBit()`) |
| Fields: label, unit as shown, main or advanced group, limits, default text | The library (`surfacingField()`, CNC-Tools 15 §20.4) |
| A field's text to its value; each field's message | The library (`fieldValue()`, the rule for every generator's number fields, CNC-Tools D70; `surfacingErrors()`) |
| The summary line and the file name, as the values are typed | The library (`planSurfacing()`) |
| The file | The library (`writeSurfacingJob()`), into the display's working folder |
| The form, the keypad, the flow, where the values are kept | The display, the same for every generator (§3 to §6) |

The display holds no G-code knowledge of its own about a generator: no limits, no labels, no
texts that the editor also shows. What the editor's dialog shows, the display shows from the
same data, so the two can't drift ([04 §4](04-gcode-library.md#4-new-features-from-the-editor)).

## 2. Adding a generator later

1. The generator is specified in CNC-Tools with a Device section (its §12.3 contract) and
   ported; its release lists it in the manifest's `features` (CNC-Tools 15 §9.5).
2. Here: the release is integrated as any other (04 §1) and its device suite group passes.
3. One **adapter** is added to the generator list (`src/services/generators.*`, §7): about 50
   lines that map the generator's typed library calls onto the common interface. Nothing else
   changes: it appears under New job in the picker, with its own form built from its data.

What the common interface needs from every generator is the list in §1. CNC-Tools 15 §12.6
(revision 16) makes it the shape every generator provides, Surfacing's, and adds a shared
descriptor, `gcode/generators.h`, with the second generator. From that release the adapters
are replaced by one generic adapter over the descriptor, and a release that adds a generator
needs no code here at all.

Fields are numbers today. A choice field (a bit from a list, a direction) comes as field data
from the library when a generator first needs one (15 §12.6: its options with ids and labels,
its default, its message); the form then draws a list where it draws a number field. It is not
built now.

## 3. Where it starts: New job in the picker

The Job file picker ([03 §3](03-screens.md#file-picker)) gets a third card beside Controller SD
and Display SD:

```
+--------------------------------------------------------------------------------+
| [Controller SD] [Display SD] [New job]                                [Close]  |
|--------------------------------------------------------------------------------|
| Surfacing                                                                      |
| Probes the surface, then skims the area in one pass with an inward clockwise   |
| spiral, so the cut is conventional.                                            |
| ...                                              (one row per generator)       |
+--------------------------------------------------------------------------------+
```

- One row per generator: its title and its description (up to two lines), both from the library. A tap
  opens its form (§4) over the picker.
- **Greyed out while a job runs or is held** (S3: making a job is a file action), with the
  reason under the card: "Not while a job runs."
- No display SD card: the rows are greyed out with "A new job is written to the display SD
  first. Insert a card." (the job is written there, §5).

## 4. The form

Opens over the screen's content (800 x 360); the status bar stays visible.

```
+--------------------------------------------------------------------------------+
| New Surfacing Job                                                      [Close] |
| Bit: 1/2in 14° dovetail (Freud 22-104), 12.7 mm at the tip                      |
| Zero X/Y at the area's lower-left corner; the bit hangs 6.35 mm past every edge.|
|--------------------------------------------------------------------------------|
| Width (X) [ 300      ] mm    Height (Y) [ 200      ] mm   Depth [ 0.1    ] mm   |
|                              Between 1 and 3000 mm                              |
| Stepover and feeds                                                             |
| Stepover [ 40 ] % of bit  Feed [ 1200 ] mm/min  Plunge [ 300 ] mm/min           |
| Spindle  [ 18000 ] rpm                                                         |
|--------------------------------------------------------------------------------|
| 300 × 200 mm at 0.1 mm · 21 laps, 12.5 m of cutting, about 10 min              |
|                                                   [Defaults]    [ Create > ]   |
+--------------------------------------------------------------------------------+
```

| Element | Behaviour |
|---------|-----------|
| Title, bit line, set-up line | From the library (`surfacingForm()`). The bit line names what to fit before the job's first prompt |
| Fields | In the library's order: the main group, then the advanced group under its title ("Stepover and feeds"). Not folded: the screen has room for both. Each shows its text as typed, its label before and its unit after |
| Editing a field | A tap opens the **text keypad** (§4.1) with the field's text |
| Message | Under its field in red, from the library (`Enter a number`, `Between 1 and 3000 mm`), checked whenever a field changes |
| Summary | The library's summary for the current values, recomputed when a field changes; "Fix the values marked in red." while any field has a message |
| Defaults | Puts every field back to its default text. No confirm: the kept values (§6) change only on Create |
| Create | Greyed out while any field has a message. Runs §5 |
| Close | Back to the picker; the typed values are kept for the session but not saved |

### 4.1 The text keypad

The Settings keypad (`Entry::number`, D56) returns a number and refuses values out of range.
A generator's form must keep each field's **text as typed** and let the library judge it
(CNC-Tools 15 §20.3, its D70: half-typed text such as `.` or `-` is `Enter a number` there
too). So the form uses a variant, `Entry::decimal`: the same keypad (digits, `.`, `-`,
backspace, Cancel, OK) with the field's limits shown under the value for information, which
returns the text unchanged on OK and refuses nothing. Its text is at most 32 characters.

## 5. Create

1. The values are kept (§6).
2. The working folder `/lrt/work/` is cleared, as for any new job (02 §5), and the job is
   written there under the library's name (`surface-300x200-0.1mm.nc`) by the library, on the
   worker task (02 §7). It takes 0.28 s for the largest surfacing job on the device (06; 3000 x
   3000 mm at 10%, 105 KB). A write that fails deletes the partial file and shows the reason on the form.
3. The file becomes the chosen job, as one chosen from the display SD: Job's file line shows
   `surface-300x200-0.1mm.nc  7 KB  New job · Surfacing`, and the analysis with the outline
   runs (D75, D77), so the preview, the info line, the transforms and the Outline Check work on
   it as on any job.
4. **Use this job** uploads it to `/sd/fluidtouch/uploads/` (D53), with the overwrite confirm
   if that name is there, and offers it to run once the controller lists it at its size (S6),
   then Run's start panel. A transform of it is uploaded under its new name, as for any job.

The job's own prompts then guide the cut: for surfacing, insert the bit and attach the probe,
the probe, remove the probe and start the router (CNC-Tools 14 §4), each a Prompt on Run (D52).

The generated file is not kept anywhere else on the display SD: the working folder is cleared
when another job is chosen, and the same job is made again from the kept values, or chosen
from the controller's uploads folder (D78).

## 6. The values kept

- Each generator's last values are kept as the **texts** typed, not as numbers (CNC-Tools 15
  §20.3: the display has no JS number-to-text), in one file on the display SD,
  `/lrt/new-job-values.txt`, written on Create. One line per field: `<generator>.<field>=<text>`
  (`surfacing.width=300`).
- Not in the settings flash: the working screens never write flash, because a flash write
  flickers the panel (D44). The SD card doesn't.
- A field missing from the file, or a file that can't be read, takes the library's default
  text; a line for a field the generator no longer has is ignored. So a library release that
  adds or removes a field needs nothing here.
- The form opens with the kept texts, or the defaults when there are none.

## 7. Services

**`Generators`** (`src/services/generators.*`): the list of generators and the common
interface the form and JobService use. Per generator, an adapter gives:

| Call | Does |
|------|------|
| `id()`, `title()`, `description()`, `setup()`, `toolLine()`, `advancedTitle()` | The texts of §3 and §4 |
| `fieldCount()`, `field(i)` | Label, unit as shown, group, limits text, default text |
| `messages(texts, out)` | Each field's message for the texts as typed (through the library's field-value and error calls) |
| `plan(texts, summary, name)` | The summary and file name, or false while a field has a message |
| `write(texts, path)` | The job into a file through the SD writer, with a 4 KB arena of its own (CNC-Tools 15 §20.4: the selected job's analysis holds the main one) |

**JobService** gains `generate(generator, texts)`: the steps of §5 on the worker task, then the
usual `Analysing -> Ready` (02 §4). Its source is "New job · <title>".

## 8. Memory and time

Nothing new held: a form is a few labels and 32-byte texts; the write takes 4 KB while it
runs. The summary is the library's plan loop without writing (at most about 4,700 points for
surfacing, 45 ms on the device for the largest job), recomputed only when a field's keypad is closed, not per key.

## 9. Testing

- **Library:** the release's device suite `surfacing` group passes on the display (06), which
  checks every field text, message, file name, summary and file against the editor's.
- **Simulator:** the form from the library's data; every message case typed on the text keypad
  (`.`, `-`, empty, out of range); Defaults; Create to the preview with the analysis and the
  outline; the kept values after a restart; no card; greyed out during a job (fake FluidNC);
  upload and the start panel.
- **On the device:** a surfacing job made on the display is **byte-identical** to the editor's
  for the same field texts; it is cut on the machine through its probe and prompts.
