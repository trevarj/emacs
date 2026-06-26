//! FFI-driven Wayland windows for Emacs (single-threaded, multi-window).
//!
//! Emacs owns the main loop; this exposes Wayland windows it drives explicitly.
//! There is ONE Wayland connection multiplexing N surfaces (toplevels and
//! popups/tooltips).  Connection-global state (seat, keyboard,
//! clipboard, cursor, key-repeat timer, the event queue) lives in `AppState`;
//! per-surface state (the xdg `Window`, its `SlotPool`, size, scale) lives in a
//! `WlWindow` keyed by an opaque `u64` handle the C side stores in
//! `struct wlshm_output.wlshm_frame`.
//!
//!   * `wlshm_window_open(title, parent, kind) -> handle` creates a surface.
//!   * `wlshm_window_fd` / `wlshm_window_dispatch` drive the (single) connection;
//!     input/configure/close are drained via `wlshm_window_poll_events`, each
//!     event carrying the `window` handle it belongs to.
//!   * `wlshm_window_present(handle, ...)` copies the C-side Cairo canvas of that
//!     frame into a free wl_shm buffer and commits it.
//!
//! This crate does NOT rasterize; all drawing happens in C via Cairo.

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_int};
use std::os::unix::io::AsRawFd;
use std::time::{Duration, Instant};

use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    data_device_manager::{
        data_device::{DataDevice, DataDeviceHandler},
        data_offer::{DataOfferHandler, DragOffer},
        data_source::{CopyPasteSource, DataSourceHandler},
        DataDeviceManagerState, ReadPipe, WritePipe,
    },
    delegate_compositor, delegate_data_device, delegate_output, delegate_primary_selection,
    delegate_registry, delegate_shm, delegate_xdg_popup, delegate_xdg_shell, delegate_xdg_window,
    output::{OutputHandler, OutputState},
    primary_selection::{
        device::{PrimarySelectionDevice, PrimarySelectionDeviceHandler},
        selection::{PrimarySelectionSource, PrimarySelectionSourceHandler},
        PrimarySelectionManagerState,
    },
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    reexports::protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::{
        Shape, WpCursorShapeDeviceV1,
    },
    seat::{
        keyboard::{KeyEvent, KeyboardHandler, Keysym, Modifiers, RepeatInfo},
        pointer::{
            cursor_shape::CursorShapeManager, PointerEvent, PointerEventKind, PointerHandler,
            BTN_LEFT, BTN_MIDDLE, BTN_RIGHT,
        },
        Capability, SeatHandler, SeatState,
    },
    shell::{
        xdg::{
            popup::{Popup, PopupConfigure, PopupHandler},
            window::{Window, WindowConfigure, WindowDecorations, WindowHandler},
            XdgPositioner, XdgShell, XdgSurface,
        },
        WaylandSurface,
    },
    shm::{slot::{Buffer, SlotPool}, Shm, ShmHandler},
    delegate_seat, delegate_keyboard, delegate_pointer,
};
use smithay_client_toolkit::reexports::protocols::xdg::shell::client::xdg_positioner::{
    Anchor, ConstraintAdjustment, Gravity,
};
use smithay_client_toolkit::reexports::protocols::wp::primary_selection::zv1::client::{
    zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1,
    zwp_primary_selection_source_v1::ZwpPrimarySelectionSourceV1,
};
use wayland_client::{
    backend::{ObjectId, WaylandError},
    globals::registry_queue_init,
    protocol::{
        wl_data_device::WlDataDevice, wl_data_device_manager::DndAction,
        wl_data_source::WlDataSource, wl_keyboard::WlKeyboard, wl_output,
        wl_pointer::WlPointer, wl_seat::WlSeat, wl_shm,
        wl_subcompositor::WlSubcompositor, wl_subsurface::WlSubsurface, wl_surface,
    },
    Connection, Dispatch, EventQueue, Proxy, QueueHandle,
};
// text-input-v3 (IME).  Not wrapped by sctk 0.19, so we bind the manager from
// the registry and implement Dispatch for both objects ourselves below.
use wayland_protocols::wp::text_input::zv3::client::{
    zwp_text_input_manager_v3::ZwpTextInputManagerV3,
    zwp_text_input_v3::{self, ZwpTextInputV3},
};
// HiDPI fractional scaling: wp_fractional_scale_v1 (the compositor sends the
// preferred scale as scale*120) + wp_viewporter (maps the physical-pixel buffer
// down to the logical surface size).  Neither is wrapped by sctk 0.19, so we
// bind the managers from the registry and implement Dispatch ourselves.
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
    wp_fractional_scale_v1::{self, WpFractionalScaleV1},
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::WpViewport, wp_viewporter::WpViewporter,
};

use crate::event::{WlshmEvent, WLSHM_MOD_ALT, WLSHM_MOD_CTRL, WLSHM_MOD_LOGO, WLSHM_MOD_SHIFT};

// ManuallyDrop: at process exit we deliberately leak the connection rather than
// run libwayland's teardown (which can crash).  The OS reclaims everything.
// Individual windows DO drop normally on wlshm_window_close (proper surface
// destroy for transient popups/tooltips while the connection is alive).
thread_local! {
    static BACKEND: RefCell<Option<ManuallyDrop<Backend>>> = const { RefCell::new(None) };
}

/// MIME types we offer/accept for the text clipboard, in preference order.
const CLIPBOARD_MIME: &[&str] = &[
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
    "TEXT",
];

/// MIME types we accept for a drag-and-drop drop, in preference order.  A
/// `text/uri-list` is preferred (file drops); otherwise we fall back to plain
/// text.  The matched type's index into this list <-> `is_uri_list` is decided
/// by name (see `mime_is_uri_list`).
const DND_MIME: &[&str] = &[
    "text/uri-list",
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
];

fn mime_is_uri_list(mime: &str) -> bool {
    mime.eq_ignore_ascii_case("text/uri-list")
}

/// Build the itimerspec for key repeat: first expire after `delay_ms`, then
/// every `rate_ms`.  Both are clamped to >= 1ms so the timer always arms.
fn repeat_itimerspec(delay_ms: u32, rate_ms: u32) -> libc::itimerspec {
    let to_ts = |ms: u32| libc::timespec {
        tv_sec: (ms / 1000) as libc::time_t,
        tv_nsec: ((ms % 1000) * 1_000_000) as libc::c_long,
    };
    libc::itimerspec {
        it_interval: to_ts(rate_ms.max(1)),
        it_value: to_ts(delay_ms.max(1)),
    }
}

/// Create a CLOCK_MONOTONIC non-blocking timerfd, logging (not aborting) on
/// failure.  A -1 here disables that timer (key repeat or the present
/// deadline); arm/pump paths guard on `< 0`, so the rest keeps working.
fn make_timer_fd(label: &str) -> i32 {
    let fd = unsafe {
        libc::timerfd_create(libc::CLOCK_MONOTONIC, libc::TFD_NONBLOCK | libc::TFD_CLOEXEC)
    };
    if fd < 0 {
        eprintln!(
            "wlshm: timerfd_create ({label}) failed: {}",
            std::io::Error::last_os_error()
        );
    }
    fd
}

/// True for X/xkb modifier and lock keysyms, which must not be delivered as
/// keystrokes (their state is tracked via update_modifiers).
fn is_modifier_keysym(ks: u32) -> bool {
    matches!(ks,
        0xffe1..=0xffee   // Shift_L .. Hyper_R
        | 0xff7e          // Mode_switch
        | 0xff7f          // Num_Lock
        | 0xfe01..=0xfe0f) // ISO_Lock .. ISO_Level5_Lock (incl. Level3_Shift)
}

/// Surface role.  Toplevels are Emacs frames; popups (menus) and tooltips are
/// xdg_popups that FLOAT over their parent (so tiling compositors never tile
/// them).  A popup is created lazily: `open_window` records `Pending` and the
/// real xdg_popup is built on the first `set_geometry`, once the anchor (x,y)
/// and size are known.
enum Role {
    Toplevel(Window),
    Popup(Popup),
    /// A child frame (parent-frame): a wl_subsurface floating over its parent,
    /// positioned by the client, with no WM decoration -- so it is NOT a
    /// separate window the WM tiles.  Holds its own wl_surface plus the
    /// wl_subsurface, and the parent's surface (committed when the child moves,
    /// since subsurface placement applies on the PARENT's commit).
    Subsurface {
        surface: wl_surface::WlSurface,
        subsurface: WlSubsurface,
        parent_surface: wl_surface::WlSurface,
    },
    /// Awaiting set_geometry to become a Popup.  `grab` = take a keyboard grab
    /// (menus do; tooltips don't).
    Pending { parent: u64, grab: bool },
}

/// Latest redraw coalesced while a frame callback is still outstanding.
///
/// This deliberately is NOT a wl_buffer.  Creating a SlotPool buffer sends a
/// Wayland create_buffer request immediately, and a benchmark can generate many
/// full-screen redisplays before the compositor releases the previous frame.
/// Keep only one client-side pixel snapshot and create the wl_buffer when that
/// snapshot is actually committed.
struct PendingFrame {
    width: u32,
    height: u32,
    stride: u32,
    dx: i32,
    dy: i32,
    dw: i32,
    dh: i32,
    pixels: Vec<u8>,
}

impl PendingFrame {
    unsafe fn new_from_source(
        src: *const u8,
        src_w: u32,
        src_h: u32,
        src_stride: u32,
        dx: i32,
        dy: i32,
        dw: i32,
        dh: i32,
    ) -> Self {
        let stride = src_w * 4;
        let mut pixels = vec![0; (src_h as usize) * (stride as usize)];
        copy_source_pixels(src, src_w, src_h, src_stride, stride, &mut pixels);
        Self {
            width: src_w,
            height: src_h,
            stride,
            dx,
            dy,
            dw,
            dh,
            pixels,
        }
    }

    unsafe fn replace_from_source(
        &mut self,
        src: *const u8,
        src_w: u32,
        src_h: u32,
        src_stride: u32,
        dx: i32,
        dy: i32,
        dw: i32,
        dh: i32,
    ) {
        let stride = src_w * 4;
        self.width = src_w;
        self.height = src_h;
        self.stride = stride;
        self.dx = dx;
        self.dy = dy;
        self.dw = dw;
        self.dh = dh;
        self.pixels.resize((src_h as usize) * (stride as usize), 0);
        copy_source_pixels(src, src_w, src_h, src_stride, stride,
                           &mut self.pixels);
    }
}

/// Per-surface state.  One per Emacs frame (toplevel), menu popup, or tooltip.
struct WlWindow {
    role: Role,
    /// wl_shm slot pool: source of the (double-buffered) buffers we present.
    pool: SlotPool,
    /// Logical surface size in LOGICAL pixels (xdg configure sizes are logical).
    size: (u32, u32),
    /// Scale * 120 (the wp_fractional_scale_v1 unit).  120 == 1.0.  With the
    /// fractional+viewport path this is fractional (e.g. 180 == 1.5x); with the
    /// integer fallback it is the wl_output scale * 120.  Default 120.
    scale120: u32,
    /// Per-surface fractional-scale + viewport objects (fractional path only).
    /// Absent when the compositor lacks the protocols (integer fallback).
    fractional: Option<WpFractionalScaleV1>,
    viewport: Option<WpViewport>,
    /// New size the compositor asked for, pending delivery to Emacs.
    pending_resize: Option<(u32, u32)>,
    configured: bool,
    /// A wl_surface.frame callback is outstanding: the compositor has not yet
    /// signalled it is ready for the next buffer.  While true, present() coalesces
    /// into `pending` instead of committing, so a fast redisplay loop (e.g.
    /// nonstop scrolling) throttles to the compositor's frame rate and cannot
    /// flood it with buffer commits.
    frame_pending: bool,
    /// Latest prepared-but-uncommitted frame snapshot.  It is copied into a
    /// wl_shm buffer only when the frame callback/deadline path will actually
    /// commit it, so throttled redraws do not create/destroy Wayland buffer
    /// objects at benchmark speed.
    pending: Option<PendingFrame>,
    /// When we last committed a buffer.  Used as a present DEADLINE: if a frame
    /// callback is overdue (the compositor withheld it -- e.g. an idle/occluded
    /// output that only repaints on cursor motion), present() commits anyway
    /// rather than stranding the latest frame off-screen (mode line stuck on
    /// stale content until an unrelated event).  Still bounded to ~1 commit per
    /// deadline, so it cannot flood the compositor.
    last_commit: Option<Instant>,
    /// Subsurfaces only: a parent commit is needed to apply this child's
    /// placement (initial map / remap).  A desync subsurface applies its own
    /// buffer on its own commit, so steady-state updates (e.g. corfu scrolling
    /// the selection) do NOT need a parent commit -- committing the parent every
    /// frame forces a redundant parent repaint.  Set on creation and on unmap,
    /// cleared after present() commits the parent.  (Moves go through
    /// wlshm_window_set_subsurface_pos, which commits the parent itself.)
    parent_dirty: bool,
}

impl WlWindow {
    /// The wl_surface, or None for a popup not yet created (Pending).
    fn wl_surface(&self) -> Option<&wl_surface::WlSurface> {
        match &self.role {
            Role::Toplevel(w) => Some(w.wl_surface()),
            Role::Popup(p) => Some(p.wl_surface()),
            Role::Subsurface { surface, .. } => Some(surface),
            Role::Pending { .. } => None,
        }
    }

    /// Set the wp_viewport DESTINATION = the LOGICAL surface size.  The SOURCE
    /// (the crisp 1:1 crop) is set in present(), clamped to the actual buffer,
    /// so it can never exceed the content area (a wp_viewport `out_of_buffer`
    /// protocol error -- which a subsurface hit when its size/scale and buffer
    /// briefly diverged, with no xdg configure to resync them).
    fn update_viewport(&self) {
        if let Some(vp) = self.viewport.as_ref() {
            let (lw, lh) = self.size;
            vp.set_destination(lw.max(1) as i32, lh.max(1) as i32);
        }
    }
}

