/* Wayland + wgpu terminal backend for Emacs -- frame/Lisp glue.

M0 scaffold.  This will hold x-create-frame and friends for the wgpu backend,
plus the test/validation primitive `wgpu-dump-frame'.  At M0 only the symbol
table init and the dump-frame stub exist.

See wgpu-backend-plan.md.  */

#include <config.h>

#include "lisp.h"
#include "frame.h"
#include "dispextern.h"
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
  defsubr (&Swgpu_dump_frame);
  defsubr (&Swgpu__frame_rgba);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sx_hide_tip);
}
