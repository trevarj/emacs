/* Wayland + wgpu terminal backend for Emacs -- font driver.

The wgpu backend reuses Emacs's FreeType font driver (ftfont) for font
discovery, opening, metrics and shaping, and overrides only:

  * `open'  -- delegate to ftfont_open, then swap in this driver (so our
               `draw' is used) and attach a per-glyph atlas cache;
  * `draw'  -- rasterize each glyph with FreeType (FT_LOAD_RENDER), upload
               its coverage bitmap to the GPU atlas (once, cached), and emit a
               textured quad plus the glyph-string background fill.

This avoids cairo entirely while keeping correct font selection/metrics.
See wgpu-backend-plan.md, "Text / glyph atlas".  */

#include <config.h>

#include <string.h>

#include <fontconfig/fontconfig.h>	/* for FcPattern, used by ftfont.h */

#include "lisp.h"
#include "frame.h"
#include "dispextern.h"
#include "composite.h"
#include "blockinput.h"
#include "charset.h"
#include "font.h"
#include "ftfont.h"
#include "pdumper.h"
#include "wgputerm.h"

/* Qwgpufont is the font type symbol (DEFSYM'd in syms_of_wgpufont, declared
   globally via globals.h); entities listed by this driver carry it so the
   matching open_font (ours) is used.  */

/* Our font driver, filled in at dump time (derived from ftfont).  */
static struct font_driver wgpu_font_driver;

/* Per-glyph atlas cache entry, indexed by glyph code.  */
struct wgpu_glyph
{
  int64_t id;       /* atlas id (>=0), -1 = not cached, -2 = empty (space) */
  int left, top;    /* FreeType bitmap bearings */
  int width, height;
  int advance;      /* pen advance in pixels */
};

static Lisp_Object
wgpufont_list (struct frame *f, Lisp_Object spec)
{
  return ftfont_list2 (f, spec, Qwgpufont);
}

static Lisp_Object
wgpufont_match (struct frame *f, Lisp_Object spec)
{
  return ftfont_match2 (f, spec, Qwgpufont);
}

static Lisp_Object
wgpufont_open (struct frame *f, Lisp_Object entity, int pixel_size)
{
  /* ftfont_open does all the FreeType work and fills struct font.  */
  Lisp_Object font_object = ftfont_open (f, entity, pixel_size);
  if (NILP (font_object))
    return Qnil;

  struct font *font = XFONT_OBJECT (font_object);
  /* Use our driver so redisplay calls wgpufont_draw.  */
  font->driver = &wgpu_font_driver;

  struct font_info *info = (struct font_info *) font;
  FT_Face face = info->ft_size->face;
  unsigned n = (face->num_glyphs > 0) ? (unsigned) face->num_glyphs : 256;
  struct wgpu_glyph *cache = xnmalloc (n, sizeof *cache);
  for (unsigned i = 0; i < n; i++)
    cache[i].id = -1;
  info->glyph_cache = cache;
  info->glyph_cache_size = n;

  return font_object;
}

static void
wgpufont_close (struct font *font)
{
  struct font_info *info = (struct font_info *) font;
  if (info->glyph_cache)
    {
      xfree (info->glyph_cache);
      info->glyph_cache = NULL;
      info->glyph_cache_size = 0;
    }
  ftfont_close (font);
}

/* Return the cached atlas entry for glyph CODE, rasterizing + uploading it on
   first use.  Returns NULL on failure.  */
