#pragma once
#include <pebble.h>
#include "list_item.h"

// =============================================================================
// list_core — PRIVATE to the ui lib. The MenuLayer-in-a-window plumbing shared
// by the public `menu` (interactive) and `list` (display-only) containers: it
// owns the window lifecycle, the icon cache, and routes MenuLayer callbacks to
// list_item_draw / list_item_height. It backs menu.h; apps use that, not this.
// =============================================================================

typedef struct ListCore ListCore;

typedef struct {
  void     *ctx;
  UiSize    size;
  bool      interactive;                            // true → focusable, selectable
  uint16_t (*get_count)(void *ctx);
  void     (*get_item)(void *ctx, uint16_t i, ListItem *out);
  void     (*on_select)(void *ctx, uint16_t i);     // NULL when !interactive
  void     (*on_unload)(void *ctx);                 // window popped/destroyed (may be NULL)
} ListCoreConfig;

// Creates a titled window+menu and pushes it. Reloads on reappear (so changes
// made in sub-windows show on return). Frees itself when popped.
ListCore *list_core_push(const char *title, ListCoreConfig cfg);
void      list_core_reload(ListCore *c);
Window   *list_core_window(ListCore *c);
