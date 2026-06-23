/* Wayland + wlshm terminal backend for Emacs -- terminal/redisplay shim.

This wires the Emacs redisplay interface (RIF) and terminal hooks to the Rust
backend over FFI (wlshm_ffi.h): RIF draw hooks paint fills/glyphs with CPU
Cairo onto a persistent image-surface canvas, and frame_up_to_date presents
that canvas via wl_shm.  Single-threaded model; structurally mirrors
pgtkterm.c.  See wlshm-backend-plan.md.  */

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
#include <stdarg.h>
#include <math.h>		/* lround for HiDPI logical<->physical scaling */
#include "font.h"
#include "fontset.h"
#include <cairo-ft.h>		/* FcPattern + cairo-ft, prereqs for ftfont.h */
#include "ftfont.h"		/* struct font_info -> cr_scaled_font (for menus) */
#include "composite.h"
#include "menu.h"		/* MENU_KEYMAPS */
#include "coding.h"		/* ENCODE_UTF_8 */
#include <poll.h>
#include "systime.h"
#include "wlshmterm.h"

/* Chain of all wlshm displays.  */
struct wlshm_display_info *x_display_list;

/* ------------------------------------------------------------------ */
/* Debug logging (enable with the WLSHM_DEBUG env var).  Writes to        */
/* /tmp/wlshm-debug.log with a monotonic timestamp, flushed per line so a  */
/* crash/freeze preserves the last entry.  Shared with the Rust side.     */
/* ------------------------------------------------------------------ */
static int wlshm_log_state = -1;	/* -1 unknown, 0 off, 1 on */
static FILE *wlshm_log_fp;

static bool
wlshm_log_enabled (void)
{
  if (wlshm_log_state < 0)
    {
      wlshm_log_state = getenv ("WLSHM_DEBUG") ? 1 : 0;
      if (wlshm_log_state)
	wlshm_log_fp = fopen ("/tmp/wlshm-debug.log", "a");
    }
  return wlshm_log_state == 1 && wlshm_log_fp != NULL;
}

static void wlshm_log (const char *fmt, ...) ATTRIBUTE_FORMAT_PRINTF (1, 2);
static void
wlshm_log (const char *fmt, ...)
{
  if (!wlshm_log_enabled ())
    return;
  struct timespec t = current_timespec ();
  fprintf (wlshm_log_fp, "[%ld.%03ld C] ", t.tv_sec,
	   t.tv_nsec / 1000000);
  va_list ap;
  va_start (ap, fmt);
  vfprintf (wlshm_log_fp, fmt, ap);
  va_end (ap);
  fputc ('\n', wlshm_log_fp);
  fflush (wlshm_log_fp);
}

/* Alist of (NAME . PIXEL) X11 color names, loaded from rgb.txt, used to
   resolve named face colors (e.g. "red", "grey75").  staticpro'd so it
   survives GC.  PIXEL is 0xRRGGBB, matching our pixel encoding.  */
static Lisp_Object wlshm_color_map;

/* Human-readable keysym name.  Returns the keysym's decimal value.  */
char *
get_keysym_name (int keysym)
{
  static char value[16];
  sprintf (value, "%d", keysym);
  return value;
}

/* Unpack a backend pixel (0xRRGGBB) into linear 0..1 RGB.  */
void
wlshm_unpack_pixel (unsigned long pixel, float *r, float *g, float *b)
{
  *r = ((pixel >> 16) & 0xff) / 255.0f;
  *g = ((pixel >> 8) & 0xff) / 255.0f;
  *b = (pixel & 0xff) / 255.0f;
}

/* ------------------------------------------------------------------ */
/* CPU rendering: a persistent Cairo image-surface "canvas".            */
/*                                                                      */
/* Emacs draws each frame incrementally onto this canvas via the RIF.   */
/* frame_up_to_date copies it into a wl_shm buffer (wlshm_window_present).*/
/* CAIRO_FORMAT_RGB24 == XRGB8888 (native-endian), which is exactly the  */
/* wl_shm format we present, so the copy is a straight memcpy in Rust.   */
/* Multi-window model: the canvas/cr live per-frame in struct wlshm_output.
   `wlshm_cur' names the frame currently being drawn; it is set at every RIF
   entry point (and by wlshm_begin_cr_clip).  The file-static wlshm_canvas /
   wlshm_cr below are convenience ALIASES that wlshm_ensure_canvas refreshes to
   point at wlshm_cur's per-frame canvas, so the many drawing helpers that use
   them are unchanged.  */
/* ------------------------------------------------------------------ */
static struct frame *wlshm_cur;
static cairo_surface_t *wlshm_canvas;
static cairo_t *wlshm_cr;
static int wlshm_canvas_w, wlshm_canvas_h;

/* The frame whose canvas the drawing helpers should target.  */
static struct frame *
wlshm_canvas_frame (void)
{
  /* Also require FRAME_X_OUTPUT: during teardown a frame can be live but have
     its output transiently freed, and wlshm_ensure_canvas would then deref a
     NULL output.  Mirrors the selected-frame fallback check below.  */
  if (wlshm_cur && FRAME_LIVE_P (wlshm_cur) && FRAME_WLSHM_P (wlshm_cur)
      && FRAME_X_OUTPUT (wlshm_cur))
    return wlshm_cur;
  struct frame *sf = SELECTED_FRAME ();
  if (sf && FRAME_WLSHM_P (sf) && FRAME_X_OUTPUT (sf))
    return sf;
  return NULL;
}

/* Device (HiDPI) scale of frame F as a double (1.0 == no scaling).  Mirrors
   pgtk_frame_scale_factor: FRAME_SCALE_FACTOR uses it so SVG images rasterize
   at the physical resolution and stay crisp on the device-scaled canvas.  */
double
wlshm_frame_scale_factor (struct frame *f)
{
  uint32_t scale120 = wlshm_window_scale120 (WLSHM_FRAME_HANDLE (f));
  return scale120 < 120 ? 1.0 : (double) scale120 / 120.0;
}

/* Ensure wlshm_cur's per-frame canvas matches its Wayland surface size, and
   point the wlshm_canvas/wlshm_cr aliases at it.  */
