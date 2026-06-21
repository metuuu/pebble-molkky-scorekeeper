#pragma once
#include <pebble.h>
#include "t9_keyboard.h"

// Load persisted settings (or defaults) and apply them to the keyboard.
// Call once at startup, after the keyboard is created.
void settings_load_and_apply(T9Keyboard *kb);

// Push the on-watch settings menu. Wire this to a long-press of the Up button.
// Changes are applied to `kb` live and persisted; Back returns to the keyboard.
void settings_window_push(T9Keyboard *kb);
