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

static bool
wgpu_defined_color (struct frame *f, const char *name, Emacs_Color *color,
		    bool alloc, bool make_index)
{
  unsigned short r, g, b;
  if (!parse_color_spec (name, &r, &g, &b))
    return false;
  color->red = r;
  color->green = g;
  color->blue = b;
  /* Pack 8-bit-per-channel into the pixel; the draw path unpacks it.  */
  color->pixel = ((unsigned long) (r >> 8) << 16
		  | (unsigned long) (g >> 8) << 8
		  | (unsigned long) (b >> 8));
  return true;
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

static void
wgpu_draw_glyph_string (struct glyph_string *s)
{
  switch (s->first_glyph->type)
    {
    case CHAR_GLYPH:
    case COMPOSITE_GLYPH:
      {
	struct font *font = s->font;
	if (font && font->driver && font->driver->draw)
	  {
	    int y = s->ybase - font->baseline_offset;
	    bool with_bg = !s->background_filled_p && !s->for_overlaps;
	    font->driver->draw (s, 0, s->nchars, s->x, y, with_bg);
	  }
      }
      break;

    case STRETCH_GLYPH:
      {
	float r, g, b;
	wgpu_unpack_pixel (s->face->background, &r, &g, &b);
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
      /* M2: images/glyphless not yet drawn.  */
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
  if (!on_p)
    return;
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  /* Minimal box cursor: fill the cursor glyph cell in the cursor color.  */
  struct glyph *cursor_glyph = get_phys_cursor_glyph (w);
  if (!cursor_glyph)
    return;
  int gx, gy, h, wd;
  get_phys_cursor_geometry (w, glyph_row, cursor_glyph, &gx, &gy, &h);
  wd = cursor_glyph->pixel_width;
  float r, g, b;
  wgpu_unpack_pixel (FRAME_CURSOR_COLOR (f), &r, &g, &b);
  block_input ();
  wgpu_window_rect ((float) gx, (float) gy, (float) wd, (float) h, r, g, b, 1.0f);
  unblock_input ();
}

/* No-op RIF hooks (called unconditionally during redisplay; M2 leaves the
   visual refinements -- fringes, dividers, hourglass -- for later).  */
static void wgpu_scroll_run (struct window *w, struct run *run) {}
static void wgpu_after_update_window_line (struct window *w, struct glyph_row *r) {}
static void wgpu_flush_display (struct frame *f) {}
static void wgpu_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
				     struct draw_fringe_bitmap_params *p) {}
static void wgpu_define_fringe_bitmap (int which, unsigned short *bits,
				       int h, int wd) {}
static void wgpu_destroy_fringe_bitmap (int which) {}
static void wgpu_clear_under_internal_border (struct frame *f) {}
static void wgpu_draw_vertical_window_border (struct window *w, int x, int y0, int y1) {}
static void wgpu_draw_window_divider (struct window *w, int x0, int x1, int y0, int y1) {}
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
  block_input ();
  wgpu_window_rect (0.0f, 0.0f, (float) FRAME_PIXEL_WIDTH (f),
		    (float) FRAME_PIXEL_HEIGHT (f), r, g, b, 1.0f);
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
      change_frame_size (f, FRAME_PIXEL_TO_TEXT_WIDTH (f, ww),
			 FRAME_PIXEL_TO_TEXT_HEIGHT (f, wh), false, true, false);
      SET_FRAME_GARBAGED (f);
    }

  for (int i = 0; f && i < nev; i++)
    {
      struct input_event ie;
      EVENT_INIT (ie);
      XSETFRAME (ie.frame_or_window, f);

      int mods = 0;
      uint32_t b = evs[i].modifiers;
      if (b & WGPU_MOD_CTRL)
	mods |= ctrl_modifier;
      if (b & WGPU_MOD_ALT)
	mods |= meta_modifier;
      if (b & WGPU_MOD_LOGO)
	mods |= super_modifier;

      uint32_t cp = evs[i].unichar, ks = evs[i].keysym;
      /* Treat only genuinely printable codepoints as text; control chars
	 (Backspace -> ^H, Tab, Return, Escape, ...) must go through the
	 keysym path so they map to <backspace>, <tab>, etc. rather than
	 C-h, C-i, ...  */
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

  return count;
}

static void
wgpu_delete_terminal (struct terminal *terminal)
{
  wgpu_window_close ();
  wgpu_backend_shutdown ();
}

/* Warp the pointer.  M3 stub (no pointer support yet).  */
void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  (void) f; (void) pix_x; (void) pix_y;
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
  terminal->frame_up_to_date_hook = wgpu_frame_up_to_date;
  terminal->delete_terminal_hook = wgpu_delete_terminal;
  terminal->get_string_resource_hook = wgpu_get_string_resource;
  terminal->set_new_font_hook = wgpu_new_font;
  terminal->defined_color_hook = wgpu_defined_color;
  terminal->query_colors = wgpu_query_colors;
  terminal->query_frame_background_color = wgpu_query_frame_background_color;

  /* M3+: focus/visibility/size/scroll-bar/menu hooks.  */
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

  unblock_input ();
  return dpyinfo;
}

void
syms_of_wgputerm (void)
{
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
