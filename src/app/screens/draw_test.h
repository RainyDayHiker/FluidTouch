#ifndef LRT_DRAW_TEST_H
#define LRT_DRAW_TEST_H

#include <lvgl.h>

// Test screen for spike S2 (specs/06-test-and-spikes.md): how much PSRAM is
// free, and how fast can a job the size of the reference job be drawn?
// Throwaway code: it answers the question and is removed when the real
// preview exists (M5).
namespace DrawTest {

// Opens the test over parent (the Settings content area). Back closes it and
// frees its buffers.
void open(lv_obj_t *parent);

}  // namespace DrawTest

#endif // LRT_DRAW_TEST_H