static void
wlshm_ensure_canvas (void)
{
  struct frame *f = wlshm_canvas_frame ();
  if (!f)
    {
      wlshm_canvas = NULL;
      wlshm_cr = NULL;
      return;
    }
  struct wlshm_output *o = FRAME_X_OUTPUT (f);
  /* W/H are LOGICAL pixels (the xdg configure size Emacs works in).  */
  uint32_t w = 0, h = 0;
  wlshm_window_size (WLSHM_FRAME_HANDLE (f), &w, &h);
  if (w == 0 || h == 0)
    {
      w = 800;
      h = 600;
    }
  /* HiDPI: the wl_shm buffer is allocated at PHYSICAL pixels
     (round(logical * scale)); the compositor's wp_viewport (fractional path)
     or set_buffer_scale (integer fallback) maps it back to the logical surface
     size.  Cairo gets a device scale so every drawing call keeps using logical
     coordinates yet rasterizes crisply at the physical resolution (ftcrfont's
     cairo glyph path honors the device scale).  scale120 == 120 -> 1.0, i.e.
     byte-identical to the non-HiDPI path.  */
  uint32_t scale120 = wlshm_window_scale120 (WLSHM_FRAME_HANDLE (f));
  if (scale120 < 120)
    scale120 = 120;
  double scale = (double) scale120 / 120.0;
  /* CEIL, not round: the wp_viewport SOURCE rect is the EXACT fractional
     logical*scale (set on the Rust side) and must lie within the buffer, so
     the buffer must be at least that big.  ceil also guarantees we never crop
     painted content.  The <=1px slack column/row at the far edge is unpainted
     frame background, excluded by the viewport source crop.  At scale 1.0 this
     equals the logical size (byte-identical to the non-HiDPI path).  */
  int pw = (int) ceil ((double) w * scale);
  int ph = (int) ceil ((double) h * scale);
  if (pw < 1)
    pw = 1;
  if (ph < 1)
    ph = 1;
  if (!o->canvas || o->canvas_w != pw || o->canvas_h != ph)
    {
      if (o->cr)
	{
	  cairo_destroy (o->cr);
	  o->cr = NULL;
	}
      if (o->canvas)
	{
	  cairo_surface_destroy (o->canvas);
	  o->canvas = NULL;
	}
      o->canvas = cairo_image_surface_create (CAIRO_FORMAT_RGB24, pw, ph);
      /* Cairo returns a nil surface (NULL data) at huge physical sizes or under
	 wl_shm/memory pressure; bail rather than later memcpy'ing from NULL.  */
      if (cairo_surface_status (o->canvas) != CAIRO_STATUS_SUCCESS)
	{
	  cairo_surface_destroy (o->canvas);
	  o->canvas = NULL;
	  wlshm_canvas = NULL;
	  wlshm_cr = NULL;
	  return;
	}
      /* Device scale: logical coords -> physical pixels.  At scale 1.0 this is
	 the identity, leaving the scale-1 output byte-identical.  */
      cairo_surface_set_device_scale (o->canvas, scale, scale);
      o->cr = cairo_create (o->canvas);
      if (cairo_status (o->cr) != CAIRO_STATUS_SUCCESS)
	{
	  cairo_destroy (o->cr);
	  o->cr = NULL;
	  cairo_surface_destroy (o->canvas);
	  o->canvas = NULL;
	  wlshm_canvas = NULL;
	  wlshm_cr = NULL;
	  return;
	}
      /* Sharp (non-antialiased) shape rasterization for fills/clips/rects.
	 With a fractional device scale, AA'd rect/clip edges land on half
	 physical pixels and bleed into faint outlines around glyph-cell
	 backgrounds and the cursor.  Glyphs use cairo_show_glyphs (the scaled
	 font's own AA), so text stays smooth.  At scale 1.0 (integer coords)
	 this is byte-identical to the AA default.  */
      cairo_set_antialias (o->cr, CAIRO_ANTIALIAS_NONE);
      /* A fresh Cairo image surface is zero-filled == opaque BLACK.  If an early
	 or partial present ships before redisplay has painted every region --
	 notably a child frame (corfu popup) just shown or just grown, whose first
	 paint may be partial -- the unpainted area would composite as a black
	 rectangle.  Pre-fill the new canvas with the frame background so any such
	 gap shows the background instead, and gets painted over normally.  */
      {
	float br, bgc, bb;
	wlshm_unpack_pixel (FRAME_BACKGROUND_PIXEL (f), &br, &bgc, &bb);
	cairo_save (o->cr);
	cairo_set_source_rgb (o->cr, br, bgc, bb);
	cairo_set_operator (o->cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint (o->cr);
	cairo_restore (o->cr);
      }
      o->canvas_w = pw;
      o->canvas_h = ph;
    }
  wlshm_canvas = o->canvas;
  wlshm_cr = o->cr;
  wlshm_canvas_w = o->canvas_w;
  wlshm_canvas_h = o->canvas_h;
}

/* Push frame F's canvas to the compositor via the Rust wl_shm present.  */
static void
wlshm_present_canvas (struct frame *f)
{
  wlshm_cur = f;
  wlshm_ensure_canvas ();
  if (!wlshm_canvas)
    return;
  cairo_surface_flush (wlshm_canvas);
  wlshm_window_present (WLSHM_FRAME_HANDLE (f),
			cairo_image_surface_get_data (wlshm_canvas),
			(uint32_t) cairo_image_surface_get_width (wlshm_canvas),
			(uint32_t) cairo_image_surface_get_height (wlshm_canvas),
			(uint32_t) cairo_image_surface_get_stride (wlshm_canvas),
			0, 0, 0, 0);
}

/* Public wrapper: present frame F's canvas (used by the tooltip code, which
   force-draws its tip frame outside the normal redisplay).  */
void
wlshm_present_frame (struct frame *f)
{
  wlshm_present_canvas (f);
}

/* Write frame F's canvas to PATH as a PNG (golden/visual test harness).  */
bool
wlshm_dump_canvas_png (struct frame *f, const char *path)
{
  wlshm_cur = f;
  wlshm_ensure_canvas ();
  if (!wlshm_canvas)
    return false;
  cairo_surface_flush (wlshm_canvas);
  return cairo_surface_write_to_png (wlshm_canvas, path) == CAIRO_STATUS_SUCCESS;
}

/* Solid rectangle fill on the canvas.  Keeps the historical name/signature
   of the old GPU FFI so the many RIF call sites are unchanged; colors are
   0..1 sRGB device values (from wlshm_unpack_pixel).  */
static void
wlshm_window_rect (float x, float y, float w, float h,
		  float r, float g, float b, float a)
{
  wlshm_ensure_canvas ();
  if (!wlshm_cr)
    return;
  cairo_save (wlshm_cr);
  cairo_set_operator (wlshm_cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (wlshm_cr, r, g, b, a);
  cairo_rectangle (wlshm_cr, x, y, w, h);
  cairo_fill (wlshm_cr);
  cairo_restore (wlshm_cr);
}

/* Fill an integer PHYSICAL-pixel rectangle at full coverage, bypassing the canvas
   device scale.  Box/relief edges must land exactly on the physical grid with a
   uniform integer thickness so all four edges match in thickness AND opacity at
   any scale.  A 1px-logical edge drawn in logical coords at scale 1.5 renders as
   ~1.5 physical px whose antialiased coverage varies with sub-pixel phase -- so,
   e.g., the right edge of an even-width box comes out lighter than the left.
   Snapping to whole physical pixels (drawn under ANTIALIAS_NONE, with the device
   scale temporarily undone so user space == physical pixels) avoids both the
   uneven thickness and the uneven opacity.  At integer scale it is identical to a
   plain logical fill.  */
static void
wlshm_fill_phys (int px, int py, int pw, int ph, float r, float g, float b)
{
  wlshm_ensure_canvas ();
  if (!wlshm_cr || pw <= 0 || ph <= 0)
    return;
  double sx = 1.0, sy = 1.0;
  cairo_surface_get_device_scale (wlshm_canvas, &sx, &sy);
  cairo_save (wlshm_cr);
  cairo_scale (wlshm_cr, 1.0 / sx, 1.0 / sy);
  cairo_set_antialias (wlshm_cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_operator (wlshm_cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgb (wlshm_cr, r, g, b);
  cairo_rectangle (wlshm_cr, px, py, pw, ph);
  cairo_fill (wlshm_cr);
  cairo_restore (wlshm_cr);
}

/* Blend two packed pixels (defined later; also used by the scroll bar).  */
static unsigned long wlshm_blend_pixel (unsigned long a, unsigned long b,
					double t);

/* Solid rectangle fill in a packed pixel (defined later; used widely above its
   definition).  */
static void wlshm_fill_rect_pixel (int x, int y, int w, int h,
				   unsigned long pixel);

/* 1px-thick rectangle outline in a packed pixel (defined below).  */
static void wlshm_draw_box_outline (unsigned long color, int x, int y,
				    int w, int h);

/* Strength of the mouse-face hover tint: how far the background is blended
   toward the foreground.  */
#define WLSHM_HOVER_BLEND 0.18

/* Draw the hover background for a mouse-face highlighted glyph string S: a
   clean flat (square) fill of the run's background BG tinted toward the
   foreground FG, replacing the face's beveled box.  Filling the glyph string's
   own cell makes adjacent strings of one run tile into a single solid button.
   Square edges (AA-none canvas) keep it crisp.  */
static void
wlshm_draw_mouse_face_bg (struct glyph_string *s, unsigned long bg,
			  unsigned long fg)
{
  int box_line = max (s->face->box_horizontal_line_width, 0);
  unsigned long tint = wlshm_blend_pixel (bg, fg, WLSHM_HOVER_BLEND);
  /* A user box wider than half the line height makes height go negative; clamp
     so we don't paint an inverted/oversized tint.  */
  int fill_h = max (s->height - 2 * box_line, 0);
  wlshm_fill_rect_pixel (s->x, s->y + box_line, s->background_width, fill_h,
			 tint);
}

/* Cairo clip/source helpers used by ftcrfont.c for glyph drawing.  */
cairo_t *
wlshm_begin_cr_clip (struct frame *f)
{
  wlshm_cur = f;
  wlshm_ensure_canvas ();
  if (wlshm_cr)
    cairo_save (wlshm_cr);
  return wlshm_cr;
}

void
wlshm_end_cr_clip (struct frame *f)
{
  if (wlshm_cr)
    cairo_restore (wlshm_cr);
}

void
wlshm_set_cr_source_with_color (struct frame *f, unsigned long color,
			       bool respects_alpha_background)
{
  float r, g, b;
  wlshm_unpack_pixel (color, &r, &g, &b);
  if (!wlshm_cr)
    return;
  cairo_set_source_rgb (wlshm_cr, r, g, b);
  cairo_set_operator (wlshm_cr, CAIRO_OPERATOR_OVER);
}

/* ------------------------------------------------------------------ */
/* Colors.                                                            */
/* ------------------------------------------------------------------ */

bool
wlshm_defined_color (struct frame *f, const char *name, Emacs_Color *color,
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
  for (Lisp_Object tail = wlshm_color_map; CONSP (tail); tail = XCDR (tail))
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
wlshm_query_colors (struct frame *f, Emacs_Color *colors, int ncolors)
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
wlshm_query_frame_background_color (struct frame *f, Emacs_Color *bgcolor)
{
  bgcolor->pixel = FRAME_BACKGROUND_PIXEL (f);
  wlshm_query_colors (f, bgcolor, 1);
}

/* ------------------------------------------------------------------ */
/* Redisplay interface.                                               */
/* ------------------------------------------------------------------ */

/* Choose the foreground/background pixels for glyph string S according to its
   highlight kind (s->hl).  Shared with the font driver so glyphs use the same
   colors as the background fill.  */
void
wlshm_glyph_string_colors (struct glyph_string *s, unsigned long *fg,
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

/* Scale a packed pixel toward white (factor>1) or black (factor<1) for the
   3D relief edges.  */
static unsigned long
wlshm_scale_pixel (unsigned long p, double factor)
{
  int r = (int) (((p >> 16) & 0xff) * factor);
  int g = (int) (((p >> 8) & 0xff) * factor);
  int b = (int) ((p & 0xff) * factor);
  r = r > 255 ? 255 : r;
  g = g > 255 ? 255 : g;
  b = b > 255 ? 255 : b;
  return ((unsigned long) r << 16) | ((unsigned long) g << 8) | b;
}

/* Compute a 3D-relief shadow color from packed pixel P: scale by FACTOR
   (>1 lighter, <1 darker) and, for dark colors, add an extra DELTA boost so
   the relief stays visible on mid-tone backgrounds.  Ported from
   pgtk_compute_lighter_color (computed in 16-bit color space so the result
   matches the other backends; DELTA is the 16-bit 0x8000/0x4000 amount).  */
static unsigned long
wlshm_relief_color (unsigned long p, double factor, int delta)
{
  long r = (long) ((p >> 16) & 0xff) * 257;	/* 8-bit -> 16-bit */
  long g = (long) ((p >> 8) & 0xff) * 257;
  long b = (long) (p & 0xff) * 257;
  long nr = min (0xffff, (long) (factor * r));
  long ng = min (0xffff, (long) (factor * g));
  long nb = min (0xffff, (long) (factor * b));
  long bright = (2 * r + 3 * g + b) / 6;
  /* HIGHLIGHT_COLOR_DARK_BOOST_LIMIT (== 48000), as in the other backends.  */
  if (bright < 48000)
    {
      double dimness = 1 - (double) bright / 48000;
      int min_delta = (int) (delta * dimness * factor / 2);
      if (factor < 1)
	{
	  nr = max (0, nr - min_delta);
	  ng = max (0, ng - min_delta);
	  nb = max (0, nb - min_delta);
	}
      else
	{
	  nr = min (0xffff, nr + min_delta);
	  ng = min (0xffff, ng + min_delta);
	  nb = min (0xffff, nb + min_delta);
	}
    }
  return ((unsigned long) (nr >> 8) << 16
	  | (unsigned long) (ng >> 8) << 8
	  | (unsigned long) (nb >> 8));
}

/* Draw a 1px-thick rectangle outline in COLOR as four edge rects: the top and
   bottom edges span width W (at Y and Y+H), the left and right edges span
   height H (at X and X+W).  Matches the open-coded outline idiom used for the
   glyphless/composite "no font" boxes.  */
static void
wlshm_draw_box_outline (unsigned long color, int x, int y, int w, int h)
{
  float r, g, b;
  wlshm_unpack_pixel (color, &r, &g, &b);
  wlshm_window_rect ((float) x, (float) y, (float) w, 1.0f, r, g, b, 1.0f);
  wlshm_window_rect ((float) x, (float) (y + h), (float) w, 1.0f, r, g, b, 1.0f);
  wlshm_window_rect ((float) x, (float) y, 1.0f, (float) h, r, g, b, 1.0f);
  wlshm_window_rect ((float) (x + w), (float) y, 1.0f, (float) h, r, g, b, 1.0f);
}

/* Draw the face box around glyph string S: a flat box for FACE_SIMPLE_BOX, or
   a 3D raised/sunken relief (light top/left, dark bottom/right, swapped when
   sunken) otherwise.  */
static void
wlshm_draw_glyph_string_box (struct glyph_string *s)
{
  if (s->face->box == FACE_NO_BOX)
    return;
  int hwidth = eabs (s->face->box_horizontal_line_width);
  int vwidth = eabs (s->face->box_vertical_line_width);
  if (hwidth == 0 && vwidth == 0)
    return;

  int left = s->x, top = s->y;
  int width = s->background_width, height = s->height;
  /* Draw the left/right edges at the box-run boundaries.  For a mouse-face
     highlight the underlying glyphs carry the box flags of the face beneath
     (e.g. the mode line's own box), so also close the box at the ends of the
     highlighted run -- where the neighbouring glyph string isn't mouse-face.
     Without this the hover box on mode-line elements has only top/bottom
     edges and looks unclosed.  Mirrors x_draw_glyph_string_box.  */
  bool left_p = (s->first_glyph->left_box_line_p
		 || (s->hl == DRAW_MOUSE_FACE
		     && (s->prev == NULL || s->prev->hl != s->hl)));
  /* For a composite/image glyph string the right box flag lives on first_glyph,
     not first_glyph[nchars-1] (mirrors pgtk's last_glyph selection).  */
  struct glyph *last_glyph = ((s->cmp || s->img)
			      ? s->first_glyph
			      : s->first_glyph + s->nchars - 1);
  bool right_p = ((s->nchars > 0 && last_glyph->right_box_line_p)
		  || (s->hl == DRAW_MOUSE_FACE
		      && (s->next == NULL || s->next->hl != s->hl)));

  unsigned long top_left, bottom_right;
  if (s->face->box == FACE_SIMPLE_BOX)
    top_left = bottom_right = s->face->box_color;
  else
    {
      /* Derive the relief shadows.  When the box was given an explicit :color
	 alongside a 3D :style (e.g. mode-line-highlight's grey40 released
	 button), shade that color rather than the face background, matching
	 pgtk_setup_relief_colors -- otherwise the relief is computed off the
	 wrong (often near-white) base and the edges wash out.  */
      unsigned long base = (s->face->use_box_color_for_shadows_p
			    ? s->face->box_color : s->face->background);
      unsigned long light = wlshm_relief_color (base, 1.2, 0x8000);
      unsigned long dark = wlshm_relief_color (base, 0.6, 0x4000);
      bool raised = (s->face->box == FACE_RAISED_BOX);
      top_left = raised ? light : dark;
      bottom_right = raised ? dark : light;
    }

  float tr, tg, tb, br, bg2, bb;
  wlshm_unpack_pixel (top_left, &tr, &tg, &tb);
  wlshm_unpack_pixel (bottom_right, &br, &bg2, &bb);
  block_input ();
  /* Snap the box rectangle to the physical pixel grid and give every edge the
     same integer physical thickness, so all four edges match in thickness AND
     opacity even at a fractional scale (a phase-dependent AA edge would otherwise
     come out lighter on, e.g., the right side of an even-width box).  bottom/right
     are anchored flush to the far snapped boundary.  */
  double sx = 1.0, sy = 1.0;
  cairo_surface_get_device_scale (wlshm_canvas, &sx, &sy);
  int pl = (int) lround (left * sx);
  int pr = (int) lround ((left + width) * sx);
  int pt = (int) lround (top * sy);
  int pb = (int) lround ((top + height) * sy);
  /* Clamp the snapped box to the active glyph-string clip.  At a fractional scale
     that clip can be up to a physical pixel shorter than the box (its logical
     bounds round down), so the bottom/right edge's outer row would otherwise be
     cut -- making the bottom thinner than the top.  Convert the clip's user-space
     extents to physical and pull the box inside them.  */
  {
    double cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    cairo_clip_extents (wlshm_cr, &cx0, &cy0, &cx1, &cy1);
    if (cx1 > cx0 && cy1 > cy0)
      {
	int clx1 = (int) floor (cx1 * sx), cly1 = (int) floor (cy1 * sy);
	/* Clamp ONLY the far (bottom/right) edges inward so they land inside the
	   clip -- that is the fractional-scale rounding the comment above is about.
	   Do NOT clamp the near (top/left) edges: at a fractional device scale the
	   clip's user-space top rounds UP (ceil), which would pull the box top edge
	   one physical row DOWN, uncovering the cell's top row.  Because the box
	   edges are drawn last and opaque (and Cairo clips any overdraw harmlessly),
	   anchoring top/left to the snapped box position lets the top edge fully
	   cover the cell top -- otherwise an incremental mode-line redraw leaves a
	   stale sliver of the previous glyph in that uncovered top row (the
	   "U:---" modified-indicator remnant on HiDPI).  */
	if (pr > clx1) pr = clx1;
	if (pb > cly1) pb = cly1;
      }
  }
  int th = (int) lround (hwidth * sy);
  if (hwidth > 0 && th < 1)
    th = 1;
  if (th > pb - pt)
    th = pb - pt;
  int tv = (int) lround (vwidth * sx);
  if (vwidth > 0 && tv < 1)
    tv = 1;
  if (tv > pr - pl)
    tv = pr - pl;
  if (hwidth > 0)
    {
      wlshm_fill_phys (pl, pt, pr - pl, th, tr, tg, tb);          /* top */
      wlshm_fill_phys (pl, pb - th, pr - pl, th, br, bg2, bb);    /* bottom */
    }
  if (left_p && vwidth > 0)
    wlshm_fill_phys (pl, pt, tv, pb - pt, tr, tg, tb);            /* left */
  if (right_p && vwidth > 0)
    wlshm_fill_phys (pr - tv, pt, tv, pb - pt, br, bg2, bb);      /* right */
  unblock_input ();
}

/* Draw the component glyphs of a composition at their composed positions
   (combining marks, ligatures, complex scripts).  Ported from
   pgtk_draw_composite_glyph_string_foreground.  */
static void
wlshm_draw_composite_glyph_string_foreground (struct glyph_string *s)
{
  int i, j, x;
  struct font *font = s->font;

  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  if (s->font_not_found_p)
    {
      /* Outline box for an unloadable composition.  */
      if (s->cmp_from == 0)
	{
	  int w = s->width - 1, h = s->height - 1;
	  wlshm_draw_box_outline (s->face->foreground, x, s->y, w, h);
	}
    }
  else if (!s->first_glyph->u.cmp.automatic)
    {
      int y = s->ybase;
      for (i = 0, j = s->cmp_from; i < s->nchars; i++, j++)
	if (COMPOSITION_GLYPH (s->cmp, j) != '\t')
	  {
	    int xx = x + s->cmp->offsets[j * 2];
	    int yy = y - s->cmp->offsets[j * 2 + 1];
	    font->driver->draw (s, j, j + 1, xx, yy, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, j + 1, xx + 1, yy, false);
	  }
    }
  else
    {
      Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);
      Lisp_Object glyph;
      int y = s->ybase;
      int width = 0;

      for (i = j = s->cmp_from; i < s->cmp_to; i++)
	{
	  glyph = LGSTRING_GLYPH (gstring, i);
	  if (NILP (LGLYPH_ADJUSTMENT (glyph)))
	    width += LGLYPH_WIDTH (glyph);
	  else
	    {
	      int xoff, yoff, wadjust;
	      if (j < i)
		{
		  font->driver->draw (s, j, i, x, y, false);
		  if (s->face->overstrike)
		    font->driver->draw (s, j, i, x + 1, y, false);
		  x += width;
		}
	      xoff = LGLYPH_XOFF (glyph);
	      yoff = LGLYPH_YOFF (glyph);
	      wadjust = LGLYPH_WADJUST (glyph);
	      font->driver->draw (s, i, i + 1, x + xoff, y + yoff, false);
	      if (s->face->overstrike)
		font->driver->draw (s, i, i + 1, x + xoff + 1, y + yoff, false);
	      x += wadjust;
	      j = i + 1;
	      width = 0;
	    }
	}
      if (j < i)
	{
	  font->driver->draw (s, j, i, x, y, false);
	  if (s->face->overstrike)
	    font->driver->draw (s, j, i, x + 1, y, false);
	}
    }
}

/* Draw a smooth wavy line in COLOR, mirroring pgtk_draw_horizontal_wave: a
   single antialiased stroked zigzag path rather than a staircase of 1px rects,
   so it stays smooth (not pixelated) at a fractional device scale.  */
static void
wlshm_draw_horizontal_wave (struct frame *f, unsigned long color, int x, int y,
			    int width, int height, int wave_length)
{
  cairo_t *cr = wlshm_begin_cr_clip (f);
  if (!cr)
    return;
  double dx = wave_length, dy = height - 1;
  int xoffset, n;
  float r, g, b;

  /* The canvas defaults to CAIRO_ANTIALIAS_NONE so axis-aligned rect fills stay
     sharp at fractional scale; the wave is a diagonal stroke and needs
     antialiasing to look smooth.  The enclosing cr_clip save restores it.  */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);
  wlshm_unpack_pixel (color, &r, &g, &b);
  cairo_set_source_rgb (cr, r, g, b);
  cairo_rectangle (cr, x, y, width, height);
  cairo_clip (cr);

  if (x >= 0)
    {
      xoffset = x % (wave_length * 2);
      if (xoffset == 0)
	xoffset = wave_length * 2;
    }
  else
    xoffset = x % (wave_length * 2) + wave_length * 2;
  n = (width + xoffset) / wave_length + 1;
  if (xoffset > wave_length)
    {
      xoffset -= wave_length;
      --n;
      y += height - 1;
      dy = -dy;
    }

  cairo_move_to (cr, x - xoffset + 0.5, y + 0.5);
  while (--n >= 0)
    {
      cairo_rel_line_to (cr, dx, dy);
      dy = -dy;
    }
  cairo_set_line_width (cr, 1);
  cairo_stroke (cr);
  wlshm_end_cr_clip (f);
}

/* Draw the underline for glyph string S in COLOR, honoring the face's
   underline style (single/double/wave/dots/dashes).  Dotted/dashed styles are
   axis-aligned rects (sharp under the canvas's ANTIALIAS_NONE); the wave is a
   smooth antialiased stroke via wlshm_draw_horizontal_wave.  */
static void
wlshm_draw_underline (struct glyph_string *s, unsigned long color)
{
  /* Compute the underline thickness and position the way the other backends
     do.  xdisp.c does NOT fill in s->underline_thickness / s->underline_position
     for us, so reading them directly gave 0 every time -- every underline drew
     1px at ybase+1, ignoring the font's metrics and with no clamp against the
     row bottom.  Honor the font metrics, the at-descent-line options, and clamp
     so the underline can't spill into the line below.  Ported from
     pgtk_draw_glyph_string's underline block.  */
  int thickness, position;
  if (s->prev
      && (s->prev->face->underline != FACE_UNDERLINE_WAVE
	  && s->prev->face->underline >= FACE_UNDERLINE_SINGLE)
      && (s->prev->face->underline_at_descent_line_p
	  == s->face->underline_at_descent_line_p)
      && (s->prev->face->underline_pixels_above_descent_line
	  == s->face->underline_pixels_above_descent_line))
    {
      /* Continue the previous glyph string's underline geometry unchanged.  */
      thickness = s->prev->underline_thickness;
      position = s->prev->underline_position;
    }
  else
    {
      struct font *font = font_for_underline_metrics (s);

      if (font && font->underline_thickness > 0)
	thickness = font->underline_thickness;
      else
	thickness = 1;
      if (x_underline_at_descent_line || s->face->underline_at_descent_line_p)
	position = ((s->height - thickness)
		    - (s->ybase - s->y)
		    - s->face->underline_pixels_above_descent_line);
      else
	{
	  /* Recommended vertical offset from the baseline to the top of the
	     underline; default ROUND ((maximum descent) / 2).  */
	  if (x_use_underline_position_properties
	      && font && font->underline_position >= 0)
	    position = font->underline_position;
	  else if (font)
	    position = (font->descent + 1) / 2;
	  else
	    position = underline_minimum_offset;
	}
      /* Ignore minimum_offset if the pixel amount was explicitly specified.  */
      if (!s->face->underline_pixels_above_descent_line)
	position = max (position, underline_minimum_offset);
    }
  /* Keep the underline inside the current line area.  */
  if (s->y + s->height <= s->ybase + position)
    position = (s->height - 1) - (s->ybase - s->y);
  if (s->y + s->height < s->ybase + position + thickness)
    thickness = (s->y + s->height) - (s->ybase + position);
  s->underline_thickness = thickness;
  s->underline_position = position;
  int pos = s->ybase + position;
  int x0 = s->x, w = s->width;
  if (w <= 0)
    return;
  float r, g, b;
  wlshm_unpack_pixel (color, &r, &g, &b);
  block_input ();
  switch (s->face->underline)
    {
    case FACE_UNDERLINE_DOUBLE_LINE:
      wlshm_window_rect ((float) x0, (float) pos, (float) w, 1.0f, r, g, b, 1.0f);
      wlshm_window_rect ((float) x0, (float) (pos + 2), (float) w, 1.0f, r, g, b, 1.0f);
      break;
    case FACE_UNDERLINE_WAVE:
      {
	/* Smooth antialiased zigzag (matches pgtk): height 3, period 4.  */
	int wave_height = 3, wave_length = 2;
	wlshm_draw_horizontal_wave (s->f, color, x0, s->ybase - wave_height + 3,
				    w, wave_height, wave_length);
      }
      break;
    case FACE_UNDERLINE_DOTS:
      for (int dx = 0; dx < w; dx += 2)
	wlshm_window_rect ((float) (x0 + dx), (float) pos, 1.0f, (float) thickness,
			  r, g, b, 1.0f);
      break;
    case FACE_UNDERLINE_DASHES:
      for (int dx = 0; dx < w; dx += 6)
	wlshm_window_rect ((float) (x0 + dx), (float) pos,
			  (float) (w - dx < 3 ? w - dx : 3), (float) thickness,
			  r, g, b, 1.0f);
      break;
    case FACE_UNDERLINE_SINGLE:
    default:
      wlshm_window_rect ((float) x0, (float) pos, (float) w, (float) thickness,
			r, g, b, 1.0f);
      break;
    }
  unblock_input ();
}

/* Draw glyphless characters (undisplayable codepoints / control chars) as a
   thin box optionally containing the hex code or an acronym, like the other
   backends.  Ported from pgtk_draw_glyphless_glyph_string_foreground.  */
static void
wlshm_draw_glyphless_glyph_string_foreground (struct glyph_string *s)
{
  struct glyph *glyph = s->first_glyph;
  unsigned char2b[8];
  int x, i, j;

  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  s->char2b = char2b;

  for (i = 0; i < s->nchars; i++, glyph++)
    {
#ifdef GCC_LINT
      enum { PACIFY_GCC_BUG_81401 = 1 };
#else
      enum { PACIFY_GCC_BUG_81401 = 0 };
#endif
      char buf[7 + PACIFY_GCC_BUG_81401];
      char *str = NULL;
      int len = glyph->u.glyphless.len;

      if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_ACRONYM)
	{
	  if (len > 0
	      && CHAR_TABLE_P (Vglyphless_char_display)
	      && (CHAR_TABLE_EXTRA_SLOTS (XCHAR_TABLE (Vglyphless_char_display))
		  >= 1))
	    {
	      Lisp_Object acronym
		= (!glyph->u.glyphless.for_no_font
		   ? CHAR_TABLE_REF (Vglyphless_char_display,
				     glyph->u.glyphless.ch)
		   : XCHAR_TABLE (Vglyphless_char_display)->extras[0]);
	      if (CONSP (acronym))
		acronym = XCAR (acronym);
	      if (STRINGP (acronym))
		str = SSDATA (acronym);
	    }
	}
      else if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_HEX_CODE)
	{
	  unsigned int ch = glyph->u.glyphless.ch;
	  sprintf (buf, "%0*X", ch < 0x10000 ? 4 : 6, ch);
	  str = buf;
	}

      if (str)
	{
	  int upper_len = (len + 1) / 2;
	  for (j = 0; j < len && j < 8; j++)
	    char2b[j]
	      = s->font->driver->encode_char (s->font, str[j]) & 0xFFFF;
	  s->font->driver->draw (s, 0, upper_len,
				 x + glyph->slice.glyphless.upper_xoff,
				 s->ybase + glyph->slice.glyphless.upper_yoff,
				 false);
	  s->font->driver->draw (s, upper_len, len,
				 x + glyph->slice.glyphless.lower_xoff,
				 s->ybase + glyph->slice.glyphless.lower_yoff,
				 false);
	}

      if (glyph->u.glyphless.method != GLYPHLESS_DISPLAY_THIN_SPACE)
	{
	  /* Outline box.  */
	  int bx = x, by = s->ybase - glyph->ascent;
	  int bw = glyph->pixel_width - 1, bh = glyph->ascent + glyph->descent - 1;
	  wlshm_draw_box_outline (s->face->foreground, bx, by, bw, bh);
	}
      x += glyph->pixel_width;
    }

  s->char2b = NULL;
}

/* Clip the canvas CR to glyph string S's clip rectangles, so overhangs and
   neighbouring glyphs don't smear into adjacent cells.  Ported from
   pgtk_set_glyph_string_clipping.  Caller must wlshm_begin_cr_clip first
   (cairo_restore in wlshm_end_cr_clip removes the clip).  */
static void
wlshm_set_glyph_string_clipping (struct glyph_string *s, cairo_t *cr)
{
  XRectangle r[2];
  int n = get_glyph_string_clip_rects (s, r, 2);

  if (n > 0)
    {
      /* Clip with AA-none so the clip edge snaps to the same physical-pixel
	 grid as the AA-none background/box fills.  An antialiased clip lets a
	 glyph's font-antialiased top/bottom scanline paint a partial-coverage
	 row one physical pixel beyond where the opaque background fill rounds
	 its edge (at a fractional device scale), leaving a coloured fringe over
	 the neighbouring line's background -- e.g. the mode-line glyph tops
	 bleeding into the buffer row above it on HiDPI.  Glyph interiors keep
	 the font's own antialiasing (that is set via the font options, not the
	 context), so only the boundary fringe is cropped.  */
      cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
      for (int i = 0; i < n; i++)
	cairo_rectangle (cr, r[i].x, r[i].y, r[i].width, r[i].height);
      cairo_clip (cr);
    }
}

/* RIF: compute the left/right overhang of glyph string S (used so bold and
   slanted glyphs that bleed past their advance width get repainted/clipped
   correctly).  Ported from pgtk_compute_glyph_string_overhangs.  */
static void
wlshm_compute_glyph_string_overhangs (struct glyph_string *s)
{
  if (s->cmp == NULL
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      struct font_metrics metrics;

      if (s->first_glyph->type == CHAR_GLYPH)
	{
	  unsigned *code = alloca (sizeof (unsigned) * s->nchars);
	  struct font *font = s->font;

	  for (int i = 0; i < s->nchars; i++)
	    code[i] = s->char2b[i];
	  font->driver->text_extents (font, code, s->nchars, &metrics);
	}
      else
	{
	  Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);

	  composition_gstring_width (gstring, s->cmp_from, s->cmp_to, &metrics);
	}
      s->right_overhang = (metrics.rbearing > metrics.width
			   ? metrics.rbearing - metrics.width : 0);
      s->left_overhang = metrics.lbearing < 0 ? -metrics.lbearing : 0;
    }
  else if (s->cmp)
    {
      s->right_overhang = s->cmp->rbearing - s->cmp->pixel_width;
      s->left_overhang = -s->cmp->lbearing;
    }
}

/* Draw a 1px-thick rectangle outline (W+1 x H+1, like the X/pgtk
   pgtk_draw_rectangle) in COLOR on the canvas.  */
static void
wlshm_draw_rectangle (struct frame *f, unsigned long color,
		      int x, int y, int w, int h)
{
  float r, g, b;
  wlshm_unpack_pixel (color, &r, &g, &b);
  wlshm_window_rect ((float) x, (float) y, (float) (w + 1), 1.0f, r, g, b, 1.0f);
  wlshm_window_rect ((float) x, (float) (y + h), (float) (w + 1), 1.0f, r, g, b, 1.0f);
  wlshm_window_rect ((float) x, (float) y, 1.0f, (float) (h + 1), r, g, b, 1.0f);
  wlshm_window_rect ((float) (x + w), (float) y, 1.0f, (float) (h + 1), r, g, b, 1.0f);
}

/* Draw a cairo image PATTERN onto the canvas at DEST_X,DEST_Y, sampling from
   SRC_X,SRC_Y for WIDTH x HEIGHT.  When !OVERLAY_P the destination is first
   filled with the glyph string's background.  Coverage masks (A1/A8) are
   stencilled with the foreground.  Ported from pgtk_cr_draw_image.  */
static void
wlshm_cr_draw_image (struct frame *f, unsigned long fg, unsigned long bg,
		     cairo_pattern_t *image, int src_x, int src_y,
		     int width, int height, int dest_x, int dest_y,
		     bool overlay_p)
{
  cairo_t *cr = wlshm_begin_cr_clip (f);
  if (!cr)
    return;
  float r, g, b;

  if (overlay_p)
    cairo_rectangle (cr, dest_x, dest_y, width, height);
  else
    {
      wlshm_unpack_pixel (bg, &r, &g, &b);
      cairo_set_source_rgb (cr, r, g, b);
      cairo_rectangle (cr, dest_x, dest_y, width, height);
      cairo_fill_preserve (cr);
    }

  cairo_translate (cr, dest_x - src_x, dest_y - src_y);

  cairo_surface_t *surface;
  cairo_pattern_get_surface (image, &surface);
  cairo_format_t format = cairo_image_surface_get_format (surface);
  if (format != CAIRO_FORMAT_A8 && format != CAIRO_FORMAT_A1)
    {
      cairo_set_source (cr, image);
      cairo_fill (cr);
    }
  else
    {
      wlshm_unpack_pixel (fg, &r, &g, &b);
      cairo_set_source_rgb (cr, r, g, b);
      cairo_clip (cr);
      cairo_mask (cr, image);
    }

  wlshm_end_cr_clip (f);
}

/* Draw the foreground (the actual image) of image glyph string S.  Ported
   from pgtk_draw_image_foreground.  */
static void
wlshm_draw_image_foreground (struct glyph_string *s)
{
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);

  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->img->cr_data)
    {
      cairo_t *cr = wlshm_begin_cr_clip (s->f);
      if (!cr)
	return;
      wlshm_set_glyph_string_clipping (s, cr);
      wlshm_cr_draw_image (s->f, s->xgcv.foreground, s->xgcv.background,
			   s->img->cr_data, s->slice.x, s->slice.y,
			   s->slice.width, s->slice.height, x, y, true);
      if (!s->img->mask && s->hl == DRAW_CURSOR)
	{
	  int relief = eabs (s->img->relief);
	  wlshm_draw_rectangle (s->f, s->xgcv.foreground, x - relief, y - relief,
				s->slice.width + relief * 2 - 1,
				s->slice.height + relief * 2 - 1);
	}
      wlshm_end_cr_clip (s->f);
    }
  else
    wlshm_draw_rectangle (s->f, s->xgcv.foreground, x, y,
			  s->slice.width - 1, s->slice.height - 1);
}