/// How long present() will keep coalescing into `pending` while a frame callback
/// is outstanding before committing anyway.  A safety net for compositors that
/// withhold the wl_surface.frame callback on an idle output; ~2 frames at 60Hz,
/// so the forced-commit rate stays bounded (no flood) yet stale content clears
/// promptly.
const PRESENT_DEADLINE: Duration = Duration::from_millis(33);

unsafe fn copy_source_pixels(
    src: *const u8,
    src_w: u32,
    src_h: u32,
    src_stride: u32,
    dst_stride: u32,
    dst: &mut [u8],
) {
    let src_stride = src_stride as usize;
    let dst_stride = dst_stride as usize;
    // Each row copies src_w*4 bytes but advances by src_stride; if a caller ever
    // passed src_stride < src_w*4 the last rows would read past the documented
    // src_h*src_stride bound.  Cairo always pads stride >= width*4, so clamp
    // defensively rather than trust the FFI caller at this unsafe boundary.
    let row_bytes = ((src_w as usize) * 4).min(src_stride).min(dst_stride);
    for y in 0..src_h as usize {
        let s = std::slice::from_raw_parts(src.add(y * src_stride), row_bytes);
        dst[y * dst_stride..y * dst_stride + row_bytes].copy_from_slice(s);
    }
}

unsafe fn create_buffer_from_source(
    pool: &mut SlotPool,
    src: *const u8,
    src_w: u32,
    src_h: u32,
    src_stride: u32,
) -> Option<Buffer> {
    let stride = src_w as i32 * 4;
    let (buffer, canvas) = match pool.create_buffer(
        src_w as i32, src_h as i32, stride, wl_shm::Format::Xrgb8888,
    ) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("wlshm: shm create_buffer: {e}");
            return None;
        }
    };
    copy_source_pixels(src, src_w, src_h, src_stride, stride as u32, canvas);
    Some(buffer)
}

fn create_buffer_from_pending(pool: &mut SlotPool, pending: &PendingFrame) -> Option<Buffer> {
    let (buffer, canvas) = match pool.create_buffer(
        pending.width as i32,
        pending.height as i32,
        pending.stride as i32,
        wl_shm::Format::Xrgb8888,
    ) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("wlshm: shm create_buffer: {e}");
            return None;
        }
    };
    canvas[..pending.pixels.len()].copy_from_slice(&pending.pixels);
    Some(buffer)
}

fn set_viewport_source(w: &WlWindow, pw: u32, ph: u32) {
    if w.fractional.is_some() {
        if let Some(vp) = w.viewport.as_ref() {
            let s = w.scale120 as f64 / 120.0;
            let sw = (w.size.0 as f64 * s).min(pw as f64).max(1.0);
            let sh = (w.size.1 as f64 * s).min(ph as f64).max(1.0);
            vp.set_source(0.0, 0.0, sw, sh);
        }
    }
}

/// Attach BUFFER to SURFACE, damage it, request a wl_surface.frame callback (so
/// the next present throttles to the compositor's pace), and commit.  Returns
/// false if the attach failed (the caller then leaves frame_pending clear so the
/// next present retries).  Shared by present() (Backend) and the frame callback
/// (AppState), both of which hold a QueueHandle<AppState>.
fn commit_buffer(qh: &QueueHandle<AppState>, surface: &wl_surface::WlSurface,
                 buffer: &Buffer, dx: i32, dy: i32, dw: i32, dh: i32) -> bool {
    if buffer.attach_to(surface).is_err() {
        return false;
    }
    surface.damage_buffer(dx, dy, dw, dh);
    // Arm the next frame callback BEFORE commit so it is part of this commit.
    surface.frame(qh, surface.clone());
    surface.commit();
    true
}

/// Connection-global sctk delegate target + all per-seat state.
struct AppState {
    registry_state: RegistryState,
    output_state: OutputState,
    seat_state: SeatState,
    keyboard: Option<WlKeyboard>,
    pointer: Option<WlPointer>,
    /// Last pointer position (surface pixels), for axis events that omit it.
    pointer_pos: (i32, i32),
    cursor_shape_mgr: Option<CursorShapeManager>,
    cursor_shape_device: Option<WpCursorShapeDeviceV1>,
    pointer_enter_serial: u32,
    /// Monotonic token for xdg_popup.reposition requests.
    reposition_token: u32,
    current_cursor: u32,
    mods: u32,
    timer_fd: i32,
    /// Single-shot timerfd armed when present() coalesces a frame into `pending`
    /// while a frame callback is outstanding.  It wakes the Emacs event loop
    /// (registered as a keyboard wait descriptor) after PRESENT_DEADLINE so the
    /// coalesced frame is committed even if the compositor never delivers the
    /// callback and nothing else happens -- otherwise a single mode-line change
    /// followed by idle would stay stale until an unrelated event (mouse-over).
    present_timer_fd: i32,
    repeat_delay_ms: u32,
    repeat_rate_ms: u32,
    /// Currently-held repeating key: (raw keycode, keysym, unichar, window id).
    /// The window id pins replays to the frame focused at press time, so a frame
    /// deleted mid-repeat can't redirect the held key into another frame.
    repeat: Option<(u32, u32, u32, u64)>,
    serial: u32,
    data_device_manager: Option<DataDeviceManagerState>,
    data_device: Option<DataDevice>,
    clipboard_source: Option<CopyPasteSource>,
    clipboard_text: Vec<u8>,
    // PRIMARY selection (Wayland zwp_primary_selection): a parallel path to the
    // CLIPBOARD above.  The manager is optional (not all compositors expose it).
    primary_manager: Option<PrimarySelectionManagerState>,
    primary_device: Option<PrimarySelectionDevice>,
    primary_source: Option<PrimarySelectionSource>,
    primary_text: Vec<u8>,
    /// Last drag-and-drop offer position + target while a drag hovers us, so a
    /// `drop_performed` (which carries no coordinates) can report where the drop
    /// landed.  Set in DnD `enter`/`motion`, cleared on `leave`.
    dnd_pos: (i32, i32),
    dnd_window: u64,
    /// A drop awaiting its pipe read: (pipe, is_uri_list, x, y, window).  Set in
    /// `drop_performed`; drained in `Backend::dispatch` (where the event queue
    /// is available to roundtrip so the source can write the data).
    /// A drop awaiting its deferred read in Backend::drain_pending_drop:
    /// (pipe, is_uri, x, y, win, offer).  The offer is held so finish() is sent
    /// only AFTER the bytes are read; some sources free the transfer on
    /// dnd_finished, which would truncate a finish-then-read sequence.
    pending_drop: Option<(ReadPipe, bool, i32, i32, u64, DragOffer)>,
    /// Bytes of the most recent completed drop, handed to C via the
    /// `wlshm_window_get_drop` side-channel after it pops the Drop event.
    drop_text: Vec<u8>,
    // --- IME (zwp_text_input_v3) ---
    /// The manager global, if the compositor exposes text-input-v3.  Absent on
    /// compositors without an IME bridge (e.g. plain weston in CI).
    text_input_mgr: Option<ZwpTextInputManagerV3>,
    /// The per-seat text-input object (created once a seat is known).
    text_input: Option<ZwpTextInputV3>,
    /// Whether we currently have text-input enabled (i.e. a wlshm surface holds
    /// keyboard focus).  Used to avoid redundant enable/disable churn.
    text_input_enabled: bool,
    /// Pending preedit text accumulated from `preedit_string`, applied on `done`.
    pending_preedit: String,
    /// Pending committed text accumulated from `commit_string`, applied on `done`.
    pending_commit: String,
    /// The most recent preedit string, handed to C via the
    /// `wlshm_window_get_preedit` side-channel after it pops a Preedit event.
    preedit_text: Vec<u8>,
    /// Pending input events for Emacs's read_socket (each stamped .window).
    events: VecDeque<WlshmEvent>,
    /// Globals needed to mint new surfaces on demand.
    compositor: CompositorState,
    xdg_shell: XdgShell,
    shm: Shm,
    /// Live windows by handle, and the reverse map for event routing.
    windows: HashMap<u64, WlWindow>,
    surface_to_id: HashMap<ObjectId, u64>,
    focused_window: Option<u64>,
    next_id: u64,
    /// Fallback integer output scale * 120 (used until a window learns its own
    /// scale; also the basis for the integer fallback path).  Default 120.
    default_scale120: u32,
    /// HiDPI fractional scaling globals (absent -> integer fallback path).
    fractional_mgr: Option<WpFractionalScaleManagerV1>,
    viewporter: Option<WpViewporter>,
    /// wl_subcompositor: makes a child frame (parent-frame) a wl_subsurface that
    /// floats over its parent instead of a separate toplevel the WM tiles.
    /// Absent on the (rare) compositor without it -> child frames stay toplevels.
    subcompositor: Option<WlSubcompositor>,
}

impl AppState {
    /// Window handle of the surface the keyboard/pointer most recently entered,
    /// or any window if none is focused (used for window-less events).
    fn primary(&self) -> u64 {
        self.focused_window
            .filter(|id| self.windows.contains_key(id))
            .or_else(|| self.windows.keys().copied().next())
            .unwrap_or(0)
    }

    fn id_for_surface(&self, s: &wl_surface::WlSurface) -> u64 {
        self.surface_to_id.get(&s.id()).copied().unwrap_or(0)
    }

    fn push(&mut self, e: WlshmEvent) {
        self.events.push_back(e);
    }

    /// Turn the IME on for the focused surface: per text-input-v3, enable()
    /// declares we accept input, then commit() flushes the request.  Called
    /// when a wlshm surface gains keyboard focus.  No-op if already enabled or
    /// the compositor lacks text-input.
    fn enable_text_input(&mut self) {
        if self.text_input_enabled {
            return;
        }
        if let Some(ti) = self.text_input.as_ref() {
            ti.enable();
            // We don't track surrounding text or a cursor rectangle yet; a bare
            // enable+commit is enough for compose/CJK candidate delivery.
            ti.commit();
            self.text_input_enabled = true;
        }
    }

    /// Turn the IME off (surface lost keyboard focus): disable() + commit().
    /// Also drops any in-progress preedit so a stale composition doesn't linger.
    fn disable_text_input(&mut self) {
        if !self.text_input_enabled {
            return;
        }
        if let Some(ti) = self.text_input.as_ref() {
            ti.disable();
            ti.commit();
        }
        self.text_input_enabled = false;
        // Clear any dangling preedit on blur.
        if !self.pending_preedit.is_empty() || !self.preedit_text.is_empty() {
            self.pending_preedit.clear();
            self.preedit_text.clear();
            let win = self.primary();
            self.push(WlshmEvent::preedit().on(win));
        }
    }

    /// Arm the repeat timerfd: first fire after `delay`, then every `rate` ms.
    fn arm_repeat(&self) {
        if self.timer_fd < 0 {
            return;
        }
        let spec = repeat_itimerspec(self.repeat_delay_ms, self.repeat_rate_ms);
        unsafe { libc::timerfd_settime(self.timer_fd, 0, &spec, std::ptr::null_mut()) };
    }

    /// Arm the present-deadline timerfd to fire ONCE after PRESENT_DEADLINE, so
    /// a coalesced frame is delivered even if no callback and no other event
    /// arrive.  Idempotent-ish: re-arming just pushes the deadline out by one
    /// interval from the latest coalesce, which is fine.
    fn arm_present_timer(&self) {
        if self.present_timer_fd < 0 {
            return;
        }
        let ms = PRESENT_DEADLINE.as_millis() as i64;
        let spec = libc::itimerspec {
            it_interval: libc::timespec { tv_sec: 0, tv_nsec: 0 },
            it_value: libc::timespec {
                tv_sec: ms / 1000,
                tv_nsec: (ms % 1000) * 1_000_000,
            },
        };
        unsafe {
            libc::timerfd_settime(self.present_timer_fd, 0, &spec, std::ptr::null_mut())
        };
    }

    /// Drain the present-deadline timerfd (so it stops waking us).
    fn drain_present_timer(&self) {
        if self.present_timer_fd < 0 {
            return;
        }
        let mut buf = [0u8; 8];
        unsafe {
            libc::read(self.present_timer_fd, buf.as_mut_ptr() as *mut libc::c_void, 8)
        };
    }

    fn disarm_repeat(&mut self) {
        self.repeat = None;
        if self.timer_fd < 0 {
            return;
        }
        let spec: libc::itimerspec = unsafe { std::mem::zeroed() };
        unsafe { libc::timerfd_settime(self.timer_fd, 0, &spec, std::ptr::null_mut()) };
    }

