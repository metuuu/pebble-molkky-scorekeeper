#pragma once
#include <pebble.h>
#include "button.h"

// =============================================================================
// button_group — an interactive container for ui buttons that unifies the two
// PebbleOS input models behind ONE set of callbacks:
//
//   • Touch (Pebble Time 2 / touch watches): tap a button, hold it, or slide
//     off before releasing to cancel. The group owns the single global touch
//     subscription and hit-tests every event, so N independent buttons fall out
//     of the one screen-wide service.
//   • Physical buttons (every watch): Up/Down move a focus ring through the
//     buttons and Select activates the focused one — the same focus+Select
//     idiom the pager and footer already use. It is the fallback on watches
//     without touch, and runs alongside touch on those that have it.
//
// Both paths drive the SAME group-level handlers, keyed by each button's caller
// `id`, so an app writes its actions once. Buttons render with ui_button_draw
// (their `look`); the focused/pressed button is drawn with an emphasis style
// (active_style/active_scheme, default SOLID·PRIMARY — the lib's focus look).
//
// Per-button behavior (mutually exclusive):
//   UI_BTN_TAP     on_click on release-in-bounds / focused Select
//   UI_BTN_HOLD    as TAP, plus on_long_press when held past the hold threshold
//                  (a short hold still clicks)
//   UI_BTN_REPEAT  on_click on press, then again every `repeat_ms` until release
//                  (no long-press — a hold means "repeat")
//
// on_click / on_long_press are dispatched on a zero-delay timer, so navigating
// (pop/push windows) inside them is safe — mirroring the throw grid's defer.
// on_press / on_release fire immediately; use them for feedback (haptics, a
// sound), NOT for navigation or for destroying the group.
//
// Lifecycle: ui_button_group_create() against a Window installs that window's
// click config and (if the watch has touch) subscribes to it; add buttons; then
// ui_button_group_destroy() on unload. To compose inside a screen that already
// owns input, create against a parent Layer and feed events with handle_*.
//
// Touch is a single global service: the group assumes it is the foreground
// interactive element (one active group at a time). That matches every screen
// in this app; nested/stacked groups sharing touch are out of scope.
// =============================================================================

#define UI_BTN_GROUP_MAX 16   // buttons per group (the throw grid uses 13)

typedef enum {
  UI_BTN_TAP,
  UI_BTN_HOLD,
  UI_BTN_REPEAT,
} UiButtonBehavior;

typedef struct {
  int              id;            // identifier passed back to the handlers
  GRect            frame;         // hit region + draw box (group-layer coords)
  UiButtonSpec     look;          // base appearance (see button.h)
  UiButtonStyle    active_style;  // emphasis when focused/pressed (default SOLID)
  UiButtonScheme   active_scheme; // emphasis scheme             (default PRIMARY)
  GColor           active_fill;   // non-clear → override the SOLID fill while
                                  // focused/pressed (e.g. a danger key darkening
                                  // to ui_danger_darker instead of flipping hue);
                                  // clear (default) → the active scheme's fill
  GColor           active_ink;    // non-clear → override the ink while focused/
                                  // pressed; pair with active_fill when the
                                  // emphasis fill needs an ink the scheme won't
                                  // resolve (e.g. a dark neutral focus wanting
                                  // light text); clear (default) → scheme's ink
  UiButtonBehavior behavior;      // TAP (default) / HOLD / REPEAT
  uint16_t         repeat_ms;     // REPEAT interval (0 => a sane default)
  bool             no_touch;      // exclude from touch hit-testing
  bool             no_focus;      // exclude from the physical focus ring
} UiButton;

typedef struct {
  void (*on_click)(int id, void *ctx);       // tap / focused Select / repeat tick
  void (*on_long_press)(int id, void *ctx);  // HOLD behavior only
  void (*on_press)(int id, void *ctx);       // immediate: a press began (feedback)
  void (*on_release)(int id, void *ctx);     // immediate: a press ended (feedback)
} UiButtonGroupHandlers;

typedef struct UiButtonGroup UiButtonGroup;

// Create against a window: the group installs the window's click config (Up/Down
// focus, Select activate) and subscribes to touch when the watch has it.
UiButtonGroup *ui_button_group_create(Window *window, GRect frame,
                                      UiButtonGroupHandlers handlers, void *ctx);

// Create against a parent layer for composition — no window/touch wiring; feed
// it events yourself with the handle_* functions below.
UiButtonGroup *ui_button_group_create_in_layer(Layer *parent, GRect frame,
                                               UiButtonGroupHandlers handlers, void *ctx);

// Register a button (copied); returns its index, or -1 if the group is full.
int    ui_button_group_add(UiButtonGroup *g, UiButton btn);

Layer *ui_button_group_get_layer(UiButtonGroup *g);
void   ui_button_group_destroy(UiButtonGroup *g);

// ---- fed mode (composition; a host owns the touch service / click config) ----
// Forward a touch event. Coordinates are screen-space (TouchEvent.x/y); the
// group maps them into its own layer.
void   ui_button_group_handle_touch(UiButtonGroup *g, const TouchEvent *event);
void   ui_button_group_focus_prev(UiButtonGroup *g);   // Up
void   ui_button_group_focus_next(UiButtonGroup *g);   // Down
void   ui_button_group_press_focused(UiButtonGroup *g);    // Select down
void   ui_button_group_release_focused(UiButtonGroup *g);  // Select up