/* Draw a raised/sunken 3D relief around image glyph string S.  Simplified
   port of pgtk_draw_image_relief: a 1px box in light/dark relief colors.  */
static void
wlshm_draw_image_relief (struct glyph_string *s)
{
  int thick, x, y, x1, y1;
  bool raised_p;

  x = s->x;
  y = s->ybase - image_ascent (s->img, s->face, &s->slice);
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->hl == DRAW_IMAGE_SUNKEN || s->hl == DRAW_IMAGE_RAISED)
    {
      thick = (tool_bar_button_relief < 0
	       ? DEFAULT_TOOL_BAR_BUTTON_RELIEF
	       : min (tool_bar_button_relief, 1000000));
      raised_p = s->hl == DRAW_IMAGE_RAISED;
    }
  else
    {
      thick = eabs (s->img->relief);
      raised_p = s->img->relief > 0;
    }
  if (thick <= 0)
    return;

  x1 = x + s->slice.width - 1;
  y1 = y + s->slice.height - 1;
  unsigned long light = wlshm_scale_pixel (s->face->background, 1.4);
  unsigned long dark = wlshm_scale_pixel (s->face->background, 0.55);
  unsigned long top_left = raised_p ? light : dark;
  unsigned long bottom_right = raised_p ? dark : light;
  float tr, tg, tb, br, bg2, bb;
  wlshm_unpack_pixel (top_left, &tr, &tg, &tb);
  wlshm_unpack_pixel (bottom_right, &br, &bg2, &bb);
  for (int i = 0; i < thick; i++)
    {
      wlshm_window_rect ((float) (x - i), (float) (y - i),
			 (float) (x1 - x + 1 + 2 * i), 1.0f, tr, tg, tb, 1.0f);
      wlshm_window_rect ((float) (x - i), (float) (y - i),
			 1.0f, (float) (y1 - y + 1 + 2 * i), tr, tg, tb, 1.0f);
      wlshm_window_rect ((float) (x - i), (float) (y1 + i),
			 (float) (x1 - x + 1 + 2 * i), 1.0f, br, bg2, bb, 1.0f);
      wlshm_window_rect ((float) (x1 + i), (float) (y - i),
			 1.0f, (float) (y1 - y + 1 + 2 * i), br, bg2, bb, 1.0f);
    }
}

/* Draw image glyph string S: optional background, the image, optional relief.
   Ported from pgtk_draw_image_glyph_string.  */
static void
wlshm_draw_image_glyph_string (struct glyph_string *s)
{
  int box_line_hwidth = max (s->face->box_vertical_line_width, 0);
  int box_line_vwidth = max (s->face->box_horizontal_line_width, 0);
  int height = s->height;

  if (s->slice.y == 0)
    height -= box_line_vwidth;
  if (s->slice.y + s->slice.height >= s->img->height)
    height -= box_line_vwidth;

  /* Tool-bar button hover (RAISED) / press (SUNKEN): a clean flat tint over
     the whole button cell instead of a 3D relief box tight around the icon,
     matching the mode-line/menu-bar mouse-face hover.  Press is a touch
     stronger than hover.  */
  bool button_hl = (s->hl == DRAW_IMAGE_RAISED || s->hl == DRAW_IMAGE_SUNKEN);
  if (button_hl)
    {
      double t = (s->hl == DRAW_IMAGE_SUNKEN
		  ? WLSHM_HOVER_BLEND * 2.0 : WLSHM_HOVER_BLEND);
      unsigned long tint = wlshm_blend_pixel (s->face->background,
					      s->face->foreground, t);
      wlshm_fill_rect_pixel (s->x, s->y, s->background_width, s->height, tint);
      s->background_filled_p = true;
    }

  if (!s->background_filled_p
      && (height > s->slice.height
	  || s->img->hmargin || s->img->vmargin || s->img->mask
	  || s->img->pixmap == 0 || s->width != s->background_width))
    {
      int x = s->x, y = s->y, width = s->background_width;
      if (s->first_glyph->left_box_line_p && s->slice.x == 0)
	{
	  x += box_line_hwidth;
	  width -= box_line_hwidth;
	}
      if (s->slice.y == 0)
	y += box_line_vwidth;

      wlshm_fill_rect_pixel (x, y, width, height, s->xgcv.background);
      s->background_filled_p = true;
    }

  wlshm_draw_image_foreground (s);

  /* The flat tint replaces the relief for hover/press; an image with its own
     :relief still gets one.  */
  if (!button_hl && s->img->relief)
    wlshm_draw_image_relief (s);
}

static void
wlshm_draw_glyph_string (struct glyph_string *s)
{
  wlshm_cur = s->f;
  unsigned long fg, bg;
  wlshm_glyph_string_colors (s, &fg, &bg);

  /* ftcrfont_draw reads the glyph colors from s->xgcv; supply them (it draws
     with with_background=false, so only the foreground is used).  */
  s->xgcv.foreground = fg;
  s->xgcv.background = bg;

  /* Clip all drawing for this glyph string to its clip rectangles so
     overhangs/neighbours don't smear (the inner wlshm_window_rect and
     ftcrfont begin/end cr-clip calls nest inside this save).  */
  cairo_t *cr = wlshm_begin_cr_clip (s->f);
  /* The canvas allocation can fail (huge size / OOM), leaving wlshm_cr NULL;
     bail rather than dereferencing it through the clipping/box/font-draw paths
     (pgtk's context is always live, but wlshm's allocate-and-bail canvas is
     not).  The next redisplay retries.  */
  if (!cr)
    return;
  wlshm_set_glyph_string_clipping (s, cr);

  switch (s->first_glyph->type)
    {
    case CHAR_GLYPH:
    case COMPOSITE_GLYPH:
      {
	struct font *font = s->font;

	/* Background: fill the FULL cell every time (mouse-face draws its rounded
	   "pill" instead) so the glyphs draw with_background=false below and no
	   stale ink (e.g. an underline in the line-spacing gap) survives a redraw.
	   The glyph-string clip (set with AA-none, see wlshm_set_glyph_string_clipping)
	   crops the glyphs to the same physical-pixel grid as this AA-none fill, so
	   at a fractional device scale the glyphs' antialiased top scanline can no
	   longer land one physical row above the fill and leave a coloured fringe
	   over the line above (the streaks seen just above the mode line on HiDPI).  */
	if (!s->background_filled_p && !s->for_overlaps)
	  {
	    block_input ();
	    if (s->hl == DRAW_MOUSE_FACE)
	      wlshm_draw_mouse_face_bg (s, bg, fg);
	    else
	      wlshm_fill_rect_pixel (s->x, s->y, s->background_width, s->height, bg);
	    s->background_filled_p = true;
	    unblock_input ();
	  }

	/* Glyphs.  with_background=true (per pgtk) whenever the background was not
	   separately filled, so each glyph paints its own opaque background box.
	   Inset the origin past a left box line so the text matches the box; the
	   box itself is drawn LAST (after the glyphs) so these per-glyph fills
	   cannot paint over its edges.  */
	bool with_bg = !(s->for_overlaps
			 || (s->background_filled_p && s->hl != DRAW_CURSOR));
	int gx = s->x;
	if (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p)
	  gx += max (s->face->box_vertical_line_width, 0);
	if (s->first_glyph->type == COMPOSITE_GLYPH)
	  wlshm_draw_composite_glyph_string_foreground (s);
	else if (font && font->driver && font->driver->draw)
	  {
	    int y = s->ybase - font->baseline_offset;
	    font->driver->draw (s, 0, s->nchars, gx, y, with_bg);
	  }

	/* Underline.  */
	if (s->face->underline && !s->for_overlaps)
	  {
	    unsigned long ul = (s->face->underline_defaulted_p
				? fg : s->face->underline_color);
	    wlshm_draw_underline (s, ul);
	  }

	/* Overline.  */
	if (s->face->overline_p && !s->for_overlaps)
	  {
	    unsigned long oc = (s->face->overline_color_defaulted_p
				? fg : s->face->overline_color);
	    block_input ();
	    wlshm_fill_rect_pixel (s->x, s->y, s->width, 1, oc);
	    unblock_input ();
	  }

	/* Strike-through (centered on the first glyph's box).  */
	if (s->face->strike_through_p && !s->for_overlaps)
	  {
	    int glyph_y = s->ybase - s->first_glyph->ascent;
	    int glyph_h = s->first_glyph->ascent + s->first_glyph->descent;
	    int dy = (glyph_h - 1) / 2;
	    unsigned long sc = (s->face->strike_through_color_defaulted_p
				? fg : s->face->strike_through_color);
	    block_input ();
	    wlshm_fill_rect_pixel (s->x, glyph_y + dy, s->width, 1, sc);
	    unblock_input ();
	  }

	/* The face's box, drawn LAST (after the glyphs) like pgtk so the
	   per-glyph backgrounds above cannot paint over its edges.  The
	   mouse-face pill replaces it for hover.  */
	if (!s->for_overlaps && s->hl != DRAW_MOUSE_FACE)
	  wlshm_draw_glyph_string_box (s);
      }
      break;

    case STRETCH_GLYPH:
      {
	/* Background then the box.  Clear the FULL cell height (not inset by the
	   box line): the box is drawn on top afterward and continues its
	   top/bottom edges over the fill, while clearing the box-line rows so an
	   incremental mode-line redraw can't leave a stale sliver of a previous
	   glyph there.  Mirrors the CHAR_GLYPH path and
	   pgtk_draw_stretch_glyph_string.  */
	block_input ();
	if (s->hl == DRAW_MOUSE_FACE)
	  wlshm_draw_mouse_face_bg (s, bg, fg);
	else
	  wlshm_fill_rect_pixel (s->x, s->y, s->background_width, s->height, bg);
	unblock_input ();
	if (!s->for_overlaps && s->hl != DRAW_MOUSE_FACE)
	  wlshm_draw_glyph_string_box (s);
      }
      break;

    case GLYPHLESS_GLYPH:
      {
	/* Background, then the box + hex/acronym.  */
	if (!s->background_filled_p && !s->for_overlaps)
	  {
	    block_input ();
	    wlshm_fill_rect_pixel (s->x, s->y, s->background_width, s->height, bg);
	    unblock_input ();
	    s->background_filled_p = true;
	  }
	block_input ();
	wlshm_draw_glyphless_glyph_string_foreground (s);
	unblock_input ();
      }
      break;

    case IMAGE_GLYPH:
      block_input ();
      wlshm_draw_image_glyph_string (s);
      unblock_input ();
      break;

    default:
      break;
    }

  s->num_clips = 0;
  wlshm_end_cr_clip (s->f);
}

static void
wlshm_clear_frame_area (struct frame *f, int x, int y, int width, int height)
{
  wlshm_cur = f;
  block_input ();
  wlshm_fill_rect_pixel (x, y, width, height, FRAME_BACKGROUND_PIXEL (f));
  unblock_input ();
}

static void
wlshm_draw_window_cursor (struct window *w, struct glyph_row *glyph_row,
			 int x, int y, enum text_cursor_kinds cursor_type,
			 int cursor_width, bool on_p, bool active_p)
{
  wlshm_cur = WINDOW_XFRAME (w);
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
	 going through wlshm_draw_glyph_string with hl == DRAW_CURSOR.  */
      draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
      break;

    case HOLLOW_BOX_CURSOR:
    case BAR_CURSOR:
    case HBAR_CURSOR:
      {
	struct glyph *cursor_glyph = get_phys_cursor_glyph (w);
	if (!cursor_glyph)
	  break;
	/* A bar/hbar over an image: draw the glyph inverted instead (mirrors
	   pgtk), since a thin bar over an image is invisible/wrong.  */
	if (cursor_type != HOLLOW_BOX_CURSOR
	    && (cursor_glyph->type == IMAGE_GLYPH
		|| cursor_glyph->type == XWIDGET_GLYPH))
	  {
	    draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
	    break;
	  }
	/* If the glyph's own background equals the cursor color the cursor would
	   be invisible; use the glyph foreground then (mirrors pgtk).  */
	struct face *cface = FACE_FROM_ID (f, cursor_glyph->face_id);
	unsigned long ccol = ((cface && cface->background == FRAME_CURSOR_COLOR (f))
			      ? cface->foreground : FRAME_CURSOR_COLOR (f));
	int gx, gy, gh, wd = cursor_glyph->pixel_width;
	get_phys_cursor_geometry (w, glyph_row, cursor_glyph, &gx, &gy, &gh);
	float r, g, b;
	wlshm_unpack_pixel (ccol, &r, &g, &b);
	block_input ();
	if (cursor_type == HOLLOW_BOX_CURSOR)
	  {
	    /* Outline only.  */
	    wlshm_window_rect ((float) gx, (float) gy, (float) wd, 1.0f, r, g, b, 1.0f);
	    wlshm_window_rect ((float) gx, (float) (gy + gh - 1), (float) wd, 1.0f, r, g, b, 1.0f);
	    wlshm_window_rect ((float) gx, (float) gy, 1.0f, (float) gh, r, g, b, 1.0f);
	    wlshm_window_rect ((float) (gx + wd - 1), (float) gy, 1.0f, (float) gh, r, g, b, 1.0f);
	  }
	else if (cursor_type == BAR_CURSOR)
	  {
	    /* Clamp the bar to the glyph width (a wide cursor-width must not
	       overshoot a narrow glyph), and record it for IM/erase geometry.  */
	    int bw = min (wd, cursor_width > 0 ? cursor_width : 2);
	    w->phys_cursor_width = bw;
	    wlshm_fill_rect_pixel (gx, gy, bw, gh, ccol);
	  }
	else /* HBAR_CURSOR */
	  {
	    int bh = cursor_width > 0 ? cursor_width : 2;
	    wlshm_fill_rect_pixel (gx, gy + gh - bh, wd, bh, ccol);
	  }
	unblock_input ();
      }
      break;

    default:
      break;
    }
}

/* Shift an already-rendered region of the window up/down on the persistent
   Cairo canvas (a CPU memmove of the image-surface pixels), so redisplay only
   has to repaint the newly-exposed lines.  Mirrors pgtk_scroll_run (sans
   xwidgets).  */
static void
wlshm_scroll_run (struct window *w, struct run *run)
{
  wlshm_cur = WINDOW_XFRAME (w);
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
  wlshm_ensure_canvas ();
  if (wlshm_canvas)
    {
      cairo_surface_flush (wlshm_canvas);
      unsigned char *data = cairo_image_surface_get_data (wlshm_canvas);
      int stride = cairo_image_surface_get_stride (wlshm_canvas);
      int cw = wlshm_canvas_w, ch = wlshm_canvas_h;
      /* The copy box (x/width/from_y/to_y/height) is in LOGICAL coordinates,
	 but this raw-pixel memmove operates on the PHYSICAL buffer.  Scale the
	 box by the canvas device scale (== physical/logical).  At scale 1.0
	 these multiplications are the identity (byte-identical path).  */
      double dsx = 1.0, dsy = 1.0;
      cairo_surface_get_device_scale (wlshm_canvas, &dsx, &dsy);
      /* Round the box EDGES and difference them, rather than rounding each
	 offset and extent independently: at a fractional scale the latter can
	 drift 1px and leave a stale row/column.  At scale 1.0 this is the
	 identity (byte-identical path).  */
      int phys_x = (int) lround (x * dsx);
      width = (int) lround ((x + width) * dsx) - phys_x;
      int phys_from = (int) lround (from_y * dsy);
      int phys_to = (int) lround (to_y * dsy);
      /* Derive the row count from BOTH the source and destination edges and take
	 the smaller.  At a fractional scale the source span [from_y, from_y+h]
	 and destination span [to_y, to_y+h] can round to different physical
	 heights; reusing the source height for the destination memmove would
	 overrun the destination's true rounded bottom edge by one physical row --
	 smearing buffer pixels down into the mode line (and, scrolling the other
	 way, mode-line pixels up into the buffer).  min() can only shrink the
	 span, never overrun either edge; the worst case is a 1px gap that
	 redisplay repaints immediately, far better than persistent garbage.  */
      int h_from = (int) lround ((from_y + height) * dsy) - phys_from;
      int h_to = (int) lround ((to_y + height) * dsy) - phys_to;
      /* Intended destination bottom (physical).  min() below may copy fewer
	 rows than this, leaving an uncovered gap at the destination bottom that
	 we must clear (see after the memmove).  */
      int dst_bottom = phys_to + h_to;
      height = min (h_from, h_to);
      if (height < 0)
	height = 0;
      x = phys_x;
      from_y = phys_from;
      to_y = phys_to;
      /* Clamp the copy box to the canvas.  */
      if (x < 0)
	x = 0;
      if (x + width > cw)
	width = cw - x;
      /* Clamp negative source/dest offsets to 0 and recompute height, so the
	 later from_y/to_y + height clamps operate on valid bases (a negative
	 base would otherwise mask a real overflow).  Advance both bases by the
	 same deficit to keep source/dest rows aligned.  */
      int top_deficit = max (max (-from_y, -to_y), 0);
      if (top_deficit > 0)
	{
	  from_y += top_deficit;
	  to_y += top_deficit;
	  height -= top_deficit;
	}
      if (from_y + height > ch)
	height = ch - from_y;
      if (to_y + height > ch)
	height = ch - to_y;
      if (width > 0 && height > 0)
	{
	  size_t row_bytes = (size_t) width * 4;
	  if (to_y < from_y)
	    for (int i = 0; i < height; i++)
	      memmove (data + (size_t) (to_y + i) * stride + (size_t) x * 4,
		       data + (size_t) (from_y + i) * stride + (size_t) x * 4,
		       row_bytes);
	  else
	    for (int i = height - 1; i >= 0; i--)
	      memmove (data + (size_t) (to_y + i) * stride + (size_t) x * 4,
		       data + (size_t) (from_y + i) * stride + (size_t) x * 4,
		       row_bytes);
	  /* The min() height can leave the destination's bottom physical row(s)
	     uncopied at a fractional scale; they keep the pre-scroll pixels (the
	     previous line's glyph bottoms), and redisplay treats the scrolled
	     rows as preserved so it never repaints them -- leaving faint
	     fragments just above the mode line.  Clear that gap to the frame
	     background; the next redisplay paints any real content over it.  The
	     gap stays within the text area (h_to was clamped to bottom_y above),
	     never the mode line.  */
	  int gap_y = to_y + height;
	  int gap_bottom = min (dst_bottom, ch);
	  if (gap_bottom > gap_y && gap_y >= 0)
	    {
	      uint32_t px = (uint32_t) (FRAME_BACKGROUND_PIXEL (wlshm_cur) & 0xffffff);
	      for (int yy = gap_y; yy < gap_bottom; yy++)
		{
		  uint32_t *row = (uint32_t *) (data + (size_t) yy * stride) + x;
		  for (int xx = 0; xx < width; xx++)
		    row[xx] = px;
		}
	    }
	  cairo_surface_mark_dirty (wlshm_canvas);
	}
    }
  unblock_input ();
}
/* Called after each window line is updated.  Mark the row's fringe bitmaps for
   redraw (otherwise stale fringe/indicator pixels linger after an update --
   the "margins don't redraw" symptom), and clear any leftover full-width row
   pixels in the internal border.  Ported from pgtk_after_update_window_line.  */
static void
wlshm_after_update_window_line (struct window *w, struct glyph_row *desired_row)
{
  struct frame *f;
  int width, height;

  eassert (w);

  if (!desired_row->mode_line_p && !w->pseudo_window_p)
    desired_row->redraw_fringe_bitmaps_p = 1;

  if (windows_or_buffers_changed
      && desired_row->full_width_p
      && (f = XFRAME (w->frame),
	  width = FRAME_INTERNAL_BORDER_WIDTH (f),
	  width != 0)
      && (height = desired_row->visible_height, height > 0))
    {
      int y = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, desired_row->y));
      block_input ();
      wlshm_clear_frame_area (f, 0, y, width, height);
      wlshm_clear_frame_area (f, FRAME_PIXEL_WIDTH (f) - width, y, width, height);
      unblock_input ();
    }
}
static void wlshm_flush_display (struct frame *f) {}

/* Fringe bitmaps (continuation/truncation arrows, empty-line and buffer
   boundary indicators).  We keep the raw bits (copied at define time, since
   define may run before the window exists) and lazily rasterize them onto the
   canvas with Cairo on first draw.  */
struct wlshm_fringe_bmp
{
  unsigned short *bits;
  int h, wd;
};
static struct wlshm_fringe_bmp *wlshm_fringe_bmps;
static int wlshm_fringe_bmp_max;

static void
wlshm_define_fringe_bitmap (int which, unsigned short *bits, int h, int wd)
{
  if (which >= wlshm_fringe_bmp_max)
    {
      int old = wlshm_fringe_bmp_max;
      wlshm_fringe_bmp_max = which + 20;
      wlshm_fringe_bmps = xrealloc (wlshm_fringe_bmps,
				   wlshm_fringe_bmp_max * sizeof *wlshm_fringe_bmps);
      memset (&wlshm_fringe_bmps[old], 0,
	      (wlshm_fringe_bmp_max - old) * sizeof *wlshm_fringe_bmps);
    }
  struct wlshm_fringe_bmp *fb = &wlshm_fringe_bmps[which];
  xfree (fb->bits);
  fb->bits = xnmalloc (h, sizeof (unsigned short));
  memcpy (fb->bits, bits, h * sizeof (unsigned short));
  fb->h = h;
  fb->wd = wd;
}

static void
wlshm_destroy_fringe_bitmap (int which)
{
  if (which < 0 || which >= wlshm_fringe_bmp_max)
    return;
  struct wlshm_fringe_bmp *fb = &wlshm_fringe_bmps[which];
  xfree (fb->bits);
  fb->bits = NULL;
  fb->h = fb->wd = 0;
}

