/* Wayland + wlshm terminal backend for Emacs -- frame/Lisp glue.

M0 scaffold.  This will hold x-create-frame and friends for the wlshm backend,
plus the test/validation primitive `wlshm-dump-frame'.  At M0 only the symbol
table init and the dump-frame stub exist.

See wlshm-backend-plan.md.  */

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
#include "wlshmterm.h"

/* Return the wlshm display info for OBJECT (a frame, terminal, display name,
   or nil for the default).  Signals if there is no wlshm display.
   M1 stub: only the default display is supported.  */
struct wlshm_display_info *
check_x_display_info (Lisp_Object object)
{
  if (!x_display_list)
    error ("There is no wlshm display");
  return x_display_list;
}

DEFUN ("wlshm-scale-factor", Fwlshm_scale_factor, Swlshm_scale_factor, 0, 0, 0,
       doc: /* Return the integer HiDPI scale factor of the wlshm display.  */)
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
  struct wlshm_display_info *dpyinfo;
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
  f->output_method = output_wlshm;
  FRAME_X_OUTPUT (f) = xzalloc (sizeof (struct wlshm_output));
  FRAME_FONTSET (f) = -1;
  FRAME_X_OUTPUT (f)->white_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->black_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->scroll_bar_foreground_pixel = -1;
  FRAME_X_OUTPUT (f)->scroll_bar_background_pixel = -1;
  FRAME_DISPLAY_INFO (f) = dpyinfo;

  /* Sensible defaults; the color parameters below refine them.  */
  FRAME_FOREGROUND_PIXEL (f) = 0x000000;
  FRAME_BACKGROUND_PIXEL (f) = 0xffffff;
  FRAME_X_OUTPUT (f)->cursor_color = 0x000000;

  /* Pointer cursor shapes (resolved to cursor-shape-v1 in Rust).  */
  FRAME_X_OUTPUT (f)->text_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_TEXT;
  FRAME_X_OUTPUT (f)->nontext_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_DEFAULT;
  FRAME_X_OUTPUT (f)->modeline_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_DEFAULT;
  FRAME_X_OUTPUT (f)->hand_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_HAND;
  FRAME_X_OUTPUT (f)->hourglass_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_WAIT;
  FRAME_X_OUTPUT (f)->horizontal_drag_cursor
    = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_HRESIZE;
  FRAME_X_OUTPUT (f)->vertical_drag_cursor
    = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_VRESIZE;
  FRAME_X_OUTPUT (f)->current_cursor = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_DEFAULT;

  if (BASE_EQ (name, Qunbound) || NILP (name))
    {
      fset_name (f, build_string ("wlshm"));
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
    }

  register_font_driver (&ftcrfont_driver, f);
#ifdef HAVE_HARFBUZZ
  register_font_driver (&ftcrhbfont_driver, f);
#endif
  gui_default_parameter (f, parms, Qfont_backend, Qnil,
			 "fontBackend", "FontBackend", RES_TYPE_STRING);
  wlshm_default_font_parameter (f, parms);
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
  gui_default_parameter (f, parms, Qvertical_scroll_bars, Qright,
			 "verticalScrollBars", "ScrollBars", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qhorizontal_scroll_bars, Qnil,
			 "horizontalScrollBars", "ScrollBars", RES_TYPE_SYMBOL);
  /* Nil -> the set_scroll_bar_default_{width,height}_hook picks the pixel
     thickness.  */
  gui_default_parameter (f, parms, Qscroll_bar_width, Qnil,
			 "scrollBarWidth", "ScrollBarWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qscroll_bar_height, Qnil,
			 "scrollBarHeight", "ScrollBarHeight", RES_TYPE_NUMBER);
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

  /* Open this frame's Wayland window (M3 multi-window) and store its handle.  */
  {
    uint64_t win = wlshm_window_open (NULL, 0, 0);
    if (win == 0)
      error ("wlshm: cannot open a Wayland window (is WAYLAND_DISPLAY set?)");
    FRAME_X_OUTPUT (f)->wlshm_frame = (void *) (uintptr_t) win;
  }

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
    wlshm_window_size (WLSHM_FRAME_HANDLE (f), &sw, &sh);
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
      struct wlshm_display_info *dpyinfo = wlshm_term_init (display);
      if (!dpyinfo)
	{
	  if (!NILP (must_succeed))
	    fatal ("Cannot connect to wlshm display");
	  else
	    error ("Cannot connect to wlshm display");
	}
    }
  return Qnil;
}

DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p, 0, 1, 0,
       doc: /* Return t if the display supports color.
M1 stub: the wlshm backend always reports a color display.  */)
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
  if (!wlshm_defined_color (f, SSDATA (color), &col, false, false))
    return Qnil;

  return list3i (col.red, col.green, col.blue);
}

