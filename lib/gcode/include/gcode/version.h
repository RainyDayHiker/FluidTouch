// The library's version: the editor's (specs/15 §9.1). Written by tools/sync-version.mjs from
// the root package.json, and stamped with the build number on main builds. Don't edit by hand.
#pragma once

#define GCODE_CORE_VERSION "0.3.0-build.87"

namespace gcode {

/** The version this library was built as, e.g. "0.0.1-build.57" on a main build. */
const char* version();

}  // namespace gcode