    fn pump_repeat(&mut self) {
        if self.timer_fd < 0 {
            return;
        }
        let mut buf = [0u8; 8];
        let n = unsafe { libc::read(self.timer_fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        if n != 8 {
            return;
        }
        let expirations = u64::from_ne_bytes(buf);
        if let Some((_, ks, unichar, win)) = self.repeat {
            // Replay to the window focused when the key was pressed, not to
            // whatever primary() resolves to now; skip it if that window is gone.
            if self.windows.contains_key(&win) {
                for _ in 0..expirations.min(4) {
                    self.push(WlshmEvent::key(ks, unichar, self.mods).on(win));
                }
            }
        }
    }
}

struct Backend {
    conn: Connection,
    qh: QueueHandle<AppState>,
    event_queue: EventQueue<AppState>,
    state: AppState,
}

impl Backend {
    /// Connect to Wayland, bind globals, and set up the seat/clipboard.  Creates
    /// NO window — callers add windows with `open_window`.
    fn connect() -> Result<Backend, String> {
        let conn = Connection::connect_to_env().map_err(|e| format!("wayland connect: {e}"))?;
        let (globals, mut event_queue) =
            registry_queue_init::<AppState>(&conn).map_err(|e| format!("registry init: {e}"))?;
        let qh = event_queue.handle();

        let compositor =
            CompositorState::bind(&globals, &qh).map_err(|e| format!("wl_compositor: {e}"))?;
        let xdg_shell = XdgShell::bind(&globals, &qh).map_err(|e| format!("xdg_wm_base: {e}"))?;
        let shm = Shm::bind(&globals, &qh).map_err(|e| format!("wl_shm: {e}"))?;
        let data_device_manager = DataDeviceManagerState::bind(&globals, &qh).ok();
        // Optional: absent on compositors without zwp_primary_selection.
        let primary_manager = PrimarySelectionManagerState::bind(&globals, &qh).ok();
        let cursor_shape_mgr = CursorShapeManager::bind(&globals, &qh).ok();
        // text-input-v3 manager.  Bound raw (sctk doesn't wrap it); absent on
        // compositors without IME support, in which case IME is silently a no-op.
        let text_input_mgr = globals
            .bind::<ZwpTextInputManagerV3, _, _>(&qh, 1..=1, ())
            .ok();
        // HiDPI fractional scaling: bind the fractional-scale manager and the
        // viewporter.  Both optional; if either is missing we use the classic
        // integer wl_output scale + wl_surface.set_buffer_scale fallback.
        let fractional_mgr = globals
            .bind::<WpFractionalScaleManagerV1, _, _>(&qh, 1..=1, ())
            .ok();
        let viewporter = globals.bind::<WpViewporter, _, _>(&qh, 1..=1, ()).ok();
        let subcompositor = globals.bind::<WlSubcompositor, _, _>(&qh, 1..=1, ()).ok();

        let mut state = AppState {
            registry_state: RegistryState::new(&globals),
            output_state: OutputState::new(&globals, &qh),
            seat_state: SeatState::new(&globals, &qh),
            keyboard: None,
            pointer: None,
            pointer_pos: (0, 0),
            cursor_shape_mgr,
            cursor_shape_device: None,
            pointer_enter_serial: 0,
            reposition_token: 0,
            current_cursor: 0,
            mods: 0,
            timer_fd: make_timer_fd("key repeat"),
            present_timer_fd: make_timer_fd("present deadline"),
            repeat_delay_ms: 400,
            repeat_rate_ms: 33,
            repeat: None,
            serial: 0,
            data_device_manager,
            data_device: None,
            clipboard_source: None,
            clipboard_text: Vec::new(),
            primary_manager,
            primary_device: None,
            primary_source: None,
            primary_text: Vec::new(),
            dnd_pos: (0, 0),
            dnd_window: 0,
            pending_drop: None,
            drop_text: Vec::new(),
            text_input_mgr,
            text_input: None,
            text_input_enabled: false,
            pending_preedit: String::new(),
            pending_commit: String::new(),
            preedit_text: Vec::new(),
            events: VecDeque::new(),
            compositor,
            xdg_shell,
            shm,
            windows: HashMap::new(),
            surface_to_id: HashMap::new(),
            focused_window: None,
            next_id: 1,
            default_scale120: 120,
            fractional_mgr,
            viewporter,
            subcompositor,
        };

        // Roundtrip so seats/outputs are known.
        event_queue.roundtrip(&mut state).map_err(|e| format!("roundtrip: {e}"))?;
        event_queue.roundtrip(&mut state).ok();

        if let Some(mgr) = state.data_device_manager.as_ref() {
            if let Some(seat) = state.seat_state.seats().next() {
                state.data_device = Some(mgr.get_data_device(&qh, &seat));
            }
        }
        if let Some(mgr) = state.primary_manager.as_ref() {
            if let Some(seat) = state.seat_state.seats().next() {
                state.primary_device = Some(mgr.get_selection_device(&qh, &seat));
            }
        }
        // Create the per-seat text-input object so the compositor can route IME
        // events to us.  We only enable() it once a wlshm surface gains keyboard
        // focus (see KeyboardHandler::enter), per the text-input-v3 lifecycle.
        if let Some(mgr) = state.text_input_mgr.as_ref() {
            if let Some(seat) = state.seat_state.seats().next() {
                state.text_input = Some(mgr.get_text_input(&seat, &qh, ()));
            }
        }

        // Integer-fallback default scale: the largest wl_output scale (only
        // used when the fractional-scale protocol is unavailable; with it the
        // per-surface preferred_scale event drives the scale instead).
        for output in state.output_state.outputs() {
            if let Some(info) = state.output_state.info(&output) {
                let s120 = (info.scale_factor.max(1) as u32) * 120;
                if s120 > state.default_scale120 {
                    state.default_scale120 = s120;
                }
            }
        }

        Ok(Backend { conn, qh, event_queue, state })
    }

    /// Create a new window and return its handle (0 on failure).
    /// `kind`: 0=Toplevel, 1=Popup (menu, grabs), 2=Tooltip (passive).
    /// Popups/tooltips with a parent are deferred: the real xdg_popup is built
    /// on the first `set_geometry` (when anchor + size are known); see
    /// `make_popup`.  A popup without a parent falls back to a toplevel.
    fn open_window(&mut self, title: &str, parent: u64, kind: u32) -> u64 {
        let pool = match SlotPool::new(800 * 600 * 4, &self.state.shm) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("wlshm: slot pool: {e}");
                return 0;
            }
        };
        let id = self.state.next_id;
        self.state.next_id += 1;
        // Provisional scale until the surface is configured.  Popups/tooltips
        // inherit their parent's scale and get a wp_viewport when promoted to an
        // xdg_popup (see make_popup); toplevels (Emacs frames) get the HiDPI
        // treatment below.
        let scale120 = 120;

        if (kind == 1 || kind == 2) && parent != 0 {
            // Defer: become an xdg_popup at set_geometry time.
            self.state.windows.insert(id, WlWindow {
                role: Role::Pending { parent, grab: kind == 1 },
                pool,
                size: (1, 1),
                scale120,
                fractional: None,
                viewport: None,
                pending_resize: None,
                configured: false,
                frame_pending: false,
                pending: None,
                last_commit: None,
                parent_dirty: false,
            });
            return id;
        }

        // Child frame (parent-frame): a wl_subsurface floating over its parent
        // instead of a separate toplevel the WM tiles.
        if kind == 3 && parent != 0 {
            let parent_scale = self.state.windows.get(&parent).map(|p| p.scale120).unwrap_or(120);
            let parent_surface = self.state.windows.get(&parent)
                .and_then(|p| p.wl_surface().cloned());
            if let (Some(subc), Some(parent_surface)) =
                (self.state.subcompositor.clone(), parent_surface)
            {
                let surface = self.state.compositor.create_surface(&self.qh);
                let subsurface = subc.get_subsurface(&surface, &parent_surface, &self.qh, ());
                // Desync: the child's own buffer commits apply immediately,
                // independent of the parent's commit cycle.
                subsurface.set_desync();
                // HiDPI: same fractional-scale + viewport treatment as a toplevel
                // so the child renders crisp at the parent's scale.
                let fractional = self.state.fractional_mgr.as_ref()
                    .map(|mgr| mgr.get_fractional_scale(&surface, &self.qh, id));
                let viewport = self.state.viewporter.as_ref()
                    .map(|vp| vp.get_viewport(&surface, &self.qh, ()));
                if fractional.is_none() {
                    surface.set_buffer_scale((parent_scale / 120) as i32);
                }
                self.state.surface_to_id.insert(surface.id(), id);
                self.state.windows.insert(id, WlWindow {
                    role: Role::Subsurface { surface, subsurface, parent_surface },
                    pool,
                    size: (1, 1),
                    scale120: parent_scale,
                    fractional,
                    viewport,
                    pending_resize: None,
                    // Subsurfaces get no xdg configure; ready to present at the
                    // size Emacs assigns via wlshm_window_set_size.
                    configured: true,
                    frame_pending: false,
                    pending: None,
                    last_commit: None,
                    // First present must commit the parent to map the subsurface.
                    parent_dirty: true,
                });
                return id;
            }
            eprintln!("wlshm: no wl_subcompositor; child frame falls back to a toplevel");
        }

        // Toplevel (or a parentless popup falling back to one).
        if kind != 0 && kind != 3 {
            eprintln!("wlshm: popup kind {kind} has no parent; using a toplevel");
        }
        let surface = self.state.compositor.create_surface(&self.qh);
        let window = self.state.xdg_shell.create_window(
            surface, WindowDecorations::RequestServer, &self.qh);
        window.set_title(title);
        window.set_app_id("org.gnu.emacs.wlshm");
        window.set_min_size(Some((160, 120)));

        // HiDPI fractional scaling: attach a wp_fractional_scale_v1 (delivers
        // preferred_scale = scale*120) and a wp_viewport (maps the physical
        // buffer down to the logical surface) to the toplevel's surface.  Both
        // are created BEFORE the first commit so the compositor can send the
        // initial preferred_scale.  Absent globals -> integer fallback.
        let wl_surf = window.wl_surface().clone();
        let fractional = self.state.fractional_mgr.as_ref().map(|mgr| {
            mgr.get_fractional_scale(&wl_surf, &self.qh, id)
        });
        let viewport = self.state.viewporter.as_ref().map(|vp| {
            vp.get_viewport(&wl_surf, &self.qh, ())
        });
        // Default scale: with the fractional path, start at 1.0 and let the
        // preferred_scale event correct it; with the integer fallback, use the
        // wl_output scale up front.
        let scale120 = if fractional.is_some() {
            120
        } else {
            self.state.default_scale120.max(120)
        };
        // Integer fallback only: set the buffer scale on the surface so the
        // compositor knows the buffer is at N* physical density.  The fractional
        // path leaves buffer_scale at 1 and relies on the viewport instead.
        if fractional.is_none() {
            wl_surf.set_buffer_scale((scale120 / 120) as i32);
        }
        window.commit();

        let sid = window.wl_surface().id();
        self.state.surface_to_id.insert(sid, id);
        self.state.windows.insert(id, WlWindow {
            role: Role::Toplevel(window),
            pool,
            size: (800, 600),
            scale120,
            fractional,
            viewport,
            pending_resize: None,
            configured: false,
            frame_pending: false,
            pending: None,
            last_commit: None,
            parent_dirty: false,
        });

        // Roundtrip so the compositor sends the initial configure (sizes us).
        let _ = self.event_queue.roundtrip(&mut self.state);
        id
    }

    /// Build an xdg_positioner anchored at (x,y) in the parent's window
    /// geometry, sized (w,h).  Shared by popup creation and reposition so the
    /// placement rules stay identical.
    fn make_positioner(&self, x: i32, y: i32, w: i32, h: i32) -> Option<XdgPositioner> {
        let positioner = match XdgPositioner::new(&self.state.xdg_shell) {
            Ok(p) => p,
            Err(e) => { eprintln!("wlshm: positioner: {e}"); return None; }
        };
        positioner.set_size(w.max(1), h.max(1));
        // 1x1 anchor rect at the requested point; popup's top-left lands there.
        positioner.set_anchor_rect(x, y, 1, 1);
        positioner.set_anchor(Anchor::TopLeft);
        positioner.set_gravity(Gravity::BottomRight);
        // Keep it on-screen: slide, then flip, then resize as needed.
        positioner.set_constraint_adjustment(
            ConstraintAdjustment::SlideX
                | ConstraintAdjustment::SlideY
                | ConstraintAdjustment::FlipY
                | ConstraintAdjustment::ResizeX
                | ConstraintAdjustment::ResizeY,
        );
        Some(positioner)
    }

    /// Reposition an already-mapped xdg_popup (e.g. a reused tooltip moving with
    /// the pointer) via xdg_popup.reposition.  No-op for non-popups.  Returns
    /// true if a reposition request was sent.
    fn reposition_popup(&mut self, id: u64, x: i32, y: i32, w: i32, h: i32) -> bool {
        let Some(popup) = self.state.windows.get(&id).and_then(|w| match &w.role {
            Role::Popup(p) => Some(p.clone()),
            _ => None,
        }) else {
            return false;
        };
        let Some(positioner) = self.make_positioner(x, y, w, h) else { return false; };
        self.state.reposition_token = self.state.reposition_token.wrapping_add(1);
        popup.reposition(&positioner, self.state.reposition_token);
        if let Some(wl) = self.state.windows.get_mut(&id) {
            wl.size = (w.max(1) as u32, h.max(1) as u32);
            if let Some(vp) = wl.viewport.as_ref() {
                vp.set_destination(w.max(1), h.max(1));
            }
        }
        // Pump the reposition/configure round-trip so the new geometry is in
        // effect before the C side presents the next buffer.
        let _ = self.event_queue.roundtrip(&mut self.state);
        true
    }

