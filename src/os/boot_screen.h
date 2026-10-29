#pragma once

// Boot screen: logo tile, version, a 4-step progress bar and the current stage. Drives LVGL directly,
// so call it from setup() before the UI task starts.
void bootScreenStart();
void bootScreenStage(int step, const char *text);  // step 0..4 sets the bar to step/4
void bootScreenEnd();                               // shows "Ready", then clears the screen
