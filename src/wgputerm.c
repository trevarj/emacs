/* Wayland + wgpu terminal backend for Emacs -- terminal/redisplay shim.

This wires the Emacs redisplay interface (RIF) and terminal hooks to the Rust
backend over FFI (wgpu_ffi.h): RIF draw hooks record fills/glyphs via the
frame-command API, and frame_up_to_date presents.  Single-threaded model
(keyboard input is M3); structurally mirrors pgtkterm.c but holds no drawing
logic of its own.  See wgpu-backend-plan.md.  */

#include <config.h>

#include "lisp.h"
#include "blockinput.h"
#include "keyboard.h"
#include "frame.h"
#include "window.h"
#include "buffer.h"
#include "termchar.h"
#include "termhooks.h"
#include "dispextern.h"
#include "font.h"
#include "fontset.h"
#include "wgputerm.h"

/* Chain of all wgpu displays.  */
struct wgpu_display_info *x_display_list;

/* Alist of (NAME . PIXEL) X11 color names, loaded from rgb.txt, used to
   resolve named face colors (e.g. "red", "grey75").  staticpro'd so it
   survives GC.  PIXEL is 0xRRGGBB, matching our pixel encoding.  */
static Lisp_Object wgpu_color_map;

/* Human-readable keysym name.  M3 stub (no keyboard yet).  */
char *
get_keysym_name (int keysym)
{
  static char value[16];
  sprintf (value, "%d", keysym);
  return value;
}

/* Unpack a backend pixel (0xRRGGBB) into linear 0..1 RGB.  */
void
wgpu_unpack_pixel (unsigned long pixel, float *r, float *g, float *b)
{
  *r = ((pixel >> 16) & 0xff) / 255.0f;
  *g = ((pixel >> 8) & 0xff) / 255.0f;
  *b = (pixel & 0xff) / 255.0f;
}

/* ------------------------------------------------------------------ */
/* Colors.                                                            */
/* ------------------------------------------------------------------ */

bool
wgpu_defined_color (struct frame *f, const char *name, Emacs_Color *color,
		    bool alloc, bool make_index)
{
  unsigned short r, g, b;
  if (parse_color_spec (name, &r, &g, &b))
    {
      color->red = r;
      color->green = g;
      color->blue = b;
      /* Pack 8-bit-per-channel into the pixel; the draw path unpacks it.  */
      color->pixel = ((unsigned long) (r >> 8) << 16
		      | (unsigned long) (g >> 8) << 8
		      | (unsigned long) (b >> 8));
      return true;
    }

  /* Not a numeric spec: look the name up in the X11 color database.
     Without this, every named face color (mode line, region, ...) fails to
     resolve and load_color2 falls back to the frame's default colors, which
     makes those faces invisible.  */
  for (Lisp_Object tail = wgpu_color_map; CONSP (tail); tail = XCDR (tail))
    {
      Lisp_Object entry = XCAR (tail);
      if (CONSP (entry) && STRINGP (XCAR (entry))
	  && !xstrcasecmp (SSDATA (XCAR (entry)), name))
	{
	  unsigned long clr = (unsigned long) XFIXNUM (XCDR (entry));
	  color->pixel = clr;
	  color->red = ((clr >> 16) & 0xff) * 0x101;
	  color->green = ((clr >> 8) & 0xff) * 0x101;
	  color->blue = (clr & 0xff) * 0x101;
	  return true;
	}
    }
  return false;
}

static void
wgpu_query_colors (struct frame *f, Emacs_Color *colors, int ncolors)
{
  for (int i = 0; i < ncolors; i++)
    {
      unsigned long p = colors[i].pixel;
      colors[i].red = ((p >> 16) & 0xff) * 0x101;
      colors[i].green = ((p >> 8) & 0xff) * 0x101;
      colors[i].blue = (p & 0xff) * 0x101;
    }
}

static void
wgpu_query_frame_background_color (struct frame *f, Emacs_Color *bgcolor)
{
  bgcolor->pixel = FRAME_BACKGROUND_PIXEL (f);
  wgpu_query_colors (f, bgcolor, 1);
}

/* ------------------------------------------------------------------ */
/* Redisplay interface.                                               */
/* ------------------------------------------------------------------ */

/* Choose the foreground/background pixels for glyph string S according to its
   highlight kind (s->hl).  Shared with the font driver so glyphs use the same
   colors as the background fill.  */
void
wgpu_glyph_string_colors (struct glyph_string *s, unsigned long *fg,
			  unsigned long *bg)
{
  if (s->hl == DRAW_CURSOR)
    {
      /* The cursor block is the cursor color; the glyph shows in the face
	 background, i.e. inverted.  */
      *bg = FRAME_CURSOR_COLOR (s->f);
      *fg = s->face->background;
    }
  else
    {
      /* DRAW_NORMAL_TEXT / DRAW_INVERSE_VIDEO / DRAW_MOUSE_FACE / images:
	 the face already carries the right (possibly swapped) colors.  */
      *fg = s->face->foreground;
      *bg = s->face->background;
    }
}

/* Draw the face box around glyph string S (flat box; raised/sunken are
   approximated as flat for now).  This is what makes the mode line look
   right.  */