    /// Turn a Pending window into a real xdg_popup anchored at (x,y) in the
    /// parent's geometry with content size (w,h).  Returns true on success.
    fn make_popup(&mut self, id: u64, x: i32, y: i32, w: i32, h: i32) -> bool {
        let (parent_id, grab) = match self.state.windows.get(&id).map(|w| &w.role) {
            Some(Role::Pending { parent, grab }) => (*parent, *grab),
            _ => return false,
        };
        // Parent must be a live toplevel xdg_surface.
        let parent_xdg = match self.state.windows.get(&parent_id).map(|w| &w.role) {
            Some(Role::Toplevel(pw)) => pw.xdg_surface().clone(),
            _ => {
                eprintln!("wlshm: popup parent {parent_id} not a live toplevel");
                return false;
            }
        };
        let positioner = match self.make_positioner(x, y, w, h) {
            Some(p) => p,
            None => return false,
        };
        let popup = match Popup::new(
            &parent_xdg, &positioner, &self.qh,
            &self.state.compositor, &self.state.xdg_shell,
        ) {
            Ok(p) => p,
            Err(e) => { eprintln!("wlshm: popup create: {e}"); return false; }
        };
        // Menus take a keyboard grab so the compositor routes keys to the popup
        // and reports click-outside as popup_done (dismiss).  Tooltips don't.
        if grab {
            if let Some(seat) = self.state.seat_state.seats().next() {
                popup.xdg_popup().grab(&seat, self.state.serial);
            }
        }
        let sid = popup.wl_surface().id();
        self.state.surface_to_id.insert(sid, id);
        // HiDPI: a popup lives on the same output as its parent, so it shares the
        // parent's scale.  Inherit it directly (the parent's scale has already
        // settled) and give the popup a wp_viewport that maps its physical-pixel
        // buffer down to the LOGICAL geometry the positioner uses.  The C side
        // reads wlshm_window_scale120(pw) and renders the menu / tip canvas at
        // physical resolution; without this the compositor upscales a logical
        // buffer and popups look soft at fractional scale.
        let parent_scale = self.state.windows.get(&parent_id)
            .map(|p| p.scale120).unwrap_or(120).max(120);
        let popup_vp = self.state.viewporter.as_ref()
            .map(|vp| vp.get_viewport(popup.wl_surface(), &self.qh, ()));
        if let Some(vp) = popup_vp.as_ref() {
            vp.set_destination(w.max(1), h.max(1));
        }
        if let Some(wl) = self.state.windows.get_mut(&id) {
            wl.role = Role::Popup(popup);
            wl.size = (w.max(1) as u32, h.max(1) as u32);
            wl.scale120 = parent_scale;
            wl.viewport = popup_vp;
            wl.configured = false;
        }
        // Pump the initial popup configure so the surface is configured before
        // the C side presents a buffer (committing a buffer first is an error).
        let _ = self.event_queue.roundtrip(&mut self.state);
        let _ = self.event_queue.roundtrip(&mut self.state);
        true
    }

    fn close_window(&mut self, win: u64) {
        if let Some(w) = self.state.windows.remove(&win) {
            if let Some(s) = w.wl_surface() {
                self.state.surface_to_id.remove(&s.id());
            }
            if self.state.focused_window == Some(win) {
                self.state.focused_window = None;
            }
            // Drop a held key-repeat aimed at this window so its timer can't
            // replay the key into an arbitrary surviving frame.
            if matches!(self.state.repeat, Some((_, _, _, w)) if w == win) {
                self.state.disarm_repeat();
            }
            // A subsurface and its plain wl_surface have no Drop destructor
            // (unlike sctk's Window/Popup), so destroy them explicitly and
            // commit the parent to unmap the child; otherwise it leaks and stays
            // visible after delete-frame.
            if let Role::Subsurface { surface, subsurface, parent_surface } = &w.role {
                subsurface.destroy();
                surface.destroy();
                parent_surface.commit();
                let _ = self.conn.flush();
            }
            // wp_viewport / wp_fractional_scale_v1 are raw wayland-client proxies
            // whose explicit destroy request Drop does NOT send (same reason the
            // subsurface/surface are destroyed above), so release them here or
            // every closed window leaks a server object.
            if let Some(vp) = &w.viewport {
                vp.destroy();
            }
            if let Some(fs) = &w.fractional {
                fs.destroy();
            }
            // w drops here: destroys the xdg surface (safe while connected).
        }
    }

    fn dispatch(&mut self) -> Result<(), String> {
        use std::os::unix::io::AsRawFd;
        let _ = self.conn.flush();
        self.event_queue
            .dispatch_pending(&mut self.state)
            .map_err(|e| format!("dispatch: {e}"))?;
        if let Some(guard) = self.conn.prepare_read() {
            let fd = self.conn.backend().poll_fd().as_raw_fd();
            let mut pfd = libc::pollfd { fd, events: libc::POLLIN, revents: 0 };
            let ready = unsafe { libc::poll(&mut pfd, 1, 0) } > 0
                && (pfd.revents & libc::POLLIN) != 0;
            if ready {
                match guard.read() {
                    Ok(_) => {}
                    // The socket can be drained between poll and read; that's
                    // benign.  A real error (disconnect / protocol) must
                    // propagate so the C side tears the connection down instead
                    // of spinning on a dead-but-readable fd.
                    Err(WaylandError::Io(e))
                        if e.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(e) => return Err(format!("wayland read: {e}")),
                }
                self.event_queue
                    .dispatch_pending(&mut self.state)
                    .map_err(|e| format!("dispatch: {e}"))?;
            }
        }
        self.state.pump_repeat();
        self.drain_pending_drop();
        self.flush_overdue_pending();
        Ok(())
    }

    /// Commit any frame coalesced into `pending` whose present deadline has
    /// passed (the compositor never delivered the frame callback that would
    /// normally drain it).  Runs on every dispatch; the present-deadline timerfd
    /// waking the event loop is what gets us here when nothing else happens.
    /// Mirrors the frame() callback's commit path.
    fn flush_overdue_pending(&mut self) {
        self.state.drain_present_timer();
        let ids: Vec<u64> = self
            .state
            .windows
            .iter()
            .filter(|(_, w)| {
                w.pending.is_some()
                    && w.last_commit.map_or(true, |t| t.elapsed() >= PRESENT_DEADLINE)
            })
            .map(|(id, _)| *id)
            .collect();
        if ids.is_empty() {
            return;
        }
        for id in ids {
            let pending = self.state.windows.get_mut(&id).and_then(|w| w.pending.take());
            let Some(pending) = pending else { continue };
            // Skip an unconfigured xdg surface: it was unmapped/hidden after this
            // frame was coalesced, so attaching it now is xdg_surface error 3
            // (unconfigured_buffer) and the compositor disconnects us.  A
            // subsurface has no unconfigured state, so it is always eligible.
            // Dropping the taken `pending` forgets the stale pixels; the next
            // present re-attaches once a fresh configure lands.
            let eligible = self.state.windows.get(&id).map_or(false, |w| {
                w.configured || matches!(w.role, Role::Subsurface { .. })
            });
            if !eligible {
                continue;
            }
            let Some(w) = self.state.windows.get_mut(&id) else { continue };
            let Some(surf) = w.wl_surface().cloned() else { continue };
            let Some(buffer) = create_buffer_from_pending(&mut w.pool, &pending) else {
                continue;
            };
            set_viewport_source(w, pending.width, pending.height);
            if commit_buffer(&self.qh, &surf, &buffer,
                             pending.dx, pending.dy, pending.dw, pending.dh) {
                if !matches!(w.role, Role::Subsurface { .. }) {
                    w.frame_pending = true;
                }
                w.last_commit = Some(Instant::now());
            }
        }
        let _ = self.conn.flush();
    }

    /// If a drop landed (set in `drop_performed`), read its data from the
    /// receive pipe and push a Drop event.  Done here (not in the AppState
    /// callback) because reading needs a flush + roundtrip so the drag source
    /// writes the bytes, and the event queue lives on `Backend`.
    fn drain_pending_drop(&mut self) {
        let Some((pipe, is_uri, x, y, win, offer)) = self.state.pending_drop.take() else { return };
        let _ = self.conn.flush();
        let _ = self.event_queue.roundtrip(&mut self.state);
        let bytes = read_pipe_timeout(&pipe, 500);
        // The transfer is fully read; only now signal completion to the source.
        offer.finish();
        if bytes.is_empty() {
            return;
        }
        self.state.drop_text = bytes;
        self.state.push(WlshmEvent::drop(x, y, is_uri).on(win));
    }

    /// Resolve a handle (0 = primary) to a live window id.
    fn resolve(&self, win: u64) -> Option<u64> {
        if win != 0 && self.state.windows.contains_key(&win) {
            Some(win)
        } else if win == 0 {
            let p = self.state.primary();
            (p != 0).then_some(p)
        } else {
            None
        }
    }

    /// Copy the C-side canvas pixels of window `win` into a free wl_shm buffer
    /// and commit it.  `src` is XRGB8888 (== Cairo CAIRO_FORMAT_RGB24).
    ///
    /// # Safety
    /// `src` must point to at least `height * src_stride` readable bytes.
    unsafe fn present(
        &mut self,
        win: u64,
        src: *const u8,
        src_w: u32,
        src_h: u32,
        src_stride: u32,
        dmg_x: i32,
        dmg_y: i32,
        dmg_w: i32,
        dmg_h: i32,
    ) {
        let Some(id) = self.resolve(win) else { return };
        if src.is_null() {
            return;
        }
        // The window may have been torn down between resolve() and this lookup
        // (a close racing a present, e.g. via the menu modal loop or tooltip
        // force-present paths), so guard instead of unwrapping.
        let (configured, is_sub) = match self.state.windows.get(&id) {
            Some(w) => (w.configured, matches!(w.role, Role::Subsurface { .. })),
            None => return,
        };
        // A subsurface is not an xdg surface: it has no unconfigured state and
        // never receives an xdg configure, so the recovery dance below would
        // bail forever (it stays !configured after a hide -- corfu/child-frame
        // popups went permanently invisible after the first hide/show).  Skip it
        // for subsurfaces; they re-attach a buffer directly on the next present.
        if !configured && !is_sub {
            // The surface is in the unconfigured xdg_surface state: it has
            // either never been configured, or it was unmapped (wlshm_window_unmap
            // commits a NULL buffer, which per xdg-shell returns the surface to
            // the initial unconfigured state -- e.g. a child frame hidden and
            // re-shown between completions).  Attaching a buffer now is
            // xdg_surface error 3 (unconfigured_buffer) and the compositor kills
            // us.  Re-enter the configure sequence: a buffer-less commit makes
            // the compositor send a fresh configure (sctk acks it), flipping
            // `configured` back to true.  Pending popups have no surface yet and
            // are configured up front via make_popup, so they short-circuit here.
            let Some(surface) = self.state.windows.get(&id).and_then(|w| w.wl_surface().cloned())
            else {
                return;
            };
            surface.commit();
            let _ = self.conn.flush();
            let _ = self.event_queue.roundtrip(&mut self.state);
            let _ = self.event_queue.roundtrip(&mut self.state);
            // Still unconfigured?  Bail; the frame is garbaged, so the next
            // redisplay presents again and retries.
            if !self.state.windows.get(&id).map(|w| w.configured).unwrap_or(false) {
                return;
            }
        }
        let Some(w) = self.state.windows.get_mut(&id) else { return };
        // Size the buffer and copy from the SOURCE dimensions, NOT the window's
        // compositor-driven size.  The source (an Emacs frame canvas or a
        // fixed-size menu/tooltip surface) can differ from w.size: the
        // compositor may resize the window (e.g. a tiling WM tiling a popup, or
        // a frame resize before the canvas is rebuilt).  Using w.size here read
        // past the end of `src` and crashed (SIGSEGV in memmove).
        let (pw, ph) = (src_w, src_h);
        if pw == 0 || ph == 0 {
            return;
        }
        // Resolve damage to a concrete rect (full buffer when none was given).
        let (dx, dy, dw, dh) = if dmg_w > 0 && dmg_h > 0 {
            (dmg_x, dmg_y, dmg_w, dmg_h)
        } else {
            (0, 0, pw as i32, ph as i32)
        };
        // Flow control: at most one buffer in flight per frame callback.  If a
        // frame callback is still outstanding, stash a client-side snapshot as
        // the pending frame and return -- it is copied into a wl_shm buffer when
        // the callback fires.  Otherwise commit now and arm the next callback.
        // This throttles a fast redisplay loop (nonstop scroll) to the
        // compositor's frame rate so we never flood it with buffer commits or
        // create/destroy uncommitted wl_buffer objects.
        // Subsurfaces (child frames) bypass the frame-callback throttle: they
        // don't rapid-update, and an unmapped subsurface may never get a frame
        // callback -- which would stick frame_pending and freeze the child at
        // its provisional size (it would never re-present at its real size).
        let is_sub = matches!(w.role, Role::Subsurface { .. });
        // Throttle while a frame callback is genuinely in flight -- UNLESS it is
        // overdue past PRESENT_DEADLINE.  Some compositors withhold the
        // wl_surface.frame callback when the output is otherwise idle (it only
        // repaints, and thus flushes callbacks, on cursor motion etc.), which
        // would strand the latest buffer in `pending` and leave stale pixels on
        // screen (mode line not redrawing until you move the mouse).  Committing
        // anyway after the deadline unsticks it while staying bounded to ~1
        // commit per deadline, so it still cannot flood the compositor.
        let stalled = w
            .last_commit
            .map_or(false, |t| t.elapsed() >= PRESENT_DEADLINE);
        if w.frame_pending && !is_sub && !stalled {
            match w.pending.as_mut() {
                Some(pending) => {
                    pending.replace_from_source(src, pw, ph, src_stride, dx, dy, dw, dh);
                }
                None => {
                    w.pending =
                        Some(PendingFrame::new_from_source(
                            src, pw, ph, src_stride, dx, dy, dw, dh));
                }
            }
            // Wake the event loop after the deadline so this coalesced frame is
            // delivered even if no callback / no other event ever arrives.
            self.state.arm_present_timer();
            return;
        }
        // We are about to commit the CURRENT (newest) buffer.  Drop any older
        // coalesced buffer so frame() can't later commit it on top of this one
        // (which would flash stale content -- possible now that the deadline
        // lets us commit while frame_pending is still set).
        w.pending = None;
        let Some(buffer) = create_buffer_from_source(&mut w.pool, src, pw, ph, src_stride) else {
            return;
        };
        // Set the viewport SOURCE to the crisp logical*scale crop, CLAMPED to
        // this buffer (pw x ph) so it can never exceed the content area (a
        // wp_viewport out_of_buffer protocol error).  Applied on the commit
        // below, so source and buffer always agree.
        set_viewport_source(w, pw, ph);
        let Some(surface) = w.wl_surface().cloned() else { return };
        let parent_surface = if let Role::Subsurface { parent_surface, .. } = &w.role {
            Some(parent_surface.clone())
        } else {
            None
        };
        let parent_dirty = w.parent_dirty;
        if commit_buffer(&self.qh, &surface, &buffer, dx, dy, dw, dh) {
            if !is_sub {
                w.frame_pending = true;
            }
            w.last_commit = Some(Instant::now());
            // A subsurface's placement/mapping is applied on the PARENT's commit.
            // Only nudge the parent when the placement actually changed (first
            // map / remap, flagged by parent_dirty); a steady-state buffer update
            // on a desync subsurface (corfu scrolling the selection) applies via
            // the child's own commit above, so committing the parent every frame
            // is a redundant parent repaint.  Moves go through
            // wlshm_window_set_subsurface_pos, which commits the parent itself.
            if let Some(ps) = parent_surface {
                if parent_dirty {
                    ps.commit();
                    w.parent_dirty = false;
                }
            }
        }
        let _ = self.conn.flush();
    }

