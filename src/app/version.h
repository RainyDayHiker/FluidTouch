#ifndef LRT_VERSION_INFO_H
#define LRT_VERSION_INFO_H

// LowRider Touch's version, from git at build time (tools/lrt_version.py, specs/09 section 6,
// D104). Shown on the splash and in Settings > Tools > About.
namespace Version {

// "lrt-m5", "lrt-m5+3" (three commits after the tag), "untagged", or "unknown" without git
const char *name();

// "af78053", with ", <branch>" off lrt/main and ", modified" with local changes
const char *detail();

// The build's date, "2026-11-27"
const char *built();

}  // namespace Version

#endif // LRT_VERSION_INFO_H
