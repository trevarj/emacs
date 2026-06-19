/* Wayland + wgpu terminal backend for Emacs -- header.

This file is part of a personal-fork experiment (see wgpu-backend-plan.md).
The heavy lifting (Wayland, wgpu, glyph atlas, render thread) lives in the
Rust crate rust/wgpu-backend; this C side is a thin shim that populates the
redisplay interface + terminal hooks and forwards across the FFI declared in
wgpu_ffi.h.

M0: scaffold only.  */

#ifndef WGPUTERM_H
#define WGPUTERM_H

#include "dispextern.h"
#include "frame.h"
#include "termhooks.h"

/* Generated from the Rust crate by cbindgen (see Makefile rule).  */
#include "wgpu_ffi.h"

/* Per-display state.  One per Wayland connection.  M0: minimal.  */
struct wgpu_display_info
{
  /* Chain of all wgpu displays.  */
  struct wgpu_display_info *next;

  /* The generic terminal/display this belongs to.  */
  struct terminal *terminal;

  /* Lisp-visible name of this display, and the chain element used by
     Fx_display_list etc.  */
  Lisp_Object name_list_element;

  /* Logical -> device pixel scale (fractional, via wp-fractional-scale).  */
  double scale;

  /* Most recently seen mouse frame, kept for the mouse-highlight path.  */
  struct frame *highlight_frame;
};

/* Per-frame backend state, hung off f->output_data.wgpu.  M0: minimal.  */
struct wgpu_output
{
  struct wgpu_display_info *display_info;

  /* Opaque handle to the Rust-side frame (window + surface + cmd buffer).  */
  void *wgpu_frame;

  /* Faces / font cache scaffolding reused by redisplay.  */
  struct face *cursor_face;
};

/* Accessors mirroring the other backends' conventions.  */
#define FRAME_WGPU_OUTPUT(f)  ((f)->output_data.wgpu)
#define FRAME_DISPLAY_INFO(f) (FRAME_WGPU_OUTPUT (f)->display_info)

extern struct wgpu_display_info *wgpu_term_init (Lisp_Object display_name);
extern struct terminal *wgpu_create_terminal (struct wgpu_display_info *dpyinfo);

/* Lisp init entry points (syms_of_*).  */
extern void syms_of_wgputerm (void);
extern void syms_of_wgpufns (void);
extern void syms_of_wgpufont (void);

/* Font driver registration.  */
extern void register_wgpufont_driver (struct frame *f);

#endif /* WGPUTERM_H */
