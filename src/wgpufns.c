/* Wayland + wgpu terminal backend for Emacs -- frame/Lisp glue.

M0 scaffold.  This will hold x-create-frame and friends for the wgpu backend,
plus the test/validation primitive `wgpu-dump-frame'.  At M0 only the symbol
table init and the dump-frame stub exist.

See wgpu-backend-plan.md.  */

#include <config.h>

#include <string.h>

#include "lisp.h"
#include "blockinput.h"
#include "frame.h"
#include "window.h"
#include "buffer.h"
#include "dispextern.h"
#include "font.h"
#include "termhooks.h"
#include "coding.h"
#include "wgputerm.h"

/* M1 placeholder "frame" contents: a solid Emacs-teal clear.  M2 replaces this
   with real glyph/command rendering, at which point the goldens gain content.
   Linear RGBA, matching examples/clear_color.rs.  */
#define WGPU_M1_CLEAR_R 0.07
#define WGPU_M1_CLEAR_G 0.15
#define WGPU_M1_CLEAR_B 0.18
#define WGPU_M1_CLEAR_A 1.00

#define WGPU_DEFAULT_DUMP_DIM 64

/* Validation primitive (see plan, "Validation harness").

   Force redisplay of FRAME, replay its command buffer into an offscreen wgpu
   texture, read the pixels back, and write a PNG to FILE.  This is the entry
   point golden-image tests call:

     (wgpu-dump-frame "/tmp/frame.png")

   then the test compares the PNG against a committed golden with tolerance.
   M0: stub; the offscreen path lands with M1.  */
static void
wgpu_dim (Lisp_Object width, Lisp_Object height, uint32_t *w, uint32_t *h)
{
  if (NILP (width))
    *w = WGPU_DEFAULT_DUMP_DIM;
  else
    {
      CHECK_FIXNAT (width);
      *w = XFIXNAT (width);
    }
  if (NILP (height))
    *h = WGPU_DEFAULT_DUMP_DIM;
  else
    {
      CHECK_FIXNAT (height);
      *h = XFIXNAT (height);
    }
}

DEFUN ("wgpu-dump-frame", Fwgpu_dump_frame, Swgpu_dump_frame, 1, 3, 0,
       doc: /* Render an offscreen frame and write it to FILE as a PNG.
Optional WIDTH and HEIGHT default to 64.  Used by the wgpu backend's
golden-image test harness.

M1: the rendered content is a solid clear color; M2 makes it glyph-aware.  */)
  (Lisp_Object file, Lisp_Object width, Lisp_Object height)
{
  uint32_t w, h;
  CHECK_STRING (file);
  wgpu_dim (width, height, &w, &h);
  if (wgpu_render_clear_to_png (SSDATA (ENCODE_FILE (file)), w, h,
				WGPU_M1_CLEAR_R, WGPU_M1_CLEAR_G,
				WGPU_M1_CLEAR_B, WGPU_M1_CLEAR_A)
      != WGPU_OK)
    error ("wgpu-dump-frame: offscreen render failed (no GPU?)");
  return Qt;
}

/* Build a small deterministic demo frame using the frame-command FFI: a row
   of synthetic "glyphs" plus a cursor block.  Exercises the exact path the RIF
   draw hooks will use (atlas upload + fills + glyph quads), end to end from
   Lisp, headless.  M2 placeholder until redisplay drives these calls.  */
static void
wgpu_build_demo (uint32_t w, uint32_t h)
{
  wgpu_frame_begin (w, h, 0.07, 0.15, 0.18, 1.0);

  /* A 6x10 solid coverage "glyph".  */
  unsigned char glyph[6 * 10];
  memset (glyph, 255, sizeof glyph);
  int64_t g = wgpu_atlas_upload (6, 10, glyph, sizeof glyph);

  /* A row of five glyphs in near-white.  */
  for (int i = 0; i < 5; i++)
    wgpu_frame_glyph (g, 8.0f + i * 12, 10.0f, 0.9f, 0.9f, 0.9f, 1.0f);

  /* A red cursor block after them.  */
  wgpu_frame_rect (74.0f, 8.0f, 8.0f, 14.0f, 0.8f, 0.1f, 0.1f, 1.0f);
}