static void
wlshm_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			 struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  wlshm_cur = f;
  struct face *face = p->face;

  block_input ();
  wlshm_ensure_canvas ();
  if (!wlshm_cr)
    {
      unblock_input ();
      return;
    }

  /* Clip to this row's band within the window so the fringe (background and
     bitmap) never bleeds past the window's text area into the mode line or the
     echo-area window below it (mirrors pgtk_clip_to_row).  */
  cairo_save (wlshm_cr);
  {
    int wbx, wby, wbw;
    window_box (w, ANY_AREA, &wbx, &wby, &wbw, 0);
    int ry = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y));
    ry = max (ry, wby);
    cairo_rectangle (wlshm_cr, wbx, ry, wbw, row->visible_height);
    cairo_clip (wlshm_cr);
  }

  /* Background behind the bitmap.  */
  if (p->bx >= 0 && !p->overlay_p && p->nx > 0 && p->ny > 0)
    {
      unsigned long bg = face ? face->background : FRAME_BACKGROUND_PIXEL (f);
      wlshm_fill_rect_pixel (p->bx, p->by, p->nx, p->ny, bg);
    }

  /* The bitmap itself: build a 1-bit cairo mask from the row words and
     paint it in the fringe color (mirrors pgtk_define/draw_fringe_bitmap).  */
  if (p->which)
    {
      /* Lazily define the bitmap if it was registered while no GUI frame
	 existed (e.g. a package loaded under a daemon).  */
      if (p->which >= wlshm_fringe_bmp_max || !wlshm_fringe_bmps[p->which].bits)
	gui_define_fringe_bitmap (f, p->which);
      struct wlshm_fringe_bmp *fb
	= (p->which < wlshm_fringe_bmp_max) ? &wlshm_fringe_bmps[p->which] : NULL;
      wlshm_ensure_canvas ();
      if (fb && fb->bits && fb->h > 0 && fb->wd > 0 && wlshm_cr)
	{
	  unsigned long fg
	    = (p->cursor_p
	       ? (p->overlay_p
		  ? (face ? face->background : FRAME_BACKGROUND_PIXEL (f))
		  : FRAME_CURSOR_COLOR (f))
	       : (face ? face->foreground : FRAME_FOREGROUND_PIXEL (f)));
	  float r, g, b;
	  wlshm_unpack_pixel (fg, &r, &g, &b);

	  cairo_surface_t *mask
	    = cairo_image_surface_create (CAIRO_FORMAT_A1, fb->wd, fb->h);
	  /* Skip the bitmap if the mask surface couldn't be allocated (its data
	     would be NULL); the row still renders, just without the indicator.  */
	  if (cairo_surface_status (mask) == CAIRO_STATUS_SUCCESS)
	    {
	      int stride = cairo_image_surface_get_stride (mask);
	      unsigned char *data = cairo_image_surface_get_data (mask);
	      for (int i = 0; i < fb->h; i++)
		*((unsigned short *) (data + i * stride)) = fb->bits[i];
	      cairo_surface_mark_dirty (mask);

	      cairo_save (wlshm_cr);
	      /* Clip to the bitmap's VISIBLE extent (p->wd x p->h).  p->h is the
		 fringe.c-adjusted height: clamped to the row and reduced by the
		 p->dh phase offset, exactly what pgtk_cr_draw_image clips to.
		 Using fb->h / p->ny here drew the wrong rows of periodic bitmaps
		 (the empty-line ~ indicator), mispositioned custom bitmaps, and
		 let a tall bitmap bleed past the row down over the mode line.  */
	      cairo_rectangle (wlshm_cr, p->x, p->y, p->wd, p->h);
	      cairo_clip (wlshm_cr);
	      cairo_set_source_rgb (wlshm_cr, r, g, b);
	      /* Offset by -dh so visible rows [dh, dh+h) land at p->y (partial
		 rows at window edges; dh is 0 in the common case).  */
	      cairo_mask_surface (wlshm_cr, mask, p->x, p->y - p->dh);
	      cairo_restore (wlshm_cr);
	    }
	  cairo_surface_destroy (mask);
	}
    }

  cairo_restore (wlshm_cr);
  unblock_input ();
}

/* Fill a frame-relative rectangle with a packed pixel color.  */
static void
wlshm_fill_rect_pixel (int x, int y, int w, int h, unsigned long pixel)
{
  if (w <= 0 || h <= 0)
    return;
  float r, g, b;
  wlshm_unpack_pixel (pixel, &r, &g, &b);
  wlshm_window_rect ((float) x, (float) y, (float) w, (float) h, r, g, b, 1.0f);
}

/* Fill the internal border strips so no stale pixels show when the window
   layout changes.  */
static void
wlshm_clear_under_internal_border (struct frame *f)
{
  wlshm_cur = f;
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
  wlshm_fill_rect_pixel (0, margin, width, border, pixel);
  wlshm_fill_rect_pixel (0, 0, border, height, pixel);
  wlshm_fill_rect_pixel (width - border, 0, border, height, pixel);
  wlshm_fill_rect_pixel (0, height - bottom_margin - border, width, border, pixel);
  unblock_input ();
}

/* 1px line separating side-by-side windows (no divider configured).  */
static void
wlshm_draw_vertical_window_border (struct window *w, int x, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  wlshm_cur = f;
  struct face *face = FACE_FROM_ID_OR_NULL (f, VERTICAL_BORDER_FACE_ID);
  unsigned long pixel = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);
  block_input ();
  wlshm_fill_rect_pixel (x, y0, 1, y1 - y0, pixel);
  unblock_input ();
}

/* Window divider (the draggable separator), drawn with first/middle/last
   pixel faces so it gets a subtle 3D edge like the other backends.  */
static void
wlshm_draw_window_divider (struct window *w, int x0, int x1, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  wlshm_cur = f;
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
      wlshm_fill_rect_pixel (x0, y0, 1, y1 - y0, color_first);
      wlshm_fill_rect_pixel (x0 + 1, y0, x1 - x0 - 2, y1 - y0, color);
      wlshm_fill_rect_pixel (x1 - 1, y0, 1, y1 - y0, color_last);
    }
  else if (x1 - x0 > y1 - y0 && y1 - y0 > 3)
    {
      /* Horizontal divider.  */
      wlshm_fill_rect_pixel (x0, y0, x1 - x0, 1, color_first);
      wlshm_fill_rect_pixel (x0, y0 + 1, x1 - x0, y1 - y0 - 2, color);
      wlshm_fill_rect_pixel (x0, y1 - 1, x1 - x0, 1, color_last);
    }
  else
    wlshm_fill_rect_pixel (x0, y0, x1 - x0, y1 - y0, color);
  unblock_input ();
}

/* Set the pointer cursor shape for frame F (I-beam over text, arrow over the
   mode line, hand over buttons, etc.).  CURSOR encodes a wlshm_cursor_shape.  */
static void
wlshm_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
  if (FRAME_OUTPUT_DATA (f)->current_cursor == cursor)
    return;
  FRAME_OUTPUT_DATA (f)->current_cursor = cursor;
  block_input ();
  wlshm_window_set_cursor (WLSHM_FRAME_HANDLE (f), (int) (intptr_t) cursor);
  unblock_input ();
}
/* Busy indicator: switch the pointer to the Wait shape while Emacs is busy,
   and back to the frame's normal cursor when done.  */
static void
wlshm_show_hourglass (struct frame *f)
{
  block_input ();
  wlshm_window_set_cursor (f ? WLSHM_FRAME_HANDLE (f) : 0, WLSHM_CURSOR_WAIT);
  unblock_input ();
}

static void
wlshm_hide_hourglass (struct frame *f)
{
  block_input ();
  int c = f ? (int) (intptr_t) FRAME_OUTPUT_DATA (f)->current_cursor : 0;
  wlshm_window_set_cursor (f ? WLSHM_FRAME_HANDLE (f) : 0,
			   c ? c : WLSHM_CURSOR_DEFAULT);
  unblock_input ();
}

/* ------------------------------------------------------------------ */
/* Frame lifecycle hooks.                                             */
/* ------------------------------------------------------------------ */

/* delete_frame_hook: destroy F's Wayland surface and Cairo canvas.  Without
   this the surface leaks until process exit.  */
static void
wlshm_destroy_window (struct frame *f)
{
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct wlshm_output *o = FRAME_X_OUTPUT (f);

  block_input ();

  /* Drop any dangling references to this frame.  */
  if (dpyinfo->highlight_frame == f)
    dpyinfo->highlight_frame = NULL;
  if (dpyinfo->x_focus_frame == f)
    dpyinfo->x_focus_frame = NULL;
  if (dpyinfo->x_focus_event_frame == f)
    dpyinfo->x_focus_event_frame = NULL;
  if (dpyinfo->last_mouse_frame == f)
    dpyinfo->last_mouse_frame = NULL;
  if (dpyinfo->last_mouse_motion_frame == f)
    dpyinfo->last_mouse_motion_frame = NULL;
  if (dpyinfo->last_mouse_glyph_frame == f)
    dpyinfo->last_mouse_glyph_frame = NULL;
  if (wlshm_cur == f)
    {
      wlshm_cur = NULL;
      wlshm_canvas = NULL;
      wlshm_cr = NULL;
    }

  if (o)
    {
      if (o->cr)
	{
	  cairo_destroy (o->cr);
	  o->cr = NULL;
	}
      if (o->canvas)
	{
	  cairo_surface_destroy (o->canvas);
	  o->canvas = NULL;
	}
      uint64_t h = WLSHM_FRAME_HANDLE (f);
      if (h)
	wlshm_window_close (h);
      o->wlshm_frame = 0;
    }

  unblock_input ();
}

/* frame_visible_invisible_hook: map (present) or unmap the surface.  */
static void
wlshm_make_frame_visible_invisible (struct frame *f, bool visible)
{
  if (visible)
    {
      SET_FRAME_VISIBLE (f, 1);
      SET_FRAME_ICONIFIED (f, false);
      /* Force a redisplay so the surface maps with current content.  */
      SET_FRAME_GARBAGED (f);
    }
  else
    {
      SET_FRAME_VISIBLE (f, 0);
      block_input ();
      wlshm_window_unmap (WLSHM_FRAME_HANDLE (f));
      unblock_input ();
    }
}

/* iconify_frame_hook: minimize via xdg_toplevel.set_minimized.  Wayland sends
   no reliable de-iconify event, so we mark the frame iconified and rely on
   make-frame-visible to restore it.  */
static void
wlshm_iconify_frame (struct frame *f)
{
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  if (dpyinfo->highlight_frame == f)
    dpyinfo->highlight_frame = NULL;
  if (FRAME_ICONIFIED_P (f))
    return;

  block_input ();
  wlshm_window_minimize (WLSHM_FRAME_HANDLE (f));
  unblock_input ();

  SET_FRAME_VISIBLE (f, 0);
  SET_FRAME_ICONIFIED (f, true);
}

/* fullscreen_hook: apply F's want_fullscreen to the Wayland surface.  Only
   FULLSCREEN_BOTH maps to xdg fullscreen; width/height-only are unsupported on
   Wayland and fall back to off.  */
static void
wlshm_fullscreen_hook (struct frame *f)
{
  if (!FRAME_VISIBLE_P (f))
    return;
  block_input ();
  int mode = (f->want_fullscreen == FULLSCREEN_BOTH
	      || f->want_fullscreen == FULLSCREEN_MAXIMIZED) ? 1 : 0;
  wlshm_window_set_fullscreen (WLSHM_FRAME_HANDLE (f), mode);
  /* The compositor confirms the new size via a Configure event, which
     wlshm_read_socket turns into a resize.  */
  unblock_input ();
}

/* set_window_size_hook: request a new pixel size.  Apply locally and request
   it from the compositor; a Configure event confirms the final size.  */
static void
wlshm_set_window_size (struct frame *f, bool change_gravity,
		       int width, int height)
{
  block_input ();
  FRAME_X_OUTPUT (f)->preferred_width = width;
  FRAME_X_OUTPUT (f)->preferred_height = height;
  /* Size only: this hook carries no position.  Using set_geometry here would
     anchor a not-yet-positioned tooltip popup at (0,0); the positioned
     placement happens later in Fx_show_tip via wlshm_window_set_geometry.  */
  wlshm_window_set_size (WLSHM_FRAME_HANDLE (f), width, height);

  /* WIDTH/HEIGHT are the native (pixel) size; change_frame_size converts to
     text internally, so pass them straight through (no double conversion).
     A child frame is Emacs-driven (no compositor configure), so apply the size
     SYNCHRONOUSLY (delay=false) -- mirroring pgtk.  Deferring it leaves
     FRAME_PIXEL_WIDTH/HEIGHT stale until the next redisplay, and corfu runs its
     popup placement under inhibit-redisplay: a sibling popup (corfu-popupinfo)
     reading frame-pixel-width right after set-frame-size would then see the
     freshly-created list frame's tiny figured size and place itself on top of
     it (the first-call overlap).  Toplevels keep delay=true; their authoritative
     size comes from the compositor configure.  */
  bool delay = !FRAME_PARENT_FRAME (f);
  change_frame_size (f, width, height, false, delay, false);
  SET_FRAME_GARBAGED (f);
  unblock_input ();
}

/* toggle_invisible_pointer_hook: hide the pointer while typing.  */
static void
wlshm_toggle_invisible_pointer (struct frame *f, bool invisible)
{
  block_input ();
  wlshm_window_hide_pointer (WLSHM_FRAME_HANDLE (f), invisible);
  f->pointer_invisible = invisible;
  unblock_input ();
}

/* free_pixmap: release a Cairo Emacs_Pixmap (mirrors pgtk_free_pixmap).  */
static void
wlshm_free_pixmap (struct frame *f, Emacs_Pixmap pixmap)
{
  if (pixmap)
    {
      xfree (pixmap->data);
      xfree (pixmap);
    }
}

/* The following are genuine Wayland limitations: a client cannot control its
   stacking order, position its own toplevels, or set a per-window icon.  PGTK
   no-ops these under Wayland too.  They exist so the generic code can call them
   unconditionally.  */
static void
wlshm_frame_raise_lower (struct frame *f, bool raise_flag)
{
  /* Wayland gives clients no control over window stacking.  No-op.  */
}

/* Resolve a negative (right/bottom-relative) child-frame offset to a top-left
   position relative to the parent, mirroring pgtk_calc_absolute_position.  Only
   child frames are positionable on Wayland, so this handles that case.  */
static void
wlshm_calc_absolute_position (struct frame *f)
{
  struct frame *p = FRAME_PARENT_FRAME (f);
  int flags = f->size_hint_flags;

  if (!p || !((flags & XNegative) || (flags & YNegative)))
    return;

  if (flags & XNegative)
    f->left_pos = (FRAME_PIXEL_WIDTH (p) - FRAME_PIXEL_WIDTH (f)
		   - 2 * f->border_width + f->left_pos);
  if (flags & YNegative)
    f->top_pos = (FRAME_PIXEL_HEIGHT (p) - FRAME_PIXEL_HEIGHT (f)
		  - 2 * f->border_width + f->top_pos);
  f->size_hint_flags &= ~ (XNegative | YNegative);
}

static void
wlshm_set_frame_offset (struct frame *f, int xoff, int yoff, int change_gravity)
{
  /* Record the requested position.  Fset_frame_position routes here WITHOUT
     updating left_pos/top_pos itself (unlike modify-frame-parameters), relying
     on the backend hook to store them -- as pgtk_set_offset/x_set_offset do.
     Child-frame packages (corfu-popupinfo, posframe) read a sibling's
     frame-position to place the next popup, so a stale value makes them
     overlap.  */
  if (change_gravity > 0)
    {
      f->top_pos = yoff;
      f->left_pos = xoff;
      f->size_hint_flags &= ~ (XNegative | YNegative);
      if (xoff < 0)
	f->size_hint_flags |= XNegative;
      if (yoff < 0)
	f->size_hint_flags |= YNegative;
      f->win_gravity = NorthWestGravity;
    }
  wlshm_calc_absolute_position (f);

  /* A plain toplevel cannot position itself on Wayland (no-op).  But a child
     frame is a wl_subsurface, which CAN be placed relative to its parent --
     position it so posframe/child-frame packages float at the right spot.  */
  if (FRAME_PARENT_FRAME (f) && WLSHM_FRAME_HANDLE (f))
    {
      block_input ();
      wlshm_window_set_subsurface_pos (WLSHM_FRAME_HANDLE (f),
				       f->left_pos, f->top_pos);
      unblock_input ();
    }
}

static bool
wlshm_set_bitmap_icon (struct frame *f, Lisp_Object file)
{
  /* Wayland has no per-window icon (only the app_id maps to a .desktop).  */
  return false;
}

/* The bell.  Wayland has no portable audible bell, so we only ever do a VISUAL
   flash, and ONLY when the user asked for one via `visible-bell' (mirrors
   xterm's XTring_bell).  With the default (visible-bell nil) the bell is
   silent -- no screen flash.  The flash is a brief translucent wash over the
   frame rather than a hard full-screen colour inversion, which is jarring.  */
static void
wlshm_ring_bell (struct frame *f)
{
  if (!visible_bell)
    return;
  if (f && FRAME_WLSHM_P (f))
    wlshm_cur = f;
  wlshm_ensure_canvas ();
  if (!wlshm_canvas || !wlshm_cr)
    return;
  f = wlshm_canvas_frame ();
  if (!f)
    return;
  block_input ();
  cairo_surface_flush (wlshm_canvas);
  int stride = cairo_image_surface_get_stride (wlshm_canvas);
  unsigned char *data = cairo_image_surface_get_data (wlshm_canvas);
  size_t nbytes = (size_t) stride * wlshm_canvas_h;
  unsigned char *snap = xmalloc (nbytes);
  memcpy (snap, data, nbytes);

  /* Soft flash: a short, semi-transparent grey wash (not a full inversion).  */
  cairo_save (wlshm_cr);
  cairo_set_operator (wlshm_cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (wlshm_cr, 0.5, 0.5, 0.5, 0.45);
  cairo_paint (wlshm_cr);
  cairo_restore (wlshm_cr);
  wlshm_present_canvas (f);

  struct timespec ts = { 0, 40 * 1000 * 1000 };  /* ~40ms flash */
  nanosleep (&ts, NULL);

  memcpy (data, snap, nbytes);
  cairo_surface_mark_dirty (wlshm_canvas);
  wlshm_present_canvas (f);
  xfree (snap);
  unblock_input ();
}

void
wlshm_default_font_parameter (struct frame *f, Lisp_Object parms)
{
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
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
      /* Default to a 12pt LOGICAL size, matching pgtk (which picks up the 12pt
	 GNOME system font) so wlshm doesn't render smaller by default.  Derive
	 the pixel size from the display resolution ourselves (12pt * resx/72 =
	 16px at 96 DPI) rather than passing a point size to fontconfig, which
	 converts with its own DPI and lands ~14px.  This tracks dpyinfo->resx
	 yet reliably matches pgtk; HiDPI crispness still comes entirely from the
	 Cairo device scale, so the font stays logical (no double-scaling).  */
      int px = (int) lround (12.0 * FRAME_DISPLAY_INFO (f)->resx / 72.0);
      if (px < 1)
	px = 16;
      char sized[64];
      snprintf (sized, sizeof sized, "Monospace:pixelsize=%d", px);
      const char *names[] = { sized, "Monospace:pixelsize=16", "fixed", NULL };
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
wlshm_set_foreground_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wlshm_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_FOREGROUND_PIXEL (f) = col.pixel;
      update_face_from_frame_parameter (f, Qforeground_color, arg);
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

static void
wlshm_set_background_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wlshm_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_BACKGROUND_PIXEL (f) = col.pixel;
      update_face_from_frame_parameter (f, Qbackground_color, arg);
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

static void
wlshm_set_cursor_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  Emacs_Color col;
  if (STRINGP (arg) && wlshm_defined_color (f, SSDATA (arg), &col, true, false))
    {
      FRAME_X_OUTPUT (f)->cursor_color = col.pixel;
      if (FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
    }
}

/* ---- Frame name / title (xdg_toplevel.set_title) ---------------------- */

static void
wlshm_set_name_internal (struct frame *f, Lisp_Object name)
{
  Lisp_Object encoded = ENCODE_UTF_8 (name);
  wlshm_log ("set_title \"%s\"", SSDATA (encoded));
  block_input ();
  wlshm_window_set_title (WLSHM_FRAME_HANDLE (f), SSDATA (encoded));
  unblock_input ();
}

static void
wlshm_set_name (struct frame *f, Lisp_Object name, int explicit)
{
  /* Lisp requests override redisplay requests.  */
  if (explicit)
    {
      if (f->explicit_name && NILP (name))
	update_mode_lines = 12;
      f->explicit_name = !NILP (name);
    }
  else if (f->explicit_name)
    return;

  if (NILP (name))
    name = build_string ("GNU Emacs");
  else
    CHECK_STRING (name);

  if (!NILP (Fstring_equal (name, f->name)))
    return;

  fset_name (f, name);

  /* Title overrides explicit name.  */
  if (!NILP (f->title))
    name = f->title;
  wlshm_set_name_internal (f, name);
}

static void
wlshm_explicitly_set_name (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  wlshm_set_name (f, arg, true);
}

static void
wlshm_implicitly_set_name (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  wlshm_set_name (f, arg, false);
}

static void
wlshm_set_title (struct frame *f, Lisp_Object name, Lisp_Object old_name)
{
  if (EQ (name, f->title))
    return;
  update_mode_lines = 22;
  fset_title (f, name);
  if (NILP (name))
    name = f->name;
  else
    CHECK_STRING (name);
  wlshm_set_name_internal (f, name);
}

/* Frame parameter: cursor-type.  Ported from pgtk_set_cursor_type.  */
static void
wlshm_set_cursor_type (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  set_frame_cursor_types (f, arg);
}

/* Frame parameter: internal-border-width.  Recompute the frame size and
   repaint the border strips.  Ported from pgtk_set_internal_border_width.  */
static void
wlshm_set_internal_border_width (struct frame *f, Lisp_Object arg,
				 Lisp_Object oldval)
{
  int border = check_int_nonnegative (arg);

  if (border != FRAME_INTERNAL_BORDER_WIDTH (f))
    {
      f->internal_border_width = border;
      if (FRAME_X_WINDOW (f))
	{
	  adjust_frame_size (f, -1, -1, 3, false, Qinternal_border_width);
	  wlshm_clear_under_internal_border (f);
	}
    }
}

/* Shared handler for the scroll-bar-foreground/-background frame parameters:
   resolve NEW_VALUE into *PIXEL (-1 for nil = use the default) and refresh the
   face named by PARM.  ERRNAME names the parameter for the "Invalid ..." error.
   */
static void
wlshm_set_scroll_bar_color (struct frame *f, Lisp_Object new_value,
			    unsigned long *pixel, Lisp_Object parm,
			    const char *errname)
{
  if (NILP (new_value))
    *pixel = -1;
  else if (STRINGP (new_value))
    {
      Emacs_Color color;
      if (!wlshm_defined_color (f, SSDATA (new_value), &color, true, false))
	error ("Unknown color");
      *pixel = color.pixel;
    }
  else
    error ("Invalid %s", errname);
  update_face_from_frame_parameter (f, parm, new_value);
}

/* Frame parameter: scroll-bar-foreground.  Store the pixel (used by the
   Emacs-drawn scroll bars); nil = use the default.  */
static void
wlshm_set_scroll_bar_foreground (struct frame *f, Lisp_Object new_value,
				 Lisp_Object old_value)
{
  wlshm_set_scroll_bar_color (f, new_value,
			      &FRAME_X_OUTPUT (f)->scroll_bar_foreground_pixel,
			      Qscroll_bar_foreground, "scroll-bar-foreground");
}

/* Frame parameter: scroll-bar-background.  As above for the trough.  */
static void
wlshm_set_scroll_bar_background (struct frame *f, Lisp_Object new_value,
				 Lisp_Object old_value)
{
  wlshm_set_scroll_bar_color (f, new_value,
			      &FRAME_X_OUTPUT (f)->scroll_bar_background_pixel,
			      Qscroll_bar_background, "scroll-bar-background");
}

/* Frame parameter: undecorated.  Drop/add server-side window decorations.  */
static void
wlshm_set_undecorated (struct frame *f, Lisp_Object new_value,
		       Lisp_Object old_value)
{
  if (!EQ (new_value, old_value))
    {
      FRAME_UNDECORATED (f) = !NILP (new_value);
      block_input ();
      /* Decorations ON when the frame is NOT undecorated.  */
      wlshm_window_set_decorations (WLSHM_FRAME_HANDLE (f), NILP (new_value));
      unblock_input ();
      store_frame_param (f, Qundecorated, FRAME_UNDECORATED (f) ? Qt : Qnil);
    }
}

/* Frame parameter: parent-frame.  Best-effort xdg_toplevel reparent.  */
static void
wlshm_set_parent_frame (struct frame *f, Lisp_Object new_value,
			Lisp_Object old_value)
{
  struct frame *p = NULL;

  if (!NILP (new_value))
    {
      CHECK_FRAME (new_value);
      p = XFRAME (new_value);
      if (!FRAME_WLSHM_P (p))
	error ("parent-frame must be on the same display");
    }

  if (p != FRAME_PARENT_FRAME (f))
    {
      block_input ();
      wlshm_window_set_parent (WLSHM_FRAME_HANDLE (f),
			       p ? WLSHM_FRAME_HANDLE (p) : 0);
      unblock_input ();
      fset_parent_frame (f, new_value);
      store_frame_param (f, Qparent_frame, new_value);
    }
}

/* Frame parameter: child-frame-border-width.  Mirrors the internal-border
   handler.  */
static void
wlshm_set_child_frame_border_width (struct frame *f, Lisp_Object arg,
				    Lisp_Object oldval)
{
  int border = (NILP (arg)
		? -1 : check_integer_range (arg, 0, INT_MAX));

  if (border != FRAME_CHILD_FRAME_BORDER_WIDTH (f))
    {
      f->child_frame_border_width = border;
      if (FRAME_X_WINDOW (f))
	{
	  adjust_frame_size (f, -1, -1, 3, false, Qchild_frame_border_width);
	  wlshm_clear_under_internal_border (f);
	}
    }
}

/* Tab bar, tool bar, and menu bar are drawn internally (no toolkit): the
   display engine lays them out as pseudo-window rows and the RIF draws them
   (text tabs, image tool-bar buttons, menu-bar text).  These handlers just
   allocate/free the rows and resize the frame.  Ported from the android
   backend (the closest no-toolkit, internal-everything model).  */

static void
wlshm_change_tool_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TOOL_BAR_HEIGHT (f);
  int lines = (height + unit - 1) / unit;
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);

  fset_redisplay (f);
  FRAME_TOOL_BAR_HEIGHT (f) = height;
  FRAME_TOOL_BAR_LINES (f) = lines;
  store_frame_param (f, Qtool_bar_lines, make_fixnum (lines));

  if (WLSHM_FRAME_HANDLE (f) && FRAME_TOOL_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }
  if ((height < old_height) && WINDOWP (f->tool_bar_window))
    clear_glyph_matrix (XWINDOW (f->tool_bar_window)->current_matrix);

  if (!f->tool_bar_resized)
    {
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtool_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtool_bar_lines);
      f->tool_bar_resized = f->tool_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtool_bar_lines);

  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
