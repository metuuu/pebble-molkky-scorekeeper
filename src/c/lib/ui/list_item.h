#pragma once
#include <pebble.h>
#include "ui_theme.h"

// =============================================================================
// list_item — the stateless heart of the ui lib. A row is a *value* (ListItem)
// plus a *draw function*; nothing is retained. Any container (an interactive
// menu, a static list) builds ListItems and hands them here, so every row in
// the app shares one layout and one interaction-state → color rule.
//
// Layout is three slots:   [leading]  title / subtitle  [trailing]
// =============================================================================

// Row geometry. Controls cell height and the title/subtitle fonts. MD is the
// default (zero value); LG is the old, taller default; SM is a compact list.
typedef enum { UI_SIZE_MD, UI_SIZE_SM, UI_SIZE_LG } UiSize;

// --- interaction state → concrete colors -------------------------------------
// This is where the highlight/scoreboard story lives. `interactive` false means
// a display-only row (a scoreboard): `highlighted` is ignored, so the text
// never flips and no cursor is implied — that is the whole fix for boards that
// used to flicker their focused row.
typedef struct {
  bool highlighted;   // the container's *animated* focus (menu_cell_layer_is_highlighted)
  bool interactive;   // false → display-only row; `highlighted` is ignored
  bool disabled;      // muted ink; a soft fill when focused; inert
  bool danger;        // destructive action: danger-red ink, a red bar when focused
} RowState;

typedef struct {
  GColor fg;          // text + tinted-icon ink
  GColor fill;        // background to paint over the cell (only when `paint`)
  bool   paint;       // override the cell background (the disabled-focus case)
} RowColors;

// Resolve a RowState to concrete colors per the active ui_theme.
RowColors list_item_colors(RowState state);

// --- accessories (the leading / trailing slots) ------------------------------
// Built-ins cover the common cases; ACC_CUSTOM is the escape hatch for semantic
// glyphs (e.g. a medal crown) whose colors must stay at the call site, not in
// the theme. A custom draw fn gets the row's resolved colors so its *non*-
// semantic parts (a number, a label) can still track selection.
typedef enum {
  ACC_NONE,
  ACC_ICON,       // ~25px resource id, recolored to the row's ink
  ACC_ICON_RAW,   // ~25px resource id, drawn as-authored (two-tone glyphs); not tinted
  ACC_CHECKBOX,   // outlined box, tick when `checked`
  ACC_CHEVRON,    // ">" disclosure
  ACC_VALUE,      // short text (e.g. "On", "12", "3.")
  ACC_CUSTOM,     // the app draws it
} AccessoryKind;

typedef void (*AccessoryDraw)(GContext *ctx, GRect box, RowColors colors, void *data);

typedef struct {
  AccessoryKind kind;
  union {
    uint32_t icon_res;                 // ACC_ICON / ACC_ICON_RAW
    bool     checked;                  // ACC_CHECKBOX
    char     value[12];                // ACC_VALUE
    struct {                           // ACC_CUSTOM
      AccessoryDraw draw;
      void         *data;
    } custom;
  };
} Accessory;

// --- the row descriptor -------------------------------------------------------
typedef struct {
  char      title[32];
  char      subtitle[32];   // empty → a shorter title-only row
  Accessory leading;
  Accessory trailing;
  bool      disabled;
  bool      danger;         // destructive action: render in the danger palette
  int8_t    content_dy;     // vertical nudge (px) for the whole row's content
} ListItem;

// A clean title-only row. Call before filling fields in a get_item callback.
static inline ListItem list_item_empty(void) { return (ListItem){0}; }

// Loads (or fetches a cached) bitmap for an ACC_ICON resource id. The container
// owns the cache; list_item_draw calls back through this to tint and blit icons.
typedef GBitmap *(*ListIconResolver)(void *ctx, uint32_t res);

// Cell height for `item` at `size` (taller when it has a subtitle).
int16_t list_item_height(const ListItem *item, UiSize size);

// Draw `item` into `bounds`. `resolve`/`resolve_ctx` supply icon bitmaps for
// ACC_ICON slots and may be NULL when a caller has no icons.
void list_item_draw(GContext *ctx, GRect bounds, const ListItem *item,
                    RowState state, UiSize size,
                    ListIconResolver resolve, void *resolve_ctx);