static void
wgpu_draw_glyph_string_box (struct glyph_string *s)
{
  if (s->face->box == FACE_NO_BOX)
    return;
  int hwidth = eabs (s->face->box_horizontal_line_width);
  int vwidth = eabs (s->face->box_vertical_line_width);
  if (hwidth == 0 && vwidth == 0)
    return;

  int left = s->x, top = s->y;
  int width = s->background_width, height = s->height;
  bool left_p = s->first_glyph->left_box_line_p;
  bool right_p = (s->nchars > 0
		  && s->first_glyph[s->nchars - 1].right_box_line_p);

  float r, g, b;
  wgpu_unpack_pixel (s->face->box_color, &r, &g, &b);
  block_input ();
  /* Top and bottom.  */
  wgpu_window_rect ((float) left, (float) top, (float) width, (float) hwidth,
		    r, g, b, 1.0f);
  wgpu_window_rect ((float) left, (float) (top + height - hwidth),
		    (float) width, (float) hwidth, r, g, b, 1.0f);
  if (left_p)
    wgpu_window_rect ((float) left, (float) top, (float) vwidth,
		      (float) height, r, g, b, 1.0f);
  if (right_p)
    wgpu_window_rect ((float) (left + width - vwidth), (float) top,
		      (float) vwidth, (float) height, r, g, b, 1.0f);
  unblock_input ();
}

static void
wgpu_draw_glyph_string (struct glyph_string *s)
{
  unsigned long fg, bg;
  wgpu_glyph_string_colors (s, &fg, &bg);

  switch (s->first_glyph->type)
    {
    case CHAR_GLYPH:
    case COMPOSITE_GLYPH:
      {
	/* Background (inset vertically by the box line so the box shows).  */
	if (!s->background_filled_p && !s->for_overlaps)
	  {
	    int box_line = max (s->face->box_horizontal_line_width, 0);
	    float r, g, b;
	    wgpu_unpack_pixel (bg, &r, &g, &b);
	    block_input ();
	    wgpu_window_rect ((float) s->x, (float) (s->y + box_line),
			      (float) s->background_width,
			      (float) (s->height - 2 * box_line), r, g, b, 1.0f);
	    unblock_input ();
	    s->background_filled_p = true;
	  }

	if (!s->for_overlaps)
	  wgpu_draw_glyph_string_box (s);

	/* Glyphs (background already drawn above).  */
	struct font *font = s->font;
	if (font && font->driver && font->driver->draw)
	  {
	    int y = s->ybase - font->baseline_offset;
	    font->driver->draw (s, 0, s->nchars, s->x, y, false);
	  }

	/* Underline.  */
	if (s->face->underline && !s->for_overlaps)
	  {
	    int thickness = (s->underline_thickness > 0
			     ? s->underline_thickness : 1);
	    int pos = s->ybase + (s->underline_position > 0
				  ? s->underline_position : 1);
	    unsigned long ul = (s->face->underline_defaulted_p
				? fg : s->face->underline_color);
	    float r, g, b;
	    wgpu_unpack_pixel (ul, &r, &g, &b);
	    block_input ();
	    wgpu_window_rect ((float) s->x, (float) pos, (float) s->width,
			      (float) thickness, r, g, b, 1.0f);
	    unblock_input ();
	  }
      }
      break;

    case STRETCH_GLYPH:
      {
	float r, g, b;
	wgpu_unpack_pixel (bg, &r, &g, &b);
	block_input ();
	wgpu_window_rect ((float) s->x, (float) s->y,
			  (float) s->background_width, (float) s->height,
			  r, g, b, 1.0f);
	unblock_input ();
      }
      break;

    case IMAGE_GLYPH:
    case GLYPHLESS_GLYPH:
    default:
      /* Images/glyphless not yet drawn.  */
      break;
    }
}

static void
wgpu_clear_frame_area (struct frame *f, int x, int y, int width, int height)
{
  float r, g, b;
  wgpu_unpack_pixel (FRAME_BACKGROUND_PIXEL (f), &r, &g, &b);
  block_input ();
  wgpu_window_rect ((float) x, (float) y, (float) width, (float) height,
		    r, g, b, 1.0f);
  unblock_input ();
}

static void
wgpu_draw_window_cursor (struct window *w, struct glyph_row *glyph_row,
			 int x, int y, enum text_cursor_kinds cursor_type,
			 int cursor_width, bool on_p, bool active_p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));

  /* When turning the cursor off, Emacs has already redrawn the underlying
     glyph (via erase_phys_cursor -> draw_phys_cursor_glyph with
     DRAW_NORMAL_TEXT), so there is nothing to paint here.  */
  if (!on_p)
    return;

  w->phys_cursor_type = cursor_type;
  w->phys_cursor_on_p = true;

  switch (cursor_type)
    {
    case NO_CURSOR:
      w->phys_cursor_width = 0;
      break;

    case FILLED_BOX_CURSOR:
      /* Redraw the glyph under the cursor inverted: this paints the cursor
	 block in the cursor color and the character in the frame background,
	 going through wgpu_draw_glyph_string with hl == DRAW_CURSOR.  */
      draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
      break;

    case HOLLOW_BOX_CURSOR:
    case BAR_CURSOR:
    case HBAR_CURSOR:
      {
	struct glyph *cursor_glyph = get_phys_cursor_glyph (w);
	if (!cursor_glyph)
	  break;
	int gx, gy, gh, wd = cursor_glyph->pixel_width;
	get_phys_cursor_geometry (w, glyph_row, cursor_glyph, &gx, &gy, &gh);
	float r, g, b;
	wgpu_unpack_pixel (FRAME_CURSOR_COLOR (f), &r, &g, &b);
	block_input ();
	if (cursor_type == HOLLOW_BOX_CURSOR)
	  {
	    /* Outline only.  */
	    wgpu_window_rect ((float) gx, (float) gy, (float) wd, 1.0f, r, g, b, 1.0f);
	    wgpu_window_rect ((float) gx, (float) (gy + gh - 1), (float) wd, 1.0f, r, g, b, 1.0f);
	    wgpu_window_rect ((float) gx, (float) gy, 1.0f, (float) gh, r, g, b, 1.0f);
	    wgpu_window_rect ((float) (gx + wd - 1), (float) gy, 1.0f, (float) gh, r, g, b, 1.0f);
	  }
	else if (cursor_type == BAR_CURSOR)
	  {
	    int bw = cursor_width > 0 ? cursor_width : 2;
	    wgpu_window_rect ((float) gx, (float) gy, (float) bw, (float) gh, r, g, b, 1.0f);
	  }
	else /* HBAR_CURSOR */
	  {
	    int bh = cursor_width > 0 ? cursor_width : 2;
	    wgpu_window_rect ((float) gx, (float) (gy + gh - bh), (float) wd,
			      (float) bh, r, g, b, 1.0f);
	  }
	unblock_input ();
      }
      break;

    default:
      break;
    }
}

