#include "button_group.h"
#include "ui_touch.h"

#define HOLD_MS          350   // touch/Select held longer than this fires a hold
#define REPEAT_DELAY_MS  350   // pause after the first REPEAT tick before repeating
#define DEFAULT_REPEAT_MS 150  // REPEAT interval when a button leaves repeat_ms == 0
#define FOCUS_NAV_MS     220   // hold Up/Down to step the focus ring at this rate

struct UiButtonGroup {
  Layer  *layer;
  Window *window;                       // NULL in layer (fed) mode
  UiButtonGroupHandlers h;
  void   *ctx;

  UiButton btns[UI_BTN_GROUP_MAX];
  int      count;

  int  focus;        // focused button index, or -1 for none
  int  pressed;      // button being held (touch or Select), or -1
  bool canceled;     // pressed, then the finger slid off the button
  bool long_fired;   // HOLD long-press already delivered this press
  bool owns_touch;   // we subscribed the global touch service

  AppTimer *hold_timer;   // HOLD threshold / REPEAT cadence

  // Activation is deferred one zero-delay hop so handlers may navigate safely.
  AppTimer *fire_timer;
  void    (*fire_fn)(int, void *);
  int       fire_id;
};

// ---- deferred activation ----------------------------------------------------

static void fire_cb(void *data) {
  UiButtonGroup *g = data;
  void (*fn)(int, void *) = g->fire_fn;
  int   id  = g->fire_id;
  void *ctx = g->ctx;
  g->fire_timer = NULL;
  g->fire_fn = NULL;
  if (fn) fn(id, ctx);   // may destroy the group — touch nothing after this
}

// Queue a handler to run after the current event returns. Discrete presses never
// stack two activations within one event, so a single pending slot suffices.
static void defer_fire(UiButtonGroup *g, void (*fn)(int, void *), int id) {
  if (!fn) return;
  g->fire_fn = fn;
  g->fire_id = id;
  if (g->fire_timer) app_timer_cancel(g->fire_timer);
  g->fire_timer = app_timer_register(0, fire_cb, g);
}

static void cancel_hold(UiButtonGroup *g) {
  if (g->hold_timer) { app_timer_cancel(g->hold_timer); g->hold_timer = NULL; }
}

static void hold_cb(void *data);
static void repeat_cb(void *data);

// ---- drawing ----------------------------------------------------------------

static void group_draw(Layer *layer, GContext *ctx) {
  UiButtonGroup *g = *(UiButtonGroup **)layer_get_data(layer);
  for (int i = 0; i < g->count; i++) {
    UiButton *b = &g->btns[i];
    bool held   = (i == g->pressed && !g->canceled);
    bool active = held || (i == g->focus && g->pressed < 0);
    UiButtonSpec s = b->look;
    if (active) {
      s.style = b->active_style; s.scheme = b->active_scheme;
      if (b->active_fill.argb != 0) s.fill_override = b->active_fill;
      if (b->active_ink.argb  != 0) s.ink_override  = b->active_ink;
    }
    s.pressed = held;   // 3D buttons (elevation > 0) sink while held; flat ones ignore it
    ui_button_draw(ctx, b->frame, &s);
  }
}

// ---- press state machine (shared by touch and physical Select) --------------

static void press_begin(UiButtonGroup *g, int i) {
  if (i < 0 || i >= g->count) return;
  UiButton *b = &g->btns[i];
  if (b->look.disabled) return;
  cancel_hold(g);
  g->pressed = i;
  g->canceled = false;
  g->long_fired = false;
  layer_mark_dirty(g->layer);
  if (g->h.on_press) g->h.on_press(b->id, g->ctx);   // feedback only

  if (b->behavior == UI_BTN_HOLD) {
    g->hold_timer = app_timer_register(HOLD_MS, hold_cb, g);
  } else if (b->behavior == UI_BTN_REPEAT) {
    defer_fire(g, g->h.on_click, b->id);             // first tick
    g->hold_timer = app_timer_register(REPEAT_DELAY_MS, repeat_cb, g);
  }
}

static void hold_cb(void *data) {
  UiButtonGroup *g = data;
  g->hold_timer = NULL;
  if (g->pressed < 0) return;
  g->long_fired = true;
  defer_fire(g, g->h.on_long_press, g->btns[g->pressed].id);
}

static void repeat_cb(void *data) {
  UiButtonGroup *g = data;
  g->hold_timer = NULL;
  if (g->pressed < 0) return;
  UiButton *b = &g->btns[g->pressed];
  defer_fire(g, g->h.on_click, b->id);
  uint16_t iv = b->repeat_ms ? b->repeat_ms : DEFAULT_REPEAT_MS;
  g->hold_timer = app_timer_register(iv, repeat_cb, g);
}

// Touch only: track whether the finger is still over the pressed button.
static void press_move(UiButtonGroup *g, GPoint p) {
  if (g->pressed < 0) return;
  bool in = grect_contains_point(&g->btns[g->pressed].frame, &p);
  if (in == !g->canceled) return;            // no change
  if (!in) { g->canceled = true; cancel_hold(g); }   // slid off: arm cancel, stop timers
  else     { g->canceled = false; }                   // slid back on: a release will click
  layer_mark_dirty(g->layer);
}