wlshm_set_tool_bar_lines (struct frame *f, Lisp_Object value, Lisp_Object oldval)
{
  int nlines;
  if (FRAME_MINIBUF_ONLY_P (f))
    return;
  nlines = RANGED_FIXNUMP (0, value, INT_MAX) ? XFIXNAT (value) : 0;
  /* Skip the no-op (e.g. the 0 -> 0 set at frame creation): an unconditional
     change_tool_bar_height would do spurious adjust_frame_glyphs/garbage that
     perturbs fringe layout.  */
  if (nlines != FRAME_TOOL_BAR_LINES (f))
    wlshm_change_tool_bar_height (f, nlines * FRAME_LINE_HEIGHT (f));
}

static void
wlshm_set_tool_bar_position (struct frame *f, Lisp_Object new_value,
			     Lisp_Object old_value)
{
  if (!EQ (new_value, Qtop) && !EQ (new_value, Qbottom))
    error ("Tool bar position must be either `top' or `bottom'");
  if (EQ (new_value, old_value))
    return;
  fset_tool_bar_position (f, new_value);
  adjust_frame_size (f, -1, -1, 3, false, Qtool_bar_position);
  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
wlshm_change_tab_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TAB_BAR_HEIGHT (f);
  int lines = height / unit;
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);

  if (lines == 0 && height != 0)
    lines = 1;
  fset_redisplay (f);
  FRAME_TAB_BAR_HEIGHT (f) = height;
  FRAME_TAB_BAR_LINES (f) = lines;
  store_frame_param (f, Qtab_bar_lines, make_fixnum (lines));

  if (WLSHM_FRAME_HANDLE (f) && FRAME_TAB_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }
  if ((height < old_height) && WINDOWP (f->tab_bar_window))
    clear_glyph_matrix (XWINDOW (f->tab_bar_window)->current_matrix);

  if (!f->tab_bar_resized)
    {
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtab_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtab_bar_lines);
      f->tab_bar_resized = f->tab_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtab_bar_lines);

  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
wlshm_set_tab_bar_lines (struct frame *f, Lisp_Object value, Lisp_Object oldval)
{
  int olines = FRAME_TAB_BAR_LINES (f);
  int nlines;
  if (FRAME_MINIBUF_ONLY_P (f))
    return;
  nlines = RANGED_FIXNUMP (0, value, INT_MAX) ? XFIXNAT (value) : 0;
  if (nlines != olines && (olines == 0 || nlines == 0))
    wlshm_change_tab_bar_height (f, nlines * FRAME_LINE_HEIGHT (f));
}

static void
wlshm_set_menu_bar_lines (struct frame *f, Lisp_Object value, Lisp_Object oldval)
{
  int nlines;
  int olines = FRAME_MENU_BAR_LINES (f);

  if (FRAME_MINIBUF_ONLY_P (f) || FRAME_PARENT_FRAME (f))
    return;
  nlines = TYPE_RANGED_FIXNUMP (int, value) ? XFIXNUM (value) : 0;

  fset_redisplay (f);
  FRAME_MENU_BAR_LINES (f) = nlines;
  FRAME_MENU_BAR_HEIGHT (f) = nlines * FRAME_LINE_HEIGHT (f);
  if (WLSHM_FRAME_HANDLE (f))
    wlshm_clear_under_internal_border (f);
  if (nlines != olines)
    adjust_frame_size (f, -1, -1, 3, true, Qmenu_bar_lines);
}

/* Positional, indexed by frame parameter (mirrors pgtk_frame_parm_handlers).
   gui_set_* are the shared generic handlers; the remaining NULLs are genuine
   Wayland-limitation no-ops (override-redirect, skip-taskbar, z-group, sticky,
   icon-name/type, mouse/border color) that the generic code tolerates.  */
static frame_parm_handler wlshm_frame_parm_handlers[] = {
  gui_set_autoraise,
  gui_set_autolower,
  wlshm_set_background_color,
  NULL,				/* border_color */
  gui_set_border_width,
  wlshm_set_cursor_color,
  wlshm_set_cursor_type,
  gui_set_font,
  wlshm_set_foreground_color,
  NULL,				/* icon_name */
  NULL,				/* icon_type */
  wlshm_set_child_frame_border_width,
  wlshm_set_internal_border_width,
  gui_set_right_divider_width,
  gui_set_bottom_divider_width,
  wlshm_set_menu_bar_lines,
  NULL,				/* mouse_color */
  wlshm_explicitly_set_name,
  gui_set_scroll_bar_width,
  gui_set_scroll_bar_height,
  wlshm_set_title,
  gui_set_unsplittable,
  gui_set_vertical_scroll_bars,
  gui_set_horizontal_scroll_bars,
  gui_set_visibility,
  wlshm_set_tab_bar_lines,
  wlshm_set_tool_bar_lines,
  wlshm_set_scroll_bar_foreground,
  wlshm_set_scroll_bar_background,
  gui_set_screen_gamma,
  gui_set_line_spacing,
  gui_set_left_fringe,
  gui_set_right_fringe,
  0,
  gui_set_fullscreen,
  gui_set_font_backend,
  gui_set_alpha,
  NULL,				/* sticky */
  wlshm_set_tool_bar_position,
  0,
  wlshm_set_undecorated,
  wlshm_set_parent_frame,
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

static struct redisplay_interface wlshm_redisplay_interface = {
  wlshm_frame_parm_handlers,
  gui_produce_glyphs,
  gui_write_glyphs,
  gui_insert_glyphs,
  gui_clear_end_of_line,
  wlshm_scroll_run,
  wlshm_after_update_window_line,
  NULL, /* update_window_begin */
  NULL, /* update_window_end */
  wlshm_flush_display,
  gui_clear_window_mouse_face,
  gui_get_glyph_overhangs,
  gui_fix_overlapping_area,
  wlshm_draw_fringe_bitmap,
  wlshm_define_fringe_bitmap,
  wlshm_destroy_fringe_bitmap,
  wlshm_compute_glyph_string_overhangs,
  wlshm_draw_glyph_string,
  wlshm_define_frame_cursor,
  wlshm_clear_frame_area,
  wlshm_clear_under_internal_border,
  wlshm_draw_window_cursor,
  wlshm_draw_vertical_window_border,
  wlshm_draw_window_divider,
  NULL, /* shift_glyphs_for_insert */
  wlshm_show_hourglass,
  wlshm_hide_hourglass,
  wlshm_default_font_parameter,
};

/* ------------------------------------------------------------------ */
/* Terminal hooks.                                                    */
/* ------------------------------------------------------------------ */

static void
wlshm_clear_frame (struct frame *f)
{
  wlshm_cur = f;
  /* Clear at least the whole surface: the frame is sized to fill it exactly,
     but if a configure has been received and the frame not yet resized the
     surface could momentarily be larger -- cover it so no stale pixels show.  */
  uint32_t sw = 0, sh = 0;
  wlshm_window_size (WLSHM_FRAME_HANDLE (f), &sw, &sh);
  int w = max ((int) sw, FRAME_PIXEL_WIDTH (f));
  int h = max ((int) sh, FRAME_PIXEL_HEIGHT (f));
  block_input ();
  wlshm_fill_rect_pixel (0, 0, w, h, FRAME_BACKGROUND_PIXEL (f));
  unblock_input ();
}

static void
wlshm_update_begin (struct frame *f)
{
  block_input ();
  wlshm_cur = f;
  wlshm_ensure_canvas ();
  unblock_input ();
}

static void
wlshm_update_end (struct frame *f)
{
  /* Mouse highlight may be displayed again (update_window_begin deferred it
     for the duration of the update).  Without this the hover highlight on
     buttons / mode-line elements / links never reappears after a redisplay.  */
  MOUSE_HL_INFO (f)->mouse_face_defer = false;
}

static void wlshm_redraw_scroll_bars (struct frame *f);

static void
wlshm_frame_up_to_date (struct frame *f)
{
  block_input ();
  /* Re-establish the mouse-face hover highlight after a complete frame update.
     A full/garbage redraw repaints rows from the glyph matrix with their
     normal faces, wiping the highlight overlay (mouse-face on buttons,
     mode-line elements, links) so hovering appears to do nothing.  Re-run
     note_mouse_highlight at the last pointer position to redraw it onto the
     canvas before we present (mirrors xterm/pgtk frame_up_to_date).  This
     relies on wlshm_update_end having cleared mouse_face_defer; for a garbage
     redraw update_window_begin already reset mouse_face_window, so this
     recomputes from scratch rather than early-returning.  */
  FRAME_MOUSE_UPDATE (f);
  /* Repaint scroll bars on top of the freshly-drawn text before presenting,
     so the text redisplay doesn't leave them overpainted.  */
  wlshm_redraw_scroll_bars (f);
  wlshm_present_canvas (f);
  unblock_input ();
}

/* Find a live wlshm frame on TERMINAL.  */
static struct frame *
wlshm_any_frame (struct terminal *terminal)
{
  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WLSHM_P (f) && FRAME_LIVE_P (f)
	  && FRAME_TERMINAL (f) == terminal)
	return f;
    }
  return NULL;
}

/* The live wlshm frame on TERMINAL whose Rust window HANDLE matches, or NULL
   (handle 0 / no match -> let the caller fall back to wlshm_any_frame).  */
static struct frame *
wlshm_frame_for_handle (struct terminal *terminal, uint64_t handle)
{
  if (handle == 0)
    return NULL;
  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WLSHM_P (f) && FRAME_LIVE_P (f)
	  && FRAME_TERMINAL (f) == terminal
	  && WLSHM_FRAME_HANDLE (f) == handle)
	return f;
    }
  return NULL;
}

/* Update which frame is highlighted (focused).  The cursor is drawn solid
   only on the highlight frame; others get a hollow cursor (xdisp.c checks
   f != dpyinfo->highlight_frame).  */
static void
wlshm_frame_rehighlight (struct wlshm_display_info *dpyinfo)
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
wlshm_frame_rehighlight_hook (struct frame *f)
{
  wlshm_frame_rehighlight (FRAME_DISPLAY_INFO (f));
}

static void
wlshm_new_focus_frame (struct wlshm_display_info *dpyinfo, struct frame *frame)
{
  if (frame != dpyinfo->x_focus_frame)
    dpyinfo->x_focus_frame = frame;
  wlshm_frame_rehighlight (dpyinfo);
}

/* ------------------------------------------------------------------ */
/* Scroll bars: drawn by Emacs onto the canvas (no toolkit/subsurface),  */
/* ported from xterm.c's #ifndef USE_TOOLKIT_SCROLL_BARS path.           */
/* ------------------------------------------------------------------ */

/* Linear blend of two 0xRRGGBB pixels: A at t=0, B at t=1.  */
static unsigned long
wlshm_blend_pixel (unsigned long a, unsigned long b, double t)
{
  float ar, ag, ab, br, bg, bb;
  wlshm_unpack_pixel (a, &ar, &ag, &ab);
  wlshm_unpack_pixel (b, &br, &bg, &bb);
  int r = (int) (((double) ar + ((double) br - (double) ar) * t) * 255.0 + 0.5);
  int g = (int) (((double) ag + ((double) bg - (double) ag) * t) * 255.0 + 0.5);
  int bl = (int) (((double) ab + ((double) bb - (double) ab) * t) * 255.0 + 0.5);
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  bl = bl < 0 ? 0 : (bl > 255 ? 255 : bl);
  return ((unsigned long) r << 16) | ((unsigned long) g << 8) | (unsigned long) bl;
}

/* Round a filled handle by clearing its four 1px corners back to the trough.  */
static void
wlshm_round_corners (int x, int y, int w, int h, unsigned long trough)
{
  if (w < 3 || h < 3)
    return;
  wlshm_fill_rect_pixel (x, y, 1, 1, trough);
  wlshm_fill_rect_pixel (x + w - 1, y, 1, 1, trough);
  wlshm_fill_rect_pixel (x, y + h - 1, 1, 1, trough);
  wlshm_fill_rect_pixel (x + w - 1, y + h - 1, 1, 1, trough);
}

/* Paint BAR onto the frame canvas: a flat, modern scroll bar -- a faint trough
   and a soft mid-tone handle inset from the edges with lightly rounded
   corners, no hard border.  Honors explicit scroll-bar colors if set.  */
static void
wlshm_scroll_bar_redraw (struct scroll_bar *bar)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  struct wlshm_output *out = FRAME_X_OUTPUT (f);
  unsigned long frame_fg = FRAME_FOREGROUND_PIXEL (f);
  unsigned long frame_bg = FRAME_BACKGROUND_PIXEL (f);
  /* Base the trough/handle on the `scroll-bar' face, not the frame background,
     so a child frame's bar (e.g. a corfu popup) matches its themed scroll-bar
     column instead of blitting the popup body color.  Honor face remapping and
     fall back to the frame colors only when the face is unavailable.  An
     explicit scroll-bar-{background,foreground} frame parameter still wins.  */
  int sb_face_id = SCROLL_BAR_FACE_ID;
  if (!NILP (Vface_remapping_alist))
    sb_face_id = lookup_basic_face (XWINDOW (bar->window), f, SCROLL_BAR_FACE_ID);
  struct face *sb_face = FACE_FROM_ID_OR_NULL (f, sb_face_id);
  unsigned long sb_bg = sb_face ? sb_face->background : frame_bg;
  unsigned long sb_fg = sb_face ? sb_face->foreground : frame_fg;
  unsigned long trough = (out->scroll_bar_background_pixel != (unsigned long) -1
			  ? out->scroll_bar_background_pixel
			  : sb_bg);
  unsigned long handle = (out->scroll_bar_foreground_pixel != (unsigned long) -1
			  ? out->scroll_bar_foreground_pixel
			  : wlshm_blend_pixel (sb_bg, sb_fg, 0.42));
  int left = bar->left, top = bar->top, width = bar->width, height = bar->height;

  if (width <= 0 || height <= 0)
    return;

  /* The minibuffer / echo-area window reserves scroll-bar space (every window
     inherits the frame's scroll-bar type via make_window), and redisplay calls
     set/redeem for it, so we must keep the bar object.  But it must not SHOW a
     bar -- toolkit backends render nothing in the echo area.  Paint only the
     frame background so the column stays blank.  */
  if (MINI_WINDOW_P (XWINDOW (bar->window)))
    {
      block_input ();
      wlshm_fill_rect_pixel (left, top, width, height, FRAME_BACKGROUND_PIXEL (f));
      unblock_input ();
      return;
    }

  block_input ();
  /* Faint trough, no border.  */
  wlshm_fill_rect_pixel (left, top, width, height, trough);

  if (!bar->horizontal)
    {
      int top_range = VERTICAL_SCROLL_BAR_TOP_RANGE (f, height);
      int s = bar->start, e = bar->end;
      if (e > top_range)
	e = top_range;
      int inset = width / 4 < 2 ? 2 : width / 4;
      int hx = left + inset, hw = width - 2 * inset;
      int hy = top + VERTICAL_SCROLL_BAR_TOP_BORDER + s;
      int hh = (e - s) + VERTICAL_SCROLL_BAR_MIN_HANDLE;
      if (hw > 0 && hh > 0)
	{
	  wlshm_fill_rect_pixel (hx, hy, hw, hh, handle);
	  wlshm_round_corners (hx, hy, hw, hh, trough);
	}
    }
  else
    {
      int left_range = HORIZONTAL_SCROLL_BAR_LEFT_RANGE (f, width);
      int s = bar->start, e = bar->end;
      if (e > left_range)
	e = left_range;
      int inset = height / 4 < 2 ? 2 : height / 4;
      int hy = top + inset, hh = height - 2 * inset;
      int hx = left + HORIZONTAL_SCROLL_BAR_LEFT_BORDER + s;
      int hw = (e - s) + HORIZONTAL_SCROLL_BAR_MIN_HANDLE;
      if (hw > 0 && hh > 0)
	{
	  wlshm_fill_rect_pixel (hx, hy, hw, hh, handle);
	  wlshm_round_corners (hx, hy, hw, hh, trough);
	}
    }
  unblock_input ();
}

/* Repaint all of F's active scroll bars (called at frame_up_to_date so the
   text redisplay does not leave them overpainted).  */
static void
wlshm_redraw_scroll_bars (struct frame *f)
{
  wlshm_cur = f;
  Lisp_Object bar;
  for (bar = FRAME_SCROLL_BARS (f); !NILP (bar);
       bar = XSCROLL_BAR (bar)->next)
    wlshm_scroll_bar_redraw (XSCROLL_BAR (bar));
}

/* Clamp and store BAR's handle [start,end], then repaint.  */
static void
wlshm_scroll_bar_set_handle (struct scroll_bar *bar, int start, int end,
			     bool rebuild)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  bool dragging = bar->dragging != -1;
  int top_range, length;
  (void) f;			/* only consumed by macros that ignore it */

  if (!rebuild && start == bar->start && end == bar->end)
    return;

  top_range = (bar->horizontal
	       ? HORIZONTAL_SCROLL_BAR_LEFT_RANGE (f, bar->width)
	       : VERTICAL_SCROLL_BAR_TOP_RANGE (f, bar->height));
  length = end - start;
  if (start < 0)
    start = 0;
  else if (start > top_range)
    start = top_range;
  end = start + length;
  if (end < start)
    end = start;
  else if (end > top_range && !dragging)
    end = top_range;

  bar->start = start;
  bar->end = end;
  wlshm_scroll_bar_redraw (bar);
}

