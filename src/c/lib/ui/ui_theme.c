#include "ui_theme.h"

// Default = the stock MenuLayer look (black ink on white, black highlight), so
// an app that never calls ui_theme_set() looks exactly as before. Each GColor is
// built from its ARGB8 so this is a constant initializer (the GColor* macros are
// compound literals and can't sit in a static initializer).
static UiTheme s_theme = {
  .background   = { .argb = GColorWhiteARGB8 },
  .text         = { .argb = GColorBlackARGB8 },
  .text_muted   = { .argb = GColorDarkGrayARGB8 },
  .neutral      = { .argb = GColorLightGrayARGB8 },
  .accent       = { .argb = GColorBlackARGB8 },
  .accent_text  = { .argb = GColorWhiteARGB8 },
  .danger       = { .argb = GColorRedARGB8 },
  .danger_light = { .argb = GColorMelonARGB8 },
};

void    ui_theme_set(UiTheme theme) { s_theme = theme; }
UiTheme ui_theme_get(void)          { return s_theme; }
GColor  ui_background(void)         { return s_theme.background; }
GColor  ui_text(void)              { return s_theme.text; }
GColor  ui_text_muted(void)        { return s_theme.text_muted; }
GColor  ui_neutral(void)           { return s_theme.neutral; }
GColor  ui_accent(void)             { return s_theme.accent; }
GColor  ui_accent_text(void)        { return s_theme.accent_text; }
GColor  ui_danger(void)            { return s_theme.danger; }
GColor  ui_danger_light(void)      { return s_theme.danger_light; }
