/* Wayland + wlshm terminal backend for Emacs -- header.

This file is part of a personal-fork experiment (see wlshm-backend-plan.md).
The heavy lifting (Wayland, wlshm, glyph atlas, render thread) lives in the
Rust crate rust/wlshm-backend; this C side is a thin shim that populates the
redisplay interface + terminal hooks and forwards across the FFI declared in
wlshm_ffi.h.

This header is the backend's TERM_HEADER: it is #included broadly across the
generic C core, so it must fully define struct wlshm_output, struct
wlshm_display_info, and the FRAME_* accessor macros the shared code expects.
It mirrors the generic skeleton of pgtkterm.h but with backend-neutral types
(no Gdk/Gtk/cairo) since the wlshm backend talks raw Wayland.

M0/M1: scaffold; GUI fields are placeholders until the matching milestone.  */

#ifndef WLSHMTERM_H
#define WLSHMTERM_H

#include "dispextern.h"
#include "frame.h"
#include "character.h"
#include "font.h"
#include "termhooks.h"

#include <cairo.h>

/* Generated from the Rust crate by cbindgen (see Makefile rule).  */
#include "wlshm_ffi.h"

/* Bitmap record, used by image.c bitmap handling.  M1+: real contents.  */
struct wlshm_bitmap_record
{
  char *file;
  int refcount;
  int height, width;
};

/* Per-display state.  One per Wayland connection.  */
struct wlshm_display_info
{
  /* Chain of all wlshm displays.  */
  struct wlshm_display_info *next;

  /* The generic terminal/display this belongs to.  */
  struct terminal *terminal;

  /* Opaque handle to the Rust-side connection (alias `display' to ease
     porting of X-flavored code).  */
  void *display;

  /* This is a cons cell of the form (NAME . FONT-LIST-CACHE).  */
  Lisp_Object name_list_element;

  /* Number of frames that are on this display.  */
  int reference_count;

  /* Logical identifier of this display and its default frame name.  */
  unsigned x_id;
  char *x_id_name;

  /* Font bookkeeping shared with the generic font code.  */
  int n_fonts;
  int smallest_char_width;
  int smallest_font_height;

  struct wlshm_bitmap_record *bitmaps;
  ptrdiff_t bitmaps_size;
  ptrdiff_t bitmaps_last;

  /* DPI of this screen, and logical->device pixel scale (fractional, via
     wp-fractional-scale).  */
  double resx, resy;
  double scale;

  int grabbed;
  int n_planes;
  int color_p;

  /* Emacs bitmap-id of the default icon bitmap, or -1.  */
  ptrdiff_t icon_bitmap_id;

  Window root_window;
  XrmDatabase rdb;

  /* Scroll bar cursors.  */
  Emacs_Cursor vertical_scroll_bar_cursor;
  Emacs_Cursor horizontal_scroll_bar_cursor;

  /* Range of text currently shown in mouse-face.  */
  Mouse_HLInfo mouse_highlight;

  struct frame *highlight_frame;
  struct frame *x_focus_frame;
  struct frame *x_focus_event_frame;
  struct frame *last_mouse_frame;
  struct frame *last_mouse_motion_frame;
  struct frame *last_mouse_glyph_frame;

  int last_mouse_motion_x;
  int last_mouse_motion_y;
  XRectangle last_mouse_glyph;
  Time last_mouse_movement_time;
  uint32_t last_user_time;

  void *last_mouse_scroll_bar;

  Emacs_Cursor invisible_cursor;
};

/* Per-frame backend state, hung off f->output_data.wlshm.  */
struct wlshm_output
{
  unsigned long foreground_color;
  unsigned long background_color;
  unsigned long cursor_color;
  unsigned long cursor_foreground_color;
  unsigned long mouse_color;
  unsigned long border_pixel;

  /* Scroll-bar colors (-1 = use the face/default).  Consumed by the
     Emacs-drawn scroll bars (M2).  */
  unsigned long scroll_bar_foreground_pixel;
  unsigned long scroll_bar_background_pixel;

