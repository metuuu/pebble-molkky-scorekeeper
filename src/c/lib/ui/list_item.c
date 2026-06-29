#include "list_item.h"
#include "checkbox.h"
#include "ui_align.h"
#include "ui_text.h"

// Slot geometry, shared by every row. A leading icon hugs the text; a leading
// value/custom badge sits in a fixed slot (so place numbers and crowns line up).
#define LPAD       6      // gap from the left edge to the leading slot
#define RPAD       6      // gap from the right edge / trailing slot to the text
#define ICON_GAP   6      // gap between a leading icon and the title
#define LEAD_SLOT  30     // width of a value/custom leading badge
#define BADGE_GAP  6      // extra gap between a leading value/custom badge and the title
#define TRAIL_SLOT 66     // width of a trailing custom accessory (wider: it holds text)
#define CHECK_BOX  20     // checkbox edge
#define CHECK_MGN  8      // checkbox inset from the right edge
#define VALUE_SLOT 44     // width reserved for a trailing value
#define LINE_GAP   3      // target visual gap between a two-line row's title and
                          // subtitle. Held constant across font sizes (below).
// A cap-box reserves breathing room above/below the glyphs ≈ cap / INK_RATIO.
// Stacking two boxes would double that gap in the middle, so we subtract both
// margins and add back LINE_GAP. The ratio assumes one type family (all GOTHIC,
// stable glyph/cap proportions); if a face with different proportions is ever
// mixed in, give UiFont a real glyph-height field instead of this constant.
#define INK_RATIO  5

// Per-size geometry. `h_one`/`h_two` are the cell heights for a title-only row
// and a title+subtitle row; `title`/`sub` carry their own centering metrics.
typedef struct {
  int16_t h_one, h_two;
  UiFont  title, sub, value;   // `value` sizes leading/trailing numbers with the title
} SizeSpec;
static SizeSpec size_spec(UiSize s) {
  switch (s) {
    case UI_SIZE_SM: return (SizeSpec){ 28, 42, UI_FONT_BODY_BOLD, UI_FONT_CAPTION, UI_FONT_BODY_BOLD };
    case UI_SIZE_LG: return (SizeSpec){ 44, 56, UI_FONT_TITLE,     UI_FONT_BODY,    UI_FONT_TITLE };
    default:         return (SizeSpec){ 34, 50, UI_FONT_TITLE,     UI_FONT_BODY,    UI_FONT_TITLE };  // MD
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
  } else if (s.danger) {
    // Destructive row: danger-red label; on focus, a red bar (painted over the
    // container's accent highlight) with white ink — mirroring a danger button.
    c.fg = hl ? GColorWhite : ui_danger();
    if (hl) { c.fill = ui_danger(); c.paint = true; }
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
static void draw_value(GContext *ctx, const char *v, GRect slot, GColor fg, GTextAlignment align, UiFont font) {
  graphics_context_set_text_color(ctx, fg);
  ui_text_draw(ctx, v, font, slot, align, true, GTextOverflowModeFill);
}

// Draw the leading slot; return the x where the title may start.
static int draw_leading(GContext *ctx, GRect b, const Accessory *a, RowColors col,
                        bool hl, ListIconResolver resolve, void *rctx, UiFont vfont) {
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
            ui_rect_align(GRect(x, b.origin.y, is.w, b.size.h), is,
                          UI_ALIGN_START, UI_ALIGN_CENTER));
        x += is.w + ICON_GAP;
      }
      break;
    }
    case ACC_VALUE:
      draw_value(ctx, a->value, GRect(x, b.origin.y, LEAD_SLOT, b.size.h), col.fg, GTextAlignmentCenter, vfont);
      x += LEAD_SLOT + BADGE_GAP;
      break;
    case ACC_CUSTOM:
      if (a->custom.draw)
        a->custom.draw(ctx, GRect(x, b.origin.y, LEAD_SLOT, b.size.h), col, a->custom.data);
      x += LEAD_SLOT + BADGE_GAP;
      break;
    default: break;   // ACC_NONE / a trailing-only kind
  }
  return x;
}