    fn set_clipboard(&mut self, text: &[u8]) -> bool {
        let Some(mgr) = self.state.data_device_manager.as_ref() else { return false };
        let Some(dd) = self.state.data_device.as_ref() else { return false };
        let source = mgr.create_copy_paste_source(&self.qh, CLIPBOARD_MIME.iter().copied());
        source.set_selection(dd, self.state.serial);
        self.state.clipboard_text = text.to_vec();
        self.state.clipboard_source = Some(source);
        let _ = self.conn.flush();
        true
    }

    fn disown_clipboard(&mut self) {
        self.state.clipboard_source = None;
        self.state.clipboard_text.clear();
    }

    fn get_clipboard(&mut self) -> Option<Vec<u8>> {
        if self.state.clipboard_source.is_some() {
            return Some(self.state.clipboard_text.clone());
        }
        let offer = self.state.data_device.as_ref()?.data().selection_offer()?;
        let mime = offer.with_mime_types(|types| {
            CLIPBOARD_MIME
                .iter()
                .find(|m| types.iter().any(|t| t == *m))
                .map(|m| m.to_string())
        })?;
        let pipe = offer.receive(mime).ok()?;
        let _ = self.conn.flush();
        let _ = self.event_queue.roundtrip(&mut self.state);
        Some(read_pipe_timeout(&pipe, 500))
    }

    // --- PRIMARY selection: parallel to the clipboard methods above. ---

    fn set_primary(&mut self, text: &[u8]) -> bool {
        let Some(mgr) = self.state.primary_manager.as_ref() else { return false };
        let Some(dev) = self.state.primary_device.as_ref() else { return false };
        let source = mgr.create_selection_source(&self.qh, CLIPBOARD_MIME.iter().copied());
        source.set_selection(dev, self.state.serial);
        self.state.primary_text = text.to_vec();
        self.state.primary_source = Some(source);
        let _ = self.conn.flush();
        true
    }

    fn disown_primary(&mut self) {
        self.state.primary_source = None;
        self.state.primary_text.clear();
    }

    fn get_primary(&mut self) -> Option<Vec<u8>> {
        if self.state.primary_source.is_some() {
            return Some(self.state.primary_text.clone());
        }
        let offer = self.state.primary_device.as_ref()?.data().selection_offer()?;
        let mime = offer.with_mime_types(|types| {
            CLIPBOARD_MIME
                .iter()
                .find(|m| types.iter().any(|t| t == *m))
                .map(|m| m.to_string())
        })?;
        let pipe = offer.receive(mime).ok()?;
        let _ = self.conn.flush();
        let _ = self.event_queue.roundtrip(&mut self.state);
        Some(read_pipe_timeout(&pipe, 500))
    }
}

/// Read all data from a clipboard pipe, non-blocking with a total timeout so a
/// misbehaving source can never hang Emacs's main loop.
fn read_pipe_timeout(pipe: &ReadPipe, timeout_ms: u64) -> Vec<u8> {
    use std::os::unix::io::AsRawFd;
    use std::time::{Duration, Instant};

    let fd = pipe.as_raw_fd();
    unsafe {
        let flags = libc::fcntl(fd, libc::F_GETFL);
        if flags >= 0 {
            libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
        }
    }
    let mut out = Vec::new();
    let mut tmp = [0u8; 8192];
    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            break;
        }
        let mut pfd = libc::pollfd { fd, events: libc::POLLIN, revents: 0 };
        let pr = unsafe {
            libc::poll(&mut pfd, 1, remaining.as_millis().min(i32::MAX as u128) as i32)
        };
        if pr <= 0 {
            break;
        }
        let n = unsafe { libc::read(fd, tmp.as_mut_ptr() as *mut libc::c_void, tmp.len()) };
        if n == 0 {
            break;
        } else if n > 0 {
            out.extend_from_slice(&tmp[..n as usize]);
        } else {
            let e = std::io::Error::last_os_error();
            match e.raw_os_error() {
                Some(libc::EAGAIN) | Some(libc::EINTR) => continue,
                _ => break,
            }
        }
    }
    out
}

/// Write all of `data` to a selection pipe, non-blocking with a total timeout so
/// a slow or stuck reader can never hang Emacs's main loop.  This is the write
/// counterpart to read_pipe_timeout: send_request runs synchronously on the main
/// thread inside the event-queue dispatch, and a POSIX pipe has a bounded kernel
/// buffer, so a blocking write_all of a large selection would otherwise freeze
/// the editor until the peer drains (or never).
fn write_pipe_timeout(pipe: &WritePipe, data: &[u8], timeout_ms: u64) {
    use std::os::unix::io::AsRawFd;
    use std::time::{Duration, Instant};

    let fd = pipe.as_raw_fd();
    unsafe {
        let flags = libc::fcntl(fd, libc::F_GETFL);
        if flags >= 0 {
            libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
        }
    }
    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    let mut off = 0usize;
    while off < data.len() {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            break;
        }
        let mut pfd = libc::pollfd { fd, events: libc::POLLOUT, revents: 0 };
        let pr = unsafe {
            libc::poll(&mut pfd, 1, remaining.as_millis().min(i32::MAX as u128) as i32)
        };
        if pr <= 0 {
            break;
        }
        // Reader closed its end: stop before writing to avoid EPIPE/SIGPIPE.
        if pfd.revents & (libc::POLLERR | libc::POLLHUP) != 0 {
            break;
        }
        let n = unsafe {
            libc::write(fd, data[off..].as_ptr() as *const libc::c_void, data.len() - off)
        };
        if n > 0 {
            off += n as usize;
        } else if n < 0 {
            let e = std::io::Error::last_os_error();
            match e.raw_os_error() {
                Some(libc::EAGAIN) | Some(libc::EINTR) => continue,
                _ => break,
            }
        } else {
            break;
        }
    }
}

// ----- sctk handlers -------------------------------------------------------

impl CompositorHandler for AppState {
    fn scale_factor_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, s: &wl_surface::WlSurface, new: i32) {
        if new < 1 {
            return;
        }
        let id = self.id_for_surface(s);
        let Some(w) = self.windows.get_mut(&id) else { return };
        // A subsurface child frame is Emacs-driven and inherits its scale at
        // creation; it has no xdg configure handshake, so pushing a Configure
        // here would garbage the frame and risk a wp_viewport buffer/scale
        // divergence (see update_viewport).  Leave its scale as set at creation.
        if matches!(w.role, Role::Subsurface { .. }) {
            return;
        }
        // Integer fallback only: when the fractional-scale protocol is active
        // (per-surface wp_fractional_scale_v1), the preferred_scale event is
        // authoritative and we ignore the wl_output integer scale here.
        if w.fractional.is_some() {
            return;
        }
        let new120 = (new as u32) * 120;
        if w.scale120 == new120 {
            return;
        }
        w.scale120 = new120;
        // Tell the compositor the buffer is at N* density (classic HiDPI), then
        // recreate the canvas via a same-size Configure (see the fractional
        // handler for the rationale).
        if let Some(surface) = w.wl_surface() {
            surface.set_buffer_scale(new);
        }
        let (lw, lh) = w.size;
        self.push(WlshmEvent::configure(lw as i32, lh as i32).on(id));
    }
    fn transform_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: wl_output::Transform) {}
    fn frame(&mut self, conn: &Connection, qh: &QueueHandle<Self>, surface: &wl_surface::WlSurface, _: u32) {
        // The compositor is ready for the next buffer.  Clear the throttle and,
        // if a present was coalesced while we waited, commit the latest now.
        let id = self.id_for_surface(surface);
        let pending = match self.windows.get_mut(&id) {
            Some(w) => {
                w.frame_pending = false;
                w.pending.take()
            }
            None => return,
        };
        if let Some(pending) = pending {
            // Don't commit onto an unconfigured xdg surface: it was unmapped
            // while this frame callback was still outstanding, and attaching a
            // buffer to a surface in the initial unconfigured state is
            // xdg_surface error 3 (the compositor disconnects us).  Subsurfaces
            // have no unconfigured state, so they stay eligible.  The taken
            // snapshot is dropped; the next present
            // re-attaches after a fresh configure.
            let eligible = self.windows.get(&id).map_or(false, |w| {
                w.configured || matches!(w.role, Role::Subsurface { .. })
            });
            if !eligible {
                return;
            }
            if let Some(w) = self.windows.get_mut(&id) {
                let Some(surf) = w.wl_surface().cloned() else { return };
                let Some(buffer) = create_buffer_from_pending(&mut w.pool, &pending) else {
                    return;
                };
                set_viewport_source(w, pending.width, pending.height);
                if commit_buffer(qh, &surf, &buffer,
                                 pending.dx, pending.dy, pending.dw, pending.dh) {
                    w.frame_pending = true;
                    w.last_commit = Some(Instant::now());
                    let _ = conn.flush();
                }
            }
        }
    }
    fn surface_enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
    fn surface_leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
}

impl ShmHandler for AppState {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

impl WindowHandler for AppState {
    fn request_close(&mut self, _: &Connection, _: &QueueHandle<Self>, window: &Window) {
        let id = self.id_for_surface(window.wl_surface());
        self.push(WlshmEvent::close().on(id));
    }
    fn configure(&mut self, _: &Connection, _: &QueueHandle<Self>, window: &Window, configure: WindowConfigure, _: u32) {
        let id = self.id_for_surface(window.wl_surface());
        if let Some(w) = self.windows.get_mut(&id) {
            if let (Some(cw), Some(ch)) = configure.new_size {
                w.size = (cw.get(), ch.get());
            }
            w.configured = true;
            // A fresh configure invites a new buffer; clear any stuck throttle
            // (e.g. left over from an unmap that got no frame callback) so the
            // next present commits immediately.
            w.frame_pending = false;
            w.pending = None;
            w.pending_resize = Some(w.size);
            let (sw, sh) = w.size;
            // Map the physical buffer 1:1 to the output (source = logical*scale,
            // destination = logical) so the compositor doesn't resample.
            w.update_viewport();
            self.push(WlshmEvent::configure(sw as i32, sh as i32).on(id));
        }
    }
}

impl PopupHandler for AppState {
    fn configure(&mut self, _: &Connection, _: &QueueHandle<Self>, popup: &Popup, config: PopupConfigure) {
        let id = self.id_for_surface(popup.wl_surface());
        if let Some(w) = self.windows.get_mut(&id) {
            if config.width > 0 && config.height > 0 {
                w.size = (config.width as u32, config.height as u32);
            }
            // Keep the viewport mapping the physical buffer 1:1 to the LOGICAL
            // geometry the compositor just assigned (HiDPI; no-op at scale 1).
            w.update_viewport();
            w.configured = true;
            w.frame_pending = false;
            w.pending = None;
        }
    }
    fn done(&mut self, _: &Connection, _: &QueueHandle<Self>, popup: &Popup) {
        // Compositor dismissed the popup (e.g. click-outside under a grab):
        // tell the C side so the menu modal loop / tooltip tears down.
        let id = self.id_for_surface(popup.wl_surface());
        self.push(WlshmEvent::close().on(id));
    }
}

impl OutputHandler for AppState {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl SeatHandler for AppState {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seat_state
    }
    fn new_seat(&mut self, _: &Connection, qh: &QueueHandle<Self>, seat: WlSeat) {
        if self.data_device.is_none() {
            if let Some(mgr) = self.data_device_manager.as_ref() {
                self.data_device = Some(mgr.get_data_device(qh, &seat));
            }
        }
        if self.primary_device.is_none() {
            if let Some(mgr) = self.primary_manager.as_ref() {
                self.primary_device = Some(mgr.get_selection_device(qh, &seat));
            }
        }
    }
    fn new_capability(&mut self, _: &Connection, qh: &QueueHandle<Self>, seat: WlSeat, capability: Capability) {
        if capability == Capability::Keyboard && self.keyboard.is_none() {
            match self.seat_state.get_keyboard(qh, &seat, None) {
                Ok(kbd) => self.keyboard = Some(kbd),
                Err(e) => eprintln!("wlshm: get_keyboard failed: {e}"),
            }
        }
        if capability == Capability::Pointer && self.pointer.is_none() {
            match self.seat_state.get_pointer(qh, &seat) {
                Ok(ptr) => {
                    if let Some(mgr) = self.cursor_shape_mgr.as_ref() {
                        self.cursor_shape_device = Some(mgr.get_shape_device(&ptr, qh));
                    }
                    self.pointer = Some(ptr);
                }
                Err(e) => eprintln!("wlshm: get_pointer failed: {e}"),
            }
        }
    }
    fn remove_capability(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat, capability: Capability) {
        if capability == Capability::Keyboard {
            if let Some(kbd) = self.keyboard.take() {
                kbd.release();
            }
        }
        if capability == Capability::Pointer {
            if let Some(ptr) = self.pointer.take() {
                ptr.release();
            }
        }
    }
    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
}

impl KeyboardHandler for AppState {
    fn enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             surface: &wl_surface::WlSurface, serial: u32, _: &[u32], _: &[Keysym]) {
        self.serial = serial;
        let id = self.id_for_surface(surface);
        self.focused_window = Some(id);
        self.push(WlshmEvent::focus(true).on(id));
        // Activate the IME for this surface (text-input-v3 enable+commit).
        self.enable_text_input();
    }
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             surface: &wl_surface::WlSurface, _: u32) {
        self.disarm_repeat();
        // Deactivate the IME (text-input-v3 disable+commit) before we forget focus.
        self.disable_text_input();
        let id = self.id_for_surface(surface);
        self.push(WlshmEvent::focus(false).on(id));
    }

    fn press_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                 serial: u32, event: KeyEvent) {
        self.serial = serial;
        let ks = event.keysym.raw();
        if is_modifier_keysym(ks) {
            return;
        }
        let unichar = event
            .utf8
            .as_ref()
            .and_then(|s| s.chars().next())
            .map(|c| c as u32)
            .unwrap_or(0);
        let win = self.primary();
        self.push(WlshmEvent::key(ks, unichar, self.mods).on(win));
        if self.repeat_rate_ms > 0 {
            self.repeat = Some((event.raw_code, ks, unichar, win));
            self.arm_repeat();
        }
    }

    fn release_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                   _: u32, event: KeyEvent) {
        if matches!(self.repeat, Some((rc, _, _, _)) if rc == event.raw_code) {
            self.disarm_repeat();
        }
    }

    fn update_repeat_info(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                          info: RepeatInfo) {
        match info {
            RepeatInfo::Repeat { rate, delay } => {
                self.repeat_rate_ms = (1000 / rate.get()).max(1);
                self.repeat_delay_ms = delay;
            }
            RepeatInfo::Disable => {
                self.repeat_rate_ms = 0;
                self.disarm_repeat();
            }
        }
    }

    fn update_modifiers(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                        _: u32, modifiers: Modifiers, _: u32) {
        let mut m = 0;
        if modifiers.ctrl { m |= WLSHM_MOD_CTRL; }
        if modifiers.alt { m |= WLSHM_MOD_ALT; }
        if modifiers.shift { m |= WLSHM_MOD_SHIFT; }
        if modifiers.logo { m |= WLSHM_MOD_LOGO; }
        self.mods = m;
    }
}

