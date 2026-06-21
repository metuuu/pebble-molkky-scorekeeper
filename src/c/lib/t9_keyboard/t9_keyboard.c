#include "t9_keyboard.h"
#include "c/lib/ui/button.h"   // 3D key shape (raised at rest, sunk on press)

// ---- Constants --------------------------------------------------------------
#define TEXT_AREA_H  44
#define CARET_BLINK_MS 500
#define CARET_IDLE_MS  4000   // rest the caret solid-on after this long without input
#define FLASH_MS       90
#define GRID_COLS    3
#define GRID_ROWS    4
#define KEY_COUNT    (GRID_COLS * GRID_ROWS)
#define BUFFER_CAP   96
#define LABEL_CAP    24
#define KEY_ELEV     2     // 3D key depth: raised by this many px, sunk on press

#define KEY_SHIFT 9
#define KEY_SPACE 10
#define KEY_DEL   11

typedef enum { PAGE_ALPHA = 0, PAGE_NUMBERS, PAGE_SYMBOLS, PAGE_COUNT } Page;
typedef enum { SHIFT_OFF = 0, SHIFT_ONCE, SHIFT_LOCK } ShiftState;

// Each character key (0..8) cycles through a NULL-terminated glyph list (UTF-8).
static const char *const A0[] = { ".", ",", "?", "!", NULL };
static const char *const A1[] = { "a", "b", "c", NULL };
static const char *const A2[] = { "d", "e", "f", NULL };
static const char *const A3[] = { "g", "h", "i", NULL };
static const char *const A4[] = { "j", "k", "l", NULL };
static const char *const A5[] = { "m", "n", "o", NULL };
static const char *const A6[] = { "p", "q", "r", "s", NULL };
static const char *const A7[] = { "t", "u", "v", NULL };
static const char *const A8[] = { "w", "x", "y", "z", NULL };

static const char *const N0[] = { "1", NULL };  static const char *const N1[] = { "2", NULL };
static const char *const N2[] = { "3", NULL };  static const char *const N3[] = { "4", NULL };
static const char *const N4[] = { "5", NULL };  static const char *const N5[] = { "6", NULL };
static const char *const N6[] = { "7", NULL };  static const char *const N7[] = { "8", NULL };
static const char *const N8[] = { "9", NULL };

static const char *const S0[] = { ".", ",", "?", "!", NULL };
static const char *const S1[] = { ":", ";", "'", "\"", NULL };
static const char *const S2[] = { "@", "#", "$", NULL };
static const char *const S3[] = { "%", "&", "*", NULL };
static const char *const S4[] = { "+", "=", "-", NULL };
static const char *const S5[] = { "_", "/", "\\", NULL };
static const char *const S6[] = { "(", ")", "[", "]", NULL };
static const char *const S7[] = { "{", "}", "<", ">", NULL };
static const char *const S8[] = { "|", "^", "~", "`", NULL };

static const char *const *const PAGE_KEY[PAGE_COUNT][9] = {
  { A0, A1, A2, A3, A4, A5, A6, A7, A8 },
  { N0, N1, N2, N3, N4, N5, N6, N7, N8 },
  { S0, S1, S2, S3, S4, S5, S6, S7, S8 },
};
static const char *const PAGE_IND[PAGE_COUNT] = { "ABC", "123", "#+=" };

// Toggleable extended (non-ASCII) characters. Each is appended to a base key's
// cycle when enabled, and hidden from the key labels. All enabled by default.
// To extend the set, add entries here (glyph + which alpha key 0..8 it joins)
// and the case mapping in prv_shifted(), then bump EXT_COUNT.
typedef struct { const char *glyph; int key; } ExtCharDef;
#define EXT_COUNT 5
#define MAX_EFF   16
static const ExtCharDef EXT_DEFS[EXT_COUNT] = {
  { "å", 1 }, { "ä", 1 }, { "æ", 1 }, { "ö", 5 }, { "ø", 5 },
};

// Keyboard color themes. Each holds ARGB8 values (built into GColor at draw
// time): screen background, foreground (text, divider, labels), the soft resting
// fill of a letter key, the accent fill for active/pending keys and the function
// row, the text color drawn on that accent, and a danger pair — the soft fill of
// the DEL key plus the ink its delete glyph is tinted with. Modeled on the throw
// grid (the turn-result screen): a field of soft filled keys plus one bold accent
// strip, with the destructive DEL key set apart in danger tones.
typedef struct {
  const char *name;
  uint8_t bg, fg, key, accent, accent_text, danger, danger_light;
} Theme;

static const Theme THEMES[] = {
  { "Light", GColorWhiteARGB8,      GColorBlackARGB8, GColorLightGrayARGB8, GColorBlackARGB8,        GColorWhiteARGB8, GColorRedARGB8,  GColorMelonARGB8 },
  { "Dark",  GColorBlackARGB8,      GColorWhiteARGB8, GColorDarkGrayARGB8,  GColorWhiteARGB8,        GColorBlackARGB8, GColorRedARGB8,  GColorBulgarianRoseARGB8 },
  { "Ocean", GColorOxfordBlueARGB8, GColorWhiteARGB8, GColorDukeBlueARGB8,  GColorVividCeruleanARGB8, GColorBlackARGB8, GColorMelonARGB8, GColorDarkCandyAppleRedARGB8 },
  { "Mint",  GColorWhiteARGB8,      GColorBlackARGB8, GColorLightGrayARGB8, GColorIslamicGreenARGB8, GColorWhiteARGB8, GColorRedARGB8,  GColorMelonARGB8 },
  { "Amber", GColorBlackARGB8,      GColorWhiteARGB8, GColorDarkGrayARGB8,  GColorChromeYellowARGB8, GColorBlackARGB8, GColorRedARGB8,  GColorBulgarianRoseARGB8 },
};
#define THEME_COUNT (int)(sizeof(THEMES) / sizeof(THEMES[0]))

