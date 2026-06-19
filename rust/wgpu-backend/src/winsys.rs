//! FFI-driven live window for the Emacs frame (single-threaded model).
//!
//! Emacs owns the main loop; this exposes a window it drives explicitly:
//!   * `wgpu_window_open` connects to Wayland, creates an xdg-shell window and
//!     a wgpu surface, and does an initial roundtrip so the size is known.
//!   * `wgpu_window_fd` returns the Wayland fd; Emacs adds it to its select
//!     set and calls `wgpu_window_dispatch` (from `read_socket_hook`) when it
//!     is readable, to process configure/close/etc.
//!   * the RIF hooks record draw commands (`wgpu_window_begin` /
//!     `wgpu_window_rect` / `wgpu_window_glyph`) and `wgpu_window_present`
//!     (from `frame_up_to_date_hook`) composites them onto a persistent frame
//!     texture and copies it to the surface. The persistent texture preserves
//!     unchanged pixels across Emacs's incremental updates.
//!
//! Keyboard/pointer input (xkbcommon, the lock-free queue) is M3; for now the
//! window renders the frame but is not yet interactive.
//!
//! A single global window via `thread_local!` is fine: Emacs drives everything
//! from one thread, so no locking or Send/Sync is needed.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_int};
use std::os::unix::io::AsRawFd;
use std::ptr::NonNull;

use raw_window_handle::{
    RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle, WaylandWindowHandle,
};
use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_compositor, delegate_output, delegate_registry, delegate_xdg_shell,
    delegate_xdg_window,
    output::{OutputHandler, OutputState},
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    seat::{
        keyboard::{KeyEvent, KeyboardHandler, Keysym, Modifiers},
        Capability, SeatHandler, SeatState,
    },
    shell::{
        xdg::{
            window::{Window, WindowConfigure, WindowDecorations, WindowHandler},
            XdgShell,
        },
        WaylandSurface,
    },
    delegate_seat, delegate_keyboard,
};
use wayland_client::{
    globals::registry_queue_init,
    protocol::{wl_keyboard::WlKeyboard, wl_output, wl_seat::WlSeat, wl_surface},
    Connection, EventQueue, Proxy, QueueHandle,
};

use crate::event::{WgpuEvent, WGPU_MOD_ALT, WGPU_MOD_CTRL, WGPU_MOD_LOGO, WGPU_MOD_SHIFT};
use crate::gpu2d::{DrawCmd, Renderer};
use crate::render::Gpu;

// ManuallyDrop: at process exit (and on close) we deliberately leak the
// wgpu/Wayland resources rather than run their destructors, which crash inside
// libwayland during teardown. The OS reclaims everything on exit.
thread_local! {
    static WINDOW: RefCell<Option<ManuallyDrop<WgpuWindow>>> = const { RefCell::new(None) };
}

struct GlyphEntry {
    uv: crate::gpu2d::Uv,
    w: f32,
    h: f32,
}

/// sctk delegate target + render state.
struct WinState {
    registry_state: RegistryState,
    output_state: OutputState,
    seat_state: SeatState,
    keyboard: Option<WlKeyboard>,
    /// Current modifier mask (WGPU_MOD_*).
    mods: u32,
    /// Pending input events for Emacs's read_socket.
    events: VecDeque<WgpuEvent>,
    window: Window,
    surface: wgpu::Surface<'static>,
    gpu: Gpu,
    renderer: Renderer,
    format: wgpu::TextureFormat,
    alpha_mode: wgpu::CompositeAlphaMode,
    /// Persistent frame texture, preserved across incremental updates.
    persistent: Option<wgpu::Texture>,
    persistent_size: (u32, u32),
    size: (u32, u32),
    /// Integer output scale factor (1, 2, ...).
    scale: i32,
    /// New size the compositor asked for, pending delivery to Emacs.
    pending_resize: Option<(u32, u32)>,
    configured: bool,
    close_requested: bool,
    /// Draw commands recorded since the last present.
    cmds: Vec<DrawCmd>,
    glyphs: Vec<GlyphEntry>,
}

struct WgpuWindow {
    conn: Connection,
    qh: QueueHandle<WinState>,
    event_queue: EventQueue<WinState>,
    state: WinState,
}