// --- text-input-v3 (IME) raw Dispatch impls ---
//
// sctk 0.19 doesn't wrap text-input, so we implement wayland-client's Dispatch
// for the manager (no events) and the text-input object directly.  The protocol
// is double-buffered: preedit_string/commit_string/delete_surrounding_text set
// pending state, and `done` applies it.  We accumulate into pending_preedit /
// pending_commit and flush on `done`.

impl Dispatch<ZwpTextInputManagerV3, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &ZwpTextInputManagerV3,
        _event: <ZwpTextInputManagerV3 as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // zwp_text_input_manager_v3 has no events.
    }
}

impl Dispatch<ZwpTextInputV3, ()> for AppState {
    fn event(
        state: &mut Self,
        _proxy: &ZwpTextInputV3,
        event: <ZwpTextInputV3 as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        use zwp_text_input_v3::Event;
        match event {
            // The compositor tells us which surface the IME is now bound to.
            // We already track focus via wl_keyboard enter/leave and enable on
            // that, so these are informational; keep focused_window in sync.
            Event::Enter { surface } => {
                let id = state.id_for_surface(&surface);
                state.focused_window = Some(id);
                state.enable_text_input();
            }
            Event::Leave { .. } => {
                state.disable_text_input();
            }
            // Pending preedit; applied on `done`.  Null text clears it.
            Event::PreeditString { text, .. } => {
                state.pending_preedit = text.unwrap_or_default();
            }
            // Pending committed (final) text; applied on `done`.
            Event::CommitString { text } => {
                if let Some(t) = text {
                    state.pending_commit.push_str(&t);
                }
            }
            // We don't expose surrounding text, so honour deletions as
            // backspaces over the committed buffer is not feasible here; the
            // common compose/CJK cases don't rely on it.  Ignore for now.
            Event::DeleteSurroundingText { .. } => {}
            // Apply the double-buffered pending state.
            Event::Done { .. } => {
                let win = state.primary();
                // Committed text first: deliver as ordinary KeyPress events
                // (one per codepoint, no modifiers) so it flows through the
                // tested keystroke path in wlshm_read_socket.
                if !state.pending_commit.is_empty() {
                    let committed = std::mem::take(&mut state.pending_commit);
                    for ch in committed.chars() {
                        state.push(WlshmEvent::key(0, ch as u32, 0).on(win));
                    }
                }
                // Then the new preedit (may be empty == cleared).  Hand the
                // string to C via the side-channel buffer and push a Preedit
                // event so the elisp handler redraws the composition overlay.
                let preedit = std::mem::take(&mut state.pending_preedit);
                state.preedit_text = preedit.into_bytes();
                state.push(WlshmEvent::preedit().on(win));
            }
            _ => {}
        }
    }
}

// --- HiDPI fractional scaling raw Dispatch impls ---
//
// sctk 0.19 doesn't wrap wp_fractional_scale / wp_viewporter, so we implement
// wayland-client's Dispatch directly.  The managers and viewport have no events;
// only wp_fractional_scale_v1 emits `preferred_scale` (scale*120).

impl Dispatch<WpFractionalScaleManagerV1, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &WpFractionalScaleManagerV1,
        _event: <WpFractionalScaleManagerV1 as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // wp_fractional_scale_manager_v1 has no events.
    }
}

impl Dispatch<WpViewporter, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &WpViewporter,
        _event: <WpViewporter as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // wp_viewporter has no events.
    }
}

impl Dispatch<WpViewport, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &WpViewport,
        _event: <WpViewport as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // wp_viewport has no events.
    }
}

impl Dispatch<WlSubcompositor, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &WlSubcompositor,
        _event: <WlSubcompositor as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // wl_subcompositor has no events.
    }
}

impl Dispatch<WlSubsurface, ()> for AppState {
    fn event(
        _state: &mut Self,
        _proxy: &WlSubsurface,
        _event: <WlSubsurface as Proxy>::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // wl_subsurface has no events.
    }
}

// The wp_fractional_scale_v1's user-data is the owning window handle (u64).
impl Dispatch<WpFractionalScaleV1, u64> for AppState {
    fn event(
        state: &mut Self,
        _proxy: &WpFractionalScaleV1,
        event: <WpFractionalScaleV1 as Proxy>::Event,
        data: &u64,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            let id = *data;
            let Some(w) = state.windows.get_mut(&id) else { return };
            if scale == 0 || w.scale120 == scale {
                return;
            }
            w.scale120 = scale;
            // Re-arm the viewport at the current logical size (unchanged), then
            // ask Emacs to recreate its canvas at the new physical size: a
            // Configure carrying the SAME logical size triggers
            // change_frame_size + garbage + redisplay on the C side, and the C
            // wlshm_ensure_canvas re-queries the (now larger) physical size.
            let (lw, lh) = w.size;
            // Re-arm the viewport source for the NEW scale so the buffer maps
            // 1:1 to the output (no resample) once the canvas is rebuilt.
            w.update_viewport();
            state.push(WlshmEvent::configure(lw as i32, lh as i32).on(id));
        }
    }
}

/// Map our cursor-shape codes (shared with the C side) to cursor-shape-v1
/// shapes.  0/1 = default arrow.
fn cursor_code_to_shape(code: u32) -> Shape {
    match code {
        2 => Shape::Text,
        3 => Shape::Pointer,
        4 => Shape::Wait,
        5 => Shape::EwResize,
        6 => Shape::NsResize,
        _ => Shape::Default,
    }
}

/// Map a Linux/Wayland button code to an Emacs button number.
fn button_to_emacs(btn: u32) -> u32 {
    match btn {
        BTN_LEFT => 0,
        BTN_MIDDLE => 1,
        BTN_RIGHT => 2,
        other => other.wrapping_sub(BTN_LEFT),
    }
}

/// Reduce an axis to a signed step count (notches).
fn axis_steps(a: &smithay_client_toolkit::seat::pointer::AxisScroll) -> i32 {
    if a.discrete != 0 {
        a.discrete
    } else if a.absolute > 0.0 {
        1
    } else if a.absolute < 0.0 {
        -1
    } else {
        0
    }
}

impl PointerHandler for AppState {
    fn pointer_frame(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlPointer,
                     events: &[PointerEvent]) {
        for e in events {
            let x = e.position.0 as i32;
            let y = e.position.1 as i32;
            let id = self.id_for_surface(&e.surface);
            match &e.kind {
                PointerEventKind::Enter { serial } => {
                    self.pointer_enter_serial = *serial;
                    if let Some(dev) = self.cursor_shape_device.as_ref() {
                        if self.current_cursor != 0 {
                            dev.set_shape(*serial, cursor_code_to_shape(self.current_cursor));
                        }
                    }
                    self.pointer_pos = (x, y);
                    self.push(WlshmEvent::motion(x, y, self.mods, 0).on(id));
                }
                PointerEventKind::Motion { time } => {
                    self.pointer_pos = (x, y);
                    self.push(WlshmEvent::motion(x, y, self.mods, *time).on(id));
                }
                PointerEventKind::Leave { .. } => {
                    self.push(WlshmEvent::pointer_leave().on(id));
                }
                PointerEventKind::Press { time, button, serial } => {
                    // Track the latest input serial: an xdg_popup (menu) opened
                    // in response to this click must grab with THIS serial, or
                    // the compositor (e.g. niri) rejects the grab and dismisses
                    // the popup immediately -> "menu never appears" + focus churn.
                    self.serial = *serial;
                    self.pointer_pos = (x, y);
                    self.push(WlshmEvent::button(true, button_to_emacs(*button), x, y, self.mods, *time).on(id));
                }
                PointerEventKind::Release { time, button, serial } => {
                    self.serial = *serial;
                    self.pointer_pos = (x, y);
                    self.push(WlshmEvent::button(false, button_to_emacs(*button), x, y, self.mods, *time).on(id));
                }
                PointerEventKind::Axis { time, horizontal, vertical, .. } => {
                    let hx = axis_steps(horizontal);
                    let vy = axis_steps(vertical);
                    if hx != 0 || vy != 0 {
                        let (px, py) = self.pointer_pos;
                        self.push(WlshmEvent::axis(hx, vy, px, py, self.mods, *time).on(id));
                    }
                }
            }
        }
    }
}

impl AppState {
    /// Pick the best DnD mime an offer advertises (preference order in
    /// `DND_MIME`), and tell the source we accept it with a Copy action so the
    /// drag cursor shows "will drop" and the source keeps the data alive.
    /// Returns the accepted mime, if any.
    fn dnd_accept(&self) -> Option<String> {
        let offer = self.data_device.as_ref()?.data().drag_offer()?;
        let mime = offer.with_mime_types(|types: &[String]| {
            DND_MIME
                .iter()
                .find(|m| types.iter().any(|t| t.eq_ignore_ascii_case(m)))
                .map(|m| m.to_string())
        })?;
        // Accept the chosen mime and advertise Copy as both supported and
        // preferred so the compositor settles on a non-"ask" action.
        offer.accept_mime_type(self.serial, Some(mime.clone()));
        offer.set_actions(DndAction::Copy, DndAction::Copy);
        Some(mime)
    }
}

impl DataDeviceHandler for AppState {
    fn enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice,
             x: f64, y: f64, surface: &wl_surface::WlSurface) {
        self.dnd_window = self.id_for_surface(surface);
        self.dnd_pos = (x as i32, y as i32);
        self.dnd_accept();
    }
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {
        self.dnd_window = 0;
        self.dnd_pos = (0, 0);
    }
    fn motion(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice, x: f64, y: f64) {
        self.dnd_pos = (x as i32, y as i32);
        // Re-accept on motion: some sources only honor the action after the
        // pointer has moved within the surface.
        self.dnd_accept();
    }
    fn selection(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {}
    fn drop_performed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {
        let Some(offer) = self.data_device.as_ref().and_then(|d| d.data().drag_offer()) else {
            return;
        };
        let Some(mime) = self.dnd_accept() else {
            // Nothing we can take.  Do NOT finish(): per the wl_data_offer
            // protocol finish() is only valid once an action has been accepted,
            // and here dnd_accept() accepted nothing, so finish() would raise the
            // fatal invalid_finish protocol error and disconnect Emacs.  Just
            // drop the offer; the source cancels the unfinished drag itself.
            return;
        };
        let is_uri = mime_is_uri_list(&mime);
        // Open the receive pipe now; the actual bytes are read later in
        // Backend::dispatch where the event queue can roundtrip so the source
        // gets a chance to write.  finish() is deferred until after that read
        // (see drain_pending_drop): per the wl_data_offer protocol the source is
        // free to free its data on dnd_finished, so finishing before reading can
        // truncate or empty the transfer.
        match offer.receive(mime) {
            Ok(pipe) => {
                let (x, y) = self.dnd_pos;
                let win = if self.dnd_window != 0 { self.dnd_window } else { self.primary() };
                self.pending_drop = Some((pipe, is_uri, x, y, win, offer));
            }
            Err(_) => {
                offer.finish();
            }
        }
    }
}