// Optional app-supplied theme: a single shared slot, registered at runtime via
// t9_keyboard_set_app_theme() and addressed as theme index THEME_COUNT (one
// past the built-ins). Lets a host app brand the keyboard without editing this
// lib. The public T9Theme uses GColor; we fold each into its ARGB8 here so the
// draw path can treat it exactly like a built-in.
static Theme s_app_theme;
static bool  s_has_app_theme;

struct T9Keyboard {
  Layer *layer;
  T9KeyboardDoneHandler done_handler;
  void *context;

  char buffer[BUFFER_CAP];
  int  max_len;             // byte cap on buffer (0 = only BUFFER_CAP limits)
  int  page;
  int  shift;               // ShiftState (explicit, user-driven)

  bool pending_active;
  int  pending_key;
  int  cycle_index;
  AppTimer *commit_timer;

  T9Settings settings;
  bool autocap_armed;       // derived: are we at a sentence start?
  bool autocap_suppressed;  // user pressed Shift to cancel the auto-capital
  bool ext_enabled[EXT_COUNT];
  int  theme;

  bool caret_on;            // blinking caret visibility
  int  caret_idle;          // ms since last edit; blink rests on past CARET_IDLE_MS
  AppTimer *caret_timer;
  int  flash_key;           // cell to briefly highlight on tap (-1 = none)
  AppTimer *flash_timer;

  GBitmap *icon_shift[3];   // indexed by ShiftState: off / once / lock
  GBitmap *icon_del;        // delete glyph drawn on the DEL key
};

// ---- Case helpers -----------------------------------------------------------

static const char *prv_shifted(const char *g) {
  if (g[0] >= 'a' && g[0] <= 'z' && g[1] == '\0') {
    static char up[2]; up[0] = (char)(g[0] - 32); up[1] = '\0'; return up;
  }
  if (strcmp(g, "å") == 0) return "Å";
  if (strcmp(g, "ä") == 0) return "Ä";
  if (strcmp(g, "ö") == 0) return "Ö";
  if (strcmp(g, "æ") == 0) return "Æ";
  if (strcmp(g, "ø") == 0) return "Ø";
  return g;
}
static bool prv_has_case(const char *g) { return strcmp(prv_shifted(g), g) != 0; }

// Shift state actually in effect = explicit shift, or auto-cap when armed.
static int prv_effective_shift(T9Keyboard *kb) {
  if (kb->shift == SHIFT_LOCK) return SHIFT_LOCK;
  if (kb->shift == SHIFT_ONCE) return SHIFT_ONCE;
  if (kb->settings.auto_caps && kb->autocap_armed && !kb->autocap_suppressed)
    return SHIFT_ONCE;
  return SHIFT_OFF;
}

static void prv_render_glyph(T9Keyboard *kb, const char *g, char *out, size_t cap) {
  const char *src = (prv_effective_shift(kb) != SHIFT_OFF) ? prv_shifted(g) : g;
  strncpy(out, src, cap - 1); out[cap - 1] = '\0';
}

// Effective glyph list for (page,key): base glyphs plus any enabled extended
// characters assigned to this key (ALPHA only). Writes pointers into `out`.
static int prv_eff_glyphs(T9Keyboard *kb, int page, int key,
                          const char **out, int cap) {
  int n = 0;
  const char *const *base = PAGE_KEY[page][key];
  for (int j = 0; base[j] && n < cap; j++) out[n++] = base[j];
  if (page == PAGE_ALPHA) {
    for (int e = 0; e < EXT_COUNT && n < cap; e++) {
      if (kb->ext_enabled[e] && EXT_DEFS[e].key == key) out[n++] = EXT_DEFS[e].glyph;
    }
  }
  return n;
}

// ---- Buffer + sentence helpers ----------------------------------------------

static void prv_recompute_autocap(T9Keyboard *kb);   // defined below

static void prv_append(T9Keyboard *kb, const char *s) {
  size_t l = strlen(kb->buffer), sl = strlen(s);
  if (l + sl >= BUFFER_CAP) return;
  if (kb->max_len > 0 && l + sl > (size_t)kb->max_len) return;  // would overflow cap
  memcpy(kb->buffer + l, s, sl + 1);
}

static void prv_utf8_backspace(T9Keyboard *kb) {
  size_t len = strlen(kb->buffer);
  if (len == 0) return;
  size_t i = len - 1;
  while (i > 0 && ((unsigned char)kb->buffer[i] & 0xC0) == 0x80) i--;
  kb->buffer[i] = '\0';
}