impl WgpuWindow {
    fn open(title: &str) -> Result<WgpuWindow, String> {
        let conn = Connection::connect_to_env().map_err(|e| format!("wayland connect: {e}"))?;
        let (globals, mut event_queue) =
            registry_queue_init::<WinState>(&conn).map_err(|e| format!("registry init: {e}"))?;
        let qh = event_queue.handle();

        let compositor =
            CompositorState::bind(&globals, &qh).map_err(|e| format!("wl_compositor: {e}"))?;
        let xdg_shell = XdgShell::bind(&globals, &qh).map_err(|e| format!("xdg_wm_base: {e}"))?;

        let surface = compositor.create_surface(&qh);
        let window = xdg_shell.create_window(surface, WindowDecorations::RequestServer, &qh);
        window.set_title(title);
        window.set_app_id("org.gnu.emacs.wgpu");
        window.set_min_size(Some((160, 120)));
        window.commit();

        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN | wgpu::Backends::GL,
            ..Default::default()
        });
        let raw_display = RawDisplayHandle::Wayland(WaylandDisplayHandle::new(
            NonNull::new(conn.backend().display_ptr() as *mut _).ok_or("null wl_display")?,
        ));
        let raw_window = RawWindowHandle::Wayland(WaylandWindowHandle::new(
            NonNull::new(window.wl_surface().id().as_ptr() as *mut _).ok_or("null wl_surface")?,
        ));
        let surface = unsafe {
            instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle: raw_display,
                raw_window_handle: raw_window,
            })
        }
        .map_err(|e| format!("create_surface: {e}"))?;

        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::LowPower,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
        }))
        .ok_or("no GPU adapter for surface")?;
        let required_limits = adapter.limits();
        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("wgpu-emacs window device"),
                required_features: wgpu::Features::empty(),
                required_limits,
                memory_hints: wgpu::MemoryHints::Performance,
            },
            None,
        ))
        .map_err(|e| format!("request_device: {e}"))?;
        let gpu = Gpu { instance, adapter, device, queue };

        let caps = surface.get_capabilities(&gpu.adapter);
        let format = caps.formats.iter().copied().find(|f| f.is_srgb()).unwrap_or(caps.formats[0]);
        let alpha_mode = caps.alpha_modes[0];
        let renderer = Renderer::new(&gpu, format);

        let mut state = WinState {
            registry_state: RegistryState::new(&globals),
            output_state: OutputState::new(&globals, &qh),
            seat_state: SeatState::new(&globals, &qh),
            keyboard: None,
            mods: 0,
            events: VecDeque::new(),
            window,
            surface,
            gpu,
            renderer,
            format,
            alpha_mode,
            persistent: None,
            persistent_size: (0, 0),
            size: (800, 600),
            scale: 1,
            pending_resize: None,
            configured: false,
            close_requested: false,
            cmds: Vec::new(),
            glyphs: Vec::new(),
        };

        // Roundtrip so the compositor sends the initial configure (sizes us).
        event_queue.roundtrip(&mut state).map_err(|e| format!("roundtrip: {e}"))?;
        event_queue.roundtrip(&mut state).ok();

        // Fall back to the output's scale if no scale_factor_changed arrived.
        if state.scale <= 1 {
            for output in state.output_state.outputs() {
                if let Some(info) = state.output_state.info(&output) {
                    if info.scale_factor > state.scale {
                        state.scale = info.scale_factor;
                    }
                }
            }
        }

        Ok(WgpuWindow { conn, qh, event_queue, state })
    }

    fn dispatch(&mut self) -> Result<(), String> {
        let _ = self.conn.flush();
        // Read any pending socket data without blocking, then dispatch.
        if let Some(guard) = self.conn.prepare_read() {
            let _ = guard.read();
        }
        self.event_queue
            .dispatch_pending(&mut self.state)
            .map_err(|e| format!("dispatch: {e}"))?;
        Ok(())
    }
}

impl WinState {
    fn configure_surface(&mut self) {
        let config = wgpu::SurfaceConfiguration {
            // COPY_DST so we can copy the persistent texture into the frame.
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_DST,
            format: self.format,
            width: self.size.0.max(1),
            height: self.size.1.max(1),
            present_mode: wgpu::PresentMode::Fifo,
            alpha_mode: self.alpha_mode,
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        self.surface.configure(&self.gpu.device, &config);
        self.configured = true;
    }

    fn ensure_persistent(&mut self) {
        if self.persistent_size != self.size || self.persistent.is_none() {
            let tex = self.gpu.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("persistent frame"),
                size: wgpu::Extent3d {
                    width: self.size.0.max(1),
                    height: self.size.1.max(1),
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: self.format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            self.persistent = Some(tex);
            self.persistent_size = self.size;
        }
    }

    /// Composite recorded commands onto the persistent texture (preserving
    /// prior pixels) and copy it to the surface.
    fn present(&mut self) {
        if !self.configured {
            return;
        }
        self.ensure_persistent();
        let persistent = self.persistent.as_ref().unwrap();
        let pview = persistent.create_view(&wgpu::TextureViewDescriptor::default());

        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(e) => {
                eprintln!("wgpu: surface lost ({e:?}); reconfiguring");
                self.configure_surface();
                return;
            }
        };

        let mut encoder = self
            .gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("present") });
        self.renderer.encode(
            &self.gpu,
            &mut encoder,
            &pview,
            self.size.0,
            self.size.1,
            wgpu::LoadOp::Load,
            &self.cmds,
        );
        encoder.copy_texture_to_texture(
            wgpu::TexelCopyTextureInfo {
                texture: persistent,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyTextureInfo {
                texture: &frame.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::Extent3d {
                width: self.size.0,
                height: self.size.1,
                depth_or_array_layers: 1,
            },
        );
        self.gpu.queue.submit(Some(encoder.finish()));
        frame.present();
        self.cmds.clear();
    }
}

// ----- sctk handlers -------------------------------------------------------

impl CompositorHandler for WinState {
    fn scale_factor_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, new: i32) {
        if new >= 1 {
            self.scale = new;
        }
    }
    fn transform_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: wl_output::Transform) {}
    fn frame(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: u32) {}
    fn surface_enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
    fn surface_leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
}