DEFUN ("xw-color-defined-p", Fxw_color_defined_p, Sxw_color_defined_p, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color col;
  struct frame *f = decode_window_system_frame (frame);

  CHECK_STRING (color);
  return (wlshm_defined_color (f, SSDATA (color), &col, false, false)
	  ? Qt : Qnil);
}

DEFUN ("x-display-planes", Fx_display_planes, Sx_display_planes, 0, 1, 0,
       doc: /* Return the number of bit planes of the wlshm display.
The wlshm backend renders to a 24-bit (true color) RGBA target.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (24);
}

DEFUN ("x-display-color-cells", Fx_display_color_cells, Sx_display_color_cells,
       0, 1, 0,
       doc: /* Return the number of color cells of the wlshm display.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1 << 24);
}

DEFUN ("x-display-visual-class", Fx_display_visual_class,
       Sx_display_visual_class, 0, 1, 0,
       doc: /* Return the visual class of the wlshm display (always true color).  */)
  (Lisp_Object terminal)
{
  return intern ("true-color");
}

DEFUN ("x-display-screens", Fx_display_screens, Sx_display_screens, 0, 1, 0,
       doc: /* Return the number of screens on the wlshm display.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1);
}

DEFUN ("x-display-pixel-width", Fx_display_pixel_width, Sx_display_pixel_width,
       0, 1, 0,
       doc: /* Return the width in pixels of the wlshm display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wlshm_window_size (0, &w, &h);
  return make_fixnum (w ? (EMACS_INT) w : 1920);
}

DEFUN ("x-display-pixel-height", Fx_display_pixel_height,
       Sx_display_pixel_height, 0, 1, 0,
       doc: /* Return the height in pixels of the wlshm display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wlshm_window_size (0, &w, &h);
  return make_fixnum (h ? (EMACS_INT) h : 1080);
}

DEFUN ("x-display-mm-width", Fx_display_mm_width, Sx_display_mm_width, 0, 1, 0,
       doc: /* Return the width in millimeters of the wlshm display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wlshm_window_size (0, &w, &h);
  return make_fixnum ((EMACS_INT) ((w ? w : 1920) * 25.4 / 96.0));
}

DEFUN ("x-display-mm-height", Fx_display_mm_height, Sx_display_mm_height,
       0, 1, 0,
       doc: /* Return the height in millimeters of the wlshm display.  */)
  (Lisp_Object terminal)
{
  uint32_t w = 0, h = 0;
  wlshm_window_size (0, &w, &h);
  return make_fixnum ((EMACS_INT) ((h ? h : 1080) * 25.4 / 96.0));
}

DEFUN ("x-display-backing-store", Fx_display_backing_store,
       Sx_display_backing_store, 0, 1, 0,
       doc: /* Return the backing store capability of the wlshm display.  */)
  (Lisp_Object terminal)
{
  return intern ("not-useful");
}

DEFUN ("x-display-save-under", Fx_display_save_under, Sx_display_save_under,
       0, 1, 0,
       doc: /* Return t if the wlshm display supports save-under.  */)
  (Lisp_Object terminal)
{
  return Qnil;
}

/* ------------------------------------------------------------------ */
/* Tooltips.  A tooltip is a small, undecorated, modeline-less frame    */
/* with its own Wayland window (kind=Tooltip), rendered through the      */
/* normal Cairo redisplay path.  Ported from the pgtk backend, dropping  */
/* GTK system tooltips (not available here).                            */
/* ------------------------------------------------------------------ */

/* The currently displayed tooltip frame, or nil.  */
static Lisp_Object tip_frame;
/* A timer that hides the tooltip when it fires.  */
static Lisp_Object tip_timer;
/* STRING/FRAME/PARMS of the last `x-show-tip' call (for reuse).  */
static Lisp_Object tip_last_string;
static Lisp_Object tip_last_frame;
static Lisp_Object tip_last_parms;

/* Handler for signals raised during frame creation: clean up a frame that
   is not yet official.  */
static Lisp_Object
unwind_create_frame (Lisp_Object frame)
{
  struct frame *f = XFRAME (frame);

  if (!FRAME_LIVE_P (f))
    return Qnil;

  if (NILP (Fmemq (frame, Vframe_list)))
    {
      if (FRAME_X_OUTPUT (f) && WLSHM_FRAME_HANDLE (f))
	wlshm_window_close (WLSHM_FRAME_HANDLE (f));
      free_glyphs (f);
      return Qt;
    }

  return Qnil;
}

static void
unwind_create_tip_frame (Lisp_Object frame)
{
  Lisp_Object deleted = unwind_create_frame (frame);
  if (EQ (deleted, Qt))
    tip_frame = Qnil;
}