// Delete a whole word: trailing whitespace, then the run of non-whitespace.
static void prv_delete_word(T9Keyboard *kb) {
  int i = (int)strlen(kb->buffer);
  while (i > 0 && (kb->buffer[i - 1] == ' ' || kb->buffer[i - 1] == '\n')) i--;
  while (i > 0 && kb->buffer[i - 1] != ' ' && kb->buffer[i - 1] != '\n') i--;
  kb->buffer[i] = '\0';
}

static void prv_do_delete(T9Keyboard *kb) {
  if (kb->pending_active) { kb->pending_active = false; return; }
  if (kb->settings.delete_mode == 1) prv_delete_word(kb);
  else prv_utf8_backspace(kb);
  prv_recompute_autocap(kb);
}

// True when the cursor sits at the start of a new sentence.
static bool prv_at_sentence_start(const char *buf) {
  int len = (int)strlen(buf);
  if (len == 0) return true;
  if (buf[len - 1] == '\n') return true;        // start of a fresh line
  if (buf[len - 1] != ' ') return false;        // mid-word, not a fresh start
  int i = len - 1;
  while (i >= 0 && buf[i] == ' ') i--;
  if (i < 0) return true;                        // only spaces so far
  char c = buf[i];
  return (c == '.' || c == '!' || c == '?' || c == '\n');
}

// Recompute auto-cap arming from the buffer; clear any one-letter suppression.
static void prv_recompute_autocap(T9Keyboard *kb) {
  kb->autocap_armed = prv_at_sentence_start(kb->buffer);
  kb->autocap_suppressed = false;
}

// ---- Commit / multi-tap -----------------------------------------------------

static void prv_emit_glyph(T9Keyboard *kb, const char *g) {
  char out[8];
  prv_render_glyph(kb, g, out, sizeof(out));
  bool cased = prv_has_case(g);
  prv_append(kb, out);
  if (cased && kb->shift == SHIFT_ONCE) kb->shift = SHIFT_OFF;  // spend explicit once
  prv_recompute_autocap(kb);
}

static void prv_commit_pending(T9Keyboard *kb) {
  if (!kb->pending_active) return;
  const char *eff[MAX_EFF];
  int n = prv_eff_glyphs(kb, kb->page, kb->pending_key, eff, MAX_EFF);
  if (kb->cycle_index >= n) kb->cycle_index = 0;
  kb->pending_active = false;
  if (n > 0) prv_emit_glyph(kb, eff[kb->cycle_index]);
}

static void prv_commit_timer_cb(void *data) {
  T9Keyboard *kb = (T9Keyboard *)data;
  kb->commit_timer = NULL;
  prv_commit_pending(kb);
  layer_mark_dirty(kb->layer);
}

static void prv_arm_commit_timer(T9Keyboard *kb) {
  int ms = kb->settings.commit_timeout_ms;
  if (ms < 150) ms = 150;
  if (kb->commit_timer) {
    if (app_timer_reschedule(kb->commit_timer, ms)) return;
    kb->commit_timer = NULL;
  }
  kb->commit_timer = app_timer_register(ms, prv_commit_timer_cb, kb);
}

static void prv_cycle_page(T9Keyboard *kb) {
  prv_commit_pending(kb);
  kb->page = (kb->page + 1) % PAGE_COUNT;
  layer_mark_dirty(kb->layer);
}

// ---- Feedback (haptics, blinking caret, tap flash) --------------------------

static void prv_haptic(T9Keyboard *kb) {
  if (!kb->settings.haptics || kb->settings.haptic_ms <= 0) return;
  // One short custom pulse, length set in settings. The durations buffer must
  // outlive the (asynchronous) playback, so it is a function-local static; the
  // watchapp is single-threaded, so reusing one buffer is safe.
  static uint32_t pulse[1];
  pulse[0] = (uint32_t)kb->settings.haptic_ms;
  vibes_enqueue_custom_pattern((VibePattern){ .durations = pulse, .num_segments = 1 });
}

static void prv_caret_timer_cb(void *data) {
  T9Keyboard *kb = (T9Keyboard *)data;
  kb->caret_idle += CARET_BLINK_MS;
  if (kb->caret_idle >= CARET_IDLE_MS) {     // idle: stop the 2 Hz redraw, rest visible
    kb->caret_timer = NULL;
    if (!kb->caret_on) { kb->caret_on = true; layer_mark_dirty(kb->layer); }
    return;
  }
  kb->caret_on = !kb->caret_on;
  kb->caret_timer = app_timer_register(CARET_BLINK_MS, prv_caret_timer_cb, kb);
  layer_mark_dirty(kb->layer);
}

// Any edit wakes the caret: solid-on, idle counter reset, blink restarted if it
// had gone to sleep. The cursor stays steady while typing, then blinks during a
// brief pause, then stops redrawing the grid entirely once the user walks away.
static void prv_caret_wake(T9Keyboard *kb) {
  kb->caret_idle = 0;
  kb->caret_on = true;
  if (!kb->caret_timer)
    kb->caret_timer = app_timer_register(CARET_BLINK_MS, prv_caret_timer_cb, kb);
}

static void prv_flash_timer_cb(void *data) {
  T9Keyboard *kb = (T9Keyboard *)data;
  kb->flash_timer = NULL;
  kb->flash_key = -1;
  layer_mark_dirty(kb->layer);
}

