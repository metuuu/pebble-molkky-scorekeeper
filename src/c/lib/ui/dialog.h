#pragma once
#include <pebble.h>
#include "button.h"

// =============================================================================
// dialog — a modal prompt window: an optional bold title, an optional body
// paragraph, and a vertical stack of themed buttons. The app's reusable "are you
// sure?" / "pick one" primitive, built on button_group. Buttons are driven by the
// physical Up/Down focus ring + Select only — touch is deliberately disabled so a
// stray tap can't fire a destructive action, and nothing is focused until the
// user steps the ring (the first Select after open is a no-op).
//
// Strings are copied, so callers may pass stack/temporary buffers. The dialog
// dismisses itself when a button fires and only THEN invokes that button's
// on_click — so the handler runs with the dialog already gone and is free to push
// or pop further windows. Back dismisses with no action (like a leading Cancel).
// =============================================================================

#define DIALOG_MAX_BUTTONS 3

typedef struct {
  const char    *label;                 // button text (required)
  UiButtonScheme scheme;                // PRIMARY (default 0) / DANGER / NEUTRAL
  void         (*on_click)(void *ctx);  // NULL → just dismiss the dialog
} DialogButton;

typedef struct {
  const char  *title;                   // bold heading (NULL/"" → omitted)
  const char  *text;                    // body paragraph (NULL/"" → omitted)
  DialogButton buttons[DIALOG_MAX_BUTTONS];
  uint8_t      button_count;            // 1 .. DIALOG_MAX_BUTTONS
  void        *ctx;                     // passed to every on_click
} DialogConfig;

// Push a modal dialog. Returns its window (rarely needed — the dialog owns its
// own lifecycle and frees itself when popped).
Window *dialog_push(DialogConfig cfg);

// Convenience: a two-button confirm. The confirm button sits on top in
// `confirm_scheme` (pass UI_BTN_DANGER for a destructive action — its focused look
// goes solid-red), with a dismiss-only "Cancel" (NEUTRAL) below it. `confirm_label`
// defaults to "OK" when NULL.
Window *dialog_confirm_push(const char *title, const char *text,
                            const char *confirm_label, UiButtonScheme confirm_scheme,
                            void (*on_confirm)(void *ctx), void *ctx);