static struct wgpu_glyph *
wgpufont_get_glyph (struct font_info *info, unsigned code)
{
  if (!info->glyph_cache || code >= info->glyph_cache_size)
    return NULL;
  struct wgpu_glyph *cache = info->glyph_cache;
  struct wgpu_glyph *gc = &cache[code];
  if (gc->id != -1)
    return gc;

  FT_Face face = info->ft_size->face;
  FT_Activate_Size (info->ft_size);
  if (FT_Load_Glyph (face, code, FT_LOAD_RENDER) != 0)
    return NULL;

  FT_GlyphSlot g = face->glyph;
  FT_Bitmap *bm = &g->bitmap;
  unsigned w = bm->width, h = bm->rows;

  gc->left = g->bitmap_left;
  gc->top = g->bitmap_top;
  gc->advance = g->advance.x >> 6;
  gc->width = w;
  gc->height = h;

  if (w == 0 || h == 0)
    {
      gc->id = -2; /* nothing to draw (e.g. space) */
      return gc;
    }

  /* Pack into a contiguous coverage buffer (FreeType rows may be padded).  */
  unsigned char *buf = xnmalloc (w, h);
  for (unsigned row = 0; row < h; row++)
    memcpy (buf + row * w, bm->buffer + row * bm->pitch, w);
  int64_t id = wgpu_window_atlas_upload (w, h, buf, (size_t) w * h);
  xfree (buf);
  if (id < 0)
    return NULL;
  gc->id = id;
  return gc;
}

static void
wgpu_unpack_color (unsigned long pixel, float *r, float *g, float *b)
{
  *r = ((pixel >> 16) & 0xff) / 255.0f;
  *g = ((pixel >> 8) & 0xff) / 255.0f;
  *b = (pixel & 0xff) / 255.0f;
}

static int
wgpufont_draw (struct glyph_string *s, int from, int to, int x, int y,
	       bool with_background)
{
  struct font_info *info = (struct font_info *) s->font;
  float fr, fg, fb;
  wgpu_unpack_color (s->face->foreground, &fr, &fg, &fb);

  block_input ();

  if (with_background)
    {
      float br, bg, bb;
      wgpu_unpack_color (s->face->background, &br, &bg, &bb);
      wgpu_window_rect ((float) s->x, (float) s->y,
			(float) s->background_width, (float) s->height,
			br, bg, bb, 1.0f);
    }

  FT_Activate_Size (info->ft_size);
  int len = to - from;
  for (int i = 0; i < len; i++)
    {
      unsigned code = s->char2b[from + i];
      struct wgpu_glyph *gc = wgpufont_get_glyph (info, code);
      int advance = 0;
      if (gc)
	{
	  advance = gc->advance;
	  if (gc->id >= 0)
	    wgpu_window_glyph (gc->id, (float) (x + gc->left),
			       (float) (y - gc->top), fr, fg, fb, 1.0f);
	}
      x += advance;
    }

  unblock_input ();
  return len;
}

void
register_wgpufont_driver (struct frame *f)
{
  register_font_driver (&wgpu_font_driver, f);
}

static void
syms_of_wgpufont_for_pdumper (void)
{
  wgpu_font_driver.type = Qwgpufont;
  wgpu_font_driver.get_cache = ftfont_get_cache;
  wgpu_font_driver.list = wgpufont_list;
  wgpu_font_driver.match = wgpufont_match;
  wgpu_font_driver.list_family = ftfont_list_family;
  wgpu_font_driver.open_font = wgpufont_open;
  wgpu_font_driver.close_font = wgpufont_close;
  wgpu_font_driver.has_char = ftfont_has_char;
  wgpu_font_driver.encode_char = ftfont_encode_char;
  wgpu_font_driver.text_extents = ftfont_text_extents;
  wgpu_font_driver.draw = wgpufont_draw;
  wgpu_font_driver.get_bitmap = ftfont_get_bitmap;
  wgpu_font_driver.anchor_point = ftfont_anchor_point;
  wgpu_font_driver.filter_properties = ftfont_filter_properties;
  wgpu_font_driver.combining_capability = ftfont_combining_capability;

  register_font_driver (&wgpu_font_driver, NULL);
}

void
syms_of_wgpufont (void)
{
  DEFSYM (Qwgpufont, "wgpu-font");
  pdumper_do_now_and_after_load (syms_of_wgpufont_for_pdumper);
}