static void prv_flash(T9Keyboard *kb, int key) {
  kb->flash_key = key;
  if (kb->flash_timer) app_timer_cancel(kb->flash_timer);
  kb->flash_timer = app_timer_register(FLASH_MS, prv_flash_timer_cb, kb);
}

static void prv_press_char_key(T9Keyboard *kb, int key) {
  const char *eff[MAX_EFF];
  int n = prv_eff_glyphs(kb, kb->page, key, eff, MAX_EFF);
  if (n == 0) return;
  if (n == 1) { prv_commit_pending(kb); prv_emit_glyph(kb, eff[0]); return; }

  if (kb->pending_active && kb->pending_key == key) {
    kb->cycle_index = (kb->cycle_index + 1) % n;   // re-press: advance the glyph
    prv_arm_commit_timer(kb);
    return;
  }
  prv_commit_pending(kb);
  kb->pending_key = key; kb->cycle_index = 0; kb->pending_active = true;
  prv_arm_commit_timer(kb);
}

// Word char for the double-space rule: not space, not sentence punctuation.
static bool prv_is_word_byte(char c) {
  return c != ' ' && c != '.' && c != '!' && c != '?';
}

static void prv_do_space(T9Keyboard *kb) {
  prv_commit_pending(kb);
  if (kb->page == PAGE_NUMBERS) { prv_append(kb, "0"); prv_recompute_autocap(kb); return; }

  size_t len = strlen(kb->buffer);
  if (kb->settings.two_space_period &&
      len >= 2 && kb->buffer[len - 1] == ' ' && prv_is_word_byte(kb->buffer[len - 2])) {
    kb->buffer[len - 1] = '\0';   // drop the trailing space...
    prv_append(kb, ". ");         // ...and replace it with ". "
  } else {
    prv_append(kb, " ");
  }
  prv_recompute_autocap(kb);
}

static void prv_press_key(T9Keyboard *kb, int key) {
  if (key < 0 || key >= KEY_COUNT) return;
  prv_caret_wake(kb);

  if (key == KEY_SHIFT) {
    if (kb->page != PAGE_ALPHA) {
      prv_cycle_page(kb);                  // doubles as the layout-cycle button
    } else if (kb->shift == SHIFT_OFF && kb->settings.auto_caps &&
               kb->autocap_armed && !kb->autocap_suppressed) {
      kb->autocap_suppressed = true;       // first press cancels an auto-capital
    } else {
      kb->shift = (kb->shift + 1) % 3;
    }
  } else if (key == KEY_DEL) {
    prv_do_delete(kb);
  } else if (key == KEY_SPACE) {
    prv_do_space(kb);
  } else {
    prv_press_char_key(kb, key);
  }
  layer_mark_dirty(kb->layer);
}

// ---- Hit testing ------------------------------------------------------------

static int prv_key_at_point(T9Keyboard *kb, GPoint p) {
  GRect b = layer_get_bounds(kb->layer);
  if (p.y < TEXT_AREA_H) return -1;
  int gw = b.size.w, gh = b.size.h - TEXT_AREA_H;
  if (gw <= 0 || gh <= 0) return -1;
  int cw = gw / GRID_COLS, ch = gh / GRID_ROWS;
  if (cw <= 0 || ch <= 0) return -1;
  int col = p.x / cw, row = (p.y - TEXT_AREA_H) / ch;
  if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return -1;
  return row * GRID_COLS + col;
}

void t9_keyboard_handle_tap(T9Keyboard *kb, GPoint point) {
  if (!kb) return;
  int key = prv_key_at_point(kb, point);
  if (key < 0) return;
  prv_flash(kb, key);
  prv_press_key(kb, key);
  prv_haptic(kb);            // every on-screen key tap buzzes, re-presses included
}

// Long-press: character keys insert their phone-keypad digit (key 0..8 ->
// '1'..'9'); the space/0 key inserts the OTHER of space/0 for its page (so on
// the 123 page, holding 0 types a space); the bottom-left key cycles layout.
void t9_keyboard_handle_hold(T9Keyboard *kb, GPoint point) {
  if (!kb) return;
  int key = prv_key_at_point(kb, point);
  if (key < 0) return;
  prv_caret_wake(kb);
  prv_flash(kb, key);
  if (key == KEY_SHIFT) { prv_cycle_page(kb); prv_haptic(kb); return; }
  if (key == KEY_DEL)   { prv_press_key(kb, KEY_DEL); prv_haptic(kb); return; }
  prv_commit_pending(kb);
  if (key == KEY_SPACE) prv_append(kb, (kb->page == PAGE_NUMBERS) ? " " : "0");
  else { char d[2] = { (char)('1' + key), '\0' }; prv_append(kb, d); }
  prv_recompute_autocap(kb);
  prv_haptic(kb);
  layer_mark_dirty(kb->layer);
}

// ---- Drawing ----------------------------------------------------------------