/* Shift an already-rendered region of the window up/down on the GPU's
   persistent texture, so redisplay only has to repaint the newly-exposed
   lines.  Mirrors pgtk_scroll_run (sans xwidgets).  */
static void
wgpu_scroll_run (struct window *w, struct run *run)
{
  int x, y, width, height, from_y, to_y, bottom_y;

  /* Frame-relative bounding box of W's text area (including fringes).  */
  window_box (w, ANY_AREA, &x, &y, &width, &height);

  from_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->current_y);
  to_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->desired_y);
  bottom_y = y + height;

  if (to_y < from_y)
    {
      /* Scrolling up: don't copy in the mode line at the bottom.  */
      if (from_y + run->height > bottom_y)
	height = bottom_y - from_y;
      else
	height = run->height;
    }
  else
    {
      /* Scrolling down: don't copy over the mode line at the bottom.  */
      if (to_y + run->height > bottom_y)
	height = bottom_y - to_y;
      else
	height = run->height;
    }

  if (height <= 0 || width <= 0)
    return;

  block_input ();
  /* Cursor off; redisplay switches it back on in update_window_end.  */
  gui_clear_cursor (w);
  wgpu_window_scroll (x, from_y, width, height, to_y);
  unblock_input ();
}
static void wgpu_after_update_window_line (struct window *w, struct glyph_row *r) {}
static void wgpu_flush_display (struct frame *f) {}

/* Fringe bitmaps (continuation/truncation arrows, empty-line and buffer
   boundary indicators).  We keep the raw bits (copied at define time, since
   define may run before the window/atlas exists) and lazily rasterize them
   into the glyph atlas on first draw.  */
struct wgpu_fringe_bmp
{
  unsigned short *bits;
  int h, wd;
  int64_t atlas_id;		/* -1 until uploaded */
};
static struct wgpu_fringe_bmp *wgpu_fringe_bmps;
static int wgpu_fringe_bmp_max;

static void
wgpu_define_fringe_bitmap (int which, unsigned short *bits, int h, int wd)
{
  if (which >= wgpu_fringe_bmp_max)
    {
      int old = wgpu_fringe_bmp_max;
      wgpu_fringe_bmp_max = which + 20;
      wgpu_fringe_bmps = xrealloc (wgpu_fringe_bmps,
				   wgpu_fringe_bmp_max * sizeof *wgpu_fringe_bmps);
      memset (&wgpu_fringe_bmps[old], 0,
	      (wgpu_fringe_bmp_max - old) * sizeof *wgpu_fringe_bmps);
    }
  struct wgpu_fringe_bmp *fb = &wgpu_fringe_bmps[which];
  xfree (fb->bits);
  fb->bits = xnmalloc (h, sizeof (unsigned short));
  memcpy (fb->bits, bits, h * sizeof (unsigned short));
  fb->h = h;
  fb->wd = wd;
  fb->atlas_id = -1;
}

static void
wgpu_destroy_fringe_bitmap (int which)
{
  if (which < 0 || which >= wgpu_fringe_bmp_max)
    return;
  struct wgpu_fringe_bmp *fb = &wgpu_fringe_bmps[which];
  xfree (fb->bits);
  fb->bits = NULL;
  fb->h = fb->wd = 0;
  fb->atlas_id = -1;
}

/* Rasterize fringe bitmap WHICH to an R8 coverage and upload it to the atlas,
   caching the id.  Bit order matches the other backends (pixel COL set iff
   bit COL of the row word is set).  Returns the atlas id, or -1.  */
static int64_t
wgpu_fringe_atlas_id (int which)
{
  if (which <= 0 || which >= wgpu_fringe_bmp_max)
    return -1;
  struct wgpu_fringe_bmp *fb = &wgpu_fringe_bmps[which];
  if (!fb->bits || fb->h <= 0 || fb->wd <= 0)
    return -1;
  if (fb->atlas_id >= 0)
    return fb->atlas_id;

  unsigned char *cov = xnmalloc (fb->wd, fb->h);
  for (int row = 0; row < fb->h; row++)
    for (int col = 0; col < fb->wd; col++)
      cov[row * fb->wd + col] = ((fb->bits[row] >> col) & 1) ? 255 : 0;
  fb->atlas_id = wgpu_window_atlas_upload (fb->wd, fb->h, cov,
					   (size_t) fb->wd * fb->h);
  xfree (cov);
  return fb->atlas_id;
}

