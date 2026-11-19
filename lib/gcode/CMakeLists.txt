# gcode-core: the C++17 port of @gcode-editor/core (specs/15). The library references only
# include/ and src/; tests and docs are added when this is the top-level project, so a consumer
# can add_subdirectory() the release archive's library/ folder, which holds neither (15 §4.1).
cmake_minimum_required(VERSION 3.21)
project(gcode_core LANGUAGES CXX)

option(GCODE_CORE_WERROR "Treat warnings as errors (CI)" OFF)

if(MSVC)
  # CMake's default /EHsc would fight the library's /EHs-c-; the test target adds /EHsc itself.
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

file(GLOB GCODE_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(gcode_core STATIC ${GCODE_CORE_SOURCES})
target_include_directories(gcode_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(gcode_core PUBLIC cxx_std_17)
set_target_properties(gcode_core PROPERTIES CXX_EXTENSIONS OFF)

# The flags ship with the library, for its own sources only (15 §4.1): no exceptions, no RTTI,
# and no contracted floating point (a fused multiply-add rounds once where TS rounds twice).
if(MSVC)
  target_compile_options(gcode_core PRIVATE /W4 /EHs-c- /GR- /fp:precise /utf-8)
  target_compile_definitions(gcode_core PRIVATE _HAS_EXCEPTIONS=0)
  if(GCODE_CORE_WERROR)
    target_compile_options(gcode_core PRIVATE /WX)
  endif()
else()
  target_compile_options(gcode_core PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti -ffp-contract=off)
  if(GCODE_CORE_WERROR)
    target_compile_options(gcode_core PRIVATE -Werror)
  endif()
endif()

# Tests only in the repo, as the top-level project: the archive's library/ folder has no test/.
if(PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  enable_testing()
  add_subdirectory(test)
endif()