static void prv_key_label(T9Keyboard *kb, int i, char *out, size_t cap) {
  out[0] = '\0';
  if (i == KEY_SHIFT) {
    if (kb->page != PAGE_ALPHA) {                        // layout-cycle button
      strncpy(out, PAGE_IND[(kb->page + 1) % PAGE_COUNT], cap - 1);
      out[cap - 1] = '\0'; return;
    }
    int eff = prv_effective_shift(kb);
    const char *s = (eff == SHIFT_OFF) ? "ab" : (eff == SHIFT_ONCE) ? "Ab" : "AB";
    strncpy(out, s, cap - 1); out[cap - 1] = '\0'; return;
  }
  if (i == KEY_DEL)   { strncpy(out, "DEL", cap - 1); out[cap - 1] = '\0'; return; }
  if (i == KEY_SPACE) { strncpy(out, (kb->page == PAGE_NUMBERS) ? "0" : "space", cap - 1);
                        out[cap - 1] = '\0'; return; }
  const char *eff[MAX_EFF];
  int n = prv_eff_glyphs(kb, kb->page, i, eff, MAX_EFF);
  for (int j = 0; j < n; j++) {
    if ((unsigned char)eff[j][0] >= 0x80) continue;   // hide non-ASCII glyphs
    char gb[8]; prv_render_glyph(kb, eff[j], gb, sizeof(gb));
    if (strlen(out) + strlen(gb) < cap - 1) strcat(out, gb);
  }
}