static void
wgpu_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			 struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = p->face;

  block_input ();

  /* Background behind the bitmap.  */
  if (p->bx >= 0 && !p->overlay_p && p->nx > 0 && p->ny > 0)
    {
      unsigned long bg = face ? face->background : FRAME_BACKGROUND_PIXEL (f);
      float r, g, b;
      wgpu_unpack_pixel (bg, &r, &g, &b);
      wgpu_window_rect ((float) p->bx, (float) p->by, (float) p->nx,
			(float) p->ny, r, g, b, 1.0f);
    }

  /* The bitmap itself.  */
  if (p->which)
    {
      /* Lazily define the bitmap if it was registered while no GUI frame
	 existed (e.g. a package loaded under a daemon).  */
      if (p->which >= wgpu_fringe_bmp_max || !wgpu_fringe_bmps[p->which].bits)
	gui_define_fringe_bitmap (f, p->which);
      int64_t id = wgpu_fringe_atlas_id (p->which);
      if (id >= 0)
	{
	  unsigned long fg
	    = (p->cursor_p
	       ? (p->overlay_p
		  ? (face ? face->background : FRAME_BACKGROUND_PIXEL (f))
		  : FRAME_CURSOR_COLOR (f))
	       : (face ? face->foreground : FRAME_FOREGROUND_PIXEL (f)));
	  float r, g, b;
	  wgpu_unpack_pixel (fg, &r, &g, &b);
	  /* Offset by -dh so visible rows [dh, dh+h) land at p->y (partial
	     rows at window edges; dh is 0 in the common case).  */
	  wgpu_window_glyph (id, (float) p->x, (float) (p->y - p->dh),
			     r, g, b, 1.0f);
	}
    }

  unblock_input ();
}

/* Fill a frame-relative rectangle with a packed pixel color.  */
static void
wgpu_fill_rect_pixel (int x, int y, int w, int h, unsigned long pixel)
{
  if (w <= 0 || h <= 0)
    return;
  float r, g, b;
  wgpu_unpack_pixel (pixel, &r, &g, &b);
  wgpu_window_rect ((float) x, (float) y, (float) w, (float) h, r, g, b, 1.0f);
}

/* Fill the internal border strips so no stale pixels show when the window
   layout changes.  */
static void
wgpu_clear_under_internal_border (struct frame *f)
{
  int border = FRAME_INTERNAL_BORDER_WIDTH (f);
  if (border <= 0)
    return;
  int width = FRAME_PIXEL_WIDTH (f);
  int height = FRAME_PIXEL_HEIGHT (f);
  int margin = FRAME_TOP_MARGIN_HEIGHT (f);
  int bottom_margin = FRAME_BOTTOM_MARGIN_HEIGHT (f);
  int face_id = (FRAME_PARENT_FRAME (f)
		 ? CHILD_FRAME_BORDER_FACE_ID : INTERNAL_BORDER_FACE_ID);
  if (!NILP (Vface_remapping_alist))
    face_id = lookup_basic_face (NULL, f, face_id);
  struct face *face = FACE_FROM_ID_OR_NULL (f, face_id);
  unsigned long pixel = face ? face->background : FRAME_BACKGROUND_PIXEL (f);

  block_input ();
  wgpu_fill_rect_pixel (0, margin, width, border, pixel);
  wgpu_fill_rect_pixel (0, 0, border, height, pixel);
  wgpu_fill_rect_pixel (width - border, 0, border, height, pixel);
  wgpu_fill_rect_pixel (0, height - bottom_margin - border, width, border, pixel);
  unblock_input ();
}

/* 1px line separating side-by-side windows (no divider configured).  */
static void
wgpu_draw_vertical_window_border (struct window *w, int x, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, VERTICAL_BORDER_FACE_ID);
  unsigned long pixel = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);
  block_input ();
  wgpu_fill_rect_pixel (x, y0, 1, y1 - y0, pixel);
  unblock_input ();
}

/* Window divider (the draggable separator), drawn with first/middle/last
   pixel faces so it gets a subtle 3D edge like the other backends.  */
static void
wgpu_draw_window_divider (struct window *w, int x0, int x1, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FACE_ID);
  struct face *face_first
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FIRST_PIXEL_FACE_ID);
  struct face *face_last
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_LAST_PIXEL_FACE_ID);
  unsigned long color = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);
  unsigned long color_first
    = face_first ? face_first->foreground : FRAME_FOREGROUND_PIXEL (f);
  unsigned long color_last
    = face_last ? face_last->foreground : FRAME_FOREGROUND_PIXEL (f);

  block_input ();
  if (y1 - y0 > x1 - x0 && x1 - x0 > 2)
    {
      /* Vertical divider.  */
      wgpu_fill_rect_pixel (x0, y0, 1, y1 - y0, color_first);
      wgpu_fill_rect_pixel (x0 + 1, y0, x1 - x0 - 2, y1 - y0, color);
      wgpu_fill_rect_pixel (x1 - 1, y0, 1, y1 - y0, color_last);
    }
  else if (x1 - x0 > y1 - y0 && y1 - y0 > 3)
    {
      /* Horizontal divider.  */
      wgpu_fill_rect_pixel (x0, y0, x1 - x0, 1, color_first);
      wgpu_fill_rect_pixel (x0, y0 + 1, x1 - x0, y1 - y0 - 2, color);
      wgpu_fill_rect_pixel (x0, y1 - 1, x1 - x0, 1, color_last);
    }
  else
    wgpu_fill_rect_pixel (x0, y0, x1 - x0, y1 - y0, color);
  unblock_input ();
}

