#include <pebble.h>
#include "t9_keyboard_window.h"
#include "t9_keyboard.h"
#include "keyboard_settings.h"

// =============================================================================
// Full-screen modal keyboard. All the touch/button wiring that used to live in
// the demo's main.c is folded in here, so an app only needs one call to get a
// keyboard and the typed text back. See t9_keyboard_window.h.
// =============================================================================

#define HOLD_MS 350   // touch-down longer than this fires a hold, not a tap

static Window     *s_window;
static T9Keyboard *s_keyboard;

static T9KeyboardResultHandler s_handler;
static void       *s_context;
static char        s_initial[256];   // copied starting text
static bool        s_has_initial;
static int         s_max_len;        // byte cap on entry (0 = unlimited)
static bool        s_done;            // guard: deliver the result exactly once

// Tap vs. hold disambiguation, identical to the verified PebbleOS touch flow.
static GPoint    s_touch_pt;
static AppTimer *s_hold_timer;
static bool      s_held;

// ---- Touch -----------------------------------------------------------------

static void prv_hold_fire(void *data) {
  s_hold_timer = NULL;
  s_held = true;
  t9_keyboard_handle_hold(s_keyboard, s_touch_pt);
}

static void prv_touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_pt = GPoint(event->x, event->y);
      s_held = false;
      if (s_hold_timer) app_timer_cancel(s_hold_timer);
      s_hold_timer = app_timer_register(HOLD_MS, prv_hold_fire, NULL);
      break;
    case TouchEvent_PositionUpdate:
      break;   // (could cancel the pending hold on large drift)
    case TouchEvent_Liftoff:
      if (s_hold_timer) { app_timer_cancel(s_hold_timer); s_hold_timer = NULL; }
      // The keyboard fills the window, so screen coords == layer-local coords.
      if (!s_held) t9_keyboard_handle_tap(s_keyboard, s_touch_pt);
      break;
  }
}

// ---- Result delivery -------------------------------------------------------

// Hand the result back to the app and close. `text` is NULL on cancel. The
// keyboard's submit fires this as its last action, so popping (which destroys
// the keyboard on unload) is safe here.
static void prv_finish(const char *text) {
  if (s_done) return;
  s_done = true;
  if (s_handler) s_handler(text, s_context);   // text valid only during call
  window_stack_pop(true);
}

static void prv_kb_done(const char *text, void *context) { prv_finish(text); }

// ---- Buttons ---------------------------------------------------------------

static void prv_up_click(ClickRecognizerRef rec, void *ctx) {
  t9_keyboard_cycle_page(s_keyboard);
}
static void prv_up_long(ClickRecognizerRef rec, void *ctx) {
  settings_window_push(s_keyboard);
}
static void prv_select_click(ClickRecognizerRef rec, void *ctx) {
  t9_keyboard_submit(s_keyboard);          // -> prv_kb_done -> prv_finish
}
static void prv_select_long(ClickRecognizerRef rec, void *ctx) {
  t9_keyboard_newline(s_keyboard);
}
static void prv_down_click(ClickRecognizerRef rec, void *ctx) {
  t9_keyboard_backspace(s_keyboard);
}
static void prv_back_click(ClickRecognizerRef rec, void *ctx) {
  prv_finish(NULL);                        // cancel
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click);
  window_long_click_subscribe(BUTTON_ID_UP, 0, prv_up_long, NULL);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_select_long, NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN,
      t9_keyboard_delete_repeat_ms(s_keyboard), prv_down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);  // override exit
}

// ---- Lifecycle -------------------------------------------------------------

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_keyboard = t9_keyboard_create(bounds, prv_kb_done, NULL);
  layer_add_child(root, t9_keyboard_get_layer(s_keyboard));
  settings_load_and_apply(s_keyboard);
  if (s_max_len > 0) t9_keyboard_set_max_len(s_keyboard, s_max_len);
  if (s_has_initial) t9_keyboard_set_text(s_keyboard, s_initial);

  if (touch_service_is_enabled()) {
    touch_service_subscribe(prv_touch_handler, NULL);
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Touch not available on this watch");
  }
}

static void prv_unload(Window *window) {
  touch_service_unsubscribe();
  if (s_hold_timer) { app_timer_cancel(s_hold_timer); s_hold_timer = NULL; }
  t9_keyboard_destroy(s_keyboard);
  s_keyboard = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void t9_keyboard_window_push(T9KeyboardResultHandler handler,
                             const char *initial_text, void *context) {
  t9_keyboard_window_push_ex(handler, initial_text, 0, context);
}

void t9_keyboard_window_push_ex(T9KeyboardResultHandler handler,
                                const char *initial_text, int max_len,
                                void *context) {
  if (s_window) return;                    // single instance

  s_handler = handler;
  s_context = context;
  s_done = false;
  s_max_len = max_len;
  s_has_initial = (initial_text != NULL);
  if (s_has_initial) {
    strncpy(s_initial, initial_text, sizeof(s_initial) - 1);
    s_initial[sizeof(s_initial) - 1] = '\0';
  }

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_click_config_provider(s_window, prv_click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_load,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}