// Draw `text` left-aligned in `box`. Order of attempts, largest effort first:
//   1) one line, shrinking through a font ladder until it fits the width;
//   2) if it won't fit one line, wrap to (up to) two lines at the largest font
//      whose wrapped block still fits the box height;
//   3) if even that overflows, truncate from the FRONT with a leading "..." so
//      the most recent characters stay visible. Text is vertically centered.
static void prv_draw_text_fitted(GContext *ctx, const char *text, GRect box) {
  static const char *const LADDER[] = {
    FONT_KEY_GOTHIC_28_BOLD, FONT_KEY_GOTHIC_24_BOLD,
    FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_14_BOLD,
  };
  const int N = (int)(sizeof(LADDER) / sizeof(LADDER[0]));
  const GRect wide = GRect(0, 0, 2000, 200);            // single-line measure
  const GRect wrap = GRect(0, 0, box.size.w, 400);      // wrapped-to-width measure
  char buf[BUFFER_CAP + 8];

  // 1) Single line.
  for (int i = 0; i < N; i++) {
    GFont f = fonts_get_system_font(LADDER[i]);
    GSize sz = graphics_text_layout_get_content_size(
                 text, f, wide, GTextOverflowModeFill, GTextAlignmentLeft);
    if (sz.w <= box.size.w) {
      int vy = box.origin.y + (box.size.h - sz.h) / 2;
      if (vy < box.origin.y) vy = box.origin.y;
      graphics_draw_text(ctx, text, f, GRect(box.origin.x, vy, box.size.w, sz.h + 4),
                         GTextOverflowModeFill, GTextAlignmentLeft, NULL);
      return;
    }
  }

  // 2) Wrap to at most two lines.
  for (int i = 0; i < N; i++) {
    GFont f = fonts_get_system_font(LADDER[i]);
    int line_h = graphics_text_layout_get_content_size(
                   "Ag", f, wide, GTextOverflowModeFill, GTextAlignmentLeft).h;
    GSize sz = graphics_text_layout_get_content_size(
                 text, f, wrap, GTextOverflowModeWordWrap, GTextAlignmentLeft);
    if (sz.h <= line_h * 2 + 1 && sz.h <= box.size.h) {
      int vy = box.origin.y + (box.size.h - sz.h) / 2;
      if (vy < box.origin.y) vy = box.origin.y;
      graphics_draw_text(ctx, text, f, GRect(box.origin.x, vy, box.size.w, sz.h + 4),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      return;
    }
  }

  // 3) Truncate from the front at the smallest font until two lines hold it.
  GFont f = fonts_get_system_font(LADDER[N - 1]);
  int line_h = graphics_text_layout_get_content_size(
                 "Ag", f, wide, GTextOverflowModeFill, GTextAlignmentLeft).h;
  const char *p = text;
  while (*p) {
    p++;
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
    snprintf(buf, sizeof(buf), "...%s", p);
    GSize sz = graphics_text_layout_get_content_size(
                 buf, f, wrap, GTextOverflowModeWordWrap, GTextAlignmentLeft);
    if (sz.h <= line_h * 2 + 1 && sz.h <= box.size.h) break;
  }
  snprintf(buf, sizeof(buf), "...%s", p);
  GSize sz = graphics_text_layout_get_content_size(
               buf, f, wrap, GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int vy = box.origin.y + (box.size.h - sz.h) / 2;
  if (vy < box.origin.y) vy = box.origin.y;
  graphics_draw_text(ctx, buf, f, GRect(box.origin.x, vy, box.size.w, sz.h + 4),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

// Recolor a palettized icon so its opaque pixels become `ink` while transparent
// palette entries stay clear. Lets one arrow asset render in any theme/state
// color. No-op if the asset didn't compile to a palettized format.
static void prv_tint_icon(GBitmap *bmp, GColor ink) {
  if (!bmp) return;
  GColor *pal = gbitmap_get_palette(bmp);
  if (!pal) return;
  int n;
  switch (gbitmap_get_format(bmp)) {
    case GBitmapFormat1BitPalette: n = 2;  break;
    case GBitmapFormat2BitPalette: n = 4;  break;
    case GBitmapFormat4BitPalette: n = 16; break;
    default: return;
  }
  for (int i = 0; i < n; i++) if (pal[i].a != 0) pal[i] = ink;
}

// Draw a ⎵ space symbol (a horizontal bar with short upturned ends), centered
// in `face` and stroked in `ink`.
static void prv_draw_space_symbol(GContext *ctx, GRect face, GColor ink) {
  int uw = face.size.w / 2, ut = 6;
  int ux = face.origin.x + (face.size.w - uw) / 2;
  int uy = face.origin.y + face.size.h * 3 / 5;
  graphics_context_set_stroke_color(ctx, ink);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(ux, uy), GPoint(ux + uw, uy));
  graphics_draw_line(ctx, GPoint(ux, uy), GPoint(ux, uy - ut));
  graphics_draw_line(ctx, GPoint(ux + uw, uy), GPoint(ux + uw, uy - ut));
  graphics_context_set_stroke_width(ctx, 1);
}

// Resolve a keyboard's selected index to a theme, with the app slot at
// THEME_COUNT and any out-of-range index falling back to the first built-in.
static const Theme *prv_theme(const T9Keyboard *kb) {
  if (s_has_app_theme && kb->theme == THEME_COUNT) return &s_app_theme;
  int i = (kb->theme < 0 || kb->theme >= THEME_COUNT) ? 0 : kb->theme;
  return &THEMES[i];
}

static void prv_update_proc(Layer *layer, GContext *ctx) {
  T9Keyboard *kb = *(T9Keyboard **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);

  const Theme *th = prv_theme(kb);
  GColor c_bg  = (GColor){ .argb = th->bg };
  GColor c_fg  = (GColor){ .argb = th->fg };
  GColor c_key = (GColor){ .argb = th->key };
  GColor c_acc = (GColor){ .argb = th->accent };
  GColor c_act = (GColor){ .argb = th->accent_text };
  GColor c_dgr  = (GColor){ .argb = th->danger };        // DEL glyph ink
  GColor c_dgrl = (GColor){ .argb = th->danger_light };  // DEL key fill

  graphics_context_set_fill_color(ctx, c_bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  GFont key_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  char shown[BUFFER_CAP + 8];
  strncpy(shown, kb->buffer, sizeof(shown)); shown[sizeof(shown) - 1] = '\0';
  if (kb->pending_active) {
    const char *eff[MAX_EFF];
    int n = prv_eff_glyphs(kb, kb->page, kb->pending_key, eff, MAX_EFF);
    if (kb->cycle_index < n) {
      char gb[8];
      prv_render_glyph(kb, eff[kb->cycle_index], gb, sizeof(gb));
      if (strlen(shown) + strlen(gb) < sizeof(shown)) strcat(shown, gb);
    }
  }
  if (kb->caret_on && strlen(shown) + 1 < sizeof(shown)) strcat(shown, "|");
  graphics_context_set_text_color(ctx, c_fg);
  prv_draw_text_fitted(ctx, shown, GRect(4, 0, b.size.w - 8, TEXT_AREA_H));

  graphics_context_set_stroke_color(ctx, c_fg);
  graphics_draw_line(ctx, GPoint(0, TEXT_AREA_H), GPoint(b.size.w, TEXT_AREA_H));

  // Keys are filled, rounded, and gapped like the throw grid: letter keys rest
  // on the soft `key` fill; the bottom function row (Shift/space/DEL) is a bold
  // accent strip — the keyboard's MISS bar. A press deepens a key for feedback:
  // each key darkens its own resting fill (keeping its label color) so a tap
  // reads as "pushed in" without changing hue. Hit testing still uses the full
  // cell (prv_key_at_point), so the gaps stay tappable.
  int cw = b.size.w / GRID_COLS, ch = (b.size.h - TEXT_AREA_H) / GRID_ROWS;
  for (int i = 0; i < KEY_COUNT; i++) {
    int col = i % GRID_COLS, row = i / GRID_COLS;
    GRect face = GRect(col * cw + 2, TEXT_AREA_H + row * ch + 2, cw - 4, ch - 4);

    bool is_function = (i == KEY_SHIFT || i == KEY_SPACE || i == KEY_DEL);
    bool shift_on = (i == KEY_SHIFT && kb->page == PAGE_ALPHA &&
                     prv_effective_shift(kb) != SHIFT_OFF);
    bool pressed = (i == kb->flash_key) ||
                   (kb->pending_active && kb->pending_key == i) || shift_on;

    GColor fill, txt;
    if (i == KEY_DEL) {
      fill = c_dgrl;   // danger key fill
      txt  = c_dgr;    // delete glyph ink
    } else if (is_function) {
      fill = c_acc;    // accent strip
      txt  = c_act;
    } else {
      fill = c_key;    // soft letter
      txt  = c_fg;
    }
    // 3D key: raised at rest, sunk into its shadow on press — tactile feedback
    // with no color change (a teal accent has no darker in-gamut shade to use).
    UiButtonSpec bs = {
      .style = UI_BTN_SOLID, .radius = 4,
      .fill_override = fill, .elevation = KEY_ELEV, .pressed = pressed,
    };
    ui_button_draw(ctx, face, &bs);
    face = ui_button_content_box(face, &bs);   // glyph sinks with the key
    graphics_context_set_text_color(ctx, txt);

    if (i == KEY_SHIFT && kb->page == PAGE_ALPHA) {
      // Shift state as an arrow icon: off (plain), once (dash), lock (dash
      // solid), recolored to the key's current text color.
      GBitmap *icon = kb->icon_shift[prv_effective_shift(kb)];
      if (icon) {
        prv_tint_icon(icon, txt);
        GSize is = gbitmap_get_bounds(icon).size;
        // +2px: the glyph sits high in its bounds, so a geometric center reads
        // a few pixels too high — nudge down to optically center.
        GRect ir = GRect(face.origin.x + (face.size.w - is.w) / 2,
                         face.origin.y + (face.size.h - is.h) / 2 + 2, is.w, is.h);
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, icon, ir);
      }
    } else if (i == KEY_SPACE && kb->page != PAGE_NUMBERS) {
      prv_draw_space_symbol(ctx, face, txt);
    } else if (i == KEY_DEL && kb->icon_del) {
      // Delete glyph in place of a "DEL" label, tinted with the danger ink.
      prv_tint_icon(kb->icon_del, txt);
      GSize is = gbitmap_get_bounds(kb->icon_del).size;
      GRect ir = GRect(face.origin.x + (face.size.w - is.w) / 2,
                       face.origin.y + (face.size.h - is.h) / 2, is.w, is.h);
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
      graphics_draw_bitmap_in_rect(ctx, kb->icon_del, ir);
    } else {
      char lbl[LABEL_CAP]; prv_key_label(kb, i, lbl, sizeof(lbl));
      // GOTHIC_18 pads above the caps, so center on its cap box (22) and nudge
      // up 2px to optically center — same correction the menu UI uses.
      GRect tr = GRect(face.origin.x, face.origin.y + (face.size.h - 22) / 2 - 2, face.size.w, 22);
      graphics_draw_text(ctx, lbl, key_font, tr,
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    }
  }
}

// ---- Lifecycle --------------------------------------------------------------

T9Keyboard *t9_keyboard_create(GRect frame, T9KeyboardDoneHandler done_handler,
                               void *context) {
  T9Keyboard *kb = malloc(sizeof(T9Keyboard));
  if (!kb) return NULL;
  memset(kb, 0, sizeof(T9Keyboard));
  kb->done_handler = done_handler;
  kb->context = context;
  kb->page = PAGE_ALPHA;
  kb->shift = SHIFT_OFF;
  kb->settings.commit_timeout_ms = T9_DEFAULT_WAIT_MS;
  kb->settings.auto_caps = true;
  kb->settings.two_space_period = false;
  kb->settings.haptics = true;
  kb->settings.haptic_ms = 20;   // one short pulse by default
  kb->settings.delete_mode = 0;
  kb->settings.del_repeat_chars_ms = 100;
  kb->settings.del_repeat_words_ms = 250;
  for (int i = 0; i < EXT_COUNT; i++) kb->ext_enabled[i] = true;
  kb->theme = 0;
  kb->flash_key = -1;
  kb->caret_on = true;
  kb->buffer[0] = '\0';
  prv_recompute_autocap(kb);

  kb->layer = layer_create_with_data(frame, sizeof(T9Keyboard *));
  if (!kb->layer) { free(kb); return NULL; }
  *(T9Keyboard **)layer_get_data(kb->layer) = kb;
  layer_set_update_proc(kb->layer, prv_update_proc);
  kb->icon_shift[SHIFT_OFF]  = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_SHIFT_OFF);
  kb->icon_shift[SHIFT_ONCE] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_SHIFT_ONCE);
  kb->icon_shift[SHIFT_LOCK] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_SHIFT_LOCK);
  kb->icon_del               = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_DELETE);
  kb->caret_timer = app_timer_register(CARET_BLINK_MS, prv_caret_timer_cb, kb);
  return kb;
}