static void press_end(UiButtonGroup *g) {
  if (g->pressed < 0) return;
  UiButton *b = &g->btns[g->pressed];
  int id = b->id;
  UiButtonBehavior beh = b->behavior;
  bool canceled = g->canceled, long_fired = g->long_fired;

  cancel_hold(g);
  g->pressed = -1;
  g->canceled = false;
  layer_mark_dirty(g->layer);
  if (g->h.on_release) g->h.on_release(id, g->ctx);   // feedback only, no nav

  if (canceled) return;
  if (beh == UI_BTN_TAP)                       defer_fire(g, g->h.on_click, id);
  else if (beh == UI_BTN_HOLD && !long_fired)  defer_fire(g, g->h.on_click, id);
  // REPEAT already fired on press and on each tick; nothing more on release.
}

// ---- touch ------------------------------------------------------------------

static int hit_test(UiButtonGroup *g, GPoint p) {
  for (int i = 0; i < g->count; i++) {
    UiButton *b = &g->btns[i];
    if (b->no_touch || b->look.disabled) continue;
    if (grect_contains_point(&b->frame, &p)) return i;
  }
  return -1;
}

static void touch_dispatch(UiButtonGroup *g, const TouchEvent *e) {
  GRect f = layer_get_frame(g->layer);
  GPoint p = GPoint(e->x - f.origin.x, e->y - f.origin.y);   // screen -> layer-local
  switch (e->type) {
    case TouchEvent_Touchdown:      press_begin(g, hit_test(g, p)); break;
    case TouchEvent_PositionUpdate: press_move(g, p);              break;
    case TouchEvent_Liftoff:        press_end(g);                  break;
    default: break;
  }
}

static void touch_handler(const TouchEvent *e, void *context) {
  UiButtonGroup *g = context;
  // Events fan out to every live group (see ui_touch.h); act only while ours is
  // the visible window — a group under a dialog must not swallow its taps.
  if (g->window && window_stack_get_top_window() != g->window) return;
  touch_dispatch(g, e);
}

void ui_button_group_handle_touch(UiButtonGroup *g, const TouchEvent *event) {
  touch_dispatch(g, event);
}

// ---- physical focus ring ----------------------------------------------------

static void focus_move(UiButtonGroup *g, int dir) {
  if (g->count == 0) return;
  // Enter focus from the nearest edge, then wrap by dir.
  int start, scan;
  if (g->focus < 0) {
    start = dir > 0 ? g->count : -1;   // virtual slot just past the edge
    scan  = -dir;                       // ...so step 1 lands on the near edge, scanning inward
  } else {
    start = g->focus;
    scan  = dir;
  }
  for (int s = 1; s <= g->count; s++) {
    int i = (start + scan * s) % g->count;
    if (i < 0) i += g->count;
    UiButton *b = &g->btns[i];
    if (!b->no_focus && !b->look.disabled) { g->focus = i; layer_mark_dirty(g->layer); return; }
  }
}

void ui_button_group_focus_prev(UiButtonGroup *g)      { focus_move(g, -1); }
void ui_button_group_focus_next(UiButtonGroup *g)      { focus_move(g, +1); }
void ui_button_group_press_focused(UiButtonGroup *g)   { if (g->focus >= 0) press_begin(g, g->focus); }
void ui_button_group_release_focused(UiButtonGroup *g) { press_end(g); }

static void click_up(ClickRecognizerRef rec, void *context)   { focus_move(context, -1); }
static void click_down(ClickRecognizerRef rec, void *context) { focus_move(context, +1); }
static void select_down(ClickRecognizerRef rec, void *context){ ui_button_group_press_focused(context); }
static void select_up(ClickRecognizerRef rec, void *context)  { ui_button_group_release_focused(context); }

static void click_config(void *context) {
  UiButtonGroup *g = context;
  window_set_click_context(BUTTON_ID_UP, g);
  window_set_click_context(BUTTON_ID_DOWN, g);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, FOCUS_NAV_MS, click_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, FOCUS_NAV_MS, click_down);
  // Raw Select reuses the same press/release state as touch.
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_down, select_up, g);
}

// ---- lifecycle --------------------------------------------------------------

UiButtonGroup *ui_button_group_create_in_layer(Layer *parent, GRect frame,
                                               UiButtonGroupHandlers handlers, void *ctx) {
  UiButtonGroup *g = malloc(sizeof *g);
  if (!g) return NULL;
  *g = (UiButtonGroup){ .h = handlers, .ctx = ctx, .focus = -1, .pressed = -1 };
  g->layer = layer_create_with_data(frame, sizeof(UiButtonGroup *));
  if (!g->layer) { free(g); return NULL; }
  *(UiButtonGroup **)layer_get_data(g->layer) = g;
  layer_set_update_proc(g->layer, group_draw);
  layer_add_child(parent, g->layer);
  return g;
}

UiButtonGroup *ui_button_group_create(Window *window, GRect frame,
                                      UiButtonGroupHandlers handlers, void *ctx) {
  UiButtonGroup *g = ui_button_group_create_in_layer(window_get_root_layer(window),
                                                     frame, handlers, ctx);
  if (!g) return NULL;
  g->window = window;
  window_set_click_config_provider_with_context(window, click_config, g);
  g->owns_touch = ui_touch_register(touch_handler, g);
  return g;
}

int ui_button_group_add(UiButtonGroup *g, UiButton btn) {
  if (g->count >= UI_BTN_GROUP_MAX) return -1;
  g->btns[g->count] = btn;
  return g->count++;
}

Layer *ui_button_group_get_layer(UiButtonGroup *g) { return g->layer; }

void ui_button_group_destroy(UiButtonGroup *g) {
  if (!g) return;
  if (g->owns_touch) ui_touch_unregister(touch_handler, g);
  cancel_hold(g);
  if (g->fire_timer) app_timer_cancel(g->fire_timer);
  layer_destroy(g->layer);
  free(g);
}
