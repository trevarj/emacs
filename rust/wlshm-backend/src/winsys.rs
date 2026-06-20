//! FFI-driven Wayland windows for Emacs (single-threaded, multi-window).
//!
//! Emacs owns the main loop; this exposes Wayland windows it drives explicitly.
//! There is ONE Wayland connection multiplexing N surfaces (toplevels, and —
//! from M4/M5 — popups/tooltips).  Connection-global state (seat, keyboard,
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
    shm::{slot::SlotPool, Shm, ShmHandler},
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
    backend::ObjectId,
    globals::registry_queue_init,
    protocol::{
        wl_data_device::WlDataDevice, wl_data_device_manager::DndAction,
        wl_data_source::WlDataSource, wl_keyboard::WlKeyboard, wl_output,
        wl_pointer::WlPointer, wl_seat::WlSeat, wl_shm, wl_surface,
    },
    Connection, EventQueue, Proxy, QueueHandle,
};

use crate::event::{WlshmEvent, WLSHM_MOD_ALT, WLSHM_MOD_CTRL, WLSHM_MOD_LOGO, WLSHM_MOD_SHIFT};

// ManuallyDrop: at process exit we deliberately leak the connection rather than
// run libwayland's teardown (which can crash).  The OS reclaims everything.
// Individual windows DO drop normally on wlshm_window_close (proper surface
// destroy for transient popups/tooltips while the connection is alive).
thread_local! {
    static BACKEND: RefCell<Option<ManuallyDrop<Backend>>> = const { RefCell::new(None) };
}

/// Debug logging: enabled by the WLSHM_DEBUG env var, appended to
/// /tmp/wlshm-debug.log (shared with the C side), flushed per line.
#[allow(dead_code)]
fn wlshm_dbg_on() -> bool {
    thread_local! { static ON: std::cell::Cell<i8> = const { std::cell::Cell::new(-1) }; }
    ON.with(|c| {
        let v = c.get();
        if v < 0 {
            let on = std::env::var_os("WLSHM_DEBUG").is_some();
            c.set(on as i8);
            on
        } else {
            v == 1
        }
    })
}

#[allow(dead_code)]
fn dbg_write(args: std::fmt::Arguments) {
    if !wlshm_dbg_on() {
        return;
    }
    use std::io::Write;
    use std::time::{SystemTime, UNIX_EPOCH};
    thread_local! {
        static LOG: RefCell<Option<std::fs::File>> = const { RefCell::new(None) };
    }
    LOG.with(|cell| {
        let mut slot = cell.borrow_mut();
        if slot.is_none() {
            *slot = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open("/tmp/wlshm-debug.log")
                .ok();
        }
        if let Some(f) = slot.as_mut() {
            let ts = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| format!("{}.{:03}", d.as_secs(), d.subsec_millis()))
                .unwrap_or_else(|_| "0.000".into());
            let _ = writeln!(f, "[{} rust] {}", ts, args);
        }
    });
}

#[allow(unused_macros)]
macro_rules! dlog {
    ($($a:tt)*) => { dbg_write(format_args!($($a)*)) };
}

/// MIME types we offer/accept for the text clipboard, in preference order.
const CLIPBOARD_MIME: &[&str] = &[
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
    "TEXT",
];

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
    /// Awaiting set_geometry to become a Popup.  `grab` = take a keyboard grab
    /// (menus do; tooltips don't).
    Pending { parent: u64, grab: bool },
}

/// Per-surface state.  One per Emacs frame (toplevel), menu popup, or tooltip.
struct WlWindow {
    role: Role,
    /// wl_shm slot pool: source of the (double-buffered) buffers we present.
    pool: SlotPool,
    size: (u32, u32),
    /// Integer output scale factor (1, 2, ...).
    scale: i32,
    /// New size the compositor asked for, pending delivery to Emacs.
    pending_resize: Option<(u32, u32)>,
    configured: bool,
}

impl WlWindow {
    /// The wl_surface, or None for a popup not yet created (Pending).
    fn wl_surface(&self) -> Option<&wl_surface::WlSurface> {
        match &self.role {
            Role::Toplevel(w) => Some(w.wl_surface()),
            Role::Popup(p) => Some(p.wl_surface()),
            Role::Pending { .. } => None,
        }
    }
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
    repeat_delay_ms: u32,
    repeat_rate_ms: u32,
    /// Currently-held repeating key: (raw keycode, keysym, unichar).
    repeat: Option<(u32, u32, u32)>,
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
    default_scale: i32,
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

