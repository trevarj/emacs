/* Wayland + wgpu terminal backend for Emacs -- terminal/redisplay shim.

M0 scaffold.  This file wires the Emacs redisplay interface (RIF) and the
terminal hooks to the Rust backend over FFI (wgpu_ffi.h).  At M0 almost every
hook is a stub: the goal is a terminal that creates and links, so the
architecture's seams are pinned down before M1 brings up an actual window.

See wgpu-backend-plan.md for the full design.  Structurally this mirrors
pgtkterm.c (pgtk_create_terminal at pgtkterm.c:4812) but holds no drawing
logic -- each live hook forwards to a wgpu_* FFI call.  */

#include <config.h>

#include "lisp.h"
#include "frame.h"
#include "termchar.h"
#include "termhooks.h"
#include "dispextern.h"
#include "wgputerm.h"

/* ------------------------------------------------------------------ */
/* Terminal hooks (struct terminal).  M0: input-drain + stubs.        */
/* ------------------------------------------------------------------ */

/* Drain input events the Rust event thread has queued, converting each into
   an Emacs input_event.  M0: the FFI returns 0 events (no window yet).  */
static int
wgpu_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  enum { BATCH = 64 };
  WgpuEvent evs[BATCH];
  int n = wgpu_backend_poll_events (evs, BATCH);
  int count = 0;

  for (int i = 0; i < n; i++)
    {
      /* M1+: translate WgpuEvent -> struct input_event and kbd_buffer_store.
         M0 has nothing to translate.  */
      (void) evs[i];
      (void) hold_quit;
      count++;
    }
  return count;
}

static void
wgpu_frame_up_to_date (struct frame *f)
{
  /* M1: hand the recorded frame command buffer to the render thread and
     request present.  */
  (void) f;
}

static void
wgpu_delete_terminal (struct terminal *terminal)
{
  (void) terminal;
  wgpu_backend_shutdown ();
}

/* ------------------------------------------------------------------ */
/* Redisplay interface (struct redisplay_interface).                  */
/* M0: all stubs; populated milestone by milestone.                   */
/* ------------------------------------------------------------------ */

static struct redisplay_interface wgpu_redisplay_interface =
  {
    /* All hooks NULL at M0.  M2 fills draw_glyph_string, clear_frame_area,
       draw_window_cursor, draw_fringe_bitmap, etc.  Designated-NULL init
       keeps this honest as hooks come online.  */
    0
  };

/* ------------------------------------------------------------------ */
/* Terminal creation.                                                 */
/* ------------------------------------------------------------------ */

struct terminal *
wgpu_create_terminal (struct wgpu_display_info *dpyinfo)
{
  struct terminal *terminal
    = create_terminal (output_wgpu, &wgpu_redisplay_interface);

  terminal->display_info.wgpu = dpyinfo;
  dpyinfo->terminal = terminal;

  /* Wire the terminal hooks we implement at this milestone.  */
  terminal->read_socket_hook = wgpu_read_socket;
  terminal->frame_up_to_date_hook = wgpu_frame_up_to_date;
  terminal->delete_terminal_hook = wgpu_delete_terminal;

  /* M1+: focus/visibility/size/scroll-bar/menu hooks.  */

  return terminal;
}

struct wgpu_display_info *
wgpu_term_init (Lisp_Object display_name)
{
  /* M1: open the Wayland connection on the Rust side and spin up the
     render+event thread.  M0: just initialize the backend global state.  */
  if (wgpu_backend_init () != WGPU_OK)
    error ("wgpu backend failed to initialize");

  (void) display_name;
  return NULL; /* M1 returns a real dpyinfo.  */
}

void
syms_of_wgputerm (void)
{
}
