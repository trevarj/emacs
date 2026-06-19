//! Stateful frame-command FFI.
//!
//! This is the interface the C redisplay hooks (RIF) drive: begin a frame,
//! push fills and glyphs in draw order, then present (to a window, M2 later)
//! or read back / encode (offscreen, for the golden harness and
//! `wgpu-dump-frame`).
//!
//! Glyph coverage bitmaps are uploaded once into the renderer's atlas and
//! referenced by id on subsequent frames, so the C side (which rasterizes via
//! FreeType) caches them exactly as the real glyph cache will.
//!
//! A single global frame builder is fine: Emacs drives redisplay from one
//! thread.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::sync::{Mutex, MutexGuard, OnceLock};

use crate::gpu2d::{DrawCmd, Renderer, Uv};
use crate::render::{encode_png, Gpu};

struct GlyphEntry {
    uv: Uv,
    w: f32,
    h: f32,
}

struct Frame {
    gpu: Gpu,
    renderer: Renderer,
    glyphs: Vec<GlyphEntry>,
    cmds: Vec<DrawCmd>,
    clear: [f64; 4],
    width: u32,
    height: u32,
}

static FRAME: OnceLock<Mutex<Option<Frame>>> = OnceLock::new();

fn lock() -> MutexGuard<'static, Option<Frame>> {
    FRAME
        .get_or_init(|| Mutex::new(None))
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

/// Lazily create the offscreen GPU + renderer the first time it's needed.
fn ensure(slot: &mut Option<Frame>) -> Option<&mut Frame> {
    if slot.is_none() {
        let gpu = match Gpu::new_headless() {
            Ok(g) => g,
            Err(e) => {
                eprintln!("wgpu ffi: no GPU: {e}");
                return None;
            }
        };
        let renderer = Renderer::new(&gpu, crate::render::TARGET_FORMAT);
        *slot = Some(Frame {
            gpu,
            renderer,
            glyphs: Vec::new(),
            cmds: Vec::new(),
            clear: [0.0; 4],
            width: 0,
            height: 0,
        });
    }
    slot.as_mut()
}

/// Begin a frame of `width`x`height` with the given clear color (linear RGBA).
/// Returns 0 on success, -1 if no GPU.
#[no_mangle]
pub extern "C" fn wgpu_frame_begin(
    width: u32,
    height: u32,
    r: f64,
    g: f64,
    b: f64,
    a: f64,
) -> c_int {
    let mut slot = lock();
    let Some(f) = ensure(&mut slot) else { return -1 };
    f.cmds.clear();
    f.clear = [r, g, b, a];
    f.width = width;
    f.height = height;
    0
}

/// Upload a `w`x`h` coverage bitmap (1 byte/pixel, row-major) into the glyph
/// atlas. Returns a glyph id (>= 0) for later `wgpu_frame_glyph` calls, or -1.
///
/// # Safety
/// `data` must point to at least `len` readable bytes, `len >= w*h`.
#[no_mangle]
pub unsafe extern "C" fn wgpu_atlas_upload(
    w: u32,
    h: u32,
    data: *const u8,
    len: usize,
) -> i64 {
    if data.is_null() || len < (w as usize) * (h as usize) {
        return -1;
    }
    let bytes = std::slice::from_raw_parts(data, (w as usize) * (h as usize));
    let mut slot = lock();
    let Some(f) = ensure(&mut slot) else { return -1 };
    match f.renderer.add_glyph(&f.gpu, w, h, bytes) {
        Some(uv) => {
            f.glyphs.push(GlyphEntry { uv, w: w as f32, h: h as f32 });
            (f.glyphs.len() - 1) as i64
        }
        None => -1,
    }
}

/// Record a solid filled rectangle (pixels, linear RGBA).
#[no_mangle]
pub extern "C" fn wgpu_frame_rect(
    x: f32,
    y: f32,
    w: f32,
    h: f32,
    r: f32,
    g: f32,
    b: f32,
    a: f32,
) {
    let mut slot = lock();
    if let Some(f) = slot.as_mut() {
        f.cmds.push(DrawCmd::Rect { x, y, w, h, color: [r, g, b, a] });
    }
}

/// Record a glyph (by id from `wgpu_atlas_upload`) at pen position x,y, tinted.
/// The glyph is drawn at its uploaded pixel size.
#[no_mangle]
pub extern "C" fn wgpu_frame_glyph(
    id: i64,
    x: f32,
    y: f32,
    r: f32,
    g: f32,
    b: f32,
    a: f32,
) {
    let mut slot = lock();
    if let Some(f) = slot.as_mut() {
        if id >= 0 {
            if let Some(ge) = f.glyphs.get(id as usize) {
                let (w, h, uv) = (ge.w, ge.h, ge.uv);
                f.cmds.push(DrawCmd::Glyph { uv, x, y, w, h, color: [r, g, b, a] });
            }
        }
    }
}

/// Render the recorded frame offscreen and copy RGBA8 into `out`
/// (`>= width*height*4` bytes). Returns 0 / -1. Does not clear the commands,
/// so it may be called more than once per frame.
///
/// # Safety
/// `out` must point to writable storage of at least `out_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn wgpu_frame_end_rgba(out: *mut u8, out_len: usize) -> c_int {
    let mut slot = lock();
    let Some(f) = slot.as_mut() else { return -1 };
    let need = (f.width as usize) * (f.height as usize) * 4;
    if out.is_null() || out_len < need {
        return -1;
    }
    match f
        .renderer
        .render_to_rgba(&f.gpu, f.width, f.height, f.clear, &f.cmds)
    {
        Ok(px) => {
            std::ptr::copy_nonoverlapping(px.as_ptr(), out, need);
            0
        }
        Err(e) => {
            eprintln!("wgpu_frame_end_rgba: {e}");
            -1
        }
    }
}

/// Render the recorded frame offscreen and write it as a PNG to `path`.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgpu_frame_end_png(path: *const c_char) -> c_int {
    if path.is_null() {
        return -1;
    }
    let path = match CStr::from_ptr(path).to_str() {
        Ok(p) => p,
        Err(_) => return -1,
    };
    let mut slot = lock();
    let Some(f) = slot.as_mut() else { return -1 };
    let res = f
        .renderer
        .render_to_rgba(&f.gpu, f.width, f.height, f.clear, &f.cmds)
        .and_then(|px| encode_png(&px, f.width, f.height))
        .and_then(|png| std::fs::write(path, png).map_err(|e| e.to_string()));
    match res {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("wgpu_frame_end_png: {e}");
            -1
        }
    }
}