DEFUN ("wgpu--draw-demo", Fwgpu__draw_demo, Swgpu__draw_demo, 1, 1, 0,
       doc: /* Render the M2 demo frame to FILE as a PNG (test helper).  */)
  (Lisp_Object file)
{
  CHECK_STRING (file);
  wgpu_build_demo (128, 48);
  if (wgpu_frame_end_png (SSDATA (ENCODE_FILE (file))) != 0)
    error ("wgpu--draw-demo: render failed (no GPU?)");
  return Qt;
}

DEFUN ("wgpu--demo-rgba", Fwgpu__demo_rgba, Swgpu__demo_rgba, 0, 0, 0,
       doc: /* Return the M2 demo frame as RGBA8 bytes (128x48; test helper).  */)
  (void)
{
  uint32_t w = 128, h = 48;
  wgpu_build_demo (w, h);
  ptrdiff_t n = (ptrdiff_t) w * h * 4;
  Lisp_Object s = make_uninit_string (n);
  if (wgpu_frame_end_rgba (SDATA (s), n) != 0)
    error ("wgpu--demo-rgba: render failed (no GPU?)");
  return s;
}

DEFUN ("wgpu--window-dump", Fwgpu__window_dump, Swgpu__window_dump, 1, 1, 0,
       doc: /* Write the live window's current frame to FILE as a PNG.
For clarity inspection / real-text golden tests.  */)
  (Lisp_Object file)
{
  CHECK_STRING (file);
  if (wgpu_window_dump_png (SSDATA (ENCODE_FILE (file))) != 0)
    error ("wgpu--window-dump: no window or readback failed");
  return Qt;
}

DEFUN ("wgpu--frame-rgba", Fwgpu__frame_rgba, Swgpu__frame_rgba, 0, 2, 0,
       doc: /* Return WIDTHxHEIGHT offscreen-rendered pixels as RGBA8 bytes.
A unibyte string of WIDTH*HEIGHT*4 bytes, row-major, top-down.  WIDTH and
HEIGHT default to 64.  For the golden-image test harness.  */)
  (Lisp_Object width, Lisp_Object height)
{
  uint32_t w, h;
  wgpu_dim (width, height, &w, &h);
  ptrdiff_t n = (ptrdiff_t) w * h * 4;
  Lisp_Object s = make_uninit_string (n);
  if (wgpu_render_clear_rgba (w, h, WGPU_M1_CLEAR_R, WGPU_M1_CLEAR_G,
			      WGPU_M1_CLEAR_B, WGPU_M1_CLEAR_A,
			      SDATA (s), n)
      != WGPU_OK)
    error ("wgpu--frame-rgba: offscreen render failed (no GPU?)");
  return s;
}

/* Return the wgpu display info for OBJECT (a frame, terminal, display name,
   or nil for the default).  Signals if there is no wgpu display.
   M1 stub: only the default display is supported.  */
struct wgpu_display_info *
check_x_display_info (Lisp_Object object)
{
  if (!x_display_list)
    error ("There is no wgpu display");
  return x_display_list;
}

DEFUN ("wgpu-scale-factor", Fwgpu_scale_factor, Swgpu_scale_factor, 0, 0, 0,
       doc: /* Return the integer HiDPI scale factor of the wgpu display.  */)
  (void)
{
  return make_fixnum (x_display_list ? (EMACS_INT) x_display_list->scale : 1);
}