static void wgpu_define_frame_cursor (struct frame *f, Emacs_Cursor cursor) {}
static void wgpu_show_hourglass (struct frame *f) {}
static void wgpu_hide_hourglass (struct frame *f) {}

void
wgpu_default_font_parameter (struct frame *f, Lisp_Object parms)
{
  struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Lisp_Object font_param = gui_display_get_arg (dpyinfo, parms, Qfont, NULL,
						NULL, RES_TYPE_STRING);
  Lisp_Object font = Qnil;
  if (BASE_EQ (font_param, Qunbound))
    font_param = Qnil;

  if (NILP (font))
    font = (!NILP (font_param) ? font_param
	    : gui_display_get_arg (dpyinfo, parms, Qfont, "font", "Font",
				   RES_TYPE_STRING));

  if (!FONTP (font) && !STRINGP (font))
    {
      /* Use an explicit pixel size scaled by the HiDPI factor, so the
	 default font is readable and unambiguous (avoids the point-size /
	 face-height path that can collapse to 1px).  */
      int scale = (int) dpyinfo->scale;
      if (scale < 1)
	scale = 1;
      char sized[64];
      snprintf (sized, sizeof sized, "Monospace:pixelsize=%d", 14 * scale);
      const char *names[] = { sized, "monospace-10", "fixed", NULL };
      for (int i = 0; names[i]; i++)
	{
	  font = font_open_by_name (f, build_unibyte_string (names[i]));
	  if (!NILP (font))
	    break;
	}
      if (NILP (font))
	error ("No suitable font was found");
    }

  gui_default_parameter (f, parms, Qfont, font, "font", "Font",
			 RES_TYPE_STRING);
}

/* ------------------------------------------------------------------ */
/* Frame parameter handlers.                                          */
/* ------------------------------------------------------------------ */

static void
wgpu_set_foreground_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wgpu_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_FOREGROUND_PIXEL (f) = col.pixel;
      update_face_from_frame_parameter (f, Qforeground_color, arg);
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

static void
wgpu_set_background_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wgpu_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_BACKGROUND_PIXEL (f) = col.pixel;
      update_face_from_frame_parameter (f, Qbackground_color, arg);
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

static void
wgpu_set_cursor_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wgpu_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_X_OUTPUT (f)->cursor_color = col.pixel;
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

/* Positional, indexed by frame parameter (mirrors pgtk_frame_parm_handlers).
   gui_set_* are the shared generic handlers; backend-specific decorations are
   NULL at M2.  */
frame_parm_handler wgpu_frame_parm_handlers[] = {
  gui_set_autoraise,
  gui_set_autolower,
  wgpu_set_background_color,
  NULL,				/* border_color */
  gui_set_border_width,
  wgpu_set_cursor_color,
  NULL,				/* cursor_type */
  gui_set_font,
  wgpu_set_foreground_color,
  NULL,				/* icon_name */
  NULL,				/* icon_type */
  NULL,				/* child_frame_border_width */
  NULL,				/* internal_border_width */
  gui_set_right_divider_width,
  gui_set_bottom_divider_width,
  NULL,				/* menu_bar_lines */
  NULL,				/* mouse_color */
  NULL,				/* explicitly_set_name */
  gui_set_scroll_bar_width,
  gui_set_scroll_bar_height,
  NULL,				/* title */
  gui_set_unsplittable,
  gui_set_vertical_scroll_bars,
  gui_set_horizontal_scroll_bars,
  gui_set_visibility,
  NULL,				/* tab_bar_lines */
  NULL,				/* tool_bar_lines */
  NULL,				/* scroll_bar_foreground */
  NULL,				/* scroll_bar_background */
  gui_set_screen_gamma,
  gui_set_line_spacing,
  gui_set_left_fringe,
  gui_set_right_fringe,
  0,
  gui_set_fullscreen,
  gui_set_font_backend,
  gui_set_alpha,
  NULL,				/* sticky */
  NULL,				/* tool_bar_position */
  0,
  NULL,				/* undecorated */
  NULL,				/* parent_frame */
  NULL,				/* skip_taskbar */
  NULL,				/* no_focus_on_map */
  NULL,				/* no_accept_focus */
  NULL,				/* z_group */
  NULL,				/* override_redirect */
  gui_set_no_special_glyphs,
  gui_set_alpha_background,
  gui_set_borders_respect_alpha_background,
  NULL,
};

static struct redisplay_interface wgpu_redisplay_interface = {
  wgpu_frame_parm_handlers,
  gui_produce_glyphs,
  gui_write_glyphs,
  gui_insert_glyphs,
  gui_clear_end_of_line,
  wgpu_scroll_run,
  wgpu_after_update_window_line,
  NULL, /* update_window_begin */
  NULL, /* update_window_end */
  wgpu_flush_display,
  gui_clear_window_mouse_face,
  gui_get_glyph_overhangs,
  gui_fix_overlapping_area,
  wgpu_draw_fringe_bitmap,
  wgpu_define_fringe_bitmap,
  wgpu_destroy_fringe_bitmap,
  NULL, /* compute_glyph_string_overhangs */
  wgpu_draw_glyph_string,
  wgpu_define_frame_cursor,
  wgpu_clear_frame_area,
  wgpu_clear_under_internal_border,
  wgpu_draw_window_cursor,
  wgpu_draw_vertical_window_border,
  wgpu_draw_window_divider,
  NULL, /* shift_glyphs_for_insert */
  wgpu_show_hourglass,
  wgpu_hide_hourglass,
  wgpu_default_font_parameter,
};