void t9_keyboard_destroy(T9Keyboard *kb) {
  if (!kb) return;
  if (kb->commit_timer) { app_timer_cancel(kb->commit_timer); kb->commit_timer = NULL; }
  if (kb->caret_timer)  { app_timer_cancel(kb->caret_timer);  kb->caret_timer = NULL; }
  if (kb->flash_timer)  { app_timer_cancel(kb->flash_timer);  kb->flash_timer = NULL; }
  for (int i = 0; i < 3; i++) if (kb->icon_shift[i]) gbitmap_destroy(kb->icon_shift[i]);
  if (kb->icon_del) gbitmap_destroy(kb->icon_del);
  if (kb->layer) layer_destroy(kb->layer);
  free(kb);
}

Layer *t9_keyboard_get_layer(T9Keyboard *kb) { return kb ? kb->layer : NULL; }
const char *t9_keyboard_get_text(T9Keyboard *kb) { return kb ? kb->buffer : ""; }

// Trim the buffer to at most `max` bytes without splitting a UTF-8 glyph.
static void prv_clamp_to_max(T9Keyboard *kb) {
  if (kb->max_len <= 0) return;
  size_t i = strlen(kb->buffer);
  if (i <= (size_t)kb->max_len) return;
  i = (size_t)kb->max_len;
  while (i > 0 && ((unsigned char)kb->buffer[i] & 0xC0) == 0x80) i--;  // drop continuation
  kb->buffer[i] = '\0';
}