  /* Cursors.  */
  Emacs_Cursor current_cursor;
  Emacs_Cursor text_cursor;
  Emacs_Cursor nontext_cursor;
  Emacs_Cursor modeline_cursor;
  Emacs_Cursor hand_cursor;
  Emacs_Cursor hourglass_cursor;
  Emacs_Cursor horizontal_drag_cursor;
  Emacs_Cursor vertical_drag_cursor;
  Emacs_Cursor left_edge_cursor;
  Emacs_Cursor top_left_corner_cursor;
  Emacs_Cursor top_edge_cursor;
  Emacs_Cursor top_right_corner_cursor;
  Emacs_Cursor right_edge_cursor;
  Emacs_Cursor bottom_right_corner_cursor;
  Emacs_Cursor bottom_edge_cursor;
  Emacs_Cursor bottom_left_corner_cursor;
  Emacs_Cursor current_pointer;

  Emacs_GC cursor_xgcv;

  /* Window ids, kept because generic code expects them.  */
  Window window_desc, parent_desc;
  char explicit_parent;

  /* If >= 0, a bitmap index for the icon.  */
  ptrdiff_t icon_bitmap;

  struct font *font;
  int baseline_offset;
  /* Fontset id, or -1.  */
  int fontset;

  int icon_top;
  int icon_left;

  /* Extra width allotted for vertical scroll bars, in pixels.  */
  int vertical_scroll_bar_extra;

  int titlebar_height;
  int toolbar_height;

  /* The display this frame is on.  */
  struct wlshm_display_info *display_info;

  /* Opaque handle (u64, cast) to the Rust-side window for this frame.  */
  void *wlshm_frame;

  /* Per-frame persistent Cairo software canvas (M3 multi-window): Emacs draws
     here incrementally; frame_up_to_date copies it into a wl_shm buffer.  */
  cairo_surface_t *canvas;
  cairo_t *cr;
  int canvas_w, canvas_h;

  int has_been_visible;
  int focus_state;

  long hint_flags;
  int preferred_width, preferred_height;

  /* Menu/tool bar geometry (generic code reads these).  */
  int menubar_height;
  int toolbar_top_height, toolbar_bottom_height;
  int toolbar_left_width, toolbar_right_width;

  /* Relief GCs/colors, used by face/relief drawing.  */
  struct relief
  {
    Emacs_GC xgcv;
    unsigned long pixel;
  }
  black_relief, white_relief;
  unsigned long relief_background;
  bool_bf relief_background_valid_p : 1;

  /* Most-recently-seen monitor scale factor.  */
  double watched_scale_factor;
};

/* Accessors mirroring the other backends' conventions.  */
#define FRAME_X_OUTPUT(f)         ((f)->output_data.wlshm)
#define FRAME_OUTPUT_DATA(f)      FRAME_X_OUTPUT (f)
#define FRAME_WLSHM_OUTPUT(f)      FRAME_X_OUTPUT (f)
#define FRAME_DISPLAY_INFO(f)     (FRAME_X_OUTPUT (f)->display_info)

#define FRAME_FOREGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->foreground_color)
#define FRAME_BACKGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->background_color)
#define FRAME_CURSOR_COLOR(f)     (FRAME_X_OUTPUT (f)->cursor_color)
#define FRAME_POINTER_TYPE(f)     (FRAME_X_OUTPUT (f)->current_pointer)

#define FRAME_FONT(f)             (FRAME_X_OUTPUT (f)->font)
#define FRAME_FONTSET(f)          (FRAME_X_OUTPUT (f)->fontset)
#define FRAME_BASELINE_OFFSET(f)  (FRAME_X_OUTPUT (f)->baseline_offset)

#define FRAME_DEFAULT_FACE(f) FACE_FROM_ID_OR_NULL (f, DEFAULT_FACE_ID)