/* Allocate a scroll bar for window W and link it into the frame list.  */
static struct scroll_bar *
wlshm_scroll_bar_create (struct window *w, int top, int left,
			 int width, int height, bool horizontal)
{
  struct frame *f = XFRAME (w->frame);
  struct scroll_bar *bar
    = ALLOCATE_PSEUDOVECTOR (struct scroll_bar, prev, PVEC_OTHER);
  Lisp_Object barobj;

  XSETWINDOW (bar->window, w);
  bar->top = top;
  bar->left = left;
  bar->width = width;
  bar->height = height;
  bar->start = 0;
  bar->end = 0;
  bar->dragging = -1;
  bar->horizontal = horizontal;

  bar->next = FRAME_SCROLL_BARS (f);
  bar->prev = Qnil;
  XSETVECTOR (barobj, bar);
  fset_scroll_bars (f, barobj);
  if (!NILP (bar->next))
    XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
  return bar;
}

/* Dissociate BAR from its window and clear the strip it occupied.  */
static void
wlshm_scroll_bar_remove (struct scroll_bar *bar)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  block_input ();
  wlshm_fill_rect_pixel (bar->left, bar->top, bar->width, bar->height,
			 FRAME_BACKGROUND_PIXEL (f));
  if (bar->horizontal)
    wset_horizontal_scroll_bar (XWINDOW (bar->window), Qnil);
  else
    wset_vertical_scroll_bar (XWINDOW (bar->window), Qnil);
  unblock_input ();
}

/* Shared implementation of the vertical/horizontal scroll-bar hooks.  The two
   axes differ only in: the window_box query, which scroll-bar area macros give
   the strip geometry, the per-axis *_RANGE macro, the bar's existing-bar slot
   (vertical_scroll_bar vs horizontal_scroll_bar), and the wset accessor.  */
static void
wlshm_set_scroll_bar (struct window *w, int portion, int whole, int position,
		      bool horizontal)
{
  struct frame *f = XFRAME (w->frame);
  Lisp_Object barobj;
  struct scroll_bar *bar;
  int top, height, left, width;

  if (horizontal)
    {
      int window_x, window_width;
      window_box (w, ANY_AREA, &window_x, 0, &window_width, 0);
      left = window_x;
      width = window_width;
      top = WINDOW_SCROLL_BAR_AREA_Y (w);
      height = WINDOW_SCROLL_BAR_AREA_HEIGHT (w);
    }
  else
    {
      int window_y, window_height;
      window_box (w, ANY_AREA, 0, &window_y, 0, &window_height);
      top = window_y;
      height = window_height;
      left = WINDOW_SCROLL_BAR_AREA_X (w);
      width = WINDOW_SCROLL_BAR_AREA_WIDTH (w);
    }

  Lisp_Object existing
    = horizontal ? w->horizontal_scroll_bar : w->vertical_scroll_bar;
  if (NILP (existing))
    {
      if (width > 0 && height > 0)
	wlshm_fill_rect_pixel (left, top, width, height,
			       FRAME_BACKGROUND_PIXEL (f));
      /* The vertical bar clamps its allocated height to at least 1.  */
      bar = wlshm_scroll_bar_create (w, top, left, width,
				     horizontal ? height : max (height, 1),
				     horizontal);
    }
  else
    {
      bar = XSCROLL_BAR (existing);
      if (left != bar->left || top != bar->top
	  || width != bar->width || height != bar->height)
	{
	  wlshm_fill_rect_pixel (bar->left, bar->top, bar->width, bar->height,
				 FRAME_BACKGROUND_PIXEL (f));
	  bar->left = left;
	  bar->top = top;
	  bar->width = width;
	  bar->height = height;
	}
    }

  if (bar->dragging == -1)
    {
      int range = (horizontal
		   ? HORIZONTAL_SCROLL_BAR_LEFT_RANGE (f, width)
		   : VERTICAL_SCROLL_BAR_TOP_RANGE (f, height));
      if (whole == 0)
	wlshm_scroll_bar_set_handle (bar, 0, range, true);
      else
	{
	  int start = ((double) position * range) / whole;
	  int end = ((double) (position + portion) * range) / whole;
	  wlshm_scroll_bar_set_handle (bar, start, end, true);
	}
    }
  else
    wlshm_scroll_bar_redraw (bar);

  XSETVECTOR (barobj, bar);
  if (horizontal)
    wset_horizontal_scroll_bar (w, barobj);
  else
    wset_vertical_scroll_bar (w, barobj);
}

static void
wlshm_set_vertical_scroll_bar (struct window *w, int portion, int whole,
			       int position)
{
  wlshm_set_scroll_bar (w, portion, whole, position, false);
}

static void
wlshm_set_horizontal_scroll_bar (struct window *w, int portion, int whole,
				 int position)
{
  wlshm_set_scroll_bar (w, portion, whole, position, true);
}

/* Condemn all of FRAME's scroll bars (port of XTcondemn_scroll_bars).  */
static void
wlshm_condemn_scroll_bars (struct frame *frame)
{
  if (!NILP (FRAME_SCROLL_BARS (frame)))
    {
      if (!NILP (FRAME_CONDEMNED_SCROLL_BARS (frame)))
	{
	  Lisp_Object last = FRAME_SCROLL_BARS (frame);

	  while (!NILP (XSCROLL_BAR (last)->next))
	    last = XSCROLL_BAR (last)->next;

	  XSCROLL_BAR (last)->next = FRAME_CONDEMNED_SCROLL_BARS (frame);
	  XSCROLL_BAR (FRAME_CONDEMNED_SCROLL_BARS (frame))->prev = last;
	}

      fset_condemned_scroll_bars (frame, FRAME_SCROLL_BARS (frame));
      fset_scroll_bars (frame, Qnil);
    }
}

/* Un-condemn WINDOW's scroll bar (port of XTredeem_scroll_bar).  */
static void
wlshm_redeem_scroll_bar (struct window *w)
{
  struct scroll_bar *bar;
  Lisp_Object barobj;
  struct frame *f;

  if (NILP (w->vertical_scroll_bar) && NILP (w->horizontal_scroll_bar))
    emacs_abort ();

  if (!NILP (w->vertical_scroll_bar) && WINDOW_HAS_VERTICAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->vertical_scroll_bar);
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  if (EQ (FRAME_SCROLL_BARS (f), w->vertical_scroll_bar))
	    goto horizontal;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f), w->vertical_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (!NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (!NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }

 horizontal:
  if (!NILP (w->horizontal_scroll_bar) && WINDOW_HAS_HORIZONTAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->horizontal_scroll_bar);
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  if (EQ (FRAME_SCROLL_BARS (f), w->horizontal_scroll_bar))
	    return;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f), w->horizontal_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (!NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (!NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }
}

/* Remove still-condemned scroll bars (port of XTjudge_scroll_bars).  */
static void
wlshm_judge_scroll_bars (struct frame *f)
{
  Lisp_Object bar, next;

  bar = FRAME_CONDEMNED_SCROLL_BARS (f);
  fset_condemned_scroll_bars (f, Qnil);

  for (; !NILP (bar); bar = next)
    {
      struct scroll_bar *b = XSCROLL_BAR (bar);
      wlshm_scroll_bar_remove (b);
      next = b->next;
      b->next = b->prev = Qnil;
    }
}

static void
wlshm_set_scroll_bar_default_width (struct frame *f)
{
  int unit = FRAME_COLUMN_WIDTH (f);
  int size = 16;
  FRAME_CONFIG_SCROLL_BAR_WIDTH (f) = size;
  FRAME_CONFIG_SCROLL_BAR_COLS (f) = (size + unit - 1) / unit;
}

static void
wlshm_set_scroll_bar_default_height (struct frame *f)
{
  int height = FRAME_LINE_HEIGHT (f);
  int size = 16;
  FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) = size;
  FRAME_CONFIG_SCROLL_BAR_LINES (f) = (size + height - 1) / height;
}

/* Return the scroll bar of F containing frame-relative (X,Y), or NULL.  */
static struct scroll_bar *
wlshm_scroll_bar_at (struct frame *f, int x, int y)
{
  Lisp_Object bar;
  for (bar = FRAME_SCROLL_BARS (f); !NILP (bar);
       bar = XSCROLL_BAR (bar)->next)
    {
      struct scroll_bar *b = XSCROLL_BAR (bar);
      if (x >= b->left && x < b->left + b->width
	  && y >= b->top && y < b->top + b->height)
	return b;
    }
  return NULL;
}

/* Classify the pointer at frame-relative (PX,PY) against BAR's handle.  Stores
   the axis-local position (clamped to [0, range]) in *POS_OUT and the axis range
   in *RANGE_OUT, and returns the scroll-bar part under the pointer.  The active
   axis (and therefore which coordinate, border and part names are used) follows
   BAR->horizontal.  */
static enum scroll_bar_part
wlshm_scroll_bar_part_at (struct scroll_bar *bar, int px, int py,
			  int *pos_out, int *range_out)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  (void) f;			/* only consumed by macros that ignore it */
  int range, pos;
  enum scroll_bar_part part;

  if (bar->horizontal)
    {
      range = HORIZONTAL_SCROLL_BAR_LEFT_RANGE (f, bar->width);
      pos = px - bar->left - HORIZONTAL_SCROLL_BAR_LEFT_BORDER;
      if (pos < 0)
	pos = 0;
      if (pos > range)
	pos = range;
      if (pos < bar->start)
	part = scroll_bar_before_handle;
      else if (pos < bar->end + HORIZONTAL_SCROLL_BAR_MIN_HANDLE)
	part = scroll_bar_horizontal_handle;
      else
	part = scroll_bar_after_handle;
    }
  else
    {
      range = VERTICAL_SCROLL_BAR_TOP_RANGE (f, bar->height);
      pos = py - bar->top - VERTICAL_SCROLL_BAR_TOP_BORDER;
      if (pos < 0)
	pos = 0;
      if (pos > range)
	pos = range;
      if (pos < bar->start)
	part = scroll_bar_above_handle;
      else if (pos < bar->end + VERTICAL_SCROLL_BAR_MIN_HANDLE)
	part = scroll_bar_handle;
      else
	part = scroll_bar_below_handle;
    }

  *pos_out = pos;
  *range_out = range;
  return part;
}

/* Fill IE for a click on BAR (port of x_scroll_bar_handle_click).  */
static void
wlshm_scroll_bar_handle_click (struct scroll_bar *bar, int button, int mods,
			       int px, int py, bool press,
			       struct input_event *ie)
{
  EVENT_INIT (*ie);
  ie->kind = (bar->horizontal
	      ? HORIZONTAL_SCROLL_BAR_CLICK_EVENT
	      : SCROLL_BAR_CLICK_EVENT);
  ie->code = button;
  ie->modifiers = mods | (press ? down_modifier : up_modifier);
  ie->frame_or_window = bar->window;
  ie->arg = Qnil;

  int pos, range;
  ie->part = wlshm_scroll_bar_part_at (bar, px, py, &pos, &range);

  if (bar->horizontal)
    {
      if (!press && bar->dragging != -1)
	{
	  int new_start = -bar->dragging;
	  int new_end = new_start + bar->end - bar->start;
	  wlshm_scroll_bar_set_handle (bar, new_start, new_end, false);
	  bar->dragging = -1;
	}
      XSETINT (ie->x, range);
      XSETINT (ie->y, pos);
    }
  else
    {
      if (!press && bar->dragging != -1)
	{
	  int new_start = pos - bar->dragging;
	  int new_end = new_start + bar->end - bar->start;
	  wlshm_scroll_bar_set_handle (bar, new_start, new_end, false);
	  bar->dragging = -1;
	}
      XSETINT (ie->x, pos);
      XSETINT (ie->y, range);
    }
}

/* Visually drag BAR's handle (port of x_scroll_bar_note_movement).  */
static void
wlshm_scroll_bar_note_movement (struct scroll_bar *bar, int px, int py)
{
  struct frame *f = XFRAME (XWINDOW (bar->window)->frame);
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  dpyinfo->last_mouse_scroll_bar = bar;
  f->mouse_moved = true;

  if (bar->dragging != -1)
    {
      int pos = (bar->horizontal
		 ? px - bar->left - HORIZONTAL_SCROLL_BAR_LEFT_BORDER
		 : py - bar->top - VERTICAL_SCROLL_BAR_TOP_BORDER);
      int new_start = pos - bar->dragging;
      if (new_start != bar->start)
	{
	  int new_end = new_start + bar->end - bar->start;
	  wlshm_scroll_bar_set_handle (bar, new_start, new_end, false);
	}
    }
}

/* Report scroll-bar position for mouse-position (drag tracking).  */
static void
wlshm_scroll_bar_report_motion (struct frame **fp, Lisp_Object *bar_window,
				enum scroll_bar_part *part, Lisp_Object *x,
				Lisp_Object *y, Time *timestamp)
{
  struct frame *f0 = *fp;
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f0);
  struct scroll_bar *bar = dpyinfo->last_mouse_scroll_bar;
  struct frame *f;
  int px = dpyinfo->last_mouse_motion_x, py = dpyinfo->last_mouse_motion_y;

  if (!bar)
    return;

  f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  *fp = f;
  *bar_window = bar->window;

  /* While dragging, report where the handle TOP would land (pointer minus the
     grab offset) and force the handle part, mirroring x_scroll_bar_report_motion
     -- scroll-bar.el feeds this position straight into set-window-start.  The
     plain part_at returns the raw pointer position, which made a drag jump the
     grabbed point to window-start on every motion (the wrong-feeling drag).  */
  int range, pos;
  if (bar->horizontal)
    {
      range = HORIZONTAL_SCROLL_BAR_LEFT_RANGE (f, bar->width);
      pos = px - bar->left - HORIZONTAL_SCROLL_BAR_LEFT_BORDER;
    }
  else
    {
      range = VERTICAL_SCROLL_BAR_TOP_RANGE (f, bar->height);
      pos = py - bar->top - VERTICAL_SCROLL_BAR_TOP_BORDER;
    }
  if (bar->dragging != -1)
    pos -= bar->dragging;
  if (pos < 0)
    pos = 0;
  if (pos > range)
    pos = range;

  if (bar->dragging != -1)
    *part = bar->horizontal ? scroll_bar_horizontal_handle : scroll_bar_handle;
  else if (pos < bar->start)
    *part = bar->horizontal ? scroll_bar_before_handle : scroll_bar_above_handle;
  else if (pos < bar->end + (bar->horizontal
			     ? HORIZONTAL_SCROLL_BAR_MIN_HANDLE
			     : VERTICAL_SCROLL_BAR_MIN_HANDLE))
    *part = bar->horizontal ? scroll_bar_horizontal_handle : scroll_bar_handle;
  else
    *part = bar->horizontal ? scroll_bar_after_handle : scroll_bar_below_handle;

  if (bar->horizontal)
    {
      XSETINT (*x, range);
      XSETINT (*y, pos);
    }
  else
    {
      XSETINT (*x, pos);
      XSETINT (*y, range);
    }

  *timestamp = dpyinfo->last_mouse_movement_time;
  f0->mouse_moved = false;
  /* Clear so mouse-position stops taking the scroll-bar branch once the drag
     ends (re-armed by note_movement on the next motion over the bar).  */
  dpyinfo->last_mouse_scroll_bar = NULL;
}

