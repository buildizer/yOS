#pragma once
#include <stdint.h>

bool touchStart();
// Whether a finger is down, with its latest position.
bool touchRead(int16_t &x, int16_t &y);