DEFUN ("x-create-frame", Fx_create_frame, Sx_create_frame, 1, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object parms)
{
  struct frame *f;
  Lisp_Object frame, tem, name;
  bool minibuffer_only = false;
  Lisp_Object display;
  struct wgpu_display_info *dpyinfo;
  struct kboard *kb;

  parms = Fcopy_alist (parms);
  Vx_resource_name = Vinvocation_name;

  display = gui_display_get_arg (NULL, parms, Qterminal, 0, 0, RES_TYPE_NUMBER);
  if (BASE_EQ (display, Qunbound))
    display = gui_display_get_arg (NULL, parms, Qdisplay, 0, 0, RES_TYPE_STRING);
  if (BASE_EQ (display, Qunbound))
    display = Qnil;
  dpyinfo = check_x_display_info (display);
  kb = dpyinfo->terminal->kboard;

  if (!dpyinfo->terminal->name)
    error ("Terminal is not live, can't create new frames on it");

  name = gui_display_get_arg (dpyinfo, parms, Qname, "name", "Name",
			      RES_TYPE_STRING);
  if (!STRINGP (name) && !BASE_EQ (name, Qunbound) && !NILP (name))
    error ("Invalid frame name--not a string or nil");
  if (STRINGP (name))
    Vx_resource_name = name;

  tem = gui_display_get_arg (dpyinfo, parms, Qminibuffer, "minibuffer",
			     "Minibuffer", RES_TYPE_SYMBOL);
  if (EQ (tem, Qnone) || NILP (tem))
    f = make_frame_without_minibuffer (Qnil, kb, display);
  else if (EQ (tem, Qonly))
    {
      f = make_minibuffer_frame ();
      minibuffer_only = true;
    }
  else if (WINDOWP (tem))
    f = make_frame_without_minibuffer (tem, kb, display);
  else
    f = make_frame (true);

  XSETFRAME (frame, f);
  f->terminal = dpyinfo->terminal;
  f->output_method = output_wgpu;
  FRAME_X_OUTPUT (f) = xzalloc (sizeof (struct wgpu_output));
  FRAME_FONTSET (f) = -1;
  FRAME_X_OUTPUT (f)->white_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->black_relief.pixel = -1;
  FRAME_DISPLAY_INFO (f) = dpyinfo;

  /* Sensible defaults; the color parameters below refine them.  */
  FRAME_FOREGROUND_PIXEL (f) = 0x000000;
  FRAME_BACKGROUND_PIXEL (f) = 0xffffff;
  FRAME_X_OUTPUT (f)->cursor_color = 0x000000;

  if (BASE_EQ (name, Qunbound) || NILP (name))
    {
      fset_name (f, build_string ("wgpu"));
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
    }

  register_wgpufont_driver (f);
  gui_default_parameter (f, parms, Qfont_backend, Qnil,
			 "fontBackend", "FontBackend", RES_TYPE_STRING);
  wgpu_default_font_parameter (f, parms);
  if (!FRAME_FONT (f))
    {
      delete_frame (frame, Qnoelisp);
      error ("Invalid frame font");
    }

  gui_default_parameter (f, parms, Qborder_width, make_fixnum (0),
			 "borderWidth", "BorderWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qinternal_border_width, make_fixnum (0),
			 "internalBorderWidth", "internalBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbottom_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qvertical_scroll_bars, Qnil,
			 "verticalScrollBars", "ScrollBars", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qhorizontal_scroll_bars, Qnil,
			 "horizontalScrollBars", "ScrollBars", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qforeground_color, build_string ("black"),
			 "foreground", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qbackground_color, build_string ("white"),
			 "background", "Background", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qline_spacing, Qnil,
			 "lineSpacing", "LineSpacing", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qleft_fringe, Qnil,
			 "leftFringe", "LeftFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_fringe, Qnil,
			 "rightFringe", "RightFringe", RES_TYPE_NUMBER);

  init_frame_faces (f);

  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 5, true,
		     Qx_create_frame_1);

  gui_default_parameter (f, parms, Qmenu_bar_lines, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtab_bar_lines, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtool_bar_lines, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbuffer_predicate, Qnil,
			 "bufferPredicate", "BufferPredicate", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qtitle, Qnil, "title", "Title",
			 RES_TYPE_STRING);

  /* Pick an initial size; the compositor's configure (delivered via
     read_socket as a resize) sizes the frame to the real window shortly
     after.  */
  gui_figure_window_size (f, parms, true, true);

  /* Size the frame to the actual Wayland surface up front, so its pixel
     dimensions match the GPU surface/persistent texture from the first paint.
     Otherwise the default 80x36 frame is drawn first and then resized, which
     leaves stale pixels (e.g. buffer text where the mode line lands) in the
     persistent texture.  */
  {
    uint32_t sw = 0, sh = 0;
    wgpu_window_size (&sw, &sh);
    if (sw >= 16 && sh >= 16)
      {
	int tw = FRAME_PIXEL_TO_TEXT_WIDTH (f, (int) sw);
	int th = FRAME_PIXEL_TO_TEXT_HEIGHT (f, (int) sh);
	/* Round the text area down to whole rows/columns so the mode line
	   lands on a glyph-row boundary; otherwise a fractional last row
	   overlaps the mode line and leaves stale pixels there.  */
	int lh = FRAME_LINE_HEIGHT (f), cw = FRAME_COLUMN_WIDTH (f);
	if (lh > 0)
	  th -= th % lh;
	if (cw > 0)
	  tw -= tw % cw;
	change_frame_size (f, tw, th, false, true, false);
      }
  }

  gui_default_parameter (f, parms, Qcursor_type, Qbox,
			 "cursorType", "CursorType", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qalpha, Qnil, "alpha", "Alpha",
			 RES_TYPE_NUMBER);

  f->terminal->reference_count++;
  FRAME_DISPLAY_INFO (f)->reference_count++;
  Vframe_list = Fcons (frame, Vframe_list);

  /* The Wayland window already exists and is mapped; mark visible and force a
     full initial paint.  */
  SET_FRAME_VISIBLE (f, true);
  store_frame_param (f, Qvisibility, Qt);
  SET_FRAME_GARBAGED (f);

  return frame;
}