// Draw the trailing slot; return the x where the title must stop.
static int draw_trailing(GContext *ctx, GRect b, const Accessory *a, RowColors col, bool hl, UiFont vfont) {
  int right = b.origin.x + b.size.w;
  switch (a->kind) {
    case ACC_CHECKBOX: {
      int bx = right - CHECK_MGN - CHECK_BOX;
      ui_draw_checkbox(ctx, ui_rect_align(GRect(bx, b.origin.y, CHECK_BOX, b.size.h),
                                          GSize(CHECK_BOX, CHECK_BOX),
                                          UI_ALIGN_START, UI_ALIGN_CENTER),
                       a->checked, hl);
      return bx - RPAD;
    }
    case ACC_VALUE: {
      int rx = right - VALUE_SLOT;
      draw_value(ctx, a->value, GRect(rx, b.origin.y, VALUE_SLOT - RPAD, b.size.h), col.fg, GTextAlignmentRight, vfont);
      return rx - 4;
    }
    case ACC_CHEVRON: {
      int rx = right - CHECK_MGN - 10;
      draw_value(ctx, ">", GRect(rx, b.origin.y, 12, b.size.h), col.fg, GTextAlignmentLeft, vfont);
      return rx - RPAD;
    }
    case ACC_CUSTOM: {
      int rx = right - TRAIL_SLOT;
      if (a->custom.draw)
        a->custom.draw(ctx, GRect(rx, b.origin.y, TRAIL_SLOT, b.size.h), col, a->custom.data);
      return rx - RPAD;
    }
    default: return right - RPAD;   // ACC_NONE
  }
}

void list_item_draw(GContext *ctx, GRect b, const ListItem *item,
                    RowState state, UiSize size,
                    ListIconResolver resolve, void *rctx) {
  SizeSpec sp = size_spec(size);
  RowState st = state;
  st.disabled = state.disabled || item->disabled;
  st.danger   = state.danger   || item->danger;
  RowColors col = list_item_colors(st);
  bool hl = st.interactive && st.highlighted;

  if (col.paint) {                                  // override the cell background
    graphics_context_set_fill_color(ctx, col.fill);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
  }

  // The leading accessory (crown / place badge) keeps its place; only the text
  // columns take the optional vertical nudge.
  GRect tb = b; tb.origin.y += item->content_dy;

  int tx = draw_leading(ctx, b, &item->leading, col, hl, resolve, rctx, sp.value);
  int tr = draw_trailing(ctx, tb, &item->trailing, col, hl, sp.value);
  int tw = tr - tx;
  if (tw < 0) tw = 0;

  b = tb;                                           // title/subtitle below draw against the nudged bounds

  graphics_context_set_text_color(ctx, col.fg);
  if (item->subtitle[0]) {
    // Center the title+subtitle as one block: position the subtitle LINE_GAP
    // below the title (font-size independent — see INK_RATIO), then align the
    // stack vertically in the cell with the same component the icon/checkbox
    // use. Each line still vcenters in its own cap-box, so it keeps the per-font
    // optical nudge, exactly like the single-line path below.
    int16_t tcap = ui_font_cap(sp.title);
    int16_t scap = ui_font_cap(sp.sub);
    int16_t step = tcap - tcap / INK_RATIO - scap / INK_RATIO + LINE_GAP;
    GRect block = ui_rect_align(GRect(tx, b.origin.y, tw, b.size.h),
                                GSize(tw, step + scap),
                                UI_ALIGN_START, UI_ALIGN_CENTER);
    ui_text_draw(ctx, item->title, sp.title,
                 GRect(block.origin.x, block.origin.y, tw, tcap),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
    ui_text_draw(ctx, item->subtitle, sp.sub,
                 GRect(block.origin.x, block.origin.y + step, tw, scap),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
  } else {
    ui_text_draw(ctx, item->title, sp.title, GRect(tx, b.origin.y, tw, b.size.h),
                 GTextAlignmentLeft, true, GTextOverflowModeTrailingEllipsis);
  }
}
