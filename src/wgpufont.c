/* Wayland + wgpu terminal backend for Emacs -- font driver.

M0 scaffold.  The wgpu backend reuses Emacs's existing FreeType + HarfBuzz
shaping/metrics machinery (as ftcrhbfont does) and overrides only `draw',
routing rasterized glyphs into a GPU atlas on the Rust side.  See
wgpu-backend-plan.md, "Text / glyph atlas".

At M0 this only declares the driver and its registration entry point; the
glyph-atlas `draw' arrives with M2.  */

#include <config.h>

#include "lisp.h"
#include "frame.h"
#include "dispextern.h"
#include "font.h"
#include "wgputerm.h"

/* The wgpu font driver.  M0: registration scaffold only.

   M2 layers list/match/open/shape/text_extents/encode_char on the existing
   ftfont/sfntfont machinery and supplies a `draw' that uploads glyph bitmaps
   to the Rust-side atlas and emits textured quads.  */
static struct font_driver wgpufont_driver;

void
register_wgpufont_driver (struct frame *f)
{
  /* M2: register_font_driver (&wgpufont_driver, f).  */
  (void) f;
  (void) wgpufont_driver;
}

void
syms_of_wgpufont (void)
{
}
