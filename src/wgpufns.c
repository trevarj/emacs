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

DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* Hide the current tooltip window, if there is any.
Value is t if tooltip was open, nil otherwise.
M1 stub: tooltips are not implemented for the wgpu backend yet.  */)
  (void)
{
  return Qnil;
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
  defsubr (&Sx_hide_tip);
}
