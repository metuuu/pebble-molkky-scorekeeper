#include "dialog.h"
#include "button_group.h"
#include "ui_theme.h"
#include "ui_text.h"

// One dialog instance. Heap-allocated per push (strings copied in), freed on
// unload. The app only ever shows one at a time, but nothing here is static, so a
// future stacked use stays correct.
typedef struct {
  char           title[40];
  char           text[120];
  uint8_t        count;
  char           labels[DIALOG_MAX_BUTTONS][20];
  UiButtonScheme schemes[DIALOG_MAX_BUTTONS];
  void         (*on_click[DIALOG_MAX_BUTTONS])(void *ctx);
  void          *ctx;
  Window        *window;
  Layer         *text_layer;
  UiButtonGroup *group;
  int16_t        text_h;            // height of the title/body region (above the buttons)
} Dialog;

#define DLG_MARGIN  6
#define DLG_BTN_H   34
#define DLG_BTN_GAP 5
#define DLG_TEXT_GAP 4               // between title and body

// ---- title / body ----
static void text_draw(Layer *layer, GContext *ctx) {
  Window *w = layer_get_window(layer);
  if (!w) return;
  Dialog *d = window_get_user_data(w);
  GRect b = layer_get_bounds(layer);
  GRect region = GRect(DLG_MARGIN, 0, b.size.w - 2 * DLG_MARGIN, b.size.h);

  bool has_title = d->title[0] != '\0';
  bool has_text  = d->text[0]  != '\0';

  // Measure so the title+body block can sit vertically centered in the region.
  GSize ts = GSizeZero, bs = GSizeZero;
  if (has_title)
    ts = graphics_text_layout_get_content_size(d->title, ui_font(UI_FONT_TITLE),
           GRect(0, 0, region.size.w, 1000), GTextOverflowModeWordWrap, GTextAlignmentCenter);
  if (has_text)
    bs = graphics_text_layout_get_content_size(d->text, ui_font(UI_FONT_BODY),
           GRect(0, 0, region.size.w, 1000), GTextOverflowModeWordWrap, GTextAlignmentCenter);
  int16_t total = ts.h + (has_title && has_text ? DLG_TEXT_GAP : 0) + bs.h;
  int16_t y = region.origin.y + (region.size.h - total) / 2;
  if (y < region.origin.y) y = region.origin.y;

  if (has_title) {
    graphics_context_set_text_color(ctx, ui_text());
    ui_text_draw(ctx, d->title, UI_FONT_TITLE,
                 GRect(region.origin.x, y, region.size.w, ts.h + 4),
                 GTextAlignmentCenter, false, GTextOverflowModeWordWrap);
    y += ts.h + (has_text ? DLG_TEXT_GAP : 0);
  }
  if (has_text) {
    graphics_context_set_text_color(ctx, ui_text_muted());
    ui_text_draw(ctx, d->text, UI_FONT_BODY,
                 GRect(region.origin.x, y, region.size.w, bs.h + 4),
                 GTextAlignmentCenter, false, GTextOverflowModeWordWrap);
  }
}

// ---- buttons ----
static void dlg_click(int id, void *ctx) {
  Dialog *d = ctx;
  // Copy what we need before dismissing — the unload below frees `d`.
  void (*cb)(void *) = (id >= 0 && id < d->count) ? d->on_click[id] : NULL;
  void *user = d->ctx;
  Window *win = d->window;
  window_stack_remove(win, true);          // → dlg_unload frees d + destroys the group
  if (cb) cb(user);                        // safe: deferred timer, dialog already gone
}

static void dlg_load(Window *window) {
  Dialog *d = window_get_user_data(window);
  window_set_background_color(window, ui_background());
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  int16_t buttons_h = d->count * DLG_BTN_H + (d->count - 1) * DLG_BTN_GAP;
  int16_t group_y = b.size.h - DLG_MARGIN - buttons_h;

  // Title/body fill everything above the buttons.
  d->text_layer = layer_create(GRect(0, 0, b.size.w, group_y));
  layer_set_update_proc(d->text_layer, text_draw);
  layer_add_child(root, d->text_layer);

  // Buttons: a vertical stack, full width, in their own group (frame-local coords).
  GRect gframe = GRect(DLG_MARGIN, group_y, b.size.w - 2 * DLG_MARGIN, buttons_h);
  d->group = ui_button_group_create(window, gframe, (UiButtonGroupHandlers){
    .on_click = dlg_click,
  }, d);
  for (int i = 0; i < d->count; i++) {
    ui_button_group_add(d->group, (UiButton){
      .id = i,
      .frame = GRect(0, i * (DLG_BTN_H + DLG_BTN_GAP), gframe.size.w, DLG_BTN_H),
      .look = (UiButtonSpec){
        .style = UI_BTN_OUTLINE, .scheme = d->schemes[i],
        .label = d->labels[i], .font = UI_FONT_BODY_BOLD, .radius = 4,
      },
      .active_style = UI_BTN_SOLID, .active_scheme = d->schemes[i],
      .no_touch = true,           // physical Up/Down + Select only — a tap is too easy to fat-finger
    });
  }
}

static void dlg_unload(Window *window) {
  Dialog *d = window_get_user_data(window);
  if (d->group) ui_button_group_destroy(d->group);
  if (d->text_layer) layer_destroy(d->text_layer);
  window_destroy(window);
  free(d);
}

static void copy_str(char *dst, size_t cap, const char *src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

Window *dialog_push(DialogConfig cfg) {
  Dialog *d = malloc(sizeof(Dialog));
  if (!d) return NULL;
  memset(d, 0, sizeof(*d));

  uint8_t n = cfg.button_count;
  if (n < 1) n = 1;
  if (n > DIALOG_MAX_BUTTONS) n = DIALOG_MAX_BUTTONS;
  d->count = n;
  copy_str(d->title, sizeof d->title, cfg.title);
  copy_str(d->text,  sizeof d->text,  cfg.text);
  for (int i = 0; i < n; i++) {
    copy_str(d->labels[i], sizeof d->labels[i],
             cfg.buttons[i].label ? cfg.buttons[i].label : "OK");
    d->schemes[i]  = cfg.buttons[i].scheme;
    d->on_click[i] = cfg.buttons[i].on_click;
  }
  d->ctx = cfg.ctx;

  d->window = window_create();
  window_set_user_data(d->window, d);
  window_set_window_handlers(d->window, (WindowHandlers){
    .load = dlg_load, .unload = dlg_unload,
  });
  window_stack_push(d->window, true);
  return d->window;
}

Window *dialog_confirm_push(const char *title, const char *text,
                            const char *confirm_label, UiButtonScheme confirm_scheme,
                            void (*on_confirm)(void *ctx), void *ctx) {
  return dialog_push((DialogConfig){
    .title = title, .text = text, .button_count = 2, .ctx = ctx,
    .buttons = {
      { .label = confirm_label ? confirm_label : "OK",
        .scheme = confirm_scheme, .on_click = on_confirm },   // confirm on top
      { .label = "Cancel", .scheme = UI_BTN_NEUTRAL, .on_click = NULL },
    },
  });
}