/* Create an undecorated tooltip frame on DPYINFO with PARMS, parented to
   window-system frame P.  Returns the frame, or signals.  */
static Lisp_Object
wlshm_create_tip_frame (struct wlshm_display_info *dpyinfo, Lisp_Object parms,
			struct frame *p)
{
  struct frame *f;
  Lisp_Object frame, name;
  specpdl_ref count = SPECPDL_INDEX ();
  bool face_change_before = face_change;

  if (!dpyinfo->terminal->name)
    error ("Terminal is not live, can't create new frames on it");

  parms = Fcopy_alist (parms);
  name = gui_display_get_arg (dpyinfo, parms, Qname, "name", "Name",
			      RES_TYPE_STRING);
  if (!STRINGP (name) && !BASE_EQ (name, Qunbound) && !NILP (name))
    error ("Invalid frame name--not a string or nil");

  frame = Qnil;
  f = make_frame (false);
  f->wants_modeline = false;
  XSETFRAME (frame, f);
  record_unwind_protect (unwind_create_tip_frame, frame);

  f->terminal = dpyinfo->terminal;
  f->output_method = output_wlshm;
  FRAME_X_OUTPUT (f) = xzalloc (sizeof (struct wlshm_output));
  FRAME_FONTSET (f) = -1;
  FRAME_X_OUTPUT (f)->white_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->black_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->scroll_bar_foreground_pixel = -1;
  FRAME_X_OUTPUT (f)->scroll_bar_background_pixel = -1;
  f->tooltip = true;
  fset_icon_name (f, Qnil);
  FRAME_DISPLAY_INFO (f) = dpyinfo;
  FRAME_FOREGROUND_PIXEL (f) = 0x000000;
  FRAME_BACKGROUND_PIXEL (f) = 0xffffff;
  FRAME_X_OUTPUT (f)->cursor_color = 0x000000;
  FRAME_X_OUTPUT (f)->current_cursor
    = (Emacs_Cursor) (intptr_t) WLSHM_CURSOR_TEXT;

  if (BASE_EQ (name, Qunbound) || NILP (name))
    {
      fset_name (f, build_string ("wlshm"));
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
      specbind (Qx_resource_name, name);
    }

  register_font_driver (&ftcrfont_driver, f);
#ifdef HAVE_HARFBUZZ
  register_font_driver (&ftcrhbfont_driver, f);
#endif
  gui_default_parameter (f, parms, Qfont_backend, Qnil,
			 "fontBackend", "FontBackend", RES_TYPE_STRING);
  wlshm_default_font_parameter (f, parms);
  if (!FRAME_FONT (f))
    {
      delete_frame (frame, Qnoelisp);
      error ("Invalid frame font");
    }

  gui_default_parameter (f, parms, Qborder_width, make_fixnum (0),
			 "borderWidth", "BorderWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qinternal_border_width, make_fixnum (1),
			 "internalBorderWidth", "internalBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbottom_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qforeground_color, build_string ("black"),
			 "foreground", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qbackground_color,
			 build_string ("lightyellow"),
			 "background", "Background", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qno_special_glyphs, Qnil,
			 NULL, NULL, RES_TYPE_BOOLEAN);

  init_frame_faces (f);
  gui_figure_window_size (f, parms, false, false);

  /* Default value for the `tooltip' frame parameter.  */
  if (NILP (Fframe_parameter (frame, Qtooltip)))
    {
      AUTO_FRAME_ARG (arg, Qtooltip, Qt);
      Fmodify_frame_parameters (frame, arg);
    }

  /* Set up faces after all frame parameters are known.  */
  {
    Lisp_Object bg = Fframe_parameter (frame, Qbackground_color);
    calln (Qface_set_after_frame_default, frame, Qnil);
    if (!EQ (bg, Fframe_parameter (frame, Qbackground_color)))
      {
	AUTO_FRAME_ARG (arg, Qbackground_color, bg);
	Fmodify_frame_parameters (frame, arg);
      }
  }

  f->no_split = true;

  /* Open the tooltip's own Wayland window (kind=Tooltip), parented to P.  */
  {
    uint64_t win = wlshm_window_open (NULL, WLSHM_FRAME_HANDLE (p), 2);
    if (win == 0)
      {
	delete_frame (frame, Qnoelisp);
	error ("wlshm: cannot open a tooltip window");
      }
    FRAME_X_OUTPUT (f)->wlshm_frame = (void *) (uintptr_t) win;
  }

  FRAME_DISPLAY_INFO (f)->reference_count++;
  f->terminal->reference_count++;
  Vframe_list = Fcons (frame, Vframe_list);
  f->can_set_window_size = true;
  adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
		     0, true, Qtip_frame);

  face_change = face_change_before;
  return unbind_to (count, frame);
}