void t9_keyboard_set_text(T9Keyboard *kb, const char *text) {
  if (!kb) return;
  if (kb->commit_timer) { app_timer_cancel(kb->commit_timer); kb->commit_timer = NULL; }
  kb->pending_active = false;
  if (text) { strncpy(kb->buffer, text, BUFFER_CAP - 1); kb->buffer[BUFFER_CAP - 1] = '\0'; }
  else kb->buffer[0] = '\0';
  prv_clamp_to_max(kb);
  prv_recompute_autocap(kb);
  layer_mark_dirty(kb->layer);
}

void t9_keyboard_set_max_len(T9Keyboard *kb, int max_bytes) {
  if (!kb) return;
  kb->max_len = (max_bytes > 0 && max_bytes < BUFFER_CAP) ? max_bytes : 0;
  prv_clamp_to_max(kb);
  layer_mark_dirty(kb->layer);
}

void t9_keyboard_get_settings(T9Keyboard *kb, T9Settings *out) {
  if (kb && out) *out = kb->settings;
}

void t9_keyboard_set_settings(T9Keyboard *kb, const T9Settings *s) {
  if (!kb || !s) return;
  kb->settings = *s;
  if (kb->settings.commit_timeout_ms < 150) kb->settings.commit_timeout_ms = 150;
  prv_recompute_autocap(kb);
  layer_mark_dirty(kb->layer);
}

// ---- Button-shortcut actions ------------------------------------------------
// These are driven by the physical watch buttons, which stay silent by design;
// only on-screen key taps buzz. So none of these call prv_haptic().

void t9_keyboard_backspace(T9Keyboard *kb) { if (kb) prv_press_key(kb, KEY_DEL); }

void t9_keyboard_submit(T9Keyboard *kb) {
  if (!kb) return;
  prv_commit_pending(kb);
  layer_mark_dirty(kb->layer);
  // Fire last: the handler may pop/destroy the keyboard, so nothing may touch
  // `kb` after this. `buffer` stays valid for the duration of the callback.
  if (kb->done_handler) kb->done_handler(kb->buffer, kb->context);
}

void t9_keyboard_cycle_page(T9Keyboard *kb) { if (kb) prv_cycle_page(kb); }

void t9_keyboard_newline(T9Keyboard *kb) {
  if (!kb) return;
  prv_caret_wake(kb);
  prv_commit_pending(kb);
  prv_append(kb, "\n");
  prv_recompute_autocap(kb);
  layer_mark_dirty(kb->layer);
}

int t9_keyboard_delete_repeat_ms(T9Keyboard *kb) {
  if (!kb) return 100;
  return (kb->settings.delete_mode == 1) ? kb->settings.del_repeat_words_ms
                                         : kb->settings.del_repeat_chars_ms;
}

// ---- Extended (toggleable) characters ---------------------------------------

int t9_keyboard_ext_count(void) { return EXT_COUNT; }

const char *t9_keyboard_ext_glyph(int index) {
  return (index >= 0 && index < EXT_COUNT) ? EXT_DEFS[index].glyph : "";
}

bool t9_keyboard_ext_enabled(T9Keyboard *kb, int index) {
  return (kb && index >= 0 && index < EXT_COUNT) ? kb->ext_enabled[index] : false;
}

void t9_keyboard_ext_set_enabled(T9Keyboard *kb, int index, bool enabled) {
  if (!kb || index < 0 || index >= EXT_COUNT) return;
  kb->ext_enabled[index] = enabled;
  kb->pending_active = false;   // cycle indices may have shifted
  layer_mark_dirty(kb->layer);
}

// ---- Themes -----------------------------------------------------------------

int t9_keyboard_theme_count(void) { return THEME_COUNT; }   // built-ins only

const char *t9_keyboard_theme_name(int index) {
  if (s_has_app_theme && index == THEME_COUNT) return s_app_theme.name;
  return (index >= 0 && index < THEME_COUNT) ? THEMES[index].name : "";
}

int t9_keyboard_get_theme(T9Keyboard *kb) { return kb ? kb->theme : 0; }

void t9_keyboard_set_theme(T9Keyboard *kb, int index) {
  int max = s_has_app_theme ? THEME_COUNT : THEME_COUNT - 1;   // app slot is last
  if (!kb || index < 0 || index > max) return;
  kb->theme = index;
  layer_mark_dirty(kb->layer);
}

void t9_keyboard_set_app_theme(const T9Theme *t) {
  if (!t) { s_has_app_theme = false; return; }
  s_app_theme.name        = t->name;
  s_app_theme.bg          = t->bg.argb;
  s_app_theme.fg          = t->fg.argb;
  s_app_theme.key         = t->key.argb;
  s_app_theme.accent      = t->accent.argb;
  s_app_theme.accent_text = t->accent_text.argb;
  s_app_theme.danger       = t->danger.argb;
  s_app_theme.danger_light = t->danger_light.argb;
  s_has_app_theme = true;
}

bool t9_keyboard_has_app_theme(void) { return s_has_app_theme; }
int  t9_keyboard_app_theme_index(void) { return s_has_app_theme ? THEME_COUNT : -1; }