    /// Arm the repeat timerfd: first fire after `delay`, then every `rate` ms.
    fn arm_repeat(&self) {
        if self.timer_fd < 0 {
            return;
        }
        let spec = repeat_itimerspec(self.repeat_delay_ms, self.repeat_rate_ms);
        unsafe { libc::timerfd_settime(self.timer_fd, 0, &spec, std::ptr::null_mut()) };
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
        let win = self.primary();
        if let Some((_, ks, unichar)) = self.repeat {
            for _ in 0..expirations.min(4) {
                self.push(WlshmEvent::key(ks, unichar, self.mods).on(win));
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
            timer_fd: unsafe {
                libc::timerfd_create(libc::CLOCK_MONOTONIC,
                                     libc::TFD_NONBLOCK | libc::TFD_CLOEXEC)
            },
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
            events: VecDeque::new(),
            compositor,
            xdg_shell,
            shm,
            windows: HashMap::new(),
            surface_to_id: HashMap::new(),
            focused_window: None,
            next_id: 1,
            default_scale: 1,
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

        for output in state.output_state.outputs() {
            if let Some(info) = state.output_state.info(&output) {
                if info.scale_factor > state.default_scale {
                    state.default_scale = info.scale_factor;
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
        let scale = self.state.default_scale.max(1);

        if (kind == 1 || kind == 2) && parent != 0 {
            // Defer: become an xdg_popup at set_geometry time.
            self.state.windows.insert(id, WlWindow {
                role: Role::Pending { parent, grab: kind == 1 },
                pool,
                size: (1, 1),
                scale,
                pending_resize: None,
                configured: false,
            });
            return id;
        }

        // Toplevel (or a parentless popup falling back to one).
        if kind != 0 {
            eprintln!("wlshm: popup kind {kind} has no parent; using a toplevel");
        }
        let surface = self.state.compositor.create_surface(&self.qh);
        let window = self.state.xdg_shell.create_window(
            surface, WindowDecorations::RequestServer, &self.qh);
        window.set_title(title);
        window.set_app_id("org.gnu.emacs.wlshm");
        window.set_min_size(Some((160, 120)));
        window.commit();

        let sid = window.wl_surface().id();
        self.state.surface_to_id.insert(sid, id);
        self.state.windows.insert(id, WlWindow {
            role: Role::Toplevel(window),
            pool,
            size: (800, 600),
            scale,
            pending_resize: None,
            configured: false,
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
        if let Some(wl) = self.state.windows.get_mut(&id) {
            wl.role = Role::Popup(popup);
            wl.size = (w.max(1) as u32, h.max(1) as u32);
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
                let _ = guard.read();
                self.event_queue
                    .dispatch_pending(&mut self.state)
                    .map_err(|e| format!("dispatch: {e}"))?;
            }
        }
        self.state.pump_repeat();
        Ok(())
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
        let w = self.state.windows.get_mut(&id).unwrap();
        if !w.configured || src.is_null() {
            return;
        }
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
        let stride = pw as i32 * 4;
        let (buffer, canvas) = match w.pool.create_buffer(
            pw as i32, ph as i32, stride, wl_shm::Format::Xrgb8888,
        ) {
            Ok(x) => x,
            Err(e) => {
                eprintln!("wlshm: shm create_buffer: {e}");
                return;
            }
        };
        let src_stride = src_stride as usize;
        let dst_stride = stride as usize;
        let row_bytes = (pw as usize) * 4;
        for y in 0..ph as usize {
            let s = std::slice::from_raw_parts(src.add(y * src_stride), row_bytes);
            canvas[y * dst_stride..y * dst_stride + row_bytes].copy_from_slice(s);
        }
        let Some(surface) = w.wl_surface() else { return };
        if let Err(e) = buffer.attach_to(surface) {
            eprintln!("wlshm: buffer attach: {e}");
            return;
        }
        if dmg_w > 0 && dmg_h > 0 {
            surface.damage_buffer(dmg_x, dmg_y, dmg_w, dmg_h);
        } else {
            surface.damage_buffer(0, 0, pw as i32, ph as i32);
        }
        surface.commit();
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

// ----- sctk handlers -------------------------------------------------------

impl CompositorHandler for AppState {
    fn scale_factor_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, s: &wl_surface::WlSurface, new: i32) {
        if new >= 1 {
            let id = self.id_for_surface(s);
            if let Some(w) = self.windows.get_mut(&id) {
                w.scale = new;
            }
        }
    }
    fn transform_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: wl_output::Transform) {}
    fn frame(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: u32) {}
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
            w.pending_resize = Some(w.size);
            let (sw, sh) = w.size;
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
            w.configured = true;
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
    }
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             surface: &wl_surface::WlSurface, _: u32) {
        self.disarm_repeat();
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
            self.repeat = Some((event.raw_code, ks, unichar));
            self.arm_repeat();
        }
    }

    fn release_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                   _: u32, event: KeyEvent) {
        if matches!(self.repeat, Some((rc, _, _)) if rc == event.raw_code) {
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
                PointerEventKind::Leave { .. } => {}
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

impl DataDeviceHandler for AppState {
    fn enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice,
             _: f64, _: f64, _: &wl_surface::WlSurface) {}
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {}
    fn motion(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice, _: f64, _: f64) {}
    fn selection(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {}
    fn drop_performed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataDevice) {}
}

impl DataSourceHandler for AppState {
    fn accept_mime(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource, _: Option<String>) {}
    fn send_request(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlDataSource,
                    _mime: String, mut fd: WritePipe) {
        use std::io::Write;
        let _ = fd.write_all(&self.clipboard_text);
        let _ = fd.flush();
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
                    _: &ZwpPrimarySelectionSourceV1, _mime: String, mut write_pipe: WritePipe) {
        use std::io::Write;
        let _ = write_pipe.write_all(&self.primary_text);
        let _ = write_pipe.flush();
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

/// Integer output scale factor of window `win` (1 = default).
#[no_mangle]
pub extern "C" fn wlshm_window_scale(win: u64) -> c_int {
    with_backend(
        |b| {
            b.resolve(win)
                .and_then(|id| b.state.windows.get(&id))
                .map(|w| w.scale)
                .unwrap_or(b.state.default_scale.max(1))
        },
        1,
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

// --- M6 stubs (signatures wired now to avoid a second FFI churn) -----------

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
                    if let Some(wl) = b.state.windows.get_mut(&id) {
                        wl.size = (w as u32, h as u32);
                        wl.configured = true;
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
                wl.size = (w as u32, h as u32);
                // A Pending popup stays Pending; toplevels mark themselves
                // configured so the next present allocates the new buffer.
                if !matches!(wl.role, Role::Pending { .. }) {
                    wl.configured = true;
                }
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
                if let Some(w) = b.state.windows.get(&id) {
                    let Some(surface) = w.wl_surface() else { return };
                    surface.attach(None, 0, 0);
                    surface.commit();
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