/* Compute where to place tip frame F.  Wayland gives clients no absolute
   pointer/root coordinates, so placement is best-effort: honor an explicit
   left/top/right/bottom in PARMS, else fall back to the parent frame's last
   tracked pointer position.  (Ideally an xdg-popup positioner; deferred.)  */
static void
compute_tip_xy (struct frame *f, Lisp_Object parms, Lisp_Object dx,
		Lisp_Object dy, int width, int height, int *root_x,
		int *root_y)
{
  Lisp_Object left = Fcdr (Fassq (Qleft, parms));
  Lisp_Object top = Fcdr (Fassq (Qtop, parms));
  Lisp_Object right = Fcdr (Fassq (Qright, parms));
  Lisp_Object bottom = Fcdr (Fassq (Qbottom, parms));
  struct wlshm_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  int min_x = 0, min_y = 0;
  int max_x = 4096, max_y = 4096;

  /* Best-effort pointer position (surface-local; no Wayland root coords).  */
  *root_x = dpyinfo->last_mouse_motion_x;
  *root_y = dpyinfo->last_mouse_motion_y;

  if (FIXNUMP (top))
    *root_y = XFIXNUM (top);
  else if (FIXNUMP (bottom))
    *root_y = XFIXNUM (bottom) - height;
  else if (*root_y + XFIXNUM (dy) <= min_y)
    *root_y = min_y;
  else if (*root_y + XFIXNUM (dy) + height <= max_y)
    *root_y += XFIXNUM (dy);
  else if (height + XFIXNUM (dy) + min_y <= *root_y)
    *root_y -= height + XFIXNUM (dy);
  else
    *root_y = min_y;

  if (FIXNUMP (left))
    *root_x = XFIXNUM (left);
  else if (FIXNUMP (right))
    *root_x = XFIXNUM (right) - width;
  else if (*root_x + XFIXNUM (dx) <= min_x)
    *root_x = 0;
  else if (*root_x + XFIXNUM (dx) + width <= max_x)
    *root_x += XFIXNUM (dx);
  else if (width + XFIXNUM (dx) + min_x <= *root_x)
    *root_x -= width + XFIXNUM (dx);
  else
    *root_x = min_x;
}

/* Hide the tooltip.  Delete its frame if DELETE.  Return t if a tip was up.  */
static Lisp_Object
wlshm_hide_tip (bool delete)
{
  if (!NILP (tip_timer))
    {
      calln (Qcancel_timer, tip_timer);
      tip_timer = Qnil;
    }

  if (NILP (tip_frame)
      || (!delete
	  && FRAMEP (tip_frame)
	  && FRAME_LIVE_P (XFRAME (tip_frame))
	  && !FRAME_VISIBLE_P (XFRAME (tip_frame))))
    return Qnil;
  else
    {
      Lisp_Object was_open = Qnil;
      specpdl_ref count = SPECPDL_INDEX ();
      specbind (Qinhibit_redisplay, Qt);
      specbind (Qinhibit_quit, Qt);

      if (FRAMEP (tip_frame))
	{
	  struct frame *f = XFRAME (tip_frame);
	  if (FRAME_LIVE_P (f))
	    {
	      if (delete)
		{
		  delete_frame (tip_frame, Qnil);
		  tip_frame = Qnil;
		}
	      else
		{
		  /* Keep the frame for reuse but make it invisible
		     (unmap its surface by closing the window handle).  */
		  wlshm_window_close (WLSHM_FRAME_HANDLE (f));
		  FRAME_X_OUTPUT (f)->wlshm_frame = NULL;
		  SET_FRAME_VISIBLE (f, false);
		}
	      was_open = Qt;
	    }
	  else
	    tip_frame = Qnil;
	}
      else
	tip_frame = Qnil;

      return unbind_to (count, was_open);
    }
}

