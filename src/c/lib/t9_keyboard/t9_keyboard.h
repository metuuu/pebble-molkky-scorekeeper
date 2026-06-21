#pragma once
#include <pebble.h>

// =============================================================================
// t9_keyboard — a Nokia-style multi-tap on-screen keyboard for PebbleOS
// touchscreen watches (Pebble Time 2 / Round 2, PebbleOS 4.9.171+).
//
// Tap a key to cycle through its characters (a -> b -> c -> å -> ä -> æ).
// Tapping the SAME key again before the commit timeout advances the character;
// tapping a DIFFERENT key or letting the timeout elapse commits it.
//
// Layout: a 3x4 grid. Keys 0-8 are the character keys; the bottom row is
// Shift (bottom-left), space, and DEL. There is no on-screen OK — submit is a
// host action (t9_keyboard_submit, e.g. wired to the Select button).
//
// Shift cycles off -> next-letter -> caps-lock, for ASCII a-z plus the Nordic
// letters å ä ö æ ø (-> Å Ä Ö Æ Ø). Those extended letters are hidden from the
// key labels (so `abc` reads "abc") but still reachable by cycling, and can be
// turned on/off individually via the extended-character API.
//
// Behavior settings (see T9Settings): the multi-tap wait time, auto-capitalize
// at the start of sentences (the shift indicator reflects it), and a
// double-space-inserts-period shortcut. Text is stored as UTF-8.
//
// This is the keyboard WIDGET (a Layer). It takes no touch dependency — feed
// it taps via t9_keyboard_handle_tap(), so it stays portable and testable.
// For the easy, full-screen drop-in (touch + buttons + return-to-app already
// wired), use t9_keyboard_window_push() from t9_keyboard_window.h instead.
// =============================================================================

#define T9_DEFAULT_WAIT_MS 800

// Tunable behaviors, surfaced by the on-watch settings menu.
typedef struct {
  int  commit_timeout_ms;  // wait between presses of the same key before commit
  bool auto_caps;          // capitalize first letter of each sentence
  bool two_space_period;   // "word " + space  ->  "word. "
  bool haptics;            // master on/off for vibration
  int  haptic_ms;          // length of the single key-press pulse (ms)
  int  delete_mode;        // 0 = characters, 1 = words
  int  del_repeat_chars_ms;// hold-to-repeat interval in characters mode
  int  del_repeat_words_ms;// hold-to-repeat interval in words mode
} T9Settings;

// Fired by t9_keyboard_submit(). `text` is only valid during the callback.
typedef void (*T9KeyboardDoneHandler)(const char *text, void *context);

typedef struct T9Keyboard T9Keyboard;

T9Keyboard *t9_keyboard_create(GRect frame, T9KeyboardDoneHandler done_handler,
                               void *context);
void  t9_keyboard_destroy(T9Keyboard *kb);
Layer *t9_keyboard_get_layer(T9Keyboard *kb);

// Single entry point for touch input (layer-local coordinates).
void t9_keyboard_handle_tap(T9Keyboard *kb, GPoint point);

// Long-press at a point: char keys / space insert their keypad digit
// (key 0..8 -> 1..9, space -> 0); the bottom-left key cycles the layout.
void t9_keyboard_handle_hold(T9Keyboard *kb, GPoint point);

// Programmatic actions — handy for wiring physical buttons.
void t9_keyboard_backspace(T9Keyboard *kb);
void t9_keyboard_submit(T9Keyboard *kb);
void t9_keyboard_cycle_page(T9Keyboard *kb);
void t9_keyboard_newline(T9Keyboard *kb);   // insert a '\n' (e.g. hold Select)

// Current hold-to-repeat delete interval (ms) for the active delete mode —
// wire this to the Down button's repeating-click subscription.
int t9_keyboard_delete_repeat_ms(T9Keyboard *kb);

const char *t9_keyboard_get_text(T9Keyboard *kb);
void t9_keyboard_set_text(T9Keyboard *kb, const char *text);

// Cap the entered text at `max_bytes` (excluding the NUL), e.g. to fit a fixed
// storage field. Keys that would overflow the cap are ignored; set_text()
// truncates to it. Counts bytes, not characters, so it never splits a UTF-8
// glyph. Pass 0 (the default) for no limit beyond the internal buffer.
void t9_keyboard_set_max_len(T9Keyboard *kb, int max_bytes);

// Behavior settings.
void t9_keyboard_get_settings(T9Keyboard *kb, T9Settings *out);
void t9_keyboard_set_settings(T9Keyboard *kb, const T9Settings *settings);

// Toggleable extended (non-ASCII) characters (e.g. å ä ö æ ø). These are
// hidden from the key labels but appended to the relevant key's cycle when
// enabled. The registry is static (shared); the on/off state is per-keyboard
// and all-on by default.
int  t9_keyboard_ext_count(void);
const char *t9_keyboard_ext_glyph(int index);
bool t9_keyboard_ext_enabled(T9Keyboard *kb, int index);
void t9_keyboard_ext_set_enabled(T9Keyboard *kb, int index, bool enabled);

// Color themes (Light, Dark, Ocean, ...). The registry is static; the selected
// index is per-keyboard. t9_keyboard_theme_count() counts the BUILT-IN themes
// only; an app-supplied theme (below) lives at index == count.
int  t9_keyboard_theme_count(void);
const char *t9_keyboard_theme_name(int index);
int  t9_keyboard_get_theme(T9Keyboard *kb);
void t9_keyboard_set_theme(T9Keyboard *kb, int index);

// An app-supplied color theme — your app's brand palette, so the lib needn't
// hardcode any app's colors. `accent` fills the function row and lights up
// pressed keys; `accent_text` is the text drawn on it, so pick `accent` dark
// enough (or `accent_text` light enough) to stay legible. `danger_light` fills
// the DEL key and `danger` tints its delete glyph, so the destructive key reads
// apart from the accent strip — keep `danger` legible on `danger_light`. `name`
// shows in the picker and must outlive the keyboard (use a string literal).
typedef struct {
  const char *name;
  GColor bg, fg, key, accent, accent_text, danger, danger_light;
} T9Theme;

// Register or replace the app's custom theme: a single shared slot appended
// after the built-ins, addressed as theme index t9_keyboard_theme_count().
// The struct is copied (the `name` pointer is kept as-is). Pass NULL to clear.
// Call once at startup, before any keyboard window is shown.
void t9_keyboard_set_app_theme(const T9Theme *theme);
bool t9_keyboard_has_app_theme(void);
int  t9_keyboard_app_theme_index(void);   // index for set_theme(), or -1 if none
