#pragma once
#include <pebble.h>
#include "list_item.h"

// =============================================================================
// menu — an interactive list in its own window. Up/Down move, Select activates,
// Back pops. Each row is a ListItem (title + optional subtitle, optional leading
// icon, optional trailing accessory). This is the app's navigation primitive.
//
// For display-only, heterogeneous content (sections, fields, gaps) use view.h.
// =============================================================================

typedef struct ListCore Menu;

typedef struct {
  void     *ctx;
  UiSize    size;                                   // row size (default MD)
  uint16_t (*get_count)(void *ctx);
  // Fill `out` (already zeroed to an empty title-only row) for row i.
  void     (*get_item)(void *ctx, uint16_t i, ListItem *out);
  void     (*on_select)(void *ctx, uint16_t i);     // may be NULL
  void     (*on_unload)(void *ctx);                 // window popped/destroyed; may be NULL
} MenuConfig;

Menu   *menu_push(const char *title, MenuConfig cfg);
void    menu_reload(Menu *m);
Window *menu_window(Menu *m);                        // for window_stack_remove