DEFUN ("x-show-tip", Fx_show_tip, Sx_show_tip, 1, 6, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object string, Lisp_Object frame, Lisp_Object parms,
   Lisp_Object timeout, Lisp_Object dx, Lisp_Object dy)
{
  struct frame *f, *tip_f;
  struct window *w;
  int root_x, root_y;
  struct buffer *old_buffer;
  struct text_pos pos;
  int width, height;
  int old_windows_or_buffers_changed = windows_or_buffers_changed;
  specpdl_ref count = SPECPDL_INDEX ();
  Lisp_Object window, size, tip_buf;
  AUTO_STRING (tip, " *tip*");

  specbind (Qinhibit_redisplay, Qt);

  CHECK_STRING (string);
  if (SCHARS (string) == 0)
    string = make_unibyte_string (" ", 1);

  if (NILP (frame))
    frame = selected_frame;
  f = decode_window_system_frame (frame);

  if (NILP (timeout))
    timeout = Vx_show_tooltip_timeout;
  CHECK_FIXNAT (timeout);

  if (NILP (dx))
    dx = make_fixnum (5);
  else
    CHECK_FIXNUM (dx);
  if (NILP (dy))
    dy = make_fixnum (-10);
  else
    CHECK_FIXNUM (dy);

  /* Reuse a visible identical tooltip: only DX/DY changed.  */
  if (FRAMEP (tip_frame) && FRAME_LIVE_P (XFRAME (tip_frame))
      && FRAME_VISIBLE_P (XFRAME (tip_frame)) && EQ (frame, tip_last_frame)
      && !NILP (Fequal_including_properties (tip_last_string, string))
      && !NILP (Fequal (tip_last_parms, parms)))
    {
      tip_f = XFRAME (tip_frame);
      if (!NILP (tip_timer))
	{
	  calln (Qcancel_timer, tip_timer);
	  tip_timer = Qnil;
	}
      compute_tip_xy (tip_f, parms, dx, dy, FRAME_PIXEL_WIDTH (tip_f),
		      FRAME_PIXEL_HEIGHT (tip_f), &root_x, &root_y);
      wlshm_window_set_geometry (WLSHM_FRAME_HANDLE (tip_f), root_x, root_y,
				 FRAME_PIXEL_WIDTH (tip_f),
				 FRAME_PIXEL_HEIGHT (tip_f));
      goto start_timer;
    }

  /* Otherwise drop any existing tooltip and (re)create one.  */
  wlshm_hide_tip (true);

  tip_last_frame = frame;
  tip_last_string = string;
  tip_last_parms = parms;

  if (NILP (Fassq (Qname, parms)))
    parms = Fcons (Fcons (Qname, build_string ("tooltip")), parms);
  if (NILP (Fassq (Qinternal_border_width, parms)))
    parms = Fcons (Fcons (Qinternal_border_width, make_fixnum (3)), parms);
  if (NILP (Fassq (Qborder_width, parms)))
    parms = Fcons (Fcons (Qborder_width, make_fixnum (1)), parms);
  if (NILP (Fassq (Qbackground_color, parms)))
    parms = Fcons (Fcons (Qbackground_color, build_string ("lightyellow")),
		   parms);

  tip_frame = wlshm_create_tip_frame (FRAME_DISPLAY_INFO (f), parms, f);
  if (NILP (tip_frame))
    return unbind_to (count, Qnil);

  tip_f = XFRAME (tip_frame);
  window = FRAME_ROOT_WINDOW (tip_f);
  tip_buf = Fget_buffer_create (tip, Qnil);
  bset_left_margin_cols (XBUFFER (tip_buf), make_fixnum (0));
  bset_right_margin_cols (XBUFFER (tip_buf), make_fixnum (0));
  set_window_buffer (window, tip_buf, false, false);
  w = XWINDOW (window);
  w->pseudo_window_p = true;
  w->left_col = 0;
  w->top_line = 0;
  w->pixel_left = 0;
  w->pixel_top = 0;

  if (CONSP (Vx_max_tooltip_size)
      && RANGED_FIXNUMP (1, XCAR (Vx_max_tooltip_size), INT_MAX)
      && RANGED_FIXNUMP (1, XCDR (Vx_max_tooltip_size), INT_MAX))
    {
      w->total_cols = XFIXNAT (XCAR (Vx_max_tooltip_size));
      w->total_lines = XFIXNAT (XCDR (Vx_max_tooltip_size));
    }
  else
    {
      w->total_cols = 80;
      w->total_lines = 40;
    }
  w->pixel_width = w->total_cols * FRAME_COLUMN_WIDTH (tip_f);
  w->pixel_height = w->total_lines * FRAME_LINE_HEIGHT (tip_f);
  FRAME_TOTAL_COLS (tip_f) = w->total_cols;
  adjust_frame_glyphs (tip_f);

  /* Insert STRING and fit the frame to it.  */
  specpdl_ref count_1 = SPECPDL_INDEX ();
  old_buffer = current_buffer;
  set_buffer_internal_1 (XBUFFER (w->contents));
  bset_truncate_lines (current_buffer, Qnil);
  specbind (Qinhibit_read_only, Qt);
  specbind (Qinhibit_modification_hooks, Qt);
  specbind (Qinhibit_point_motion_hooks, Qt);
  Ferase_buffer ();
  Finsert (1, &string);
  clear_glyph_matrix (w->desired_matrix);
  clear_glyph_matrix (w->current_matrix);
  SET_TEXT_POS (pos, BEGV, BEGV_BYTE);
  try_window (window, pos, TRY_WINDOW_IGNORE_FONTS_CHANGE);

  size = Fwindow_text_pixel_size (window, Qnil, Qnil, Qnil,
				  make_fixnum (w->pixel_height), Qnil, Qnil);
  width = XFIXNUM (Fcar (size)) + 2 * FRAME_INTERNAL_BORDER_WIDTH (tip_f);
  height = XFIXNUM (Fcdr (size)) + 2 * FRAME_INTERNAL_BORDER_WIDTH (tip_f);
  width += FRAME_COLUMN_WIDTH (tip_f);

  compute_tip_xy (tip_f, parms, dx, dy, width, height, &root_x, &root_y);

  /* Size the Wayland window (and thus the canvas) to the text, size the
     frame to match, then force-draw and present.  */
  wlshm_window_set_geometry (WLSHM_FRAME_HANDLE (tip_f), root_x, root_y,
			     width, height);
  SET_FRAME_VISIBLE (tip_f, 1);

  w->must_be_updated_p = true;
  update_single_window (w);
  wlshm_present_frame (tip_f);

  set_buffer_internal_1 (old_buffer);
  unbind_to (count_1, Qnil);
  windows_or_buffers_changed = old_windows_or_buffers_changed;

 start_timer:
  tip_timer = calln (Qrun_at_time, timeout, Qnil, Qx_hide_tip);
  return unbind_to (count, Qnil);
}

DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* Hide the current tooltip window, if there is any.
Value is t if tooltip was open, nil otherwise.  */)
  (void)
{
  return wlshm_hide_tip (!tooltip_reuse_hidden_frame);
}

/* ------------------------------------------------------------------ */
/* Clipboard (CLIPBOARD selection).  Thin shims over the Rust Wayland   */
/* data-device clipboard; the lisp methods in term/wlshm-win.el gate     */
/* these to the CLIPBOARD selection (PRIMARY is not yet supported).     */
/* ------------------------------------------------------------------ */

DEFUN ("wlshm-own-selection-internal", Fwlshm_own_selection_internal,
       Swlshm_own_selection_internal, 2, 3, 0,
       doc: /* Assert ownership of the clipboard with VALUE (a string).
SELECTION and FRAME are accepted for compatibility; only the system
CLIPBOARD is supported.  */)
  (Lisp_Object selection, Lisp_Object value, Lisp_Object frame)
{
  CHECK_STRING (value);
  Lisp_Object enc = ENCODE_UTF_8 (value);
  wlshm_window_set_clipboard ((const uint8_t *) SDATA (enc),
			     (uintptr_t) SBYTES (enc));
  return value;
}

DEFUN ("wlshm-disown-selection-internal", Fwlshm_disown_selection_internal,
       Swlshm_disown_selection_internal, 1, 3, 0,
       doc: /* Release ownership of the clipboard.  */)
  (Lisp_Object selection, Lisp_Object time_object, Lisp_Object terminal)
{
  wlshm_window_disown_clipboard ();
  return Qt;
}

DEFUN ("wlshm-get-selection-internal", Fwlshm_get_selection_internal,
       Swlshm_get_selection_internal, 2, 4, 0,
       doc: /* Return the clipboard text, or nil if empty.  */)
  (Lisp_Object selection_symbol, Lisp_Object target_type,
   Lisp_Object time_stamp, Lisp_Object terminal)
{
  const uint8_t *ptr = NULL;
  uintptr_t len = 0;
  if (wlshm_window_get_clipboard (&ptr, &len) != 0 || ptr == NULL || len == 0)
    return Qnil;
  Lisp_Object bytes = make_unibyte_string ((const char *) ptr, (ptrdiff_t) len);
  return code_convert_string_norecord (bytes, Qutf_8, false);
}

DEFUN ("wlshm-selection-owner-p", Fwlshm_selection_owner_p,
       Swlshm_selection_owner_p, 0, 2, 0,
       doc: /* Return t if this Emacs owns the clipboard.  */)
  (Lisp_Object selection, Lisp_Object terminal)
{
  return wlshm_window_owns_clipboard () ? Qt : Qnil;
}

DEFUN ("wlshm-selection-exists-p", Fwlshm_selection_exists_p,
       Swlshm_selection_exists_p, 0, 2, 0,
       doc: /* Return t if there is a clipboard selection.  */)
  (Lisp_Object selection, Lisp_Object terminal)
{
  return wlshm_window_clipboard_exists () ? Qt : Qnil;
}