DEFUN ("x-open-connection", Fx_open_connection, Sx_open_connection, 1, 3, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object display, Lisp_Object xrm_string, Lisp_Object must_succeed)
{
  CHECK_STRING (display);

  /* Single Wayland display: reuse it if already open.  */
  if (!x_display_list)
    {
      struct wgpu_display_info *dpyinfo = wgpu_term_init (display);
      if (!dpyinfo)
	{
	  if (!NILP (must_succeed))
	    fatal ("Cannot connect to wgpu display");
	  else
	    error ("Cannot connect to wgpu display");
	}
    }
  return Qnil;
}

DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p, 0, 1, 0,
       doc: /* Return t if the display supports color.
M1 stub: the wgpu backend always reports a color display.  */)
  (Lisp_Object terminal)
{
  return Qt;
}

DEFUN ("x-display-grayscale-p", Fx_display_grayscale_p, Sx_display_grayscale_p,
       0, 1, 0,
       doc: /* Return t if the display supports shades of gray.
M1 stub.  */)
  (Lisp_Object terminal)
{
  return Qnil;
}

DEFUN ("xw-color-values", Fxw_color_values, Sxw_color_values, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color col;
  struct frame *f = decode_window_system_frame (frame);

  CHECK_STRING (color);
  if (!wgpu_defined_color (f, SSDATA (color), &col, false, false))
    return Qnil;

  return list3i (col.red, col.green, col.blue);
}

DEFUN ("x-display-planes", Fx_display_planes, Sx_display_planes, 0, 1, 0,
       doc: /* Return the number of bit planes of the wgpu display.
The wgpu backend renders to a 24-bit (true color) RGBA target.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (24);
}

DEFUN ("x-display-color-cells", Fx_display_color_cells, Sx_display_color_cells,
       0, 1, 0,
       doc: /* Return the number of color cells of the wgpu display.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1 << 24);
}

DEFUN ("x-display-visual-class", Fx_display_visual_class,
       Sx_display_visual_class, 0, 1, 0,
       doc: /* Return the visual class of the wgpu display (always true color).  */)
  (Lisp_Object terminal)
{
  return intern ("true-color");
}

DEFUN ("x-display-screens", Fx_display_screens, Sx_display_screens, 0, 1, 0,
       doc: /* Return the number of screens on the wgpu display.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1);
}

DEFUN ("x-display-pixel-width", Fx_display_pixel_width, Sx_display_pixel_width,
       0, 1, 0,
       doc: /* Return the width in pixels of the wgpu display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wgpu_window_size (&w, &h);
  return make_fixnum (w ? (EMACS_INT) w : 1920);
}

DEFUN ("x-display-pixel-height", Fx_display_pixel_height,
       Sx_display_pixel_height, 0, 1, 0,
       doc: /* Return the height in pixels of the wgpu display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wgpu_window_size (&w, &h);
  return make_fixnum (h ? (EMACS_INT) h : 1080);
}

DEFUN ("x-display-mm-width", Fx_display_mm_width, Sx_display_mm_width, 0, 1, 0,
       doc: /* Return the width in millimeters of the wgpu display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wgpu_window_size (&w, &h);
  return make_fixnum ((EMACS_INT) ((w ? w : 1920) * 25.4 / 96.0));
}

DEFUN ("x-display-mm-height", Fx_display_mm_height, Sx_display_mm_height,
       0, 1, 0,
       doc: /* Return the height in millimeters of the wgpu display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wgpu_window_size (&w, &h);
  return make_fixnum ((EMACS_INT) ((h ? h : 1080) * 25.4 / 96.0));
}

DEFUN ("x-display-backing-store", Fx_display_backing_store,
       Sx_display_backing_store, 0, 1, 0,
       doc: /* Return the backing store capability of the wgpu display.  */)
  (Lisp_Object terminal)
{
  return intern ("not-useful");
}

