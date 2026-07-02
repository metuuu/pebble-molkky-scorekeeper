#include "ui_tick.h"

// Enough for every widget kind that shows a clock; a fixed table keeps this in
// a few bytes of bss.
#define UI_TICK_MAX 4

static UiTickFn s_fns[UI_TICK_MAX];
static int      s_count;

static void dispatch(struct tm *t, TimeUnits units) {
  (void)t; (void)units;
  for (int i = 0; i < s_count; i++) s_fns[i]();
}

void ui_tick_register(UiTickFn fn) {
  if (!fn) return;
  for (int i = 0; i < s_count; i++) if (s_fns[i] == fn) return;
  if (s_count >= UI_TICK_MAX) return;
  s_fns[s_count++] = fn;
  if (s_count == 1) tick_timer_service_subscribe(MINUTE_UNIT, dispatch);
}

void ui_tick_unregister(UiTickFn fn) {
  for (int i = 0; i < s_count; i++) {
    if (s_fns[i] != fn) continue;
    for (int j = i; j + 1 < s_count; j++) s_fns[j] = s_fns[j + 1];
    s_count--;
    if (s_count == 0) tick_timer_service_unsubscribe();
    return;
  }
}