#define FRAME_MENUBAR_HEIGHT(f) (FRAME_X_OUTPUT (f)->menubar_height)
#define FRAME_TOOLBAR_TOP_HEIGHT(f) (FRAME_X_OUTPUT (f)->toolbar_top_height)
#define FRAME_TOOLBAR_BOTTOM_HEIGHT(f) (FRAME_X_OUTPUT (f)->toolbar_bottom_height)
#define FRAME_TOOLBAR_HEIGHT(f) \
  (FRAME_TOOLBAR_TOP_HEIGHT (f) + FRAME_TOOLBAR_BOTTOM_HEIGHT (f))
#define FRAME_TOOLBAR_LEFT_WIDTH(f) (FRAME_X_OUTPUT (f)->toolbar_left_width)
#define FRAME_TOOLBAR_RIGHT_WIDTH(f) (FRAME_X_OUTPUT (f)->toolbar_right_width)
#define FRAME_TOOLBAR_WIDTH(f) \
  (FRAME_TOOLBAR_LEFT_WIDTH (f) + FRAME_TOOLBAR_RIGHT_WIDTH (f))

/* Native window / display handles.  Opaque at this milestone.  */
#define FRAME_X_WINDOW(f)         (FRAME_X_OUTPUT (f)->wlshm_frame)
/* The u64 Rust window handle for this frame (0 if none yet).  */
#define WLSHM_FRAME_HANDLE(f)     ((uint64_t) (uintptr_t) FRAME_X_OUTPUT (f)->wlshm_frame)
#define FRAME_NATIVE_WINDOW(f)    (FRAME_X_OUTPUT (f)->window_desc)
#define FRAME_X_DISPLAY(f)        (FRAME_DISPLAY_INFO (f)->display)

#define BLACK_PIX_DEFAULT(f) 0x000000
#define WHITE_PIX_DEFAULT(f) 0xFFFFFF

/* First position where characters can be shown (instead of a left
   scrollbar).  */
#define FIRST_CHAR_POSITION(f)				\
  (! (FRAME_HAS_VERTICAL_SCROLL_BARS_ON_LEFT (f)) ? 0	\
   : FRAME_SCROLL_BAR_COLS (f))

/* Scroll bars are drawn by Emacs onto the frame canvas (no toolkit, no
   subsurface).  Layout mirrors the backend-neutral struct in xterm.h.  */
struct scroll_bar
{
  /* These fields are shared by all vectors.  */
  union vectorlike_header header;

  /* The window we're a scroll bar for.  */
  Lisp_Object window;

  /* The next and previous in the chain of scroll bars in this frame.  */
  Lisp_Object next, prev;

  /* Fields after 'prev' are not traced by the GC.  */

  /* Position and size of the scroll bar in pixels, relative to the frame.  */
  int top, left, width, height;

  /* Start/end of the handle, relative to the handle area (0 = top).  */
  int start, end;

  /* Pixels from the top of the handle to where the user grabbed it while
     dragging, or -1 when not dragging.  */
  int dragging;

  /* True if the scroll bar is horizontal.  */
  bool horizontal;
} GCALIGNED_STRUCT;

/* Turning a lisp vector value into a pointer to a struct scroll_bar.  */
#define XSCROLL_BAR(vec) ((struct scroll_bar *) XVECTOR (vec))

/* Scroll-bar geometry (copied from xterm.h; the layout is backend-neutral).  */
#define VERTICAL_SCROLL_BAR_LEFT_BORDER (2)
#define VERTICAL_SCROLL_BAR_RIGHT_BORDER (2)
#define VERTICAL_SCROLL_BAR_TOP_BORDER (2)
#define VERTICAL_SCROLL_BAR_BOTTOM_BORDER (2)
#define HORIZONTAL_SCROLL_BAR_LEFT_BORDER (2)
#define HORIZONTAL_SCROLL_BAR_RIGHT_BORDER (2)
#define HORIZONTAL_SCROLL_BAR_TOP_BORDER (2)
#define HORIZONTAL_SCROLL_BAR_BOTTOM_BORDER (2)
#define VERTICAL_SCROLL_BAR_MIN_HANDLE (5)
#define HORIZONTAL_SCROLL_BAR_MIN_HANDLE (5)

#define VERTICAL_SCROLL_BAR_INSIDE_WIDTH(f, width)	\
  ((width) - VERTICAL_SCROLL_BAR_LEFT_BORDER - VERTICAL_SCROLL_BAR_RIGHT_BORDER)