DEFUN ("wlshm-dump-canvas", Fwlshm_dump_canvas, Swlshm_dump_canvas, 1, 2, 0,
       doc: /* Write FRAME's canvas to FILE as a PNG (FRAME defaults to selected).
Return t on success.  Used by the graphical test harness.  */)
  (Lisp_Object file, Lisp_Object frame)
{
  CHECK_STRING (file);
  struct frame *f = NILP (frame) ? SELECTED_FRAME ()
				 : decode_window_system_frame (frame);
  return wlshm_dump_canvas_png (f, SSDATA (ENCODE_FILE (file))) ? Qt : Qnil;
}

DEFUN ("wlshm-display-monitor-attributes-list",
       Fwlshm_display_monitor_attributes_list,
       Swlshm_display_monitor_attributes_list, 0, 1, 0,
       doc: /* Return a list of physical monitor attributes on the display.
Internal use only, use `display-monitor-attributes-list' instead.  */)
  (Lisp_Object terminal)
{
  struct wlshm_display_info *dpyinfo = check_x_display_info (terminal);
  Lisp_Object attributes_list = Qnil, monitor_frames, rest, frame;
  static const char *source = "Wayland";
  struct MonitorInfo *monitors;
  int n_monitors, i, primary_monitor = 0;

  block_input ();
  n_monitors = wlshm_output_count ();
  if (n_monitors <= 0)
    {
      unblock_input ();
      return Qnil;
    }
  monitor_frames = make_nil_vector (n_monitors);
  monitors = xcalloc (n_monitors, sizeof *monitors);

  /* Wayland does not tell a client which output its toplevel is on, so
     attribute every (non-tooltip) frame to the primary monitor.  */
  FOR_EACH_FRAME (rest, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WLSHM_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && !FRAME_TOOLTIP_P (f))
	ASET (monitor_frames, 0, Fcons (frame, AREF (monitor_frames, 0)));
    }

  for (i = 0; i < n_monitors; i++)
    {
      struct MonitorInfo *mi = &monitors[i];
      int x = 0, y = 0, w = 0, h = 0, mmw = 0, mmh = 0, scale = 1;
      char name[128];
      name[0] = '\0';
      wlshm_output_get (i, &x, &y, &w, &h, &mmw, &mmh, &scale,
			name, sizeof name);
      mi->geom.x = x;
      mi->geom.y = y;
      mi->geom.width = w;
      mi->geom.height = h;
      mi->work = mi->geom;
      mi->mm_width = mmw;
      mi->mm_height = mmh;
      dupstring (&mi->name, name[0] ? name : "Monitor");
    }

  attributes_list = make_monitor_attribute_list (monitors, n_monitors,
						 primary_monitor,
						 monitor_frames, source);
  free_monitors (monitors, n_monitors);
  unblock_input ();
  return attributes_list;
}

/* Geometry of FRAME, like x_frame_geometry but for wlshm.  Wayland gives a
   client no global toplevel position and wlshm draws no client-side border or
   title bar (the compositor owns the decorations), so outer == native and the
   reported position is (0,0).  ATTRIBUTE selects a sub-result (see below).  */
static Lisp_Object
wlshm_frame_geometry (Lisp_Object frame, Lisp_Object attribute)
{
  struct frame *f = decode_live_frame (frame);
  int native_width = FRAME_PIXEL_WIDTH (f);
  int native_height = FRAME_PIXEL_HEIGHT (f);
  int outer_left = 0, outer_top = 0;
  int outer_right = native_width, outer_bottom = native_height;
  int native_left = 0, native_top = 0;
  int native_right = native_width, native_bottom = native_height;
  int internal_border_width = FRAME_INTERNAL_BORDER_WIDTH (f);
  int inner_left = native_left + internal_border_width;
  int inner_top = native_top + internal_border_width;
  int inner_right = native_right - internal_border_width;
  int inner_bottom = native_bottom - internal_border_width;

  int menu_bar_height = FRAME_MENU_BAR_HEIGHT (f);
  int menu_bar_width = menu_bar_height ? native_width : 0;
  inner_top += menu_bar_height;

  int tab_bar_height = FRAME_TAB_BAR_HEIGHT (f);
  int tab_bar_width = (tab_bar_height
		       ? native_width - 2 * internal_border_width : 0);
  inner_top += tab_bar_height;

  int tool_bar_height = FRAME_TOOL_BAR_HEIGHT (f);
  int tool_bar_width = (tool_bar_height
			? native_width - 2 * internal_border_width : 0);
  if (EQ (FRAME_TOOL_BAR_POSITION (f), Qtop))
    inner_top += tool_bar_height;
  else
    inner_bottom -= tool_bar_height;

  if (EQ (attribute, Qouter_edges))
    return list4i (outer_left, outer_top, outer_right, outer_bottom);
  else if (EQ (attribute, Qnative_edges))
    return list4i (native_left, native_top, native_right, native_bottom);
  else if (EQ (attribute, Qinner_edges))
    return list4i (inner_left, inner_top, inner_right, inner_bottom);
  else
    return
      list (Fcons (Qouter_position,
		   Fcons (make_fixnum (outer_left), make_fixnum (outer_top))),
	    Fcons (Qouter_size,
		   Fcons (make_fixnum (outer_right - outer_left),
			  make_fixnum (outer_bottom - outer_top))),
	    Fcons (Qexternal_border_size,
		   Fcons (make_fixnum (0), make_fixnum (0))),
	    Fcons (Qtitle_bar_size,
		   Fcons (make_fixnum (0), make_fixnum (0))),
	    Fcons (Qmenu_bar_external, Qnil),
	    Fcons (Qmenu_bar_size,
		   Fcons (make_fixnum (menu_bar_width),
			  make_fixnum (menu_bar_height))),
	    Fcons (Qtab_bar_size,
		   Fcons (make_fixnum (tab_bar_width),
			  make_fixnum (tab_bar_height))),
	    Fcons (Qtool_bar_external, Qnil),
	    Fcons (Qtool_bar_position, FRAME_TOOL_BAR_POSITION (f)),
	    Fcons (Qtool_bar_size,
		   Fcons (make_fixnum (tool_bar_width),
			  make_fixnum (tool_bar_height))),
	    Fcons (Qinternal_border_width,
		   make_fixnum (internal_border_width)));
}