impl DataSourceHandler for AppState {
    fn accept_mime(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource, _: Option<String>) {}
    fn send_request(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource,
                    _mime: String, fd: WritePipe) {
        write_pipe_timeout(&fd, &self.clipboard_text, 500);
    }
    fn cancelled(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource) {
        self.clipboard_source = None;
        self.clipboard_text.clear();
    }
    fn dnd_dropped(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource) {}
    fn dnd_finished(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource) {}
    fn action(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource, _: DndAction) {}
}

impl DataOfferHandler for AppState {
    fn source_actions(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &mut DragOffer, _: DndAction) {}
    fn selected_action(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &mut DragOffer, _: DndAction) {}
}

// PRIMARY selection handlers, mirroring the data_device ones above.
impl PrimarySelectionDeviceHandler for AppState {
    // A new primary selection was offered: the offer is stored on the device's
    // user data (PrimarySelectionDeviceData), read back in get_primary.
    fn selection(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &ZwpPrimarySelectionDeviceV1) {}
}

impl PrimarySelectionSourceHandler for AppState {
    // A client wants our primary text: write it to the fd, mirroring
    // DataSourceHandler::send_request for the clipboard.
    fn send_request(&mut self, _: &Connection, _: &QueueHandle<Self>,
                    _: &ZwpPrimarySelectionSourceV1, _mime: String, write_pipe: WritePipe) {
        write_pipe_timeout(&write_pipe, &self.primary_text, 500);
    }
    fn cancelled(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &ZwpPrimarySelectionSourceV1) {
        self.primary_source = None;
        self.primary_text.clear();
    }
}

impl ProvidesRegistryState for AppState {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState, SeatState];
}

delegate_compositor!(AppState);
delegate_output!(AppState);
delegate_shm!(AppState);
delegate_seat!(AppState);
delegate_keyboard!(AppState);
delegate_pointer!(AppState);
delegate_data_device!(AppState);
delegate_primary_selection!(AppState);
delegate_xdg_shell!(AppState);
delegate_xdg_window!(AppState);
delegate_xdg_popup!(AppState);
delegate_registry!(AppState);

// ----- FFI -----------------------------------------------------------------

fn with_backend<R>(f: impl FnOnce(&mut Backend) -> R, default: R) -> R {
    BACKEND.with(|b| match b.borrow_mut().as_mut() {
        Some(be) => f(&mut **be),
        None => default,
    })
}

/// Open a window; returns an opaque handle (0 on failure).  Lazily connects to
/// Wayland on the first call.  `title` may be null.  `kind`: 0=Toplevel,
/// 1=Popup, 2=Tooltip.  `parent` is a handle or 0.
///
/// # Safety
/// `title`, if non-null, must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_open(title: *const c_char, parent: u64, kind: u32) -> u64 {
    let title = if title.is_null() {
        "emacs".to_string()
    } else {
        std::ffi::CStr::from_ptr(title).to_string_lossy().into_owned()
    };
    BACKEND.with(|b| {
        let mut slot = b.borrow_mut();
        if slot.is_none() {
            match Backend::connect() {
                Ok(be) => *slot = Some(ManuallyDrop::new(be)),
                Err(e) => {
                    eprintln!("wlshm_window_open: {e}");
                    return 0;
                }
            }
        }
        slot.as_mut().unwrap().open_window(&title, parent, kind)
    })
}

/// Ensure a Wayland connection exists (without opening a window).  Returns 0 on
/// success, -1 on failure.  Lets term-init learn the output scale early.
#[no_mangle]
pub extern "C" fn wlshm_backend_connect() -> c_int {
    BACKEND.with(|b| {
        let mut slot = b.borrow_mut();
        if slot.is_some() {
            return 0;
        }
        match Backend::connect() {
            Ok(be) => {
                *slot = Some(ManuallyDrop::new(be));
                0
            }
            Err(e) => {
                eprintln!("wlshm_backend_connect: {e}");
                -1
            }
        }
    })
}

/// Wayland connection fd for Emacs to select on. -1 if not connected.
#[no_mangle]
pub extern "C" fn wlshm_window_fd() -> c_int {
    with_backend(|b| b.conn.backend().poll_fd().as_raw_fd(), -1)
}

/// Key-repeat timerfd for Emacs to select on. -1 if unavailable.
#[no_mangle]
pub extern "C" fn wlshm_window_timer_fd() -> c_int {
    with_backend(|b| b.state.timer_fd, -1)
}

/// The present-deadline timerfd; Emacs adds it as a keyboard wait descriptor so
/// its firing wakes read_socket -> dispatch -> flush_overdue_pending().
#[no_mangle]
pub extern "C" fn wlshm_window_present_timer_fd() -> c_int {
    with_backend(|b| b.state.present_timer_fd, -1)
}

/// Disarm the key-repeat timer and forget the held key.  The C side calls this
/// when entering/leaving a modal popup loop, so a key held during the menu does
/// not keep repeating into the main loop after the menu closes.
#[no_mangle]
pub extern "C" fn wlshm_window_disarm_repeat() {
    with_backend(|b| b.state.disarm_repeat(), ());
}

/// Process pending Wayland events. Returns 0 normally, negative on error.
/// (Window-close arrives as a Close event via poll_events.)
#[no_mangle]
pub extern "C" fn wlshm_window_dispatch() -> c_int {
    with_backend(
        |b| match b.dispatch() {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("wlshm_window_dispatch: {e}");
                -1
            }
        },
        -1,
    )
}

/// Rounded integer scale factor of window `win` (1 = default).  Kept for
/// callers (e.g. Emacs DPI/resolution) that want a plain integer; the precise
/// fractional value is `wlshm_window_scale120`.
#[no_mangle]
pub extern "C" fn wlshm_window_scale(win: u64) -> c_int {
    let s120 = wlshm_window_scale120(win);
    ((s120 + 60) / 120).max(1) as c_int
}

/// Scale factor of window `win` times 120 (the wp_fractional_scale_v1 unit).
/// 120 == 1.0, 180 == 1.5x, 240 == 2.0x.  The C side multiplies the logical
/// surface size by this/120 to size its physical-pixel Cairo canvas, and sets
/// the Cairo device scale to this/120 so all drawing stays in logical
/// coordinates yet rasterizes at physical resolution.
#[no_mangle]
pub extern "C" fn wlshm_window_scale120(win: u64) -> u32 {
    with_backend(
        |b| {
            b.resolve(win)
                .and_then(|id| b.state.windows.get(&id))
                .map(|w| w.scale120)
                .unwrap_or(b.state.default_scale120.max(120))
        },
        120,
    )
}

/// Current size of window `win` in pixels, written to *w/*h.
///
/// # Safety
/// `w` and `h` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_size(win: u64, w: *mut u32, h: *mut u32) {
    let (sw, sh) = with_backend(
        |b| b.resolve(win).and_then(|id| b.state.windows.get(&id)).map(|x| x.size).unwrap_or((0, 0)),
        (0, 0),
    );
    if !w.is_null() {
        *w = sw;
    }
    if !h.is_null() {
        *h = sh;
    }
}

/// Drain up to `max` pending input events into `buf`; returns the count.
///
/// # Safety
/// `buf` must point to writable storage for at least `max` `WlshmEvent`s.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_poll_events(buf: *mut WlshmEvent, max: c_int) -> c_int {
    if buf.is_null() || max <= 0 {
        return 0;
    }
    let out = std::slice::from_raw_parts_mut(buf, max as usize);
    with_backend(
        |b| {
            let n = out.len().min(b.state.events.len());
            for slot in out.iter_mut().take(n) {
                *slot = b.state.events.pop_front().unwrap();
            }
            n as c_int
        },
        0,
    )
}

/// Present window `win`'s Cairo canvas (XRGB8888, `src_w`x`src_h`, `src_stride`
/// bytes/row) into a free wl_shm buffer sized to the SOURCE.  Zero damage rect
/// means whole surface.
///
/// # Safety
/// `src` must point to at least `src_h * src_stride` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_present(
    win: u64,
    src: *const u8,
    src_w: u32,
    src_h: u32,
    src_stride: u32,
    dmg_x: c_int,
    dmg_y: c_int,
    dmg_w: c_int,
    dmg_h: c_int,
) {
    with_backend(|b| b.present(win, src, src_w, src_h, src_stride, dmg_x, dmg_y, dmg_w, dmg_h), ());
}

