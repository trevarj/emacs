/* Wayland + wgpu terminal backend for Emacs -- frame/Lisp glue.

M0 scaffold.  This will hold x-create-frame and friends for the wgpu backend,
plus the test/validation primitive `wgpu-dump-frame'.  At M0 only the symbol
table init and the dump-frame stub exist.

See wgpu-backend-plan.md.  */

#include <config.h>

#include "lisp.h"
#include "frame.h"
#include "dispextern.h"
#include "wgputerm.h"

/* Validation primitive (see plan, "Validation harness").

   Force redisplay of FRAME, replay its command buffer into an offscreen wgpu
   texture, read the pixels back, and write a PNG to FILE.  This is the entry
   point golden-image tests call:

     (wgpu-dump-frame "/tmp/frame.png")

   then the test compares the PNG against a committed golden with tolerance.
   M0: stub; the offscreen path lands with M1.  */
DEFUN ("wgpu-dump-frame", Fwgpu_dump_frame, Swgpu_dump_frame, 1, 2, 0,
       doc: /* Render FRAME offscreen and write it to FILE as a PNG.
FILE is a file name.  Optional FRAME defaults to the selected frame.
Used by the wgpu backend's golden-image test harness.  */)
  (Lisp_Object file, Lisp_Object frame)
{
  CHECK_STRING (file);
  (void) frame;
  /* M1: forces redisplay, calls into the Rust offscreen render + readback,
     writes the PNG.  */
  error ("wgpu-dump-frame: offscreen render not implemented until M1");
  return Qnil;
}

void
syms_of_wgpufns (void)
{
  defsubr (&Swgpu_dump_frame);
}
