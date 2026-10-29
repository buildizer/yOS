#pragma once
#include <stdint.h>

// Panel, LVGL driver, touch input and backlight for the Guition ESP32-4848S040.
enum SystemGesture { GESTURE_NONE, GESTURE_HOME, GESTURE_BACK };

namespace hal {
void begin();
// LVGL runs in its own task; anything else that touches the UI must hold this lock.
void setRotation(uint8_t r);
void backlightOnNow();          // full brightness at once (boot: the UI loop that fades it isn't running yet)   // 0..3 quarter turns, applied live (software rotation)
void lock();
void unlock();
void loop();                       // runs LVGL timers and backlight fading
SystemGesture takeGesture();       // edge swipes detected since the last call
void wake();                       // full brightness now, reset idle timer
void setAlarm(bool on);            // keep the screen on while something is down
bool screenAsleep();
uint16_t *framebuffer();
// Developer tool: drive the backlight PWM directly (freq Hz, duty 0-100 %); freq 0 returns control to the OS.
void testBacklight(uint32_t freq, float dutyPct);
// Frame statistics since the last call: frames drawn, average and worst render+flush time, and wall time.
struct Perf { uint32_t frames, avgMs, maxMs, windowMs, flushUs; };
Perf takePerf();           // RGB565, 480x480
uint32_t currentFps();       // refreshes in the last second (non-destructive)
float refreshHz();           // panel refresh rate from the pixel clock
// Developer tool: replay a touch from (x1,y1) to (x2,y2) over ms (a tap when the points match).
void injectTouch(int x1, int y1, int x2, int y2, uint32_t ms);
}
