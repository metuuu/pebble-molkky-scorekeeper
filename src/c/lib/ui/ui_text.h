#pragma once
#include <pebble.h>

// =============================================================================
// ui_text — a font bundled with the metrics needed to place it. graphics_draw_text
// is top-aligned and the GOTHIC fonts pad above the caps, so vertically centering
// text needs two per-font numbers: the cap-box height to center on and a small
// optical nudge upward. Carrying those *with* the font means any component handed
// a UiFont centers correctly with zero magic numbers at the call site — which is
// also why a "font size" is just a UiFont (see the size presets below).
// =============================================================================

typedef struct {
  const char *key;    // a FONT_KEY_* system font
  int16_t     cap;    // cap-box height to vertically center on
  int16_t     nudge;  // optical upward correction (GOTHIC pads above the caps)
} UiFont;

// Presets for the system fonts the app uses. Numbers match the corrections that
// were previously copy-pasted across draw_centered / list_item / the t9 keyboard.
extern const UiFont UI_FONT_TITLE;        // GOTHIC_24_BOLD
extern const UiFont UI_FONT_BODY;         // GOTHIC_18
extern const UiFont UI_FONT_BODY_BOLD;    // GOTHIC_18_BOLD
extern const UiFont UI_FONT_CAPTION;      // GOTHIC_14
extern const UiFont UI_FONT_CAPTION_BOLD; // GOTHIC_14_BOLD (section headers)

GFont   ui_font(UiFont f);
int16_t ui_font_cap(UiFont f);          // cap-box height (a row's natural text height)

// Draw `text` in `box`. With `vcenter`, the text is vertically centered in box
// via the font's metrics; otherwise it is top-aligned at box.origin.y.
void ui_text_draw(GContext *ctx, const char *text, UiFont f, GRect box,
                  GTextAlignment halign, bool vcenter, GTextOverflowMode overflow);