static int
wlshm_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  enum { BATCH = 64 };
  WlshmEvent evs[BATCH];

  block_input ();
  wlshm_window_dispatch ();
  int nev = wlshm_window_poll_events (evs, BATCH);
  unblock_input ();

  struct frame *f = wlshm_any_frame (terminal);
  int count = 0;
  bool need_present = false;	/* set when hover highlight may have changed */

  for (int i = 0; f && i < nev; i++)
    {
      /* Route this event to the frame whose window it belongs to;
	 fall back to any frame for window-less events.  */
      struct frame *ef = wlshm_frame_for_handle (terminal, evs[i].window);
      /* If the event names a window with no live Emacs frame -- a menu/tooltip
	 popup (Rust-only surface) or a frame deleted since the event was
	 queued -- drop it.  Falling through to an unrelated frame here and
	 dereferencing it (FRAME_DISPLAY_INFO / FRAME_PIXEL_TO_TEXT_WIDTH /
	 change_frame_size / note_mouse_highlight below) is a crash.  */
      if (evs[i].window != 0 && !ef)
	continue;
      if (ef)
	f = ef;
      /* Never process an event against a dead or not-yet-initialized frame.  */
      if (!FRAME_LIVE_P (f) || !FRAME_X_OUTPUT (f))
	continue;

      int mods = 0;
      uint32_t b = evs[i].modifiers;
      if (b & WLSHM_MOD_CTRL)
	mods |= ctrl_modifier;
      if (b & WLSHM_MOD_ALT)
	mods |= meta_modifier;
      if (b & WLSHM_MOD_LOGO)
	mods |= super_modifier;

      switch (evs[i].kind)
	{
	case WlshmEventKind_Configure:
	  {
	    uint32_t cw2 = (uint32_t) evs[i].x, ch2 = (uint32_t) evs[i].y;
	    if (cw2 >= 16 && ch2 >= 16)
	      {
		/* change_frame_size takes the NATIVE (pixel) size and does the
		   pixel->text conversion itself (see dispnew.c), so pass the
		   configured surface size straight through -- exactly like
		   xterm's ConfigureNotify path.  Pre-converting here with
		   FRAME_PIXEL_TO_TEXT_WIDTH double-subtracted the fringe and
		   scroll-bar extents, shrinking the frame ~32px below the
		   surface and leaving an unfilled strip on the right/bottom.  */
		change_frame_size (f, (int) cw2, (int) ch2, false, true, false);
		SET_FRAME_GARBAGED (f);
	      }
	  }
	  break;

	case WlshmEventKind_Close:
	  {
	    struct input_event ie;
	    EVENT_INIT (ie);
	    ie.kind = DELETE_WINDOW_EVENT;
	    XSETFRAME (ie.frame_or_window, f);
	    kbd_buffer_store_event_hold (&ie, hold_quit);
	    count++;
	  }
	  break;

	case WlshmEventKind_FocusIn:
	  {
	    struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    if (dpyinfo->x_focus_event_frame != f)
	      {
		wlshm_new_focus_frame (dpyinfo, f);
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

	case WlshmEventKind_FocusOut:
	  {
	    struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    if (dpyinfo->x_focus_event_frame == f)
	      {
		dpyinfo->x_focus_event_frame = NULL;
		wlshm_new_focus_frame (dpyinfo, NULL);
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = FOCUS_OUT_EVENT;
		XSETFRAME (ie.frame_or_window, f);
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	  }
	  break;

	case WlshmEventKind_PointerMotion:
	  {
	    /* Drive hover highlighting (mouse-face, help-echo, buttons).  */
	    struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    int mx = evs[i].x, my = evs[i].y;
	    dpyinfo->last_mouse_movement_time = evs[i].time;
	    dpyinfo->last_mouse_motion_frame = f;
	    dpyinfo->last_mouse_motion_x = mx;
	    dpyinfo->last_mouse_motion_y = my;
	    /* Scroll-bar drag tracking takes precedence over hover.  */
	    {
	      struct scroll_bar *sb = wlshm_scroll_bar_at (f, mx, my);
	      struct scroll_bar *active
		= (struct scroll_bar *) dpyinfo->last_mouse_scroll_bar;
	      if (sb || (active && active->dragging != -1))
		{
		  wlshm_scroll_bar_note_movement (sb ? sb : active, mx, my);
		  need_present = true;
		  break;
		}
	      dpyinfo->last_mouse_scroll_bar = NULL;
	    }
	    XRectangle *r = &dpyinfo->last_mouse_glyph;
	    if (f != dpyinfo->last_mouse_glyph_frame
		|| mx < r->x || mx >= r->x + r->width
		|| my < r->y || my >= r->y + r->height)
	      {
		f->mouse_moved = true;
		/* Only touch the glyph matrices when they're consistent: not
		   mid-redisplay, initialized, and not garbaged.  Calling
		   note_mouse_highlight otherwise can dereference half-built
		   matrices and crash.  */
		if (!redisplaying_p && f->glyphs_initialized_p
		    && !FRAME_GARBAGED_P (f) && FRAME_X_OUTPUT (f))
		  {
		    wlshm_log ("motion %d,%d -> note_mouse_highlight", mx, my);
		    /* Track help-echo across this hover so we can post a
		       HELP_EVENT when it changes (the echo-area / tooltip help
		       shown over clickable elements, e.g. mode-line buttons).  */
		    previous_help_echo_string = help_echo_string;
		    help_echo_string = Qnil;
		    note_mouse_highlight (f, mx, my);
		    remember_mouse_glyph (f, mx, my, r);
		    dpyinfo->last_mouse_glyph_frame = f;
		    /* note_mouse_highlight draws the mouse-face highlight into
		       the command list; present it so hover is visible without
		       waiting for the next redisplay.  */
		    need_present = true;
		    if (!NILP (help_echo_string)
			|| !NILP (previous_help_echo_string))
		      {
			Lisp_Object frame;
			XSETFRAME (frame, f);
			gen_help_event (help_echo_string, frame, help_echo_window,
					help_echo_object, help_echo_pos);
		      }
		    wlshm_log ("motion %d,%d done", mx, my);
		  }
		else
		  wlshm_log ("motion %d,%d SKIPPED (redisp=%d init=%d garb=%d)",
			    mx, my, redisplaying_p, f->glyphs_initialized_p,
			    FRAME_GARBAGED_P (f));
	      }
	  }
	  break;

	case WlshmEventKind_PointerPress:
	case WlshmEventKind_PointerRelease:
	  {
	    struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	    bool press = (evs[i].kind == WlshmEventKind_PointerPress);
	    wlshm_log ("button %s code=%u at %d,%d mods=%d",
		      press ? "press" : "release", evs[i].button,
		      evs[i].x, evs[i].y, mods);

	    /* A click on a scroll bar becomes a SCROLL_BAR_CLICK_EVENT.  */
	    struct scroll_bar *sb = wlshm_scroll_bar_at (f, evs[i].x, evs[i].y);
	    if (!sb && !press)
	      sb = (struct scroll_bar *) dpyinfo->last_mouse_scroll_bar;
	    if (sb)
	      {
		if (press)
		  {
		    int rel = (sb->horizontal
			       ? evs[i].x - sb->left - HORIZONTAL_SCROLL_BAR_LEFT_BORDER
			       : evs[i].y - sb->top - VERTICAL_SCROLL_BAR_TOP_BORDER);
		    int minh = (sb->horizontal
				? HORIZONTAL_SCROLL_BAR_MIN_HANDLE
				: VERTICAL_SCROLL_BAR_MIN_HANDLE);
		    if (rel >= sb->start && rel < sb->end + minh)
		      sb->dragging = rel - sb->start;
		    dpyinfo->grabbed |= (1 << evs[i].button);
		    dpyinfo->last_mouse_scroll_bar = sb;
		  }
		else
		  dpyinfo->grabbed &= ~(1 << evs[i].button);
		struct input_event sie;
		wlshm_scroll_bar_handle_click (sb, evs[i].button, mods,
					       evs[i].x, evs[i].y, press, &sie);
		sie.timestamp = evs[i].time;
		kbd_buffer_store_event_hold (&sie, hold_quit);
		count++;
		need_present = true;
		break;
	      }

	    /* A click in the tab bar becomes a tab-bar event so tabs switch
	       buffers (mirrors pgtk_handle_event's tab-bar path).  Hover
	       highlight is already handled by note_mouse_highlight on motion.  */
	    Lisp_Object tab_bar_arg = Qnil;
	    bool tab_bar_p = false;
	    if (WINDOWP (f->tab_bar_window)
		&& WINDOW_TOTAL_LINES (XWINDOW (f->tab_bar_window)))
	      {
		Lisp_Object window
		  = window_from_coordinates (f, evs[i].x, evs[i].y, 0,
					     true, true, true);
		tab_bar_p = EQ (window, f->tab_bar_window);
		if (tab_bar_p)
		  tab_bar_arg = handle_tab_bar_click (f, evs[i].x, evs[i].y,
						      press, mods);
	      }

	    /* A click in the tool bar activates the button: handle_tool_bar_click
	       highlights it on press and runs its command on release.  Mirrors
	       xterm/pgtk; without this the Emacs-drawn tool bar was inert.  */
	    bool tool_bar_p = false;
	    if (WINDOWP (f->tool_bar_window)
		&& WINDOW_TOTAL_LINES (XWINDOW (f->tool_bar_window)))
	      {
		Lisp_Object window
		  = window_from_coordinates (f, evs[i].x, evs[i].y, 0,
					     true, true, true);
		tool_bar_p = (EQ (window, f->tool_bar_window)
			      && (press || f->last_tool_bar_item != -1));
		/* Only the primary buttons (left/middle/right) activate tool-bar
		   items; side buttons don't (mirrors xterm's button < 4).  */
		if (tool_bar_p && evs[i].button < 3)
		  handle_tool_bar_click (f, evs[i].x, evs[i].y, press, mods);
	      }

	    if (press)
	      {
		dpyinfo->grabbed |= (1 << evs[i].button);
		dpyinfo->last_mouse_frame = f;
		/* The pointer is stationary at the press; clear any pending
		   "moved" flag from gliding onto the target.  Otherwise the
		   first event a down-mouse handler reads under `track-mouse'
		   (e.g. widget-button-click) is a synthetic motion, which it
		   treats as a drag and cancels -- so widget/custom buttons
		   needed a second click.  A real drag re-sets mouse_moved on
		   the next motion after the press.  */
		f->mouse_moved = false;
	      }
	    else
	      dpyinfo->grabbed &= ~(1 << evs[i].button);

	    /* Suppress the generic click for a tab-bar press not yet resolved to
	       a tab, and for any tool-bar click (handled above); otherwise emit
	       it, carrying the tab-bar arg when present so the tab-bar keymap
	       runs.  */
	    if (!(tab_bar_p && NILP (tab_bar_arg)) && !tool_bar_p)
	      {
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = MOUSE_CLICK_EVENT;
		ie.code = evs[i].button;
		ie.timestamp = evs[i].time;
		ie.modifiers = mods | (press ? down_modifier : up_modifier);
		XSETINT (ie.x, evs[i].x);
		XSETINT (ie.y, evs[i].y);
		XSETFRAME (ie.frame_or_window, f);
		if (!NILP (tab_bar_arg))
		  ie.arg = tab_bar_arg;
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	    /* Forget any pressed tool-bar item once the click is not on the tool
	       bar (mirrors xterm), so a release elsewhere doesn't re-fire it.  */
	    if (!tool_bar_p)
	      f->last_tool_bar_item = -1;
	    wlshm_log ("button stored");
	  }
	  break;

	case WlshmEventKind_Drop:
	  {
	    /* A drag-and-drop drop landed on this frame.  The Rust side has
	       already received the payload over a pipe; fetch it via the
	       side-channel getter (the POD event can't carry a string).  Its
	       `.button` flags whether the payload is a `text/uri-list`.  We
	       build a DRAG_N_DROP_EVENT whose .arg is `(uri-list . STRING)` or
	       `(text . STRING)`; the wlshm `[drag-n-drop]` handler in
	       wlshm-win.el routes it through dnd.el.  */
	    const uint8_t *ptr = NULL;
	    uintptr_t len = 0;
	    if (wlshm_window_get_drop (&ptr, &len) == 0 && ptr && len > 0)
	      {
		/* Payload is UTF-8 (text or uri-list); decode like the
		   clipboard getter so multibyte text/filenames survive.  */
		Lisp_Object bytes
		  = make_unibyte_string ((const char *) ptr, (ptrdiff_t) len);
		Lisp_Object text
		  = code_convert_string_norecord (bytes, Qutf_8, false);
		Lisp_Object tag = evs[i].button ? intern ("uri-list")
						: intern ("text");
		struct input_event ie;
		EVENT_INIT (ie);
		ie.kind = DRAG_N_DROP_EVENT;
		ie.modifiers = 0;
		ie.timestamp = evs[i].time;
		ie.arg = Fcons (tag, text);
		XSETINT (ie.x, evs[i].x);
		XSETINT (ie.y, evs[i].y);
		XSETFRAME (ie.frame_or_window, f);
		kbd_buffer_store_event_hold (&ie, hold_quit);
		count++;
	      }
	  }
	  break;

	case WlshmEventKind_Preedit:
	  {
	    /* The IME (zwp_text_input_v3) updated the in-progress composition.
	       The text comes via a side-channel getter (the POD event can't
	       carry a string); an empty string clears the preedit.  We post a
	       backend-agnostic PREEDIT_TEXT_EVENT whose .arg is the same
	       list-of-parts shape pgtk/x/android use: ((STRING . ATTRS) ...).
	       text-input-v3 gives a single unstyled segment, so we build one
	       part marked underlined; the `[preedit-text]' handler in
	       wlshm-win.el draws it as a zero-width overlay at point.  Committed
	       (final) IME text arrives separately as ordinary KeyPress events.  */
	    const uint8_t *ptr = NULL;
	    uintptr_t len = 0;
	    Lisp_Object arg = Qnil;
	    if (wlshm_window_get_preedit (&ptr, &len) == 0 && ptr && len > 0)
	      {
		Lisp_Object bytes
		  = make_unibyte_string ((const char *) ptr, (ptrdiff_t) len);
		Lisp_Object text
		  = code_convert_string_norecord (bytes, Qutf_8, false);
		/* One part: (TEXT (ul . t)) -> underlined composition.  */
		Lisp_Object part
		  = list2 (text, Fcons (intern ("ul"), Qt));
		arg = list1 (part);
	      }
	    /* arg == Qnil clears the preedit (composition ended/empty).  */
	    struct input_event ie;
	    EVENT_INIT (ie);
	    ie.kind = PREEDIT_TEXT_EVENT;
	    ie.arg = arg;
	    ie.code = 0;
	    ie.modifiers = 0;
	    ie.timestamp = evs[i].time;
	    XSETFRAME (ie.frame_or_window, f);
	    kbd_buffer_store_event_hold (&ie, hold_quit);
	    count++;
	  }
	  break;

	case WlshmEventKind_PointerAxis:
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

	case WlshmEventKind_KeyPress:
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

  /* Flush the hover highlight to the screen (mouse-face on buttons, links,
     mode-line elements).  Skip unless the frame is fully ready: not
     mid-redisplay, live, initialized, not garbaged, with output data.
     Presenting a half-built frame (e.g. a just-created child frame) can
     dereference an inconsistent glyph matrix and crash.  */
  if (need_present && !redisplaying_p
      && FRAME_LIVE_P (f) && FRAME_X_OUTPUT (f)
      && f->glyphs_initialized_p && !FRAME_GARBAGED_P (f))
    {
      block_input ();
      wlshm_present_canvas (f);
      unblock_input ();
    }

  return count;
}

void
wlshm_delete_terminal (struct terminal *terminal)
{
  /* Windows are leaked at process exit (the connection is ManuallyDrop on the
     Rust side to avoid libwayland teardown crashes); per-frame close happens
     via the delete_frame hook.  */
  wlshm_backend_shutdown ();
}

/* terminal->get_focus_frame: the frame that currently holds keyboard focus on
   F's display (tracked from wl_keyboard enter/leave), or nil.  Lets
   `frame-focus-state' and friends report focus accurately.  */
static Lisp_Object
wlshm_get_focus_frame (struct frame *f)
{
  struct frame *focus = FRAME_DISPLAY_INFO (f)->x_focus_frame;
  Lisp_Object lisp_focus;
  if (!focus)
    return Qnil;
  XSETFRAME (lisp_focus, focus);
  return lisp_focus;
}

/* Warp the pointer.  Not implemented (no pointer warping).  */
void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  (void) f; (void) pix_x; (void) pix_y;
}

/* Report the last known pointer position (from motion events) to the core.
   Used by mouse-position and friends.  */
static void
wlshm_mouse_position (struct frame **fp, int insist, Lisp_Object *bar_window,
		     enum scroll_bar_part *part, Lisp_Object *x, Lisp_Object *y,
		     Time *timestamp)
{
  struct frame *f = *fp;
  if (!f || !FRAME_WLSHM_P (f))
    return;
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  *bar_window = Qnil;
  *part = (enum scroll_bar_part) 0;

  /* While interacting with a scroll bar, report its position so Lisp-side
     drag tracking (scroll-bar.el) can follow the handle.  */
  if (dpyinfo->last_mouse_scroll_bar && insist == 0)
    {
      wlshm_scroll_bar_report_motion (fp, bar_window, part, x, y, timestamp);
      return;
    }

  if (dpyinfo->last_mouse_motion_frame)
    *fp = dpyinfo->last_mouse_motion_frame;
  XSETINT (*x, dpyinfo->last_mouse_motion_x);
  XSETINT (*y, dpyinfo->last_mouse_motion_y);
  *timestamp = dpyinfo->last_mouse_movement_time;

  /* Consume the motion: the mouse_position_hook MUST clear mouse_moved, or the
     core keeps regenerating mouse-movement events with nothing new -- an
     unbounded spin during a track-mouse drag/click (each event re-runs
     buffer_posn_from_coords), i.e. an instant freeze on click/highlight.
     Mirrors pgtk_mouse_position / XTmouse_position.  */
  f->mouse_moved = false;
  if (*fp)
    (*fp)->mouse_moved = false;
}

/* No X resource database; return no value for every query.  */
static const char *
wlshm_get_string_resource (void *rdb, const char *name, const char *class)
{
  return NULL;
}

/* Install FONT_OBJECT as frame F's font and recompute the char metrics.  */
static Lisp_Object
wlshm_new_font (struct frame *f, Lisp_Object font_object, int fontset)
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
    int height = FRAME_LINE_HEIGHT (f);
    int sb = 16;			/* scroll-bar thickness in pixels */
    if (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) <= 0)
      {
	FRAME_CONFIG_SCROLL_BAR_WIDTH (f) = sb;
	FRAME_CONFIG_SCROLL_BAR_COLS (f) = (sb + wid - 1) / wid;
      }
    if (FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) <= 0)
      {
	FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) = sb;
	FRAME_CONFIG_SCROLL_BAR_LINES (f) = (sb + height - 1) / height;
      }
  }

  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 3, false, Qfont);
  return font_object;
}

/* ------------------------------------------------------------------ */
/* Popup menus and dialogs.                                           */
/*                                                                    */
/* No toolkit: a menu/dialog is drawn directly onto a dedicated popup */
/* Wayland window's Cairo surface (using the parent frame's scaled    */
/* font), and a self-contained modal loop pumps Wayland events        */
/* (non-blocking, mirroring wlshm_read_socket's discipline so it does */
/* not freeze).  menu_show_hook is synchronous and returns the chosen */
/* item's value, exactly as the toolkit backends do.                  */
/*                                                                    */
/* The popup is anchored at the requested frame-local (x,y) via an     */
/* xdg_positioner (see make_positioner / wlshm_window_set_geometry),   */
/* so menus open at the click/pointer just like the toolkit backends.  */
/* Limitation (documented): the menu is shown as a single flat list    */
/* (nested submenus are flattened with their pane headings).           */
/* ------------------------------------------------------------------ */

/* X keysyms we react to in the modal loop.  */
enum { WLSHM_KS_RET = 0xff0d, WLSHM_KS_ESC = 0xff1b, WLSHM_KS_UP = 0xff52,
       WLSHM_KS_DOWN = 0xff54, WLSHM_KS_HOME = 0xff50, WLSHM_KS_END = 0xff57 };

/* One displayed menu/dialog row.  */
struct wlshm_mrow
{
  char *label_s;	/* display text (UTF-8, xmalloc'd), or NULL */
  char *key_s;		/* equiv-key hint (UTF-8, xmalloc'd), or NULL */
  Lisp_Object value;	/* dialog: button value (menus use `index') */
  ptrdiff_t index;	/* menu_items slot, or -1 if not selectable */
  bool enabled;
  bool title;		/* a heading (pane name / dialog question) */
  bool separator;
};

/* Copy a Lisp string to a freshly xmalloc'd UTF-8 C string (NULL if not a
   non-empty string).  The menu modal loop renders from these C strings rather
   than dereferencing Lisp_Objects, so a GC mid-loop can never dangle them and
   we avoid re-encoding every redraw.  */
static char *
wlshm_dup_utf8 (Lisp_Object s)
{
  if (!STRINGP (s) || SCHARS (s) == 0)
    return NULL;
  return xstrdup (SSDATA (ENCODE_UTF_8 (s)));
}

/* Free ROWS and every row's xmalloc'd strings.  */
static void
wlshm_free_rows (struct wlshm_mrow *rows, int n)
{
  for (int k = 0; k < n; k++)
    {
      xfree (rows[k].label_s);
      xfree (rows[k].key_s);
    }
  xfree (rows);
}

/* Heap bundle freed (rows + strings + the bundle, and PW closed if nonzero) on
   any exit via record_unwind_protect_ptr.  */
struct wlshm_menu_data
{
  struct wlshm_mrow *rows;
  int n;
  uint64_t pw;
};

static void
wlshm_free_menu_data (void *p)
{
  struct wlshm_menu_data *d = p;
  if (d->pw)
    wlshm_window_close (d->pw);
  wlshm_free_rows (d->rows, d->n);
  xfree (d);
}

static cairo_scaled_font_t *
wlshm_frame_scaled_font (struct frame *f)
{
  struct font *ft = FRAME_FONT (f);
  return ft ? ((struct font_info *) ft)->cr_scaled_font : NULL;
}

static double
wlshm_text_width (cairo_scaled_font_t *sf, struct frame *f, const char *s)
{
  if (!s || !*s)
    return 0;
  if (sf)
    {
      cairo_text_extents_t ext;
      cairo_scaled_font_text_extents (sf, s, &ext);
      return ext.x_advance;
    }
  return strlen (s) * FRAME_COLUMN_WIDTH (f);
}

/* Geometry constants for the popup.  */
enum { WLSHM_M_PADX = 14, WLSHM_M_PADY = 3, WLSHM_M_BORDER = 1, WLSHM_M_GAP = 28 };

/* Render ROWS into a fresh ARGB image surface; report size + row height.  */
static cairo_surface_t *
wlshm_render_menu (struct frame *f, struct wlshm_mrow *rows, int n, int hi,
		   int *out_w, int *out_h, int *out_rh, int *out_seph)
{
  cairo_scaled_font_t *sf = wlshm_frame_scaled_font (f);
  int ascent = FRAME_FONT (f) ? FRAME_FONT (f)->ascent : 12;
  int fh = FRAME_FONT (f) ? FRAME_FONT (f)->height : 16;
  int rh = fh + 2 * WLSHM_M_PADY;
  int seph = max (5, rh / 2);

  double maxw = 40;
  for (int k = 0; k < n; k++)
    {
      double w = wlshm_text_width (sf, f, rows[k].label_s);
      if (rows[k].key_s)
	w += WLSHM_M_GAP + wlshm_text_width (sf, f, rows[k].key_s);
      maxw = max (maxw, w);
    }
  int menu_w = (int) maxw + 2 * WLSHM_M_PADX + 2 * WLSHM_M_BORDER;
  int menu_h = WLSHM_M_BORDER;
  for (int k = 0; k < n; k++)
    menu_h += rows[k].separator ? seph : rh;
  menu_h += WLSHM_M_BORDER;

  float bgr, bgg, bgb, fgr, fgg, fgb;
  wlshm_unpack_pixel (FRAME_BACKGROUND_PIXEL (f), &bgr, &bgg, &bgb);
  wlshm_unpack_pixel (FRAME_FOREGROUND_PIXEL (f), &fgr, &fgg, &fgb);

  /* HiDPI: the popup inherits its parent frame's scale (set in make_popup).
     Render onto a PHYSICAL-pixel surface with a matching device scale so the
     drawing below stays in logical coordinates; the popup's wp_viewport maps
     this physical buffer back down to the logical menu_w x menu_h geometry.
     Byte-identical at scale 1.0.  */
  uint32_t scale120 = wlshm_window_scale120 (WLSHM_FRAME_HANDLE (f));
  if (scale120 < 120)
    scale120 = 120;
  double scale = (double) scale120 / 120.0;
  int phys_w = (int) lround (menu_w * scale);
  int phys_h = (int) lround (menu_h * scale);
  cairo_surface_t *s
    = cairo_image_surface_create (CAIRO_FORMAT_RGB24, phys_w, phys_h);
  /* Bail on allocation failure (nil surface has NULL data); callers treat a
     NULL return like the empty-menu case and skip presenting it.  */
  if (cairo_surface_status (s) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (s);
      return NULL;
    }
  cairo_surface_set_device_scale (s, scale, scale);
  cairo_t *cr = cairo_create (s);
  if (cairo_status (cr) != CAIRO_STATUS_SUCCESS)
    {
      cairo_destroy (cr);
      cairo_surface_destroy (s);
      return NULL;
    }
  /* Snap fills/clips/strokes to physical pixels (sharp at fractional scale);
     text uses cairo_show_text, which keeps the font's own AA.  */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  /* Background + border.  */
  cairo_set_source_rgb (cr, bgr, bgg, bgb);
  cairo_paint (cr);
  cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, 0.5, 0.5, menu_w - 1, menu_h - 1);
  cairo_stroke (cr);
  if (sf)
    cairo_set_scaled_font (cr, sf);

  int y = WLSHM_M_BORDER;
  for (int k = 0; k < n; k++)
    {
      int h = rows[k].separator ? seph : rh;
      bool hot = (k == hi && rows[k].index >= 0 && rows[k].enabled);
      if (hot)
	{
	  cairo_set_source_rgb (cr, 0.20, 0.40, 0.69);	/* selection blue */
	  cairo_rectangle (cr, WLSHM_M_BORDER, y,
			   menu_w - 2 * WLSHM_M_BORDER, h);
	  cairo_fill (cr);
	}
      if (rows[k].separator)
	{
	  cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
	  cairo_set_line_width (cr, 1);
	  cairo_move_to (cr, WLSHM_M_PADX, y + h / 2 + 0.5);
	  cairo_line_to (cr, menu_w - WLSHM_M_PADX, y + h / 2 + 0.5);
	  cairo_stroke (cr);
	}
      else if (rows[k].label_s)
	{
	  if (hot)
	    cairo_set_source_rgb (cr, 1, 1, 1);
	  else if (rows[k].title)
	    cairo_set_source_rgb (cr, (double) fgr * 0.6 + 0.2,
				  (double) fgg * 0.6 + 0.2,
				  (double) fgb * 0.6 + 0.4);
	  else if (!rows[k].enabled)
	    cairo_set_source_rgb (cr, (fgr + bgr) / 2, (fgg + bgg) / 2,
				  (fgb + bgb) / 2);
	  else
	    cairo_set_source_rgb (cr, fgr, fgg, fgb);
	  cairo_move_to (cr, WLSHM_M_PADX, y + WLSHM_M_PADY + ascent);
	  cairo_show_text (cr, rows[k].label_s);
	  if (rows[k].key_s)
	    {
	      double kw = wlshm_text_width (sf, f, rows[k].key_s);
	      cairo_move_to (cr, menu_w - WLSHM_M_PADX - kw,
			     y + WLSHM_M_PADY + ascent);
	      cairo_show_text (cr, rows[k].key_s);
	    }
	}
      y += h;
    }
  cairo_destroy (cr);
  cairo_surface_flush (s);
  *out_w = menu_w; *out_h = menu_h; *out_rh = rh; *out_seph = seph;
  return s;
}

/* Which row contains buffer-y YY, or -1.  */
static int
wlshm_menu_row_at (struct wlshm_mrow *rows, int n, int yy, int rh, int seph)
{
  int y = WLSHM_M_BORDER;
  for (int k = 0; k < n; k++)
    {
      int h = rows[k].separator ? seph : rh;
      if (yy >= y && yy < y + h)
	return k;
      y += h;
    }
  return -1;
}

/* Move highlight to the next/prev selectable row (dir +1/-1).  */
static int
wlshm_menu_step (struct wlshm_mrow *rows, int n, int hi, int dir)
{
  for (int step = 0; step < n; step++)
    {
      hi += dir;
      if (hi < 0) hi = n - 1;
      if (hi >= n) hi = 0;
      if (rows[hi].index >= 0 && rows[hi].enabled)
	return hi;
    }
  return hi;
}

