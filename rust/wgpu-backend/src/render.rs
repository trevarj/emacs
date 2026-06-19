//! GPU rendering via wgpu.
//!
//! M1c provides the minimum: clear a render target to a solid color, both
//! offscreen (deterministic, headless, used by the golden-image harness) and
//! to a Wayland window (see `window` module). Glyph atlas + command replay
//! land in M2.

/// COPY_BYTES_PER_ROW_ALIGNMENT: texture->buffer copies need rows aligned.
const ROW_ALIGN: u32 = 256;

fn padded_bytes_per_row(width: u32) -> u32 {
    let unpadded = width * 4;
    let rem = unpadded % ROW_ALIGN;
    if rem == 0 { unpadded } else { unpadded + (ROW_ALIGN - rem) }
}

/// A wgpu device/queue pair, plus the instance that owns them.
pub struct Gpu {
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
}

impl Gpu {
    /// Create a GPU context not tied to any surface (for offscreen rendering).
    pub fn new_headless() -> Result<Gpu, String> {
        Self::new_with(None)
    }

    /// Create a GPU context, optionally requiring compatibility with `surface`.
    pub fn new_with(compatible: Option<&wgpu::Surface<'_>>) -> Result<Gpu, String> {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN | wgpu::Backends::GL,
            ..Default::default()
        });
        pollster::block_on(async {
            let adapter = instance
                .request_adapter(&wgpu::RequestAdapterOptions {
                    power_preference: wgpu::PowerPreference::LowPower,
                    compatible_surface: compatible,
                    force_fallback_adapter: false,
                })
                .await
                .ok_or_else(|| "no suitable GPU adapter".to_string())?;
            // Use the adapter's real limits so large Emacs frames (textures
            // wider than the conservative 2048 downlevel cap) are allowed.
            let required_limits = adapter.limits();
            let (device, queue) = adapter
                .request_device(&wgpu::DeviceDescriptor {
                    label: Some("wgpu-emacs device"),
                    required_features: wgpu::Features::empty(),
                    required_limits,
                    memory_hints: wgpu::MemoryHints::Performance,
                }, None)
                .await
                .map_err(|e| format!("request_device failed: {e}"))?;
            Ok(Gpu { instance, adapter, device, queue })
        })
    }

    pub fn adapter_info(&self) -> String {
        let i = self.adapter.get_info();
        format!("{} ({:?}, {:?})", i.name, i.backend, i.device_type)
    }
}

/// Render an offscreen frame cleared to `color` (linear RGBA, 0..=1) and read
/// the pixels back as tightly-packed RGBA8 (`width*height*4` bytes, row-major,
/// top-down). Deterministic; needs a Vulkan/GL device but no display.
pub fn render_offscreen_clear(
    gpu: &Gpu,
    width: u32,
    height: u32,
    color: [f64; 4],
) -> Result<Vec<u8>, String> {
    let format = wgpu::TextureFormat::Rgba8UnormSrgb;
    let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("offscreen target"),
        size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

    let bpr = padded_bytes_per_row(width);
    let readback = gpu.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: (bpr * height) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });

    let mut encoder = gpu
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("clear") });
    {
        encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("clear pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &view,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color {
                        r: color[0],
                        g: color[1],
                        b: color[2],
                        a: color[3],
                    }),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
        });
    }
    encoder.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: &texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &readback,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(bpr),
                rows_per_image: Some(height),
            },
        },
        wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
    );
    gpu.queue.submit(Some(encoder.finish()));

    // Map and read back, dropping row padding.
    let slice = readback.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    gpu.device.poll(wgpu::Maintain::Wait);
    rx.recv()
        .map_err(|e| format!("map channel: {e}"))?
        .map_err(|e| format!("buffer map: {e:?}"))?;

    let data = slice.get_mapped_range();
    let mut out = Vec::with_capacity((width * height * 4) as usize);
    for row in 0..height {
        let start = (row * bpr) as usize;
        out.extend_from_slice(&data[start..start + (width * 4) as usize]);
    }
    drop(data);
    readback.unmap();
    Ok(out)
}

/// Encode tightly-packed RGBA8 pixels as a PNG byte stream.
pub fn encode_png(rgba: &[u8], width: u32, height: u32) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut out, width, height);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc.write_header().map_err(|e| e.to_string())?;
        writer.write_image_data(rgba).map_err(|e| e.to_string())?;
    }
    Ok(out)
}

/// A process-wide headless GPU, created on first use. `None` if no device is
/// available. Cached so repeated dumps don't re-init wgpu each call.
pub fn shared_headless_gpu() -> Option<&'static Gpu> {
    use std::sync::OnceLock;
    static GPU: OnceLock<Option<Gpu>> = OnceLock::new();
    GPU.get_or_init(|| match Gpu::new_headless() {
        Ok(g) => Some(g),
        Err(e) => {
            eprintln!("wgpu: headless GPU unavailable: {e}");
            None
        }
    })
    .as_ref()
}

#[cfg(test)]
mod tests {
    use super::*;

    // Pure helper test, always runs.
    #[test]
    fn row_padding_aligns_to_256() {
        assert_eq!(padded_bytes_per_row(1), 256);
        assert_eq!(padded_bytes_per_row(64), 256); // 64*4 == 256 exactly
        assert_eq!(padded_bytes_per_row(65), 512);
    }

    // GPU test: needs a Vulkan/GL device. Ignored by default so plain
    // `cargo test` stays hermetic; run with `cargo test -- --ignored`.
    #[test]
    #[ignore]
    fn offscreen_clear_is_solid_color() {
        let gpu = Gpu::new_headless().expect("gpu");
        let px = render_offscreen_clear(&gpu, 8, 8, [0.0, 0.0, 1.0, 1.0]).expect("render");
        assert_eq!(px.len(), 8 * 8 * 4);
        // Blue in sRGB8: R≈0, G≈0, B=255, A=255.
        assert_eq!(px[2], 255);
        assert_eq!(px[3], 255);
        assert!(px[0] < 8 && px[1] < 8);
    }
}
