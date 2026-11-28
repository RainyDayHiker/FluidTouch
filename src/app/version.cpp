#include "app/version.h"

// Written by tools/lrt_version.py into the build folder; only this file includes it, so a
// new commit rebuilds only this file
#if __has_include("lrt_version.h")
#include "lrt_version.h"
#endif

#ifndef LRT_VERSION
#define LRT_VERSION "unknown"
#define LRT_DETAIL ""
#define LRT_BUILT __DATE__
#endif

const char *Version::name() { return LRT_VERSION; }
const char *Version::detail() { return LRT_DETAIL; }
const char *Version::built() { return LRT_BUILT; }