#define VERTICAL_SCROLL_BAR_INSIDE_HEIGHT(f, height)	\
  ((height) - VERTICAL_SCROLL_BAR_TOP_BORDER - VERTICAL_SCROLL_BAR_BOTTOM_BORDER)
#define VERTICAL_SCROLL_BAR_TOP_RANGE(f, height)	\
  (VERTICAL_SCROLL_BAR_INSIDE_HEIGHT (f, height) - VERTICAL_SCROLL_BAR_MIN_HANDLE)
#define HORIZONTAL_SCROLL_BAR_INSIDE_WIDTH(f, width)	\
  ((width) - HORIZONTAL_SCROLL_BAR_LEFT_BORDER - HORIZONTAL_SCROLL_BAR_RIGHT_BORDER)
#define HORIZONTAL_SCROLL_BAR_INSIDE_HEIGHT(f, height)	\
  ((height) - HORIZONTAL_SCROLL_BAR_TOP_BORDER - HORIZONTAL_SCROLL_BAR_BOTTOM_BORDER)
#define HORIZONTAL_SCROLL_BAR_LEFT_RANGE(f, width)	\
  (HORIZONTAL_SCROLL_BAR_INSIDE_WIDTH (f, width) - HORIZONTAL_SCROLL_BAR_MIN_HANDLE)

/* Chain of all wlshm displays (named x_display_list to satisfy generic code
   that pokes the backend's display list, e.g. display_available).  */
extern struct wlshm_display_info *x_display_list;

extern struct wlshm_display_info *wlshm_term_init (Lisp_Object display_name);
extern struct terminal *wlshm_create_terminal (struct wlshm_display_info *dpyinfo);
extern void wlshm_delete_terminal (struct terminal *terminal);

/* Lisp init entry points (syms_of_*).  */
extern void syms_of_wlshmterm (void);
extern void syms_of_wlshmfns (void);

/* Cairo drawing onto the persistent software canvas (single-window model).
   ftcrfont.c drives these to render glyphs; the RIF uses them for fills.  */
extern cairo_t *wlshm_begin_cr_clip (struct frame *f);
extern void wlshm_end_cr_clip (struct frame *f);
/* Device (HiDPI) scale of F; used by FRAME_SCALE_FACTOR (frame.h).  */
extern double wlshm_frame_scale_factor (struct frame *f);
extern void wlshm_set_cr_source_with_color (struct frame *f, unsigned long color,
					   bool respects_alpha_background);
/* Write the current frame canvas to a PNG (golden/visual test harness).  */
extern bool wlshm_dump_canvas_png (struct frame *f, const char *path);
/* Present frame F's canvas to its Wayland window (used by tooltips).  */
extern void wlshm_present_frame (struct frame *f);

/* RIF default-font hook + pixel helper, used by frame creation.  */
extern void wlshm_default_font_parameter (struct frame *f, Lisp_Object parms);
/* Pointer cursor shapes, encoded into Emacs_Cursor (a void *) and passed to
   the Rust cursor-shape FFI.  Values must match cursor_code_to_shape in
   rust/wlshm-backend/src/winsys.rs.  Nonzero so the fields differ from NULL.  */
enum wlshm_cursor_shape
{
  WLSHM_CURSOR_DEFAULT = 1,
  WLSHM_CURSOR_TEXT = 2,
  WLSHM_CURSOR_HAND = 3,
  WLSHM_CURSOR_WAIT = 4,
  WLSHM_CURSOR_HRESIZE = 5,
  WLSHM_CURSOR_VRESIZE = 6,
};

extern void wlshm_unpack_pixel (unsigned long pixel, float *r, float *g, float *b);
extern void wlshm_glyph_string_colors (struct glyph_string *s, unsigned long *fg,
				      unsigned long *bg);
extern bool wlshm_defined_color (struct frame *f, const char *name,
				Emacs_Color *color, bool alloc, bool make_index);

#endif /* WLSHMTERM_H */
