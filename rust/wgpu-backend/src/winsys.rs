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
        keyboard::{KeyEvent, KeyboardHandler, Keysym, Modifiers, RepeatInfo},
        pointer::{PointerEvent, PointerEventKind, PointerHandler, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT},
        Capability, SeatHandler, SeatState,
    },
    shell::{
        xdg::{
            window::{Window, WindowConfigure, WindowDecorations, WindowHandler},
            XdgShell,
        },
        WaylandSurface,
    },
    delegate_seat, delegate_keyboard, delegate_pointer,
};
use wayland_client::{
    globals::registry_queue_init,
    protocol::{
        wl_keyboard::WlKeyboard, wl_output, wl_pointer::WlPointer, wl_seat::WlSeat, wl_surface,
    },
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

/// sctk delegate target + render state.
struct WinState {
    registry_state: RegistryState,
    output_state: OutputState,
    seat_state: SeatState,
    keyboard: Option<WlKeyboard>,
    pointer: Option<WlPointer>,
    /// Last pointer position (surface pixels), for axis events that omit it.
    pointer_pos: (i32, i32),
    /// Current modifier mask (WGPU_MOD_*).
    mods: u32,
    /// Key repeat (Wayland delegates auto-repeat to the client).  We use a
    /// timerfd that Emacs selects on; when it fires we re-emit the held key.
    timer_fd: i32,
    repeat_delay_ms: u32,
    repeat_rate_ms: u32,
    /// Currently-held repeating key: (raw keycode, keysym, unichar).
    repeat: Option<(u32, u32, u32)>,
    /// Pending input events for Emacs's read_socket.
    events: VecDeque<WgpuEvent>,
    window: Window,
    surface: wgpu::Surface<'static>,
    gpu: Gpu,
    renderer: Renderer,
    format: wgpu::TextureFormat,
    alpha_mode: wgpu::CompositeAlphaMode,
    present_mode: wgpu::PresentMode,
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

        // Prefer a non-sRGB (UNORM) surface so our already-sRGB-encoded colors
        // pass through and glyph AA blends in gamma space (crisp text).
        let caps = surface.get_capabilities(&gpu.adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| !f.is_srgb())
            .unwrap_or(caps.formats[0]);
        let alpha_mode = caps.alpha_modes[0];
        // Prefer Mailbox: it never blocks get_current_texture (which, with
        // Fifo, can stall Emacs's main thread indefinitely when the window is
        // occluded/unfocused -- a hard freeze).  Fall back to Fifo.
        let present_mode = if caps.present_modes.contains(&wgpu::PresentMode::Mailbox) {
            wgpu::PresentMode::Mailbox
        } else {
            wgpu::PresentMode::Fifo
        };
        let renderer = Renderer::new(&gpu, format);

        let mut state = WinState {
            registry_state: RegistryState::new(&globals),
            output_state: OutputState::new(&globals, &qh),
            seat_state: SeatState::new(&globals, &qh),
            keyboard: None,
            pointer: None,
            pointer_pos: (0, 0),
            mods: 0,
            // CLOCK_MONOTONIC, non-blocking so draining never stalls Emacs.
            timer_fd: unsafe {
                libc::timerfd_create(libc::CLOCK_MONOTONIC,
                                     libc::TFD_NONBLOCK | libc::TFD_CLOEXEC)
            },
            // Sensible defaults until the compositor sends repeat_info.
            repeat_delay_ms: 400,
            repeat_rate_ms: 33,
            repeat: None,
            events: VecDeque::new(),
            window,
            surface,
            gpu,
            renderer,
            format,
            alpha_mode,
            present_mode,
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
        // Drain the key-repeat timerfd (it may be why Emacs woke us).
        self.state.pump_repeat();
        Ok(())
    }
}

impl WinState {
    /// Arm the repeat timerfd: first fire after `delay`, then every `rate` ms.
    fn arm_repeat(&self) {
        if self.timer_fd < 0 {
            return;
        }
        let spec = repeat_itimerspec(self.repeat_delay_ms, self.repeat_rate_ms);
        unsafe { libc::timerfd_settime(self.timer_fd, 0, &spec, std::ptr::null_mut()) };
    }

    /// Disarm the repeat timerfd (no more repeats) and forget the held key.
    fn disarm_repeat(&mut self) {
        self.repeat = None;
        if self.timer_fd < 0 {
            return;
        }
        let spec: libc::itimerspec = unsafe { std::mem::zeroed() };
        unsafe { libc::timerfd_settime(self.timer_fd, 0, &spec, std::ptr::null_mut()) };
    }

    /// Drain the repeat timerfd; for each expiration, re-emit the held key.
    fn pump_repeat(&mut self) {
        if self.timer_fd < 0 {
            return;
        }
        let mut buf = [0u8; 8];
        let n = unsafe {
            libc::read(self.timer_fd, buf.as_mut_ptr() as *mut libc::c_void, 8)
        };
        if n != 8 {
            return; // EAGAIN (not fired) or error
        }
        let expirations = u64::from_ne_bytes(buf);
        if let Some((_, ks, unichar)) = self.repeat {
            // Cap bursts so a stalled main loop doesn't flood the queue.
            for _ in 0..expirations.min(4) {
                self.events.push_back(WgpuEvent::key(ks, unichar, self.mods));
            }
        }
    }

    fn configure_surface(&mut self) {
        let config = wgpu::SurfaceConfiguration {
            // COPY_DST so we can copy the persistent texture into the frame.
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_DST,
            format: self.format,
            width: self.size.0.max(1),
            height: self.size.1.max(1),
            present_mode: self.present_mode,
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
                // COPY_DST so scroll can copy a shifted region back in.
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::COPY_SRC
                    | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            self.persistent = Some(tex);
            self.persistent_size = self.size;
        }
    }

    /// Scroll a rectangular region of the persistent texture vertically:
    /// copy [x, from_y, w, h] to [x, to_y, w, h].  Uses a temp texture because
    /// wgpu forbids overlapping intra-texture copies.  Executed immediately so
    /// that the newly-exposed lines (recorded afterwards) composite on top at
    /// the next present.
    fn scroll(&mut self, x: u32, from_y: u32, w: u32, mut h: u32, to_y: u32) {
        if w == 0 || h == 0 {
            return;
        }
        self.ensure_persistent();
        let (tw, th) = self.persistent_size;
        if x >= tw || from_y >= th || to_y >= th {
            return;
        }
        let w = w.min(tw - x);
        // Clamp height so neither the source nor destination exceeds the texture.
        h = h.min(th - from_y).min(th - to_y);
        if w == 0 || h == 0 {
            return;
        }
        let persistent = self.persistent.as_ref().unwrap();
        let temp = self.gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("scroll temp"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: self.format,
            usage: wgpu::TextureUsages::COPY_SRC | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let mut encoder = self
            .gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("scroll") });
        let extent = wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 };
        encoder.copy_texture_to_texture(
            wgpu::TexelCopyTextureInfo {
                texture: persistent,
                mip_level: 0,
                origin: wgpu::Origin3d { x, y: from_y, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyTextureInfo {
                texture: &temp,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            extent,
        );
        encoder.copy_texture_to_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &temp,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyTextureInfo {
                texture: persistent,
                mip_level: 0,
                origin: wgpu::Origin3d { x, y: to_y, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            extent,
        );
        self.gpu.queue.submit(Some(encoder.finish()));
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
        if capability == Capability::Pointer && self.pointer.is_none() {
            match self.seat_state.get_pointer(qh, &seat) {
                Ok(ptr) => self.pointer = Some(ptr),
                Err(e) => eprintln!("wgpu: get_pointer failed: {e}"),
            }
        }
    }
    fn remove_capability(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat, capability: Capability) {
        if capability == Capability::Keyboard {
            if let Some(kbd) = self.keyboard.take() {
                kbd.release ();
            }
        }
        if capability == Capability::Pointer {
            if let Some(ptr) = self.pointer.take() {
                ptr.release ();
            }
        }
    }
    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
}

impl KeyboardHandler for WinState {
    fn enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             _: &wl_surface::WlSurface, _: u32, _: &[u32], _: &[Keysym]) {}
    fn leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
             _: &wl_surface::WlSurface, _: u32) {
        // Lost focus: stop repeating so a held key doesn't keep firing.
        self.disarm_repeat();
    }

    fn press_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                 _: u32, event: KeyEvent) {
        let ks = event.keysym.raw();
        // Modifier/lock keys are tracked via update_modifiers; they must not
        // be delivered as keystrokes (otherwise e.g. Super_L = 0xffeb arrives
        // as a bogus key and breaks prefix sequences).
        if is_modifier_keysym(ks) {
            return;
        }
        let unichar = event
            .utf8
            .as_ref()
            .and_then(|s| s.chars().next())
            .map(|c| c as u32)
            .unwrap_or(0);
        self.events.push_back(WgpuEvent::key(ks, unichar, self.mods));
        // Begin auto-repeat for this key (Wayland leaves repeat to the client).
        if self.repeat_rate_ms > 0 {
            self.repeat = Some((event.raw_code, ks, unichar));
            self.arm_repeat();
        }
    }

    fn release_key(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlKeyboard,
                   _: u32, event: KeyEvent) {
        // Only stop repeating if the released key is the one repeating; this
        // keeps a newer held key repeating if an older one is let go.
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
        if modifiers.ctrl { m |= WGPU_MOD_CTRL; }
        if modifiers.alt { m |= WGPU_MOD_ALT; }
        if modifiers.shift { m |= WGPU_MOD_SHIFT; }
        if modifiers.logo { m |= WGPU_MOD_LOGO; }
        self.mods = m;
    }
}

