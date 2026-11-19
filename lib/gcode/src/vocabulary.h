// Recognized vocabulary (packages/core/src/parse/vocabulary.ts and sim/simulator.ts's code
// tables), by code key (gcode::codeKey). Internal to the library.
#pragma once

#include <cstdint>

namespace gcode {
namespace vocab {

/** Index of a recognized G-code in G_CODE_ORDER (the bit in a line's G masks), or -1. */
int gIndex(int key);
/** Index of a recognized M-code in M_CODE_ORDER, or -1. */
int mIndex(int key);
/** The modal group bit of a recognized G-code (vocabulary.ts GROUP_BIT), or 0. */
uint32_t groupBit(int key);
/** Whether `letter` (upper case) is a recognized address letter. */
bool letterRecognized(char letter);

}  // namespace vocab
}  // namespace gcode
