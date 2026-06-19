//! 2D quad renderer with a dynamic glyph atlas (M2 engine).
//!
//! Everything Emacs draws is expressed as a tinted textured quad:
//!   * solid fills (face backgrounds, cursor, borders) sample a fully-opaque
//!     "white" texel, so the tint color shows through directly;
//!   * glyphs sample their coverage from the atlas (R8), modulating the tint's
//!     alpha for anti-aliased text in the foreground color.
//!
//! Quads are emitted in draw order and blended (painter's algorithm), matching
//! Emacs's incremental, back-to-front drawing. This is the same approach used
//! by alacritty/wezterm/ghostty.
//!
//! Glyph bitmaps are uploaded by the caller (Emacs rasterizes via FreeType and
//! hands us coverage bitmaps); this module does no shaping.

use bytemuck::{Pod, Zeroable};

use crate::render::{create_target, texture_to_rgba, Gpu};

// 2048² R8 coverage atlas (~4 MiB): enough headroom for many fonts/sizes
// (bold, italic, multiple scales) before the shelf packer fills up.
const ATLAS_SIZE: u32 = 2048;

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Vertex {
    pos: [f32; 2],   // pixel coordinates (origin top-left)
    uv: [f32; 2],    // atlas texture coordinates, 0..1
    color: [f32; 4], // linear RGBA tint
}

/// A rectangle in atlas UV space (u0, v0, u1, v1).
pub type Uv = [f32; 4];

/// A draw command in frame pixel coordinates.
#[derive(Clone, Copy)]
pub enum DrawCmd {
    /// Solid filled rectangle (background, cursor, border, ...).
    Rect { x: f32, y: f32, w: f32, h: f32, color: [f32; 4] },
    /// A glyph (or any atlas sprite) drawn at x,y with size w,h, tinted.
    Glyph { uv: Uv, x: f32, y: f32, w: f32, h: f32, color: [f32; 4] },
}

/// Dynamic shelf-packed coverage atlas (single-channel R8).
struct Atlas {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    cursor_x: u32,
    cursor_y: u32,
    shelf_h: u32,
    /// UV of a fully-opaque texel used by solid `Rect` quads.
    white_uv: Uv,
}

impl Atlas {
    fn new(gpu: &Gpu) -> Atlas {
        let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("glyph atlas"),
            size: wgpu::Extent3d { width: ATLAS_SIZE, height: ATLAS_SIZE, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut atlas = Atlas {
            texture,
            view,
            cursor_x: 0,
            cursor_y: 0,
            shelf_h: 0,
            white_uv: [0.0; 4],
        };
        // Reserve a 2x2 fully-opaque block for solid fills; sample its center.
        let uv = atlas.insert(gpu, 2, 2, &[255u8; 4]).expect("white texel");
        let cx = (uv[0] + uv[2]) * 0.5;
        let cy = (uv[1] + uv[3]) * 0.5;
        atlas.white_uv = [cx, cy, cx, cy];
        atlas
    }

    /// Insert a `w`x`h` coverage bitmap (row-major, 1 byte/pixel). Returns its
    /// UV rectangle, or None if the atlas is full.
    fn insert(&mut self, gpu: &Gpu, w: u32, h: u32, bitmap: &[u8]) -> Option<Uv> {
        if w == 0 || h == 0 {
            return Some([0.0, 0.0, 0.0, 0.0]);
        }
        if self.cursor_x + w > ATLAS_SIZE {
            // New shelf.
            self.cursor_x = 0;
            self.cursor_y += self.shelf_h;
            self.shelf_h = 0;
        }
        if self.cursor_y + h > ATLAS_SIZE {
            return None; // full (M2: single atlas; eviction is later work)
        }
        let (x, y) = (self.cursor_x, self.cursor_y);
        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &self.texture,
                mip_level: 0,
                origin: wgpu::Origin3d { x, y, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            bitmap,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(w),
                rows_per_image: Some(h),
            },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.cursor_x += w;
        self.shelf_h = self.shelf_h.max(h);
        let s = ATLAS_SIZE as f32;
        Some([x as f32 / s, y as f32 / s, (x + w) as f32 / s, (y + h) as f32 / s])
    }
}

/// The 2D renderer: owns the atlas, pipeline, and uniform buffer.
pub struct Renderer {
    atlas: Atlas,
    pipeline: wgpu::RenderPipeline,
    bind_group: wgpu::BindGroup,
    uniform: wgpu::Buffer,
}

const SHADER: &str = r#"
struct Uniforms { viewport: vec2<f32> };
@group(0) @binding(0) var atlas_tex: texture_2d<f32>;
@group(0) @binding(1) var atlas_smp: sampler;
@group(0) @binding(2) var<uniform> u: Uniforms;

struct VsOut {
  @builtin(position) clip: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) color: vec4<f32>,
};