/// Map a Linux/Wayland button code to an Emacs button number.
/// Emacs uses 0=left (mouse-1), 1=middle (mouse-2), 2=right (mouse-3).
fn button_to_emacs(btn: u32) -> u32 {
    match btn {
        BTN_LEFT => 0,
        BTN_MIDDLE => 1,
        BTN_RIGHT => 2,
        // Side/extra and others: 0x113 -> 3, 0x114 -> 4, ...
        other => other.wrapping_sub(BTN_LEFT),
    }
}

/// Reduce an axis to a signed step count (notches), preferring the discrete
/// value and falling back to the sign of the pixel delta for touchpads.
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

impl PointerHandler for WinState {
    fn pointer_frame(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &WlPointer,
                     events: &[PointerEvent]) {
        for e in events {
            let x = e.position.0 as i32;
            let y = e.position.1 as i32;
            match &e.kind {
                PointerEventKind::Enter { .. } => {
                    self.pointer_pos = (x, y);
                    self.events.push_back(WgpuEvent::motion(x, y, self.mods, 0));
                }
                PointerEventKind::Motion { time } => {
                    self.pointer_pos = (x, y);
                    self.events.push_back(WgpuEvent::motion(x, y, self.mods, *time));
                }
                PointerEventKind::Leave { .. } => {}
                PointerEventKind::Press { time, button, .. } => {
                    self.pointer_pos = (x, y);
                    self.events.push_back(WgpuEvent::button(
                        true, button_to_emacs(*button), x, y, self.mods, *time));
                }
                PointerEventKind::Release { time, button, .. } => {
                    self.pointer_pos = (x, y);
                    self.events.push_back(WgpuEvent::button(
                        false, button_to_emacs(*button), x, y, self.mods, *time));
                }
                PointerEventKind::Axis { time, horizontal, vertical, .. } => {
                    let hx = axis_steps(horizontal);
                    let vy = axis_steps(vertical);
                    if hx != 0 || vy != 0 {
                        let (px, py) = self.pointer_pos;
                        self.events.push_back(
                            WgpuEvent::axis(hx, vy, px, py, self.mods, *time));
                    }
                }
            }
        }
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
delegate_pointer!(WinState);
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

/// Key-repeat timerfd for Emacs to select on (so it wakes to emit repeats).
/// -1 if unavailable.
#[no_mangle]
pub extern "C" fn wgpu_window_timer_fd() -> c_int {
    with_window(|win| win.state.timer_fd, -1)
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

/// Begin a redisplay batch.  NOTE: this must NOT clear the command list --
/// Emacs calls update_begin per window/region (many times) with a single
/// frame_up_to_date at the end; clearing here would drop all but the last
/// update and cause heavy artifacting.  Commands accumulate and are cleared by
/// `present` after they are composited onto the persistent texture.
#[no_mangle]
pub extern "C" fn wgpu_window_begin() {}

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

/// Scroll the region [x, from_y, w, h] of the persistent frame to [x, to_y].
/// Called from scroll_run before the newly-exposed lines are drawn.
#[no_mangle]
pub extern "C" fn wgpu_window_scroll(x: c_int, from_y: c_int, w: c_int, h: c_int, to_y: c_int) {
    if x < 0 || from_y < 0 || to_y < 0 || w <= 0 || h <= 0 {
        return;
    }
    with_window(
        |win| win.state.scroll(x as u32, from_y as u32, w as u32, h as u32, to_y as u32),
        (),
    );
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

/// Read the window's current frame (persistent texture) back and write it as a
/// PNG to `path`.  For clarity inspection / real-text golden tests.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgpu_window_dump_png(path: *const c_char) -> c_int {
    if path.is_null() {
        return -1;
    }
    let path = match std::ffi::CStr::from_ptr(path).to_str() {
        Ok(p) => p.to_owned(),
        Err(_) => return -1,
    };
    with_window(
        |win| {
            let Some(tex) = win.state.persistent.as_ref() else {
                return -1;
            };
            let (w, h) = win.state.persistent_size;
            let is_bgra = matches!(
                win.state.format,
                wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Bgra8UnormSrgb
            );
            match crate::render::texture_to_rgba(&win.state.gpu, tex, w, h) {
                Ok(mut px) => {
                    if is_bgra {
                        for p in px.chunks_exact_mut(4) {
                            p.swap(0, 2); // BGRA -> RGBA
                        }
                    }
                    match crate::render::encode_png(&px, w, h)
                        .and_then(|png| std::fs::write(&path, png).map_err(|e| e.to_string()))
                    {
                        Ok(()) => 0,
                        Err(e) => {
                            eprintln!("wgpu_window_dump_png: {e}");
                            -1
                        }
                    }
                }
                Err(e) => {
                    eprintln!("wgpu_window_dump_png: {e}");
                    -1
                }
            }
        },
        -1,
    )
}

/// Destroy the window and release GPU/Wayland resources.
#[no_mangle]
pub extern "C" fn wgpu_window_close() {
    WINDOW.with(|w| *w.borrow_mut() = None);
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

    // End-to-end of the OS mechanism we rely on for repeat: arm a timerfd the
    // same way arm_repeat does, and confirm it fires repeatedly so pump_repeat
    // can drain it.
    #[test]
    fn timerfd_arms_and_repeats() {
        let fd = unsafe {
            libc::timerfd_create(libc::CLOCK_MONOTONIC,
                                 libc::TFD_NONBLOCK | libc::TFD_CLOEXEC)
        };
        assert!(fd >= 0, "timerfd_create failed");
        let spec = repeat_itimerspec(30, 20);
        let rc = unsafe { libc::timerfd_settime(fd, 0, &spec, std::ptr::null_mut()) };
        assert_eq!(rc, 0, "timerfd_settime failed");

        // Before the delay elapses it has not fired (non-blocking read => EAGAIN).
        let mut buf = [0u8; 8];
        let early = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        assert!(early < 0, "timer fired too early");

        std::thread::sleep(std::time::Duration::from_millis(120));
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        assert_eq!(n, 8, "expected an expiration count");
        let count = u64::from_ne_bytes(buf);
        assert!(count >= 1, "timer should have fired, got {count}");
        unsafe { libc::close(fd) };
    }
}
