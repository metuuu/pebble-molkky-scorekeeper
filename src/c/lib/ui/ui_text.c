#include "ui_text.h"

// cap / nudge: the cap-box height and optical correction per font. TITLE 28/3
// and BODY 22/2 are the values draw_centered and the multitap keyboard arrived at;
// the rest follow the same cap ≈ size + 4 pattern (Roboto Condensed runs a touch
// tighter). See ui_text.h on what's deliberately omitted.
const UiFont UI_FONT_HEADER       = { FONT_KEY_GOTHIC_28_BOLD, 32, 3 };
const UiFont UI_FONT_TITLE        = { FONT_KEY_GOTHIC_24_BOLD, 28, 3 };
const UiFont UI_FONT_BODY_LARGE   = { FONT_KEY_GOTHIC_24,      28, 3 };
const UiFont UI_FONT_BODY         = { FONT_KEY_GOTHIC_18,      22, 2 };
const UiFont UI_FONT_BODY_BOLD    = { FONT_KEY_GOTHIC_18_BOLD, 22, 2 };
const UiFont UI_FONT_CAPTION      = { FONT_KEY_GOTHIC_14,      18, 2 };
const UiFont UI_FONT_CAPTION_BOLD = { FONT_KEY_GOTHIC_14_BOLD, 18, 2 };
const UiFont UI_FONT_MICRO        = { FONT_KEY_GOTHIC_09,      13, 1 };

GFont   ui_font(UiFont f)     { return fonts_get_system_font(f.key); }
int16_t ui_font_cap(UiFont f) { return f.cap; }

void ui_text_draw(GContext *ctx, const char *text, UiFont f, GRect box,
                  GTextAlignment halign, bool vcenter, GTextOverflowMode overflow) {
  GRect r = box;
  if (vcenter) {
    r.origin.y = box.origin.y + (box.size.h - f.cap) / 2 - f.nudge;
    r.size.h   = f.cap;
  }
  graphics_draw_text(ctx, text, ui_font(f), r, overflow, halign, NULL);
}