impl WindowHandler for WinState {
    fn request_close(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &Window) {
        self.close_requested = true;
    }
    fn configure(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &Window, configure: WindowConfigure, _: u32) {
        if let (Some(w), Some(h)) = configure.new_size {
            self.size = (w.get(), h.get());
        }
        self.configure_surface();
        // Tell Emacs to resize the frame to match.
        self.pending_resize = Some(self.size);
    }
}

impl OutputHandler for WinState {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl SeatHandler for WinState {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seat_state
    }
    fn new_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
    fn new_capability(&mut self, _: &Connection, qh: &QueueHandle<Self>, seat: WlSeat, capability: Capability) {
        if capability == Capability::Keyboard && self.keyboard.is_none() {
            match self.seat_state.get_keyboard(qh, &seat, None) {
                Ok(kbd) => self.keyboard = Some(kbd),
                Err(e) => eprintln!("wgpu: get_keyboard failed: {e}"),
            }
        }
    }
    fn remove_capability(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat, capability: Capability) {
        if capability == Capability::Keyboard {
            if let Some(kbd) = self.keyboard.take() {
                kbd.release ();
            }
        }
    }
    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
}

impl KeyboardHandler for WinState {
    fn enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             _: &wl_surface::WlSurface, _: u32, _: &[u32], _: &[Keysym]) {}
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             _: &wl_surface::WlSurface, _: u32) {}

    fn press_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                 _: u32, event: KeyEvent) {
        let unichar = event
            .utf8
            .as_ref()
            .and_then(|s| s.chars().next())
            .map(|c| c as u32)
            .unwrap_or(0);
        self.events
            .push_back(WgpuEvent::key(event.keysym.raw(), unichar, self.mods));
    }

    fn release_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                   _: u32, _: KeyEvent) {}

    fn update_modifiers(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                        _: u32, modifiers: Modifiers, _: u32) {
        let mut m = 0;
        if modifiers.ctrl { m |= WGPU_MOD_CTRL; }
        if modifiers.alt { m |= WGPU_MOD_ALT; }
        if modifiers.shift { m |= WGPU_MOD_SHIFT; }
        if modifiers.logo { m |= WGPU_MOD_LOGO; }
        self.mods = m;
    }
}

impl ProvidesRegistryState for WinState {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState, SeatState];
}

delegate_compositor!(WinState);
delegate_output!(WinState);
delegate_seat!(WinState);
delegate_keyboard!(WinState);
delegate_xdg_shell!(WinState);
delegate_xdg_window!(WinState);
delegate_registry!(WinState);

// ----- FFI -----------------------------------------------------------------

fn with_window<R>(f: impl FnOnce(&mut WgpuWindow) -> R, default: R) -> R {
    WINDOW.with(|w| match w.borrow_mut().as_mut() {
        Some(win) => f(&mut **win),
        None => default,
    })
}

/// Open the (single) Emacs window. `title` is a NUL-terminated C string (may be
/// null for a default). Returns 0 on success, -1 on failure.
///
/// # Safety
/// `title`, if non-null, must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_open(title: *const c_char) -> c_int {
    let title = if title.is_null() {
        "emacs".to_string()
    } else {
        std::ffi::CStr::from_ptr(title).to_string_lossy().into_owned()
    };
    match WgpuWindow::open(&title) {
        Ok(win) => {
            WINDOW.with(|w| *w.borrow_mut() = Some(ManuallyDrop::new(win)));
            0
        }
        Err(e) => {
            eprintln!("wgpu_window_open: {e}");
            -1
        }
    }
}

/// Wayland connection fd for Emacs to select on. -1 if no window.
#[no_mangle]
pub extern "C" fn wgpu_window_fd() -> c_int {
    with_window(|win| win.conn.backend().poll_fd().as_raw_fd(), -1)
}