@vertex
fn vs(@location(0) pos: vec2<f32>,
      @location(1) uv: vec2<f32>,
      @location(2) color: vec4<f32>) -> VsOut {
  var o: VsOut;
  let ndc = vec2<f32>(pos.x / u.viewport.x * 2.0 - 1.0,
                      1.0 - pos.y / u.viewport.y * 2.0);
  o.clip = vec4<f32>(ndc, 0.0, 1.0);
  o.uv = uv;
  o.color = color;
  return o;
}

@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let coverage = textureSample(atlas_tex, atlas_smp, in.uv).r;
  return vec4<f32>(in.color.rgb, in.color.a * coverage);
}
"#;

impl Renderer {
    pub fn new(gpu: &Gpu, target_format: wgpu::TextureFormat) -> Renderer {
        let atlas = Atlas::new(gpu);

        let shader = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("gpu2d shader"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });

        // Nearest: glyphs are blitted 1:1 (atlas region size == quad pixel
        // size at integer positions), so point sampling keeps them crisp.
        let sampler = gpu.device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("atlas sampler"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let uniform = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("viewport uniform"),
            size: 16, // vec2<f32> padded to 16
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let bgl = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("gpu2d bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let bind_group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("gpu2d bg"),
            layout: &bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&atlas.view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&sampler) },
                wgpu::BindGroupEntry { binding: 2, resource: uniform.as_entire_binding() },
            ],
        });

        let layout = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("gpu2d layout"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });

        let vertex_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<Vertex>() as u64,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 0, shader_location: 0 },
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 8, shader_location: 1 },
                wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 16, shader_location: 2 },
            ],
        };

        let pipeline = gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("gpu2d pipeline"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs"),
                buffers: &[vertex_layout],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: target_format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
            cache: None,
        });

        Renderer { atlas, pipeline, bind_group, uniform }
    }

    /// Upload a coverage glyph bitmap; returns its atlas UV (None if full).
    pub fn add_glyph(&mut self, gpu: &Gpu, w: u32, h: u32, bitmap: &[u8]) -> Option<Uv> {
        self.atlas.insert(gpu, w, h, bitmap)
    }

    fn vertices(&self, cmds: &[DrawCmd]) -> Vec<Vertex> {
        let mut v = Vec::with_capacity(cmds.len() * 6);
        for cmd in cmds {
            let (x, y, w, h, uv, color) = match *cmd {
                DrawCmd::Rect { x, y, w, h, color } => {
                    let u = self.atlas.white_uv;
                    (x, y, w, h, u, color)
                }
                DrawCmd::Glyph { uv, x, y, w, h, color } => (x, y, w, h, uv, color),
            };
            let (l, t, r, b) = (x, y, x + w, y + h);
            let (u0, v0, u1, v1) = (uv[0], uv[1], uv[2], uv[3]);
            let tl = Vertex { pos: [l, t], uv: [u0, v0], color };
            let tr = Vertex { pos: [r, t], uv: [u1, v0], color };
            let bl = Vertex { pos: [l, b], uv: [u0, v1], color };
            let br = Vertex { pos: [r, b], uv: [u1, v1], color };
            v.extend_from_slice(&[tl, bl, br, tl, br, tr]);
        }
        v
    }

    /// Render `cmds` into `view` of the given pixel size, recording into
    /// `encoder`. `load` controls the attachment load op: `Clear(color)` to
    /// start fresh (offscreen frames), or `Load` to draw over an existing
    /// persistent target (the live window's incremental updates).
    pub fn encode(
        &self,
        gpu: &Gpu,
        encoder: &mut wgpu::CommandEncoder,
        view: &wgpu::TextureView,
        width: u32,
        height: u32,
        load: wgpu::LoadOp<wgpu::Color>,
        cmds: &[DrawCmd],
    ) {
        gpu.queue
            .write_buffer(&self.uniform, 0, bytemuck::cast_slice(&[width as f32, height as f32, 0.0f32, 0.0f32]));
        let verts = self.vertices(cmds);
        let vbuf = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("gpu2d verts"),
            size: (std::mem::size_of::<Vertex>() * verts.len().max(1)) as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        if !verts.is_empty() {
            gpu.queue.write_buffer(&vbuf, 0, bytemuck::cast_slice(&verts));
        }

        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("gpu2d pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
                resolve_target: None,
                ops: wgpu::Operations { load, store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
        });
        if !verts.is_empty() {
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &self.bind_group, &[]);
            pass.set_vertex_buffer(0, vbuf.slice(..));
            pass.draw(0..verts.len() as u32, 0..1);
        }
    }

    /// Convenience: render `cmds` offscreen and read back RGBA8.
    pub fn render_to_rgba(
        &self,
        gpu: &Gpu,
        width: u32,
        height: u32,
        clear: [f64; 4],
        cmds: &[DrawCmd],
    ) -> Result<Vec<u8>, String> {
        let target = create_target(gpu, width, height);
        let view = target.create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("gpu2d") });
        let load = wgpu::LoadOp::Clear(wgpu::Color {
            r: clear[0],
            g: clear[1],
            b: clear[2],
            a: clear[3],
        });
        self.encode(gpu, &mut encoder, &view, width, height, load, cmds);
        gpu.queue.submit(Some(encoder.finish()));
        texture_to_rgba(gpu, &target, width, height)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // GPU test (ignored by default; run with `cargo test -- --ignored`).
    #[test]
    #[ignore]
    fn draws_rect_and_glyph() {
        let gpu = Gpu::new_headless().expect("gpu");
        let mut r = Renderer::new(&gpu, crate::render::TARGET_FORMAT);

        // A 4x4 fully-opaque "glyph".
        let uv = r.add_glyph(&gpu, 4, 4, &[255u8; 16]).expect("atlas");

        let cmds = [
            // Red rect filling the left half.
            DrawCmd::Rect { x: 0.0, y: 0.0, w: 16.0, h: 32.0, color: [1.0, 0.0, 0.0, 1.0] },
            // White glyph block at (20,4).
            DrawCmd::Glyph { uv, x: 20.0, y: 4.0, w: 4.0, h: 4.0, color: [1.0, 1.0, 1.0, 1.0] },
        ];
        let px = r.render_to_rgba(&gpu, 32, 32, [0.0, 0.0, 0.0, 1.0], &cmds).expect("render");
        assert_eq!(px.len(), 32 * 32 * 4);

        let at = |x: usize, y: usize| {
            let i = (y * 32 + x) * 4;
            (px[i], px[i + 1], px[i + 2], px[i + 3])
        };
        // Inside the red rect.
        let (r0, g0, b0, _) = at(4, 4);
        assert!(r0 > 200 && g0 < 40 && b0 < 40, "red rect: {:?}", at(4, 4));
        // Background (far right, bottom) stays black.
        let (r1, g1, b1, _) = at(30, 30);
        assert!(r1 < 16 && g1 < 16 && b1 < 16, "bg: {:?}", at(30, 30));
        // Inside the glyph block: white-ish.
        let (r2, g2, b2, _) = at(21, 5);
        assert!(r2 > 200 && g2 > 200 && b2 > 200, "glyph: {:?}", at(21, 5));
    }
}
