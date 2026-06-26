/* Definitions and headers for the wlshm (Wayland + wl_shm) backend.

This file is part of a personal-fork experiment (see wlshm-architecture.md).
It mirrors pgtkgui.h: it supplies the X-flavored type/aliases the generic
display code still expects, but with backend-neutral handle types since the
wlshm backend talks raw Wayland (no Gdk/Xlib).  */

#ifndef __WLSHMGUI_H__
#define __WLSHMGUI_H__

/* Emulate XCharStruct.  */
typedef struct _XCharStruct
{
  int rbearing;
  int lbearing;
  int width;
  int ascent;
  int descent;
} XCharStruct;

/* Fake structure from Xlib.h to represent two-byte characters.  */
typedef unsigned short unichar;
typedef unichar XChar2b;

#define STORE_XCHAR2B(chp, b1, b2) \
  (*(chp) = ((XChar2b)((((b1) & 0x00ff) << 8) | ((b2) & 0x00ff))))

#define XCHAR2B_BYTE1(chp) \
  ((*(chp) & 0xff00) >> 8)

#define XCHAR2B_BYTE2(chp) \
  (*(chp) & 0x00ff)

/* Backend-neutral handles.  The wlshm backend manages real cursors/windows on
   the Rust side; here they are opaque.  */
typedef void *Emacs_Cursor;

typedef void *Color;
typedef int Window;
typedef void Display;

#define WINDOW_HANDLE_UINTPTR(h) ((uintptr_t) (h))

/* Xism */
typedef void *XrmDatabase;

/* Rectangle handling, matching the other backends.  */
typedef struct
{
  int x, y;
  unsigned width, height;
} XRectangle;

/* This stuff is needed by frame.c.  */
#define ForgetGravity		0
#define NorthWestGravity	1
#define NorthGravity		2
#define NorthEastGravity	3
#define WestGravity		4
#define CenterGravity		5
#define EastGravity		6
#define SouthWestGravity	7
#define SouthGravity		8
#define SouthEastGravity	9
#define StaticGravity		10

#define NoValue		0x0000
#define XValue  	0x0001
#define YValue		0x0002
#define WidthValue  	0x0004
#define HeightValue  	0x0008
#define AllValues 	0x000F
#define XNegative 	0x0010
#define YNegative 	0x0020

#define USPosition	(1L << 0)	/* user specified x, y */
#define USSize		(1L << 1)	/* user specified width, height */

#define PPosition	(1L << 2)	/* program specified position */
#define PSize		(1L << 3)	/* program specified size */
#define PMinSize	(1L << 4)	/* program specified minimum size */
#define PMaxSize	(1L << 5)	/* program specified maximum size */
#define PResizeInc	(1L << 6)	/* program specified resize increments */
#define PAspect		(1L << 7)	/* program specified min, max aspect ratios */
#define PBaseSize	(1L << 8)	/* program specified base for incrementing */
#define PWinGravity	(1L << 9)	/* program specified window gravity */

#define NativeRectangle XRectangle

#define CONVERT_TO_EMACS_RECT(xr, nr)		\
  ((xr).x     = (nr).x,				\
   (xr).y     = (nr).y,				\
   (xr).width = (nr).width,			\
   (xr).height = (nr).height)

#define CONVERT_FROM_EMACS_RECT(xr, nr)		\
  ((nr).x      = (xr).x,			\
   (nr).y      = (xr).y,			\
   (nr).width  = (xr).width,			\
   (nr).height = (xr).height)

#define STORE_NATIVE_RECT(nr, px, py, pwidth, pheight)	\
  ((nr).x      = (px),					\
   (nr).y      = (py),					\
   (nr).width  = (pwidth),				\
   (nr).height = (pheight))

#endif /* __WLSHMGUI_H__ */