DEFUN ("wlshm-frame-geometry", Fwlshm_frame_geometry, Swlshm_frame_geometry,
       0, 1, 0,
       doc: /* Return geometric attributes of FRAME.
FRAME must be a live frame and defaults to the selected one.  The return
value is an association list of the same shape as `x-frame-geometry'.
Wayland does not expose a toplevel's position or decorations to clients,
so `outer-position' is reported as (0 . 0) and there is no title bar or
external border.  */)
  (Lisp_Object frame)
{
  return wlshm_frame_geometry (frame, Qnil);
}

DEFUN ("wlshm-frame-edges", Fwlshm_frame_edges, Swlshm_frame_edges, 0, 2, 0,
       doc: /* Return edge coordinates of FRAME.
FRAME must be a live frame and defaults to the selected one.  See
`frame-edges' for the meaning of TYPE.  */)
  (Lisp_Object frame, Lisp_Object type)
{
  return wlshm_frame_geometry (frame, ((EQ (type, Qouter_edges)
					|| EQ (type, Qinner_edges))
				       ? type : Qnative_edges));
}

DEFUN ("wlshm-frame-restack", Fwlshm_frame_restack, Swlshm_frame_restack,
       2, 3, 0,
       doc: /* Restack FRAME1 below FRAME2.
On Wayland a client cannot control the stacking order of its toplevels,
so this is a no-op that returns nil.  See `frame-restack'.  */)
  (Lisp_Object frame1, Lisp_Object frame2, Lisp_Object above)
{
  decode_live_frame (frame1);
  decode_live_frame (frame2);
  return Qnil;
}

void
syms_of_wlshmfns (void)
{
  defsubr (&Swlshm_dump_canvas);
  defsubr (&Swlshm_display_monitor_attributes_list);
  defsubr (&Sx_create_frame);
  defsubr (&Sx_open_connection);
  defsubr (&Swlshm_scale_factor);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sxw_color_values);
  defsubr (&Sxw_color_defined_p);
  defsubr (&Swlshm_frame_geometry);
  defsubr (&Swlshm_frame_edges);
  defsubr (&Swlshm_frame_restack);
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
  defsubr (&Sx_show_tip);
  defsubr (&Sx_hide_tip);

  DEFSYM (Qrun_at_time, "run-at-time");
  DEFSYM (Qcancel_timer, "cancel-timer");
  DEFSYM (Qx_hide_tip, "x-hide-tip");

  DEFVAR_LISP ("x-max-tooltip-size", Vx_max_tooltip_size,
	       doc: /* Maximum size for tooltips.
Value is a pair (COLUMNS . ROWS).  Text larger than this is clipped.  */);
  Vx_max_tooltip_size = Qnil;

  tip_frame = Qnil;
  staticpro (&tip_frame);
  tip_timer = Qnil;
  staticpro (&tip_timer);
  tip_last_frame = Qnil;
  staticpro (&tip_last_frame);
  tip_last_string = Qnil;
  staticpro (&tip_last_string);
  tip_last_parms = Qnil;
  staticpro (&tip_last_parms);
  defsubr (&Swlshm_own_selection_internal);
  defsubr (&Swlshm_disown_selection_internal);
  defsubr (&Swlshm_get_selection_internal);
  defsubr (&Swlshm_selection_owner_p);
  defsubr (&Swlshm_selection_exists_p);
}
