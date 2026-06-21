#include "list_item.h"
#include "checkbox.h"
#include "ui_text.h"

// Slot geometry, shared by every row. A leading icon hugs the text; a leading
// value/custom badge sits in a fixed slot (so place numbers and crowns line up).
#define LPAD       6      // gap from the left edge to the leading slot
#define RPAD       6      // gap from the right edge / trailing slot to the text
#define ICON_GAP   6      // gap between a leading icon and the title
#define LEAD_SLOT  30     // width of a value/custom leading badge
#define CHECK_BOX  20     // checkbox edge
#define CHECK_MGN  8      // checkbox inset from the right edge
#define VALUE_SLOT 44     // width reserved for a trailing value

// Per-size geometry. `h_one`/`h_two` are the cell heights for a title-only row
// and a title+subtitle row; `title`/`sub` carry their own centering metrics.
typedef struct {
  int16_t h_one, h_two;
  UiFont  title, sub;
} SizeSpec;
static SizeSpec size_spec(UiSize s) {
  switch (s) {
    case UI_SIZE_SM: return (SizeSpec){ 28, 42, UI_FONT_BODY_BOLD, UI_FONT_CAPTION };
    case UI_SIZE_LG: return (SizeSpec){ 44, 56, UI_FONT_TITLE,     UI_FONT_BODY };
    default:         return (SizeSpec){ 34, 50, UI_FONT_TITLE,     UI_FONT_BODY };  // MD
  }
}

// Recolor a palettized icon so its opaque pixels become `ink` while transparent
// entries stay clear — lets one asset render in any row state. No-op (draws as
// authored) if it didn't compile to a palettized format.
static void tint_icon(GBitmap *bmp, GColor ink) {
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

RowColors list_item_colors(RowState s) {
  bool hl = s.interactive && s.highlighted;   // a display-only row never highlights
  RowColors c = { .fg = ui_text(), .fill = ui_background(), .paint = false };
  if (s.disabled) {
    c.fg = ui_text_muted();
    if (hl) { c.fill = ui_neutral(); c.paint = true; }   // soft focus, not the accent
  } else if (hl) {
    c.fg = ui_accent_text();                             // ink over the accent bar
  }
  return c;
}

int16_t list_item_height(const ListItem *item, UiSize size) {
  SizeSpec sp = size_spec(size);
  return item->subtitle[0] ? sp.h_two : sp.h_one;
}

// A short value (a place number, a score) centered in a slot.
static void draw_value(GContext *ctx, const char *v, GRect slot, GColor fg, GTextAlignment align) {
  graphics_context_set_text_color(ctx, fg);
  ui_text_draw(ctx, v, UI_FONT_BODY_BOLD, slot, align, true, GTextOverflowModeFill);
}

// Draw the leading slot; return the x where the title may start.
static int draw_leading(GContext *ctx, GRect b, const Accessory *a, RowColors col,
                        bool hl, ListIconResolver resolve, void *rctx) {
  int x = b.origin.x + LPAD;
  switch (a->kind) {
    case ACC_ICON:
    case ACC_ICON_RAW: {
      GBitmap *bmp = resolve ? resolve(rctx, a->icon_res) : NULL;
      if (bmp) {
        if (a->kind == ACC_ICON) tint_icon(bmp, col.fg);   // RAW keeps its authored colors
        GSize is = gbitmap_get_bounds(bmp).size;
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, bmp,
            GRect(x, b.origin.y + (b.size.h - is.h) / 2, is.w, is.h));
        x += is.w + ICON_GAP;
      }
      break;
    }
    case ACC_VALUE:
      draw_value(ctx, a->value, GRect(x, b.origin.y, LEAD_SLOT, b.size.h), col.fg, GTextAlignmentCenter);
      x += LEAD_SLOT;
      break;
    case ACC_CUSTOM:
      if (a->custom.draw)
        a->custom.draw(ctx, GRect(x, b.origin.y, LEAD_SLOT, b.size.h), col, a->custom.data);
      x += LEAD_SLOT;
      break;
    default: break;   // ACC_NONE / a trailing-only kind
  }
  return x;
}

// Draw the trailing slot; return the x where the title must stop.
static int draw_trailing(GContext *ctx, GRect b, const Accessory *a, RowColors col, bool hl) {
  int right = b.origin.x + b.size.w;
  switch (a->kind) {
    case ACC_CHECKBOX: {
      int bx = right - CHECK_MGN - CHECK_BOX;
      ui_draw_checkbox(ctx, GRect(bx, b.origin.y + (b.size.h - CHECK_BOX) / 2, CHECK_BOX, CHECK_BOX),
                       a->checked, hl);
      return bx - RPAD;
    }
    case ACC_VALUE: {
      int rx = right - VALUE_SLOT;
      draw_value(ctx, a->value, GRect(rx, b.origin.y, VALUE_SLOT - RPAD, b.size.h), col.fg, GTextAlignmentRight);
      return rx - 4;
    }
    case ACC_CHEVRON: {
      int rx = right - CHECK_MGN - 10;
      draw_value(ctx, ">", GRect(rx, b.origin.y, 12, b.size.h), col.fg, GTextAlignmentLeft);
      return rx - RPAD;
    }
    case ACC_CUSTOM: {
      int rx = right - LEAD_SLOT;
      if (a->custom.draw)
        a->custom.draw(ctx, GRect(rx, b.origin.y, LEAD_SLOT, b.size.h), col, a->custom.data);
      return rx - RPAD;
    }
    default: return right - RPAD;   // ACC_NONE
  }
}

void list_item_draw(GContext *ctx, GRect b, const ListItem *item,
                    RowState state, UiSize size,
                    ListIconResolver resolve, void *rctx) {
  SizeSpec sp = size_spec(size);
  RowState st = state; st.disabled = state.disabled || item->disabled;
  RowColors col = list_item_colors(st);
  bool hl = st.interactive && st.highlighted;

  if (col.paint) {                                  // override the cell background
    graphics_context_set_fill_color(ctx, col.fill);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
  }

  int tx = draw_leading(ctx, b, &item->leading, col, hl, resolve, rctx);
  int tr = draw_trailing(ctx, b, &item->trailing, col, hl);
  int tw = tr - tx;
  if (tw < 0) tw = 0;

  graphics_context_set_text_color(ctx, col.fg);
  if (item->subtitle[0]) {
    int16_t tcap = ui_font_cap(sp.title);
    ui_text_draw(ctx, item->title, sp.title, GRect(tx, b.origin.y + 2, tw, tcap),
                 GTextAlignmentLeft, false, GTextOverflowModeTrailingEllipsis);
    ui_text_draw(ctx, item->subtitle, sp.sub,
                 GRect(tx, b.origin.y + tcap, tw, b.size.h - tcap - 2),
                 GTextAlignmentLeft, false, GTextOverflowModeTrailingEllipsis);
  } else {
    ui_text_draw(ctx, item->title, sp.title, GRect(tx, b.origin.y, tw, b.size.h),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
  }
}