/// Process pending Wayland events. Returns 1 if the compositor asked the
/// window to close, else 0 (negative on error).
#[no_mangle]
pub extern "C" fn wgpu_window_dispatch() -> c_int {
    with_window(
        |win| match win.dispatch() {
            Ok(()) => {
                if win.state.close_requested {
                    1
                } else {
                    0
                }
            }
            Err(e) => {
                eprintln!("wgpu_window_dispatch: {e}");
                -1
            }
        },
        -1,
    )
}

/// Integer output scale factor (1, 2, ...) for HiDPI. 1 if no window.
#[no_mangle]
pub extern "C" fn wgpu_window_scale() -> c_int {
    with_window(|win| win.state.scale, 1)
}

/// Current window size in pixels, written to *w/*h.
///
/// # Safety
/// `w` and `h` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_size(w: *mut u32, h: *mut u32) {
    let (sw, sh) = with_window(|win| win.state.size, (0, 0));
    if !w.is_null() {
        *w = sw;
    }
    if !h.is_null() {
        *h = sh;
    }
}

/// Upload a coverage glyph (w*h bytes) into the window's atlas; returns id >=0.
///
/// # Safety
/// `data` must point to at least `len >= w*h` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_atlas_upload(w: u32, h: u32, data: *const u8, len: usize) -> i64 {
    if data.is_null() || len < (w as usize) * (h as usize) {
        return -1;
    }
    let bytes = std::slice::from_raw_parts(data, (w as usize) * (h as usize));
    with_window(
        |win| match win.state.renderer.add_glyph(&win.state.gpu, w, h, bytes) {
            Some(uv) => {
                win.state.glyphs.push(GlyphEntry { uv, w: w as f32, h: h as f32 });
                (win.state.glyphs.len() - 1) as i64
            }
            None => -1,
        },
        -1,
    )
}

/// Drain up to `max` pending input events into `buf`; returns the count.
/// Called from read_socket_hook.
///
/// # Safety
/// `buf` must point to writable storage for at least `max` `WgpuEvent`s.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_poll_events(buf: *mut WgpuEvent, max: c_int) -> c_int {
    if buf.is_null() || max <= 0 {
        return 0;
    }
    let out = std::slice::from_raw_parts_mut(buf, max as usize);
    with_window(
        |win| {
            let n = out.len().min(win.state.events.len());
            for slot in out.iter_mut().take(n) {
                *slot = win.state.events.pop_front().unwrap();
            }
            n as c_int
        },
        0,
    )
}

/// Start a new batch of draw commands.
#[no_mangle]
pub extern "C" fn wgpu_window_begin() {
    with_window(|win| win.state.cmds.clear(), ());
}

/// Record a solid filled rectangle (pixels, linear RGBA 0..=1).
#[no_mangle]
pub extern "C" fn wgpu_window_rect(x: f32, y: f32, w: f32, h: f32, r: f32, g: f32, b: f32, a: f32) {
    with_window(|win| win.state.cmds.push(DrawCmd::Rect { x, y, w, h, color: [r, g, b, a] }), ());
}

/// Record a glyph (atlas id from `wgpu_window_atlas_upload`) at x,y, tinted.
#[no_mangle]
pub extern "C" fn wgpu_window_glyph(id: i64, x: f32, y: f32, r: f32, g: f32, b: f32, a: f32) {
    with_window(
        |win| {
            if id >= 0 {
                if let Some(ge) = win.state.glyphs.get(id as usize) {
                    let (gw, gh, uv) = (ge.w, ge.h, ge.uv);
                    win.state.cmds.push(DrawCmd::Glyph { uv, x, y, w: gw, h: gh, color: [r, g, b, a] });
                }
            }
        },
        (),
    );
}

/// Composite the recorded commands and present to the window.
#[no_mangle]
pub extern "C" fn wgpu_window_present() {
    with_window(|win| win.state.present(), ());
}

/// If the compositor asked for a new size since the last call, write it to
/// *w/*h and return 1; else return 0. Emacs calls this from read_socket and
/// resizes the frame accordingly.
///
/// # Safety
/// `w` and `h` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_take_resize(w: *mut u32, h: *mut u32) -> c_int {
    with_window(
        |win| match win.state.pending_resize.take() {
            Some((rw, rh)) => {
                if !w.is_null() {
                    *w = rw;
                }
                if !h.is_null() {
                    *h = rh;
                }
                1
            }
            None => 0,
        },
        0,
    )
}

/// Destroy the window and release GPU/Wayland resources.
#[no_mangle]
pub extern "C" fn wgpu_window_close() {
    WINDOW.with(|w| *w.borrow_mut() = None);
}