/* ------------------------------------------------------------------ */
/* Terminal hooks.                                                    */
/* ------------------------------------------------------------------ */

static void
wgpu_clear_frame (struct frame *f)
{
  float r, g, b;
  wgpu_unpack_pixel (FRAME_BACKGROUND_PIXEL (f), &r, &g, &b);
  /* Clear the whole surface, not just the frame: when the frame rounds down
     to whole rows/columns it is slightly smaller than the Wayland surface,
     and the leftover strip would otherwise show stale pixels.  */
  uint32_t sw = 0, sh = 0;
  wgpu_window_size (&sw, &sh);
  float w = (float) max ((int) sw, FRAME_PIXEL_WIDTH (f));
  float h = (float) max ((int) sh, FRAME_PIXEL_HEIGHT (f));
  block_input ();
  wgpu_window_rect (0.0f, 0.0f, w, h, r, g, b, 1.0f);
  unblock_input ();
}

static void
wgpu_update_begin (struct frame *f)
{
  block_input ();
  wgpu_window_begin ();
  unblock_input ();
}

static void
wgpu_frame_up_to_date (struct frame *f)
{
  block_input ();
  wgpu_window_present ();
  unblock_input ();
}

/* Drain Wayland events.  M2: only window lifecycle (configure/close); keyboard
   input is M3.  */
/* Find a live wgpu frame on TERMINAL (M2: there is a single one).  */
static struct frame *
wgpu_any_frame (struct terminal *terminal)
{
  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WGPU_P (f) && FRAME_LIVE_P (f)
	  && FRAME_TERMINAL (f) == terminal)
	return f;
    }
  return NULL;
}

/* Update which frame is highlighted (focused).  The cursor is drawn solid
   only on the highlight frame; others get a hollow cursor (xdisp.c checks
   f != dpyinfo->highlight_frame).  */
static void
wgpu_frame_rehighlight (struct wgpu_display_info *dpyinfo)
{
  struct frame *old_highlight = dpyinfo->highlight_frame;

  if (dpyinfo->x_focus_frame)
    {
      dpyinfo->highlight_frame
	= (FRAMEP (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame))
	   ? XFRAME (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame))
	   : dpyinfo->x_focus_frame);
      if (!FRAME_LIVE_P (dpyinfo->highlight_frame))
	{
	  fset_focus_frame (dpyinfo->x_focus_frame, Qnil);
	  dpyinfo->highlight_frame = dpyinfo->x_focus_frame;
	}
    }
  else
    dpyinfo->highlight_frame = NULL;

  if (old_highlight != dpyinfo->highlight_frame)
    {
      /* Redraw the cursor on both frames so it flips solid/hollow.  */
      if (old_highlight)
	gui_update_cursor (old_highlight, true);
      if (dpyinfo->highlight_frame)
	gui_update_cursor (dpyinfo->highlight_frame, true);
    }
}

/* Generic hook wrapper.  */
static void
wgpu_frame_rehighlight_hook (struct frame *f)
{
  wgpu_frame_rehighlight (FRAME_DISPLAY_INFO (f));
}

static void
wgpu_new_focus_frame (struct wgpu_display_info *dpyinfo, struct frame *frame)
{
  if (frame != dpyinfo->x_focus_frame)
    dpyinfo->x_focus_frame = frame;
  wgpu_frame_rehighlight (dpyinfo);
}

