//! wgpu-backend: a Wayland + wgpu graphical backend for Emacs.
//!
//! This crate owns everything platform/GPU: the Wayland connection
//! (smithay-client-toolkit), the wgpu device/surface, the glyph atlas, the
//! render thread, and input decoding. The Emacs C side (`src/wgputerm.c` et al.)
//! is a thin shim that populates the redisplay interface + terminal hooks and
//! forwards each call across this FFI boundary.
//!
//! M0 (this scaffold): ABI-stable types + stub exports that build, link, and
//! pass `cargo test`. No Wayland/wgpu yet — those land in M1.
//!
//! FFI naming convention: every exported symbol is prefixed `wgpu_backend_`
//! (lifecycle/global) or `wgpu_` (per-frame/draw ops). The C header is generated
//! from this crate by cbindgen into `src/wgpu_ffi.h`.

pub mod event;
pub mod render;
pub mod window;

pub use event::{WgpuEvent, WgpuEventKind};

use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::sync::OnceLock;

/// Status codes returned across the FFI boundary.
pub const WGPU_OK: c_int = 0;
pub const WGPU_ERR_INIT: c_int = -1;

/// Initialize the backend process-global state. Idempotent. Returns `WGPU_OK`
/// on success. M0: no-op scaffold.
#[no_mangle]
pub extern "C" fn wgpu_backend_init() -> c_int {
    WGPU_OK
}

/// Tear down process-global backend state. M0: no-op.
#[no_mangle]
pub extern "C" fn wgpu_backend_shutdown() {}

/// Backend version string (static, NUL-terminated, owned by the backend).
/// The C side must not free it.
#[no_mangle]
pub extern "C" fn wgpu_backend_version() -> *const c_char {
    static V: OnceLock<CString> = OnceLock::new();
    V.get_or_init(|| CString::new(env!("CARGO_PKG_VERSION")).expect("version has no NUL"))
        .as_ptr()
}

/// File descriptor Emacs should add to its `select` set to be woken when input
/// is available. Returns -1 until a window/event-thread exists (M0 always -1).
#[no_mangle]
pub extern "C" fn wgpu_backend_wakeup_fd() -> c_int {
    -1
}

/// Drain up to `max` pending input events into the C-provided buffer `buf`,
/// returning the number written. Called from `read_socket_hook`.
///
/// # Safety
/// `buf` must point to writable storage for at least `max` `WgpuEvent` values,
/// or be null when `max == 0`.
#[no_mangle]
pub unsafe extern "C" fn wgpu_backend_poll_events(buf: *mut WgpuEvent, max: c_int) -> c_int {
    if buf.is_null() || max <= 0 {
        return 0;
    }
    // M0: no event source wired yet; nothing to drain.
    let _out = std::slice::from_raw_parts_mut(buf, max as usize);
    0
}

/// Render an offscreen frame of `width`x`height` cleared to the given color
/// (linear RGBA, 0..=1) and write tightly-packed RGBA8 into `out` (which must
/// have room for `out_len` >= width*height*4 bytes). Returns `WGPU_OK` or a
/// negative error code. Used by the golden-image test harness.
///
/// # Safety
/// `out` must point to writable storage of at least `out_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn wgpu_render_clear_rgba(
    width: u32,
    height: u32,
    r: f64,
    g: f64,
    b: f64,
    a: f64,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    let need = (width as usize) * (height as usize) * 4;
    if out.is_null() || out_len < need {
        return WGPU_ERR_INIT;
    }
    let Some(gpu) = render::shared_headless_gpu() else {
        return WGPU_ERR_INIT;
    };
    match render::render_offscreen_clear(gpu, width, height, [r, g, b, a]) {
        Ok(px) => {
            std::ptr::copy_nonoverlapping(px.as_ptr(), out, need);
            WGPU_OK
        }
        Err(e) => {
            eprintln!("wgpu_render_clear_rgba: {e}");
            WGPU_ERR_INIT
        }
    }
}

/// Render an offscreen clear frame and write it as a PNG to `path`.
/// Returns `WGPU_OK` or a negative error code.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgpu_render_clear_to_png(
    path: *const c_char,
    width: u32,
    height: u32,
    r: f64,
    g: f64,
    b: f64,
    a: f64,
) -> c_int {
    if path.is_null() {
        return WGPU_ERR_INIT;
    }
    let path = match std::ffi::CStr::from_ptr(path).to_str() {
        Ok(p) => p,
        Err(_) => return WGPU_ERR_INIT,
    };
    let Some(gpu) = render::shared_headless_gpu() else {
        return WGPU_ERR_INIT;
    };
    let result = render::render_offscreen_clear(gpu, width, height, [r, g, b, a])
        .and_then(|px| render::encode_png(&px, width, height))
        .and_then(|png| std::fs::write(path, png).map_err(|e| e.to_string()));
    match result {
        Ok(()) => WGPU_OK,
        Err(e) => {
            eprintln!("wgpu_render_clear_to_png: {e}");
            WGPU_ERR_INIT
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn init_then_shutdown_is_ok() {
        assert_eq!(wgpu_backend_init(), WGPU_OK);
        wgpu_backend_shutdown();
    }

    #[test]
    fn version_is_nonempty_cstr() {
        let p = wgpu_backend_version();
        assert!(!p.is_null());
        let s = unsafe { CStr::from_ptr(p) }.to_str().unwrap();
        assert!(!s.is_empty());
        // Stable pointer across calls (interned).
        assert_eq!(p, wgpu_backend_version());
    }

    #[test]
    fn wakeup_fd_is_minus_one_until_window_exists() {
        assert_eq!(wgpu_backend_wakeup_fd(), -1);
    }

    #[test]
    fn poll_events_handles_null_and_zero() {
        assert_eq!(unsafe { wgpu_backend_poll_events(std::ptr::null_mut(), 0) }, 0);
        let mut buf = [WgpuEvent::key(0, 0); 4];
        assert_eq!(unsafe { wgpu_backend_poll_events(buf.as_mut_ptr(), 4) }, 0);
    }
}
