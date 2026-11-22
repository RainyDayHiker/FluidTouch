// New Surfacing Job (specs/14, 15 §20): gen/surfacing.ts and gen/fields.ts. A job that probes the
// surface, then skims a width × height area at a set depth with one inward clockwise spiral. It
// reads no file: the parameters go in, a new file comes out, and the caller then opens that file
// like any other. The form is built from data (surfacingForm(), surfacingField()), so the display
// shows the editor's labels, units, grouping and messages, and the same field text gives the
// same value (fieldValue()) and the same file, byte for byte.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/io.h"

namespace gcode {

/** What the user sets (14 §1): SURFACING_DEFAULTS. Lengths in mm, feeds in mm/min, spindle in rpm. */
struct SurfacingParams {
  double width = 100;     ///< X, from where X/Y is zeroed: the area the bit's centre covers.
  double height = 100;    ///< Y.
  double depth = 0.1;     ///< Below the probed surface, in one pass.
  double stepover = 40;   ///< Percent of the bit's diameter between laps.
  double feed = 1200;     ///< Cutting feed.
  double plunge = 300;    ///< Plunge feed.
  double spindle = 18000; ///< Spindle speed.
};

/** The fields, in the form's order (SURFACING_FIELDS). */
enum class SurfacingField : uint8_t { Width, Height, Depth, Stepover, Feed, Plunge, Spindle };
/** How many fields. */
constexpr int kSurfacingFieldCount = 7;

/** One field of the form (SURFACING_FIELDS, SURFACING_LIMITS, SURFACING_FORM.fields). UTF-8. */
struct SurfacingFieldInfo {
  const char* name;         ///< "width": the field's id.
  const char* label;        ///< "Width (X)".
  const char* fieldUnit;    ///< Shown beside the field: "mm", "% of bit", "mm/min", "rpm".
  bool advanced;            ///< Under the fold (SurfacingForm::advancedTitle), not in the main group.
  double min;               ///< The smallest value allowed (included).
  double max;               ///< The largest (included).
  double defaultValue;      ///< SURFACING_DEFAULTS.
  const char* defaultText;  ///< The default as the field shows it: "100", "0.1".
};

/** A field's data, in the form's order. */
const SurfacingFieldInfo& surfacingField(SurfacingField field);

/** The value of one field of `p`. */
double surfacingValue(const SurfacingParams& p, SurfacingField field);
/** Sets one field of `p`. */
void setSurfacingValue(SurfacingParams& p, SurfacingField field, double value);

/** The form's texts (SURFACING_FORM), finished, UTF-8. */
struct SurfacingForm {
  const char* title;          ///< "New Surfacing Job".
  const char* description;    ///< "Probes the surface, then skims the area in one pass …".
  const char* setup;          ///< "Zero X/Y at the area's lower-left corner; the bit hangs 6.35 mm past every edge."
  const char* bitLine;        ///< "Bit: 1/2in 14° dovetail (Freud 22-104), 12.7 mm at the tip".
  const char* advancedTitle;  ///< "Stepover and feeds": the fold's title.
};
/** The form's texts. */
const SurfacingForm& surfacingForm();

/**
 * A field's text as typed to its value (14 §5, 09 D70): spaces and tabs at either end are ignored,
 * empty is NaN, then decimal text ("12", "12.", ".5", "-0", "+5") is read as the nearest double.
 * Anything else (".", "1e3", "0x10", "Infinity", a no-break space) is NaN, which
 * surfacingErrors() reports as "Enter a number". The rule for every generator's number fields.
 */
double fieldValue(const char* text, size_t length);

/** The bit (DOVETAIL_BIT, 14 §2), for showing which bit the job needs. */
struct SurfacingBit {
  const char* title;        ///< "1/2in 14° dovetail (Freud 22-104)" (UTF-8), as the form shows it.
  const char* label;        ///< "T1 1/2in 14deg dovetail": the probe message and the tool's name.
  const char* description;  ///< "1/2in 14deg dovetail, Freud 22-104": the file's header comment.
  double diameter;          ///< 12.7 mm, at the tip: what the toolpath uses.
  double maxRpm;            ///< 24000: the spindle field's maximum.
};
/** The one bit (15 §20.8 S1). */
const SurfacingBit& surfacingBit();

/**
 * Each field's problem (surfacingErrors): a bit per field with one (1 << field), and its message
 * ("Enter a number", "Between 0.01 and 3 mm") in messages[field], or null where there's none.
 * `messages` may be null. Zero means the job can be made.
 */
uint32_t surfacingErrors(const SurfacingParams& p, const char* messages[kSurfacingFieldCount]);

/** The job's facts, without its text. */
struct SurfacingPlan {
  uint32_t laps;       ///< The spiral's laps, the middle line included.
  double cutLength;    ///< mm, the plunge excluded.
  /** "surface-300x200-0.1mm.nc": at most 36 bytes, terminated. */
  char name[64];
  /** "300 × 200 mm at 0.1 mm · 24 laps, 14.2 m of cutting, about 12 min": UTF-8, at most about 90 bytes, terminated. */
  char summary[160];
  uint32_t nameLength;     ///< Bytes of name.
  uint32_t summaryLength;  ///< Bytes of summary.
};

/**
 * The name and summary without writing, for the form's line as the user types: the same loop as
 * writeSurfacingJob(), so the summary shown is the file's. Allocates nothing. Ok, or
 * InvalidParameters when surfacingErrors() finds anything (`plan` then zeroed).
 */
Status planSurfacing(const SurfacingParams& p, SurfacingPlan& plan);

/**
 * Writes the job (14 §4) through `writer`: ASCII, LF line endings. Its 4 KB write buffer comes from
 * `arena`, which holds nothing else (any arena of 4 KB or more does). Ok; InvalidParameters when
 * surfacingErrors() finds anything, writing nothing; OutOfMemory for an arena under 4 KB, writing
 * nothing; WriteError, which may leave a partial file the caller deletes.
 */
Status writeSurfacingJob(const SurfacingParams& p, ByteWriter& writer, Arena arena, SurfacingPlan& plan);

/** The write buffer writeSurfacingJob() takes from its arena. */
constexpr size_t kSurfacingBufferSize = 4096;

}  // namespace gcode