/// Set window `win`'s title.  `title` is a NUL-terminated C string.
///
/// # Safety
/// `title` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_set_title(win: u64, title: *const c_char) {
    if title.is_null() {
        return;
    }
    let t = std::ffi::CStr::from_ptr(title).to_string_lossy().into_owned();
    with_backend(
        |b| {
            if let Some(id) = b.resolve(win) {
                if let Some(Role::Toplevel(w)) = b.state.windows.get(&id).map(|w| &w.role) {
                    w.set_title(&t);
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// Set the pointer cursor shape (codes match the C `wlshm_cursor_shape` enum).
/// Cursor is per-seat, so `win` is advisory.
#[no_mangle]
pub extern "C" fn wlshm_window_set_cursor(_win: u64, code: c_int) {
    with_backend(
        |b| {
            let code = code as u32;
            b.state.current_cursor = code;
            if let Some(dev) = b.state.cursor_shape_device.as_ref() {
                if b.state.pointer_enter_serial != 0 {
                    dev.set_shape(b.state.pointer_enter_serial, cursor_code_to_shape(code));
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// If window `win` has a pending resize, write it to *w/*h and return 1.
///
/// # Safety
/// `w` and `h` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_take_resize(win: u64, w: *mut u32, h: *mut u32) -> c_int {
    with_backend(
        |b| {
            let Some(id) = b.resolve(win) else { return 0 };
            let Some(wd) = b.state.windows.get_mut(&id) else { return 0 };
            match wd.pending_resize.take() {
                Some((rw, rh)) => {
                    if !w.is_null() { *w = rw; }
                    if !h.is_null() { *h = rh; }
                    1
                }
                None => 0,
            }
        },
        0,
    )
}

/// Close (destroy) window `win`.
#[no_mangle]
pub extern "C" fn wlshm_window_close(win: u64) {
    with_backend(|b| b.close_window(win), ());
}

// --- Window geometry / positioning ----------------------------------------

/// Set `win`'s geometry AND position.  For a Pending popup/tooltip this CREATES
/// the xdg_popup anchored at (x,y) in the parent's geometry with content size
/// (w,h) — so it floats over the parent (never tiled) and is placed precisely.
/// For an already-mapped popup it repositions it to (x,y) (e.g. a reused tooltip
/// following the pointer).  For a toplevel it just records the size (the next
/// present allocates a buffer of that size).
///
/// NOTE: this is the *positioned* entry point.  Frame-size requests that carry
/// no meaningful position (the set_window_size hook) must use
/// `wlshm_window_set_size` instead, or a tooltip would be created/anchored at a
/// bogus (0,0) before its real position has been computed.
#[no_mangle]
pub extern "C" fn wlshm_window_set_geometry(win: u64, x: c_int, y: c_int, w: c_int, h: c_int) {
    if w <= 0 || h <= 0 {
        return;
    }
    with_backend(
        |b| {
            let Some(id) = b.resolve(win) else { return };
            match b.state.windows.get(&id).map(|w| &w.role) {
                Some(Role::Pending { .. }) => {
                    b.make_popup(id, x, y, w, h);
                }
                Some(Role::Popup(_)) => {
                    b.reposition_popup(id, x, y, w, h);
                }
                _ => {
                    // Toplevel: just record the requested size; the next present
                    // allocates a buffer of that size.  `configured` is owned by
                    // the real xdg configure handshake (and the remap path in
                    // present()) -- faking it here would let present() commit a
                    // buffer to an unconfigured surface (xdg_surface error 3).
                    if let Some(wl) = b.state.windows.get_mut(&id) {
                        wl.size = (w as u32, h as u32);
                    }
                }
            }
        },
        (),
    );
}

/// Set `win`'s content size WITHOUT touching its position.  Used by the
/// set_window_size hook, which has no position to offer.  A Pending popup is
/// left Pending (it becomes a real popup only via the positioned
/// `wlshm_window_set_geometry`), so a tooltip is never anchored at (0,0) before
/// `compute_tip_xy` has run.
#[no_mangle]
pub extern "C" fn wlshm_window_set_size(win: u64, w: c_int, h: c_int) {
    if w <= 0 || h <= 0 {
        return;
    }
    with_backend(
        |b| {
            let Some(id) = b.resolve(win) else { return };
            if let Some(wl) = b.state.windows.get_mut(&id) {
                // Record the requested size only.  A Pending popup stays Pending
                // (it becomes a real popup via wlshm_window_set_geometry).
                // `configured` is owned by the xdg configure handshake and the
                // remap path in present(); faking it here would let present()
                // commit a buffer to an unconfigured surface (xdg_surface
                // error 3, e.g. a child frame hidden and re-shown).
                wl.size = (w as u32, h as u32);
                // A subsurface has no xdg configure to re-arm its viewport, so
                // map its (now known) logical size to the physical buffer here.
                if matches!(wl.role, Role::Subsurface { .. }) {
                    wl.update_viewport();
                }
            }
        },
        (),
    );
}

/// Position child subsurface `win` at (x, y) LOGICAL pixels relative to its
/// parent frame.  No-op for non-subsurface windows (Wayland forbids a client
/// positioning its own toplevel).  Placement applies on the parent's commit.
#[no_mangle]
pub extern "C" fn wlshm_window_set_subsurface_pos(win: u64, x: c_int, y: c_int) {
    with_backend(
        |b| {
            let Some(id) = b.resolve(win) else { return };
            let parent_surface = match b.state.windows.get(&id).map(|w| &w.role) {
                Some(Role::Subsurface { subsurface, parent_surface, .. }) => {
                    subsurface.set_position(x, y);
                    Some(parent_surface.clone())
                }
                _ => None,
            };
            if let Some(ps) = parent_surface {
                ps.commit();
                let _ = b.conn.flush();
            }
        },
        (),
    );
}

/// Reparent `win` to `parent` (xdg_toplevel.set_parent).  `parent` 0 clears it.
#[no_mangle]
pub extern "C" fn wlshm_window_set_parent(win: u64, parent: u64) {
    with_backend(
        |b| {
            let Some(cid) = b.resolve(win) else { return };
            // Only toplevels have an xdg_toplevel.set_parent; popups are
            // parented via their positioner at creation.
            if parent == 0 {
                if let Some(Role::Toplevel(c)) = b.state.windows.get(&cid).map(|w| &w.role) {
                    c.xdg_toplevel().set_parent(None);
                    let _ = b.conn.flush();
                }
                return;
            }
            if let Some(pid) = b.resolve(parent) {
                let child = match b.state.windows.get(&cid).map(|w| &w.role) {
                    Some(Role::Toplevel(c)) => Some(c.xdg_toplevel().clone()),
                    _ => None,
                };
                let par = match b.state.windows.get(&pid).map(|w| &w.role) {
                    Some(Role::Toplevel(p)) => Some(p.xdg_toplevel().clone()),
                    _ => None,
                };
                if let (Some(c), Some(p)) = (child, par) {
                    c.set_parent(Some(&p));
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// Minimize `win` (xdg_toplevel.set_minimized).
#[no_mangle]
pub extern "C" fn wlshm_window_minimize(win: u64) {
    with_backend(
        |b| {
            if let Some(id) = b.resolve(win) {
                if let Some(Role::Toplevel(w)) = b.state.windows.get(&id).map(|w| &w.role) {
                    w.set_minimized();
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// Set/unset fullscreen for `win`. `mode`: 0=off, 1=on.
#[no_mangle]
pub extern "C" fn wlshm_window_set_fullscreen(win: u64, mode: c_int) {
    with_backend(
        |b| {
            if let Some(id) = b.resolve(win) {
                if let Some(Role::Toplevel(w)) = b.state.windows.get(&id).map(|w| &w.role) {
                    if mode != 0 {
                        w.set_fullscreen(None);
                    } else {
                        w.unset_fullscreen();
                    }
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// Toggle server-side decorations on `win`.  Decorations are chosen at window
/// creation (WindowDecorations); a reliable runtime toggle needs xdg-decoration
/// renegotiation that most compositors only honor at map time, so this is a
/// documented best-effort no-op for now (undecorated tooltip/menu/child frames
/// already get created without decorations).
#[no_mangle]
pub extern "C" fn wlshm_window_set_decorations(_win: u64, _on: bool) {}

/// Hide/show the hardware pointer over `win` (Emacs hides it while typing).
/// Hidden = attach a null cursor surface; shown = re-assert our cursor shape.
#[no_mangle]
pub extern "C" fn wlshm_window_hide_pointer(_win: u64, hide: bool) {
    with_backend(
        |b| {
            let Some(ptr) = b.state.pointer.as_ref() else { return };
            let serial = b.state.pointer_enter_serial;
            if serial == 0 {
                return;
            }
            if hide {
                ptr.set_cursor(serial, None, 0, 0);
            } else if let Some(dev) = b.state.cursor_shape_device.as_ref() {
                dev.set_shape(serial, cursor_code_to_shape(b.state.current_cursor));
            }
            let _ = b.conn.flush();
        },
        (),
    );
}

/// Unmap `win` (hide it) by committing a null buffer.  Re-present to remap.
#[no_mangle]
pub extern "C" fn wlshm_window_unmap(win: u64) {
    with_backend(
        |b| {
            if let Some(id) = b.resolve(win) {
                if let Some(w) = b.state.windows.get_mut(&id) {
                    let Some(surface) = w.wl_surface().cloned() else { return };
                    surface.attach(None, 0, 0);
                    surface.commit();
                    // Clone the parent first (so we can mutate `w` below without
                    // holding a borrow of w.role).
                    let parent = if let Role::Subsurface { parent_surface, .. } = &w.role {
                        Some(parent_surface.clone())
                    } else {
                        None
                    };
                    // Unmapping an xdg surface returns it to the unconfigured
                    // state: a fresh configure is required before the next
                    // buffer.  Clear `configured` so the remap path in present()
                    // re-enters the configure sequence instead of committing a
                    // buffer (xdg_surface error 3).  But a SUBSURFACE is not an
                    // xdg surface -- it gets no configure to clear the flag
                    // again, so clearing it here would wedge it invisible after
                    // the first hide (corfu popups).  Keep it configured; the
                    // next present re-attaches a buffer + parent commit.
                    if parent.is_none() {
                        w.configured = false;
                    }
                    // Drop any buffer coalesced before this unmap and clear the
                    // throttle.  Otherwise a stranded frame() callback or the
                    // present-deadline flush could later commit that stale buffer
                    // onto the now unmapped/unconfigured surface (xdg_surface
                    // error 3).  The next present re-attaches a fresh buffer.
                    w.pending = None;
                    w.frame_pending = false;
                    // Re-map (the next present) must commit the parent again to
                    // re-apply this subsurface's placement.
                    w.parent_dirty = true;
                    // A subsurface's NULL-buffer unmap applies on the child commit
                    // (desync), but its PLACEMENT is parent-cached.  Commit the
                    // parent here so the hide is ordered atomically -- otherwise a
                    // sibling child's present() parent-commit can flush stale
                    // placement of this hidden child, repainting it in the gap
                    // between the two corfu popups (the "overlay between frames").
                    if let Some(ps) = parent {
                        ps.commit();
                    }
                    let _ = b.conn.flush();
                }
            }
        },
        (),
    );
}

/// Number of connected outputs (monitors).
#[no_mangle]
pub extern "C" fn wlshm_output_count() -> c_int {
    with_backend(|b| b.state.output_state.outputs().count() as c_int, 0)
}

/// Geometry of output `idx` for display-monitor-attributes-list.  Writes
/// logical x/y and px w/h, physical mm w/h, integer scale, and the NUL-
/// terminated name (truncated to `name_cap`).  Returns 1 on success.
///
/// # Safety
/// Output pointers, if non-null, must be writable; `name` must have room for
/// `name_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn wlshm_output_get(
    idx: c_int,
    x: *mut c_int,
    y: *mut c_int,
    w: *mut c_int,
    h: *mut c_int,
    mm_w: *mut c_int,
    mm_h: *mut c_int,
    scale: *mut c_int,
    name: *mut c_char,
    name_cap: usize,
) -> c_int {
    with_backend(
        |b| {
            let Some(out) = b.state.output_state.outputs().nth(idx as usize) else {
                return 0;
            };
            let Some(info) = b.state.output_state.info(&out) else {
                return 0;
            };
            let (px, py) = info.logical_position.unwrap_or((0, 0));
            let (pw, ph) = info.logical_size.unwrap_or_else(|| {
                info.modes
                    .iter()
                    .find(|m| m.current)
                    .map(|m| m.dimensions)
                    .unwrap_or((0, 0))
            });
            let (pmw, pmh) = info.physical_size;
            if !x.is_null() { *x = px; }
            if !y.is_null() { *y = py; }
            if !w.is_null() { *w = pw; }
            if !h.is_null() { *h = ph; }
            if !mm_w.is_null() { *mm_w = pmw; }
            if !mm_h.is_null() { *mm_h = pmh; }
            if !scale.is_null() { *scale = info.scale_factor; }
            if !name.is_null() && name_cap > 0 {
                let s = info.name.clone().unwrap_or_default();
                let bytes = s.as_bytes();
                let n = bytes.len().min(name_cap - 1);
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), name as *mut u8, n);
                *name.add(n) = 0;
            }
            1
        },
        0,
    )
}

// Holds the last clipboard read so we can hand C a pointer that stays valid
// until the next call (C copies it immediately into a Lisp string).
thread_local! {
    static CLIPBOARD_BUF: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

/// Take ownership of the CLIPBOARD selection with `len` bytes at `data`.
///
/// # Safety
/// `data` must point to at least `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_set_clipboard(data: *const u8, len: usize) -> c_int {
    if data.is_null() {
        return -1;
    }
    let bytes = std::slice::from_raw_parts(data, len);
    with_backend(|b| if b.set_clipboard(bytes) { 0 } else { -1 }, -1)
}

/// Read the CLIPBOARD selection.  Writes a pointer/length valid until the next
/// call into *out_ptr/*out_len and returns 0; -1 if empty.
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_get_clipboard(
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> c_int {
    match with_backend(|b| b.get_clipboard(), None) {
        Some(bytes) => CLIPBOARD_BUF.with(|b| {
            let mut b = b.borrow_mut();
            *b = bytes;
            if !out_ptr.is_null() {
                *out_ptr = b.as_ptr();
            }
            if !out_len.is_null() {
                *out_len = b.len();
            }
            0
        }),
        None => -1,
    }
}

/// Retrieve the payload of the most recently delivered Drop event.  Writes a
/// pointer/length valid until the next call into *out_ptr/*out_len and returns
/// 0; -1 if there is no pending drop text.  C must call this right after it pops
/// a `WlshmEventKind_Drop` event (whose `.button` flags a `text/uri-list`).
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_get_drop(
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> c_int {
    with_backend(
        |b| {
            if b.state.drop_text.is_empty() {
                return -1;
            }
            if !out_ptr.is_null() {
                *out_ptr = b.state.drop_text.as_ptr();
            }
            if !out_len.is_null() {
                *out_len = b.state.drop_text.len();
            }
            0
        },
        -1,
    )
}

/// Retrieve the current IME preedit (composition) string.  Writes a
/// pointer/length valid until the next call into *out_ptr/*out_len and returns
/// 0; -1 if there is no preedit object at all.  The buffer is UTF-8 and may be
/// empty (length 0) to mean "clear the preedit".  C must call this right after
/// it pops a `WlshmEventKind_Preedit` event.
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_get_preedit(
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> c_int {
    with_backend(
        |b| {
            if !out_ptr.is_null() {
                *out_ptr = b.state.preedit_text.as_ptr();
            }
            if !out_len.is_null() {
                *out_len = b.state.preedit_text.len();
            }
            0
        },
        -1,
    )
}

/// Release our ownership of the CLIPBOARD selection.
#[no_mangle]
pub extern "C" fn wlshm_window_disown_clipboard() {
    with_backend(|b| b.disown_clipboard(), ());
}

/// 1 if we own the CLIPBOARD selection, else 0.
#[no_mangle]
pub extern "C" fn wlshm_window_owns_clipboard() -> c_int {
    with_backend(|b| b.state.clipboard_source.is_some() as c_int, 0)
}

/// 1 if a CLIPBOARD selection exists (we or another client own it), else 0.
#[no_mangle]
pub extern "C" fn wlshm_window_clipboard_exists() -> c_int {
    with_backend(
        |b| {
            let owned = b.state.clipboard_source.is_some();
            let foreign = b
                .state
                .data_device
                .as_ref()
                .map(|d| d.data().selection_offer().is_some())
                .unwrap_or(false);
            (owned || foreign) as c_int
        },
        0,
    )
}

// --- PRIMARY selection FFI: parallel to the CLIPBOARD functions above. ---

/// Take ownership of the PRIMARY selection with `len` bytes at `data`.
///
/// # Safety
/// `data` must point to at least `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_set_primary(data: *const u8, len: usize) -> c_int {
    if data.is_null() {
        return -1;
    }
    let bytes = std::slice::from_raw_parts(data, len);
    with_backend(|b| if b.set_primary(bytes) { 0 } else { -1 }, -1)
}

/// Read the PRIMARY selection.  Writes a pointer/length valid until the next
/// call into *out_ptr/*out_len and returns 0; -1 if empty.
///
/// # Safety
/// `out_ptr` and `out_len` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wlshm_window_get_primary(
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> c_int {
    match with_backend(|b| b.get_primary(), None) {
        Some(bytes) => CLIPBOARD_BUF.with(|b| {
            let mut b = b.borrow_mut();
            *b = bytes;
            if !out_ptr.is_null() {
                *out_ptr = b.as_ptr();
            }
            if !out_len.is_null() {
                *out_len = b.len();
            }
            0
        }),
        None => -1,
    }
}

/// Release our ownership of the PRIMARY selection.
#[no_mangle]
pub extern "C" fn wlshm_window_disown_primary() {
    with_backend(|b| b.disown_primary(), ());
}

/// 1 if we own the PRIMARY selection, else 0.
#[no_mangle]
pub extern "C" fn wlshm_window_owns_primary() -> c_int {
    with_backend(|b| b.state.primary_source.is_some() as c_int, 0)
}

/// 1 if a PRIMARY selection exists (we or another client own it), else 0.
#[no_mangle]
pub extern "C" fn wlshm_window_primary_exists() -> c_int {
    with_backend(
        |b| {
            let owned = b.state.primary_source.is_some();
            let foreign = b
                .state
                .primary_device
                .as_ref()
                .map(|d| d.data().selection_offer().is_some())
                .unwrap_or(false);
            (owned || foreign) as c_int
        },
        0,
    )
}

#[cfg(test)]
mod tests {
    use super::repeat_itimerspec;

    #[test]
    fn itimerspec_splits_milliseconds() {
        let s = repeat_itimerspec(1500, 33);
        assert_eq!(s.it_value.tv_sec, 1);
        assert_eq!(s.it_value.tv_nsec, 500 * 1_000_000);
        assert_eq!(s.it_interval.tv_sec, 0);
        assert_eq!(s.it_interval.tv_nsec, 33 * 1_000_000);
    }

    #[test]
    fn zero_clamps_to_one_ms() {
        let s = repeat_itimerspec(0, 0);
        assert_eq!(s.it_value.tv_nsec, 1_000_000);
        assert_eq!(s.it_interval.tv_nsec, 1_000_000);
    }
}
