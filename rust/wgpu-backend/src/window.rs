//! Raw-Wayland window + wgpu surface, rendering via the shared `Renderer`.
//!
//! M1c brought up a clear color; this now draws a real command list (atlas
//! glyphs + fills) on the live surface using the same `gpu2d::Renderer` the
//! offscreen golden path uses — so what's pixel-verified headless is exactly
//! what the window presents. This is the seed of the Emacs render/event
//! thread: M2's RIF hooks will feed the command list instead of the demo.

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
    shell::{
        xdg::{
            window::{Window, WindowConfigure, WindowDecorations, WindowHandler},
            XdgShell,
        },
        WaylandSurface,
    },
};
use wayland_client::{
    globals::registry_queue_init,
    protocol::{wl_output, wl_surface},
    Connection, Proxy, QueueHandle,
};

use crate::gpu2d::{DrawCmd, Renderer};
use crate::render::Gpu;

/// Build the demo command list (matches the C `wgpu--draw-demo`): a row of
/// synthetic atlas glyphs plus a cursor fill.
fn demo_cmds(renderer: &mut Renderer, gpu: &Gpu) -> Vec<DrawCmd> {
    let glyph = [255u8; 6 * 10];
    let uv = renderer.add_glyph(gpu, 6, 10, &glyph).unwrap_or([0.0; 4]);
    let mut cmds = Vec::new();
    for i in 0..5 {
        cmds.push(DrawCmd::Glyph {
            uv,
            x: 8.0 + i as f32 * 12.0,
            y: 10.0,
            w: 6.0,
            h: 10.0,
            color: [0.9, 0.9, 0.9, 1.0],
        });
    }
    cmds.push(DrawCmd::Rect { x: 74.0, y: 8.0, w: 8.0, h: 14.0, color: [0.8, 0.1, 0.1, 1.0] });
    cmds
}

/// Open a Wayland window with `clear` as the background and present the demo
/// content via the shared renderer. Runs until closed, or until `max_frames`
/// have been presented (0 = until closed). Blocking.
pub fn run_windowed_clear(clear: [f64; 4], max_frames: u32) -> Result<(), String> {
    let conn = Connection::connect_to_env().map_err(|e| format!("wayland connect: {e}"))?;
    let (globals, mut event_queue) =
        registry_queue_init(&conn).map_err(|e| format!("registry init: {e}"))?;
    let qh: QueueHandle<App> = event_queue.handle();

    let compositor =
        CompositorState::bind(&globals, &qh).map_err(|e| format!("wl_compositor: {e}"))?;
    let xdg_shell = XdgShell::bind(&globals, &qh).map_err(|e| format!("xdg_wm_base: {e}"))?;

    let surface = compositor.create_surface(&qh);
    let window = xdg_shell.create_window(surface, WindowDecorations::RequestServer, &qh);
    window.set_title("emacs (wgpu)");
    window.set_app_id("org.gnu.emacs.wgpu");
    window.set_min_size(Some((320, 240)));
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
    let wgpu_surface = unsafe {
        instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_display_handle: raw_display,
            raw_window_handle: raw_window,
        })
    }
    .map_err(|e| format!("create_surface: {e}"))?;

    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::LowPower,
        compatible_surface: Some(&wgpu_surface),
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

    // Pick a surface format and build the renderer for it.
    let caps = wgpu_surface.get_capabilities(&gpu.adapter);
    let format = caps
        .formats
        .iter()
        .copied()
        .find(|f| f.is_srgb())
        .unwrap_or(caps.formats[0]);
    let alpha_mode = caps.alpha_modes[0];
    let mut renderer = Renderer::new(&gpu, format);
    let cmds = demo_cmds(&mut renderer, &gpu);

    eprintln!("wgpu window: adapter={} backend={:?}", gpu.adapter_info(), format);

    let mut app = App {
        registry_state: RegistryState::new(&globals),
        output_state: OutputState::new(&globals, &qh),
        window,
        surface: wgpu_surface,
        gpu,
        renderer,
        format,
        alpha_mode,
        clear,
        cmds,
        size: (640, 480),
        configured: false,
        exit: false,
        frames: 0,
        max_frames,
    };

    while !app.exit {
        event_queue
            .blocking_dispatch(&mut app)
            .map_err(|e| format!("dispatch: {e}"))?;
    }
    eprintln!("wgpu window: presented {} frame(s), exiting", app.frames);
    Ok(())
}

struct App {
    registry_state: RegistryState,
    output_state: OutputState,
    window: Window,
    surface: wgpu::Surface<'static>,
    gpu: Gpu,
    renderer: Renderer,
    format: wgpu::TextureFormat,
    alpha_mode: wgpu::CompositeAlphaMode,
    clear: [f64; 4],
    cmds: Vec<DrawCmd>,
    size: (u32, u32),
    configured: bool,
    exit: bool,
    frames: u32,
    max_frames: u32,
}

impl App {
    fn configure_surface(&mut self) {
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
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

    fn render(&mut self, qh: &QueueHandle<App>) {
        if !self.configured {
            return;
        }
        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.configure_surface();
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        self.renderer.encode(
            &self.gpu,
            &mut encoder,
            &view,
            self.size.0,
            self.size.1,
            self.clear,
            &self.cmds,
        );
        // Keep presenting.
        self.window.wl_surface().frame(qh, self.window.wl_surface().clone());
        self.gpu.queue.submit(Some(encoder.finish()));
        frame.present();

        self.frames += 1;
        if self.max_frames != 0 && self.frames >= self.max_frames {
            self.exit = true;
        }
    }
}

impl CompositorHandler for App {
    fn scale_factor_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: i32) {}
    fn transform_changed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: wl_output::Transform) {}
    fn frame(&mut self, _: &Connection, qh: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: u32) {
        self.render(qh);
    }
    fn surface_enter(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
    fn surface_leave(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: &wl_output::WlOutput) {}
}

impl WindowHandler for App {
    fn request_close(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &Window) {
        self.exit = true;
    }
    fn configure(&mut self, _: &Connection, qh: &QueueHandle<Self>, _: &Window, configure: WindowConfigure, _serial: u32) {
        if let (Some(w), Some(h)) = configure.new_size {
            self.size = (w.get(), h.get());
        }
        self.configure_surface();
        self.render(qh);
    }
}

impl OutputHandler for App {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl ProvidesRegistryState for App {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState];
}

delegate_compositor!(App);
delegate_output!(App);
delegate_xdg_shell!(App);
delegate_xdg_window!(App);
delegate_registry!(App);