static int
wgpu_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  enum { BATCH = 64 };
  WgpuEvent evs[BATCH];
  uint32_t ww = 0, wh = 0;

  block_input ();
  wgpu_window_dispatch ();		/* M3: close -> DELETE_WINDOW_EVENT.  */
  bool resized = wgpu_window_take_resize (&ww, &wh) != 0;
  int nev = wgpu_window_poll_events (evs, BATCH);
  unblock_input ();

  struct frame *f = wgpu_any_frame (terminal);
  int count = 0;

  if (f && resized && ww >= 16 && wh >= 16)
    {
      int tw = FRAME_PIXEL_TO_TEXT_WIDTH (f, (int) ww);
      int th = FRAME_PIXEL_TO_TEXT_HEIGHT (f, (int) wh);
      /* Whole rows/columns only, so the mode line stays on a row boundary
	 (avoids a fractional last row overlapping it).  */
      int lh = FRAME_LINE_HEIGHT (f), cw = FRAME_COLUMN_WIDTH (f);
      if (lh > 0)
	th -= th % lh;
      if (cw > 0)
	tw -= tw % cw;
      change_frame_size (f, tw, th, false, true, false);
      SET_FRAME_GARBAGED (f);
    }

  for (int i = 0; f && i < nev; i++)
    {
      int mods = 0;
      uint32_t b = evs[i].modifiers;
      if (b & WGPU_MOD_CTRL)
	mods |= ctrl_modifier;
      if (b & WGPU_MOD_ALT)
	mods |= meta_modifier;
      if (b & WGPU_MOD_LOGO)
	mods |= super_modifier;

      switch (evs[i].kind)
	{
	case WgpuEventKind_FocusIn:
	  {
	    struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    if (dpyinfo->x_focus_event_frame != f)
	      {
		wgpu_new_focus_frame (dpyinfo, f);
		dpyinfo->x_focus_event_frame = f;
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = FOCUS_IN_EVENT;
		XSETFRAME (ie.frame_or_window, f);
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	  }
	  break;

	case WgpuEventKind_FocusOut:
	  {
	    struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    if (dpyinfo->x_focus_event_frame == f)
	      {
		dpyinfo->x_focus_event_frame = NULL;
		wgpu_new_focus_frame (dpyinfo, NULL);
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = FOCUS_OUT_EVENT;
		XSETFRAME (ie.frame_or_window, f);
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	  }
	  break;

	case WgpuEventKind_PointerMotion:
	  {
	    /* Drive hover highlighting (mouse-face, help-echo, buttons).  */
	    struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    int mx = evs[i].x, my = evs[i].y;
	    dpyinfo->last_mouse_movement_time = evs[i].time;
	    dpyinfo->last_mouse_motion_frame = f;
	    dpyinfo->last_mouse_motion_x = mx;
	    dpyinfo->last_mouse_motion_y = my;
	    XRectangle *r = &dpyinfo->last_mouse_glyph;
	    if (f != dpyinfo->last_mouse_glyph_frame
		|| mx < r->x || mx >= r->x + r->width
		|| my < r->y || my >= r->y + r->height)
	      {
		f->mouse_moved = true;
		note_mouse_highlight (f, mx, my);
		remember_mouse_glyph (f, mx, my, r);
		dpyinfo->last_mouse_glyph_frame = f;
	      }
	  }
	  break;

	case WgpuEventKind_PointerPress:
	case WgpuEventKind_PointerRelease:
	  {
	    struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    bool press = (evs[i].kind == WgpuEventKind_PointerPress);
	    struct input_event ie;
	    EVENT_INIT (ie);
	    ie.kind = MOUSE_CLICK_EVENT;
	    ie.code = evs[i].button;
	    ie.timestamp = evs[i].time;
	    ie.modifiers = mods | (press ? down_modifier : up_modifier);
	    XSETINT (ie.x, evs[i].x);
	    XSETINT (ie.y, evs[i].y);
	    XSETFRAME (ie.frame_or_window, f);
	    if (press)
	      {
		dpyinfo->grabbed |= (1 << evs[i].button);
		dpyinfo->last_mouse_frame = f;
	      }
	    else
	      dpyinfo->grabbed &= ~(1 << evs[i].button);
	    kbd_buffer_store_event_hold (&ie, hold_quit);
	    count++;
	  }
	  break;

	case WgpuEventKind_PointerAxis:
	  {
	    /* Vertical takes precedence; emit one wheel event per notch.  */
	    int steps = evs[i].axis_y != 0 ? evs[i].axis_y : evs[i].axis_x;
	    bool horiz = (evs[i].axis_y == 0 && evs[i].axis_x != 0);
	    int n = eabs (steps);
	    if (n > 10)
	      n = 10;
	    for (int s = 0; s < n; s++)
	      {
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = horiz ? HORIZ_WHEEL_EVENT : WHEEL_EVENT;
		ie.timestamp = evs[i].time;
		ie.modifiers = mods | (steps > 0 ? down_modifier : up_modifier);
		XSETINT (ie.x, evs[i].x);
		XSETINT (ie.y, evs[i].y);
		XSETFRAME (ie.frame_or_window, f);
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	  }
	  break;

	case WgpuEventKind_KeyPress:
	default:
	  {
	    struct input_event ie;
	    EVENT_INIT (ie);
	    XSETFRAME (ie.frame_or_window, f);
	    uint32_t cp = evs[i].unichar, ks = evs[i].keysym;
	    /* Treat only genuinely printable codepoints as text; control
	       chars (Backspace -> ^H, Tab, Return, Escape, ...) must go
	       through the keysym path so they map to <backspace>, <tab>, etc.
	       rather than C-h, C-i, ...  */
	    bool printable = cp >= 32 && cp != 127;
	    if (printable && mods == 0)
	      {
		/* Plain printable text (shift already applied).  */
		ie.kind = (cp < 128) ? ASCII_KEYSTROKE_EVENT
			  : MULTIBYTE_CHAR_KEYSTROKE_EVENT;
		ie.code = cp;
	      }
	    else if (ks >= 0x20 && ks <= 0x7e)
	      {
		/* Printable base key with modifiers, e.g. C-x.  */
		ie.kind = ASCII_KEYSTROKE_EVENT;
		ie.code = ks;
		ie.modifiers = mods;
	      }
	    else
	      {
		/* Function/navigation key: the xkb keysym matches X, which
		   keyboard.c maps to a Lisp symbol.  */
		ie.kind = NON_ASCII_KEYSTROKE_EVENT;
		ie.code = ks;
		ie.modifiers = mods;
	      }
	    kbd_buffer_store_event_hold (&ie, hold_quit);
	    count++;
	  }
	  break;
	}
    }

  return count;
}

static void
wgpu_delete_terminal (struct terminal *terminal)
{
  wgpu_window_close ();
  wgpu_backend_shutdown ();
}

/* Warp the pointer.  M3 stub (no pointer warping yet).  */
void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  (void) f; (void) pix_x; (void) pix_y;
}

/* Report the last known pointer position (from motion events) to the core.
   Used by mouse-position and friends.  */
static void
wgpu_mouse_position (struct frame **fp, int insist, Lisp_Object *bar_window,
		     enum scroll_bar_part *part, Lisp_Object *x, Lisp_Object *y,
		     Time *timestamp)
{
  struct frame *f = *fp;
  if (!f || !FRAME_WGPU_P (f))
    return;
  struct wgpu_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  *bar_window = Qnil;
  *part = (enum scroll_bar_part) 0;
  if (dpyinfo->last_mouse_motion_frame)
    *fp = dpyinfo->last_mouse_motion_frame;
  XSETINT (*x, dpyinfo->last_mouse_motion_x);
  XSETINT (*y, dpyinfo->last_mouse_motion_y);
  *timestamp = dpyinfo->last_mouse_movement_time;
}

/* No X resource database; return no value for every query.  */
static const char *
wgpu_get_string_resource (void *rdb, const char *name, const char *class)
{
  return NULL;
}

/* Install FONT_OBJECT as frame F's font and recompute the char metrics.  */
static Lisp_Object
wgpu_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct font *font = XFONT_OBJECT (font_object);
  int font_ascent, font_descent;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;

  if (FRAME_FONT (f) == font)
    return font_object;

  FRAME_FONT (f) = font;
  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = font->average_width;
  get_font_ascent_descent (font, &font_ascent, &font_descent);
  FRAME_LINE_HEIGHT (f) = font_ascent + font_descent;
  FRAME_TAB_BAR_HEIGHT (f) = FRAME_TAB_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);

  {
    int wid = FRAME_COLUMN_WIDTH (f);
    FRAME_CONFIG_SCROLL_BAR_COLS (f) = (14 + wid - 1) / wid;
    int height = FRAME_LINE_HEIGHT (f);
    FRAME_CONFIG_SCROLL_BAR_LINES (f) = (14 + height - 1) / height;
  }

  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 3, false, Qfont);
  return font_object;
}