static int
wlshm_menu_first_selectable (struct wlshm_mrow *rows, int n)
{
  for (int k = 0; k < n; k++)
    if (rows[k].index >= 0 && rows[k].enabled)
      return k;
  return -1;
}

/* Present SURF to popup window PW, then run the modal loop.  Returns the
   highlighted row index chosen (>=0), or -1 on cancel.  ROWS/N/RH/SEPH
   describe the layout; *HIP is the in/out highlight.  Recreates the surface
   on highlight change via RENDER (NULL → caller redraws).  */
static int
wlshm_menu_modal_loop (struct frame *f, uint64_t pw,
		       struct wlshm_mrow *rows, int n, int mw, int mh,
		       int rh, int seph, int *hip)
{
  int hi = *hip, result = -1;
  bool done = false, cancelled = false;
  int wlfd = wlshm_window_fd ();
  cairo_surface_t *surf = NULL;
  bool need_draw = true;

  /* Clear any key-repeat armed by the keystroke that opened the menu, so it
     does not auto-fire while we run our own modal loop.  */
  wlshm_window_disarm_repeat ();

  while (!done)
    {
      if (need_draw)
	{
	  if (surf)
	    cairo_surface_destroy (surf);
	  int tw, th, trh, tsh;
	  surf = wlshm_render_menu (f, rows, n, hi, &tw, &th, &trh, &tsh);
	  /* Skip presenting if the surface failed to allocate (NULL data).  */
	  if (surf)
	    wlshm_window_present (pw, cairo_image_surface_get_data (surf),
				 (uint32_t) cairo_image_surface_get_width (surf),
				 (uint32_t) cairo_image_surface_get_height (surf),
				 cairo_image_surface_get_stride (surf),
				 0, 0, 0, 0);
	  need_draw = false;
	}
      if (wlfd >= 0)
	{
	  struct pollfd pfd = { .fd = wlfd, .events = POLLIN, .revents = 0 };
	  poll (&pfd, 1, 40);
	}
      wlshm_window_dispatch ();
      WlshmEvent evs[64];
      int ne = wlshm_window_poll_events (evs, 64);
      for (int j = 0; j < ne && !done; j++)
	{
	  WlshmEvent *e = &evs[j];
	  switch (e->kind)
	    {
	    case WlshmEventKind_PointerMotion:
	      if (e->window == pw)
		{
		  int r = wlshm_menu_row_at (rows, n, e->y, rh, seph);
		  if (r >= 0 && rows[r].index >= 0 && rows[r].enabled && r != hi)
		    { hi = r; need_draw = true; }
		}
	      break;
	    case WlshmEventKind_PointerRelease:
	      if (e->window == pw)
		{
		  int r = wlshm_menu_row_at (rows, n, e->y, rh, seph);
		  if (r >= 0 && rows[r].index >= 0 && rows[r].enabled)
		    { result = r; done = true; }
		}
	      else
		{ cancelled = true; done = true; }	/* click outside */
	      break;
	    case WlshmEventKind_KeyPress:
	      switch (e->keysym)
		{
		case WLSHM_KS_UP:
		  hi = wlshm_menu_step (rows, n, hi, -1); need_draw = true; break;
		case WLSHM_KS_DOWN:
		  hi = wlshm_menu_step (rows, n, hi, +1); need_draw = true; break;
		case WLSHM_KS_HOME:
		  hi = wlshm_menu_first_selectable (rows, n); need_draw = true; break;
		case WLSHM_KS_RET:
		  if (hi >= 0 && rows[hi].index >= 0 && rows[hi].enabled)
		    { result = hi; done = true; }
		  break;
		case WLSHM_KS_ESC:
		  cancelled = true; done = true; break;
		default: break;
		}
	      break;
	    case WlshmEventKind_Close:
	      if (e->window == pw) { cancelled = true; done = true; }
	      break;
	    default:
	      break;
	    }
	}
    }
  if (surf)
    cairo_surface_destroy (surf);
  /* A key held while dismissing the menu must not keep repeating afterward.  */
  wlshm_window_disarm_repeat ();
  *hip = hi;
  return cancelled ? -1 : result;
}

/* Build the menu return value for the selected menu_items slot SEL, mirroring
   x_menu_show's MENU_KEYMAPS / submenu-prefix logic exactly.  */
static Lisp_Object
wlshm_menu_value (ptrdiff_t sel, int menuflags)
{
  Lisp_Object *target = aref_addr (menu_items, sel);
  Lisp_Object prefix = Qnil, entry = Qnil;
  Lisp_Object *substack = alloca (menu_items_used * sizeof *substack);
  int depth = 0, i = 0;
  while (i < menu_items_used)
    {
      Lisp_Object e = AREF (menu_items, i);
      if (NILP (e))
	{ substack[depth++] = prefix; prefix = entry; i++; }
      else if (EQ (e, Qlambda))
	{ prefix = substack[--depth]; i++; }
      else if (EQ (e, Qt))
	{ prefix = AREF (menu_items, i + MENU_ITEMS_PANE_PREFIX);
	  i += MENU_ITEMS_PANE_LENGTH; }
      else if (EQ (e, Qquote))
	i++;
      else
	{
	  entry = AREF (menu_items, i + MENU_ITEMS_ITEM_VALUE);
	  if (aref_addr (menu_items, i) == target)
	    {
	      if (menuflags & MENU_KEYMAPS)
		{
		  entry = list1 (entry);
		  if (!NILP (prefix))
		    entry = Fcons (prefix, entry);
		  for (int jj = depth - 1; jj >= 0; jj--)
		    if (!NILP (substack[jj]))
		      entry = Fcons (substack[jj], entry);
		}
	      return entry;
	    }
	  i += MENU_ITEMS_ITEM_LENGTH;
	}
    }
  return Qnil;
}

/* Flatten the global menu_items vector into display rows.  */
static struct wlshm_mrow *
wlshm_menu_rows (int *nout)
{
  struct wlshm_mrow *rows = xnmalloc (menu_items_used + 1, sizeof *rows);
  int n = 0, i = 0;
  while (i < menu_items_used)
    {
      Lisp_Object e = AREF (menu_items, i);
      if (NILP (e) || EQ (e, Qlambda) || EQ (e, Qquote))
	i++;			/* submenu/markers: flatten */
      else if (EQ (e, Qt))
	{
	  Lisp_Object name = AREF (menu_items, i + MENU_ITEMS_PANE_NAME);
	  if (STRINGP (name) && SCHARS (name) > 0)
	    {
	      rows[n] = (struct wlshm_mrow) { .label_s = wlshm_dup_utf8 (name),
		.key_s = NULL, .value = Qnil, .index = -1, .enabled = false,
		.title = true };
	      n++;
	    }
	  i += MENU_ITEMS_PANE_LENGTH;
	}
      else
	{
	  Lisp_Object name = AREF (menu_items, i + MENU_ITEMS_ITEM_NAME);
	  bool sep = (!STRINGP (name) || SCHARS (name) == 0
		      || menu_separator_name_p (SSDATA (name)));
	  rows[n] = (struct wlshm_mrow) {
	    .label_s = sep ? NULL : wlshm_dup_utf8 (name),
	    .key_s = wlshm_dup_utf8 (AREF (menu_items, i + MENU_ITEMS_ITEM_EQUIV_KEY)),
	    .value = Qnil,
	    .index = sep ? -1 : i,
	    .enabled = !NILP (AREF (menu_items, i + MENU_ITEMS_ITEM_ENABLE)),
	    .title = false, .separator = sep };
	  n++;
	  i += MENU_ITEMS_ITEM_LENGTH;
	}
    }
  *nout = n;
  return rows;
}

/* menu_show_hook: display the prepared menu_items as a popup and return the
   selected value (or Qnil if cancelled).  */
static Lisp_Object
wlshm_menu_show (struct frame *f, int x, int y, int menuflags,
		Lisp_Object title, const char **error_name)
{
  if (error_name)
    *error_name = NULL;
  int n;
  struct wlshm_mrow *rows = wlshm_menu_rows (&n);
  if (n == 0)
    { wlshm_free_rows (rows, n); return Qnil; }

  int hi = wlshm_menu_first_selectable (rows, n);
  int mw, mh, rh, seph;
  cairo_surface_t *probe = wlshm_render_menu (f, rows, n, hi, &mw, &mh, &rh, &seph);
  /* Bail if rendering failed: the geometry out-params are then unset.  */
  if (!probe)
    { wlshm_free_rows (rows, n); return Qnil; }
  cairo_surface_destroy (probe);	/* just for geometry */

  char *tc = STRINGP (title) ? SSDATA (ENCODE_UTF_8 (title)) : NULL;
  uint64_t pw = wlshm_window_open (tc, WLSHM_FRAME_HANDLE (f), 1 /* Popup */);
  if (pw == 0)
    { wlshm_free_rows (rows, n); return Qnil; }

  /* Free the rows + close the popup on ANY exit (incl. a non-local one).  */
  struct wlshm_menu_data *md = xmalloc (sizeof *md);
  *md = (struct wlshm_menu_data){ rows, n, pw };
  specpdl_ref count = SPECPDL_INDEX ();
  record_unwind_protect_ptr (wlshm_free_menu_data, md);

  wlshm_window_set_geometry (pw, x, y, mw, mh);

  block_input ();
  int chosen = wlshm_menu_modal_loop (f, pw, rows, n, mw, mh, rh, seph, &hi);
  unblock_input ();

  /* Capture the selected menu_items slot BEFORE unbind frees `rows'.  */
  ptrdiff_t sel = (chosen >= 0 && rows[chosen].index >= 0) ? rows[chosen].index : -1;
  unbind_to (count, Qnil);	/* closes pw + frees rows/strings */
  return sel >= 0 ? wlshm_menu_value (sel, menuflags) : Qnil;
}

/* popup_dialog_hook: CONTENTS is (TITLE (BUTTON . VALUE) ...).  Draw the
   question + buttons and return the chosen button's value.  */
static Lisp_Object
wlshm_popup_dialog (struct frame *f, Lisp_Object header, Lisp_Object contents)
{
  if (!CONSP (contents))
    return Qnil;
  Lisp_Object question = CAR (contents);
  Lisp_Object items = CDR (contents);
  ptrdiff_t cnt = list_length (items);
  struct wlshm_mrow *rows = xnmalloc (cnt + 1, sizeof *rows);
  int n = 0;
  if (STRINGP (question))
    rows[n++] = (struct wlshm_mrow) { .label_s = wlshm_dup_utf8 (question),
      .key_s = NULL, .value = Qnil, .index = -1, .enabled = false,
      .title = true };
  for (Lisp_Object t = items; CONSP (t); t = CDR (t))
    {
      Lisp_Object it = CAR (t);
      if (CONSP (it) && STRINGP (CAR (it)))
	rows[n++] = (struct wlshm_mrow) { .label_s = wlshm_dup_utf8 (CAR (it)),
	  .key_s = NULL, .value = CDR (it), .index = n, .enabled = true,
	  .title = false };
      else if (NILP (it))
	rows[n++] = (struct wlshm_mrow) { .label_s = NULL,
	  .key_s = NULL, .value = Qnil, .index = -1, .separator = true };
    }
  if (n == 0)
    { wlshm_free_rows (rows, n); return Qnil; }

  int hi = wlshm_menu_first_selectable (rows, n);
  int mw, mh, rh, seph;
  cairo_surface_t *probe = wlshm_render_menu (f, rows, n, hi, &mw, &mh, &rh, &seph);
  /* Bail if rendering failed: the geometry out-params are then unset.  */
  if (!probe)
    { wlshm_free_rows (rows, n); return Qnil; }
  cairo_surface_destroy (probe);
  uint64_t pw = wlshm_window_open (STRINGP (question) ? SSDATA (ENCODE_UTF_8 (question)) : NULL,
				   WLSHM_FRAME_HANDLE (f), 1);
  if (pw == 0)
    { wlshm_free_rows (rows, n); return Qnil; }

  struct wlshm_menu_data *md = xmalloc (sizeof *md);
  *md = (struct wlshm_menu_data){ rows, n, pw };
  specpdl_ref count = SPECPDL_INDEX ();
  record_unwind_protect_ptr (wlshm_free_menu_data, md);

  wlshm_window_set_geometry (pw, 0, 0, mw, mh);
  block_input ();
  int chosen = wlshm_menu_modal_loop (f, pw, rows, n, mw, mh, rh, seph, &hi);
  unblock_input ();
  /* Capture the chosen button value BEFORE unbind frees `rows'.  */
  Lisp_Object result = (chosen >= 0) ? rows[chosen].value : Qnil;
  unbind_to (count, Qnil);	/* closes pw + frees rows/strings */
  /* A cancelled dialog signals quit, like the other backends.  */
  if (chosen < 0)
    quit ();
  return result;
}

struct terminal *
wlshm_create_terminal (struct wlshm_display_info *dpyinfo)
{
  struct terminal *terminal
    = create_terminal (output_wlshm, &wlshm_redisplay_interface);

  terminal->display_info.wlshm = dpyinfo;
  dpyinfo->terminal = terminal;

  terminal->clear_frame_hook = wlshm_clear_frame;
  terminal->update_begin_hook = wlshm_update_begin;
  terminal->update_end_hook = wlshm_update_end;
  terminal->ring_bell_hook = wlshm_ring_bell;
  terminal->read_socket_hook = wlshm_read_socket;
  terminal->mouse_position_hook = wlshm_mouse_position;
  terminal->frame_rehighlight_hook = wlshm_frame_rehighlight_hook;
  terminal->implicit_set_name_hook = wlshm_implicitly_set_name;
  terminal->menu_show_hook = wlshm_menu_show;
  terminal->popup_dialog_hook = wlshm_popup_dialog;
  terminal->frame_up_to_date_hook = wlshm_frame_up_to_date;
  terminal->delete_terminal_hook = wlshm_delete_terminal;
  terminal->get_focus_frame = wlshm_get_focus_frame;
  terminal->change_tab_bar_height_hook = wlshm_change_tab_bar_height;
  terminal->change_tool_bar_height_hook = wlshm_change_tool_bar_height;
  terminal->get_string_resource_hook = wlshm_get_string_resource;
  terminal->set_new_font_hook = wlshm_new_font;
  terminal->defined_color_hook = wlshm_defined_color;
  terminal->query_colors = wlshm_query_colors;
  terminal->query_frame_background_color = wlshm_query_frame_background_color;

  /* Scroll bars (Emacs-drawn onto the canvas).  */
  terminal->set_vertical_scroll_bar_hook = wlshm_set_vertical_scroll_bar;
  terminal->set_horizontal_scroll_bar_hook = wlshm_set_horizontal_scroll_bar;
  terminal->condemn_scroll_bars_hook = wlshm_condemn_scroll_bars;
  terminal->redeem_scroll_bar_hook = wlshm_redeem_scroll_bar;
  terminal->judge_scroll_bars_hook = wlshm_judge_scroll_bars;
  terminal->set_scroll_bar_default_width_hook = wlshm_set_scroll_bar_default_width;
  terminal->set_scroll_bar_default_height_hook = wlshm_set_scroll_bar_default_height;

  /* Define the standard fringe bitmaps (continuation/truncation arrows,
     empty-line and buffer-boundary indicators).  */
  gui_init_fringe (terminal->rif);

  /* Frame lifecycle.  */
  terminal->delete_frame_hook = wlshm_destroy_window;
  terminal->frame_visible_invisible_hook = wlshm_make_frame_visible_invisible;
  terminal->iconify_frame_hook = wlshm_iconify_frame;
  terminal->fullscreen_hook = wlshm_fullscreen_hook;
  terminal->set_window_size_hook = wlshm_set_window_size;
  terminal->toggle_invisible_pointer_hook = wlshm_toggle_invisible_pointer;
  terminal->free_pixmap = wlshm_free_pixmap;
  /* Documented Wayland-limitation no-ops.  */
  terminal->frame_raise_lower_hook = wlshm_frame_raise_lower;
  terminal->set_frame_offset_hook = wlshm_set_frame_offset;
  terminal->set_bitmap_icon_hook = wlshm_set_bitmap_icon;

  return terminal;
}

struct wlshm_display_info *
wlshm_term_init (Lisp_Object display_name)
{
  if (wlshm_backend_init () != WLSHM_OK)
    error ("wlshm backend failed to initialize");
  /* Connect to Wayland now (so the output scale is known); per-frame windows
     are opened in Fx_create_frame.  */
  if (wlshm_backend_connect () != 0)
    error ("wlshm: cannot connect to Wayland (is WAYLAND_DISPLAY set?)");

  block_input ();

  struct wlshm_display_info *dpyinfo = xzalloc (sizeof *dpyinfo);
  struct terminal *terminal = wlshm_create_terminal (dpyinfo);

  terminal->kboard = allocate_kboard (Qwlshm);
  /* Don't let the initial kboard remain current longer than necessary.  */
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;
  terminal->kboard->reference_count++;

  /* Emacs works in LOGICAL pixels: HiDPI crispness is handled transparently
     by the Cairo device scale on each frame's canvas (logical drawing,
     physical rasterization), so the display resolution and scale stay logical
     (96 DPI, scale 1).  dpyinfo->scale is informational only.  */
  dpyinfo->name_list_element = Fcons (display_name, Qnil);
  /* Load the X11 color-name database so named face colors resolve.  */
  if (NILP (wlshm_color_map))
    wlshm_color_map
      = Fx_load_color_file (Fexpand_file_name (build_string ("rgb.txt"),
					       Vdata_directory));
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;
  /* True-color depth: like pgtk.  A nonzero value (>= 2) also makes disabled
     tool-bar images use the gray-fade path instead of a cross-out (the
     monochrome n_planes < 2 fallback in image_disable_image).  */
  dpyinfo->n_planes = 24;
  dpyinfo->resx = 96.0;
  dpyinfo->resy = 96.0;
  dpyinfo->scale = 1.0;
  reset_mouse_highlight (&dpyinfo->mouse_highlight);

  terminal->name = xlispstrdup (display_name);

  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  int fd = wlshm_window_fd ();
  if (fd >= 0)
    add_keyboard_wait_descriptor (fd);
  /* Also wake on the key-repeat timerfd so held keys repeat.  */
  int tfd = wlshm_window_timer_fd ();
  if (tfd >= 0)
    add_keyboard_wait_descriptor (tfd);
  /* And on the present-deadline timerfd, so a frame coalesced by the present
     throttle is flushed even if the compositor withholds its frame callback and
     nothing else wakes the loop (mode line otherwise stays stale until a mouse
     event); read_socket -> wlshm_window_dispatch -> flush_overdue_pending.  */
  int ptfd = wlshm_window_present_timer_fd ();
  if (ptfd >= 0)
    add_keyboard_wait_descriptor (ptfd);

  unblock_input ();
  return dpyinfo;
}

DEFUN ("wlshm-test-menu-render", Fwlshm_test_menu_render,
       Swlshm_test_menu_render, 3, 3, 0,
       doc: /* Render a menu of ITEMS (highlight row HIGHLIGHT) to PNG FILE.
ITEMS is a list whose elements are: a string (enabled item), a cons
(LABEL . ENABLED) (ENABLED nil = disabled), or nil (a separator).  Used by
the test harness to prove menu rendering without the interactive loop.  */)
  (Lisp_Object items, Lisp_Object highlight, Lisp_Object file)
{
  struct frame *f = SELECTED_FRAME ();
  CHECK_STRING (file);
  /* Test-only surface: gated behind WLSHM_DEBUG (the harness sets it).  */
  if (!getenv ("WLSHM_DEBUG"))
    error ("wlshm-test-menu-render is a test primitive; set WLSHM_DEBUG to use it");
  ptrdiff_t cnt = list_length (items);
  struct wlshm_mrow *rows = xnmalloc (cnt + 1, sizeof *rows);
  int n = 0;
  for (Lisp_Object t = items; CONSP (t); t = CDR (t))
    {
      Lisp_Object it = CAR (t);
      if (NILP (it))
	rows[n] = (struct wlshm_mrow) { .label_s = NULL,
	  .key_s = NULL, .value = Qnil, .index = -1, .separator = true };
      else if (STRINGP (it))
	rows[n] = (struct wlshm_mrow) { .label_s = wlshm_dup_utf8 (it),
	  .key_s = NULL, .value = Qnil, .index = n, .enabled = true };
      else if (CONSP (it) && STRINGP (CAR (it)))
	rows[n] = (struct wlshm_mrow) { .label_s = wlshm_dup_utf8 (CAR (it)),
	  .key_s = NULL, .value = Qnil, .index = NILP (CDR (it)) ? -1 : n,
	  .enabled = !NILP (CDR (it)) };
      else
	continue;
      n++;
    }
  int hi = FIXNUMP (highlight) ? XFIXNUM (highlight) : -1;
  int w, h, rh, sh;
  cairo_surface_t *s = wlshm_render_menu (f, rows, n, hi, &w, &h, &rh, &sh);
  /* Render can fail to allocate its surface; report failure rather than
     writing a NULL surface.  */
  if (!s)
    { wlshm_free_rows (rows, n); return Qnil; }
  cairo_status_t st
    = cairo_surface_write_to_png (s, SSDATA (ENCODE_FILE (file)));
  cairo_surface_destroy (s);
  wlshm_free_rows (rows, n);
  return st == CAIRO_STATUS_SUCCESS ? Qt : Qnil;
}

void
syms_of_wlshmterm (void)
{
  defsubr (&Swlshm_test_menu_render);
  staticpro (&wlshm_color_map);
  wlshm_color_map = Qnil;

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = 1;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = 0;

  /* nil: this backend draws its own (non-toolkit) scroll bars.  Defining the
     variable at all is what makes loadup.el preload scroll-bar.el, so
     scroll-bar-mode and friends exist in the dumped image (they have no
     autoload cookies; preloading is the only way they become available).  */
  DEFVAR_LISP ("x-toolkit-scroll-bars", Vx_toolkit_scroll_bars,
     doc: /* SKIP: real doc in xterm.c.  */);
  Vx_toolkit_scroll_bars = Qnil;

  Fprovide (Qwlshm, Qnil);
}
