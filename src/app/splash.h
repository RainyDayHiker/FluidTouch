#ifndef LRT_SPLASH_H
#define LRT_SPLASH_H

// The boot splash (specs/03-screens.md section 1, D104): the FluidNC logo, the name and the
// version, over the whole screen. Drawn first thing, so the screen isn't blank while the
// frame is built and WiFi and FluidNC are reached behind it; gone after about 2 s or at a tap.
namespace Splash {

// Draws it at once (call before anything slow)
void show();

// Removes it once its time is up; call from the main loop
void poll();

}  // namespace Splash

#endif // LRT_SPLASH_H