struct terminal *
wgpu_create_terminal (struct wgpu_display_info *dpyinfo)
{
  struct terminal *terminal
    = create_terminal (output_wgpu, &wgpu_redisplay_interface);

  terminal->display_info.wgpu = dpyinfo;
  dpyinfo->terminal = terminal;

  terminal->clear_frame_hook = wgpu_clear_frame;
  terminal->update_begin_hook = wgpu_update_begin;
  terminal->read_socket_hook = wgpu_read_socket;
  terminal->mouse_position_hook = wgpu_mouse_position;
  terminal->frame_rehighlight_hook = wgpu_frame_rehighlight_hook;
  terminal->frame_up_to_date_hook = wgpu_frame_up_to_date;
  terminal->delete_terminal_hook = wgpu_delete_terminal;
  terminal->get_string_resource_hook = wgpu_get_string_resource;
  terminal->set_new_font_hook = wgpu_new_font;
  terminal->defined_color_hook = wgpu_defined_color;
  terminal->query_colors = wgpu_query_colors;
  terminal->query_frame_background_color = wgpu_query_frame_background_color;

  /* Define the standard fringe bitmaps (continuation/truncation arrows,
     empty-line and buffer-boundary indicators).  */
  gui_init_fringe (terminal->rif);

  /* M3+: visibility/size/scroll-bar/menu hooks.  */
  return terminal;
}

struct wgpu_display_info *
wgpu_term_init (Lisp_Object display_name)
{
  if (wgpu_backend_init () != WGPU_OK)
    error ("wgpu backend failed to initialize");
  if (wgpu_window_open (NULL) != 0)
    error ("wgpu: cannot open a Wayland window (is WAYLAND_DISPLAY set?)");

  block_input ();

  struct wgpu_display_info *dpyinfo = xzalloc (sizeof *dpyinfo);
  struct terminal *terminal = wgpu_create_terminal (dpyinfo);

  terminal->kboard = allocate_kboard (Qwgpu);
  /* Don't let the initial kboard remain current longer than necessary.  */
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;
  terminal->kboard->reference_count++;

  /* Normalize font sizes for HiDPI: a scale-2 display gets 192 DPI so a
     10pt font renders ~26px instead of ~13px.  */
  int scale = wgpu_window_scale ();
  if (scale < 1)
    scale = 1;
  dpyinfo->name_list_element = Fcons (display_name, Qnil);
  /* Load the X11 color-name database so named face colors resolve.  */
  if (NILP (wgpu_color_map))
    wgpu_color_map
      = Fx_load_color_file (Fexpand_file_name (build_string ("rgb.txt"),
					       Vdata_directory));
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;
  dpyinfo->resx = 96.0;
  dpyinfo->resy = 96.0;
  dpyinfo->scale = scale;
  reset_mouse_highlight (&dpyinfo->mouse_highlight);

  terminal->name = xlispstrdup (display_name);

  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  int fd = wgpu_window_fd ();
  if (fd >= 0)
    add_keyboard_wait_descriptor (fd);
  /* Also wake on the key-repeat timerfd so held keys repeat.  */
  int tfd = wgpu_window_timer_fd ();
  if (tfd >= 0)
    add_keyboard_wait_descriptor (tfd);

  unblock_input ();
  return dpyinfo;
}

void
syms_of_wgputerm (void)
{
  staticpro (&wgpu_color_map);
  wgpu_color_map = Qnil;

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = 1;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = 0;

  Fprovide (Qwgpu, Qnil);
}