DEFUN ("x-display-save-under", Fx_display_save_under, Sx_display_save_under,
       0, 1, 0,
       doc: /* Return t if the wgpu display supports save-under.  */)
  (Lisp_Object terminal)
{
  return Qnil;
}

DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* Hide the current tooltip window, if there is any.
Value is t if tooltip was open, nil otherwise.
M1 stub: tooltips are not implemented for the wgpu backend yet.  */)
  (void)
{
  return Qnil;
}

/* ------------------------------------------------------------------ */
/* Clipboard (CLIPBOARD selection).  Thin shims over the Rust Wayland   */
/* data-device clipboard; the lisp methods in term/wgpu-win.el gate     */
/* these to the CLIPBOARD selection (PRIMARY is not yet supported).     */
/* ------------------------------------------------------------------ */

DEFUN ("wgpu-own-selection-internal", Fwgpu_own_selection_internal,
       Swgpu_own_selection_internal, 2, 3, 0,
       doc: /* Assert ownership of the clipboard with VALUE (a string).
SELECTION and FRAME are accepted for compatibility; only the system
CLIPBOARD is supported.  */)
  (Lisp_Object selection, Lisp_Object value, Lisp_Object frame)
{
  CHECK_STRING (value);
  Lisp_Object enc = ENCODE_UTF_8 (value);
  wgpu_window_set_clipboard ((const uint8_t *) SDATA (enc),
			     (uintptr_t) SBYTES (enc));
  return value;
}

DEFUN ("wgpu-disown-selection-internal", Fwgpu_disown_selection_internal,
       Swgpu_disown_selection_internal, 1, 3, 0,
       doc: /* Release ownership of the clipboard.  */)
  (Lisp_Object selection, Lisp_Object time_object, Lisp_Object terminal)
{
  wgpu_window_disown_clipboard ();
  return Qt;
}

DEFUN ("wgpu-get-selection-internal", Fwgpu_get_selection_internal,
       Swgpu_get_selection_internal, 2, 4, 0,
       doc: /* Return the clipboard text, or nil if empty.  */)
  (Lisp_Object selection_symbol, Lisp_Object target_type,
   Lisp_Object time_stamp, Lisp_Object terminal)
{
  const uint8_t *ptr = NULL;
  uintptr_t len = 0;
  if (wgpu_window_get_clipboard (&ptr, &len) != 0 || ptr == NULL || len == 0)
    return Qnil;
  Lisp_Object bytes = make_unibyte_string ((const char *) ptr, (ptrdiff_t) len);
  return code_convert_string_norecord (bytes, Qutf_8, false);
}

DEFUN ("wgpu-selection-owner-p", Fwgpu_selection_owner_p,
       Swgpu_selection_owner_p, 0, 2, 0,
       doc: /* Return t if this Emacs owns the clipboard.  */)
  (Lisp_Object selection, Lisp_Object terminal)
{
  return wgpu_window_owns_clipboard () ? Qt : Qnil;
}

DEFUN ("wgpu-selection-exists-p", Fwgpu_selection_exists_p,
       Swgpu_selection_exists_p, 0, 2, 0,
       doc: /* Return t if there is a clipboard selection.  */)
  (Lisp_Object selection, Lisp_Object terminal)
{
  return wgpu_window_clipboard_exists () ? Qt : Qnil;
}

void
syms_of_wgpufns (void)
{
  defsubr (&Sx_create_frame);
  defsubr (&Sx_open_connection);
  defsubr (&Swgpu_scale_factor);
  defsubr (&Swgpu_dump_frame);
  defsubr (&Swgpu__frame_rgba);
  defsubr (&Swgpu__draw_demo);
  defsubr (&Swgpu__demo_rgba);
  defsubr (&Swgpu__window_dump);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sxw_color_values);
  defsubr (&Sx_display_planes);
  defsubr (&Sx_display_color_cells);
  defsubr (&Sx_display_visual_class);
  defsubr (&Sx_display_screens);
  defsubr (&Sx_display_pixel_width);
  defsubr (&Sx_display_pixel_height);
  defsubr (&Sx_display_mm_width);
  defsubr (&Sx_display_mm_height);
  defsubr (&Sx_display_backing_store);
  defsubr (&Sx_display_save_under);
  defsubr (&Sx_hide_tip);
  defsubr (&Swgpu_own_selection_internal);
  defsubr (&Swgpu_disown_selection_internal);
  defsubr (&Swgpu_get_selection_internal);
  defsubr (&Swgpu_selection_owner_p);
  defsubr (&Swgpu_selection_exists_p);
}
