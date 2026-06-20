//! Raw Wayland + CPU (Cairo-on-wl_shm) graphical backend for Emacs.
//!
//! This crate owns the platform layer only: the Wayland connection
//! (smithay-client-toolkit), the xdg-shell window, a wl_shm double buffer, and
//! input decoding (xkbcommon, key repeat, clipboard).  It does NOT rasterize:
//! the Emacs C side (`src/wlshmterm.c` et al.) draws each frame with Cairo onto a
//! persistent image surface ("canvas") and hands us the pixels to present.
//!
//! Naming: every exported symbol keeps the historical `wlshm_backend_`
//! (lifecycle) / `wlshm_` (window/present) prefix even though no GPU is involved
//! anymore.  The C header is generated from this crate by cbindgen into
//! `src/wlshm_ffi.h`.

pub mod event;
pub mod winsys;

pub use event::{WlshmEvent, WlshmEventKind};

use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::sync::OnceLock;

/// Status codes returned across the FFI boundary.
pub const WLSHM_OK: c_int = 0;
pub const WLSHM_ERR_INIT: c_int = -1;

/// Initialize the backend process-global state. Idempotent. Returns `WLSHM_OK`.
#[no_mangle]
pub extern "C" fn wlshm_backend_init() -> c_int {
    WLSHM_OK
}

/// Tear down process-global backend state. No-op.
#[no_mangle]
pub extern "C" fn wlshm_backend_shutdown() {}

/// Backend version string (static, NUL-terminated, owned by the backend).
/// The C side must not free it.
#[no_mangle]
pub extern "C" fn wlshm_backend_version() -> *const c_char {
    static V: OnceLock<CString> = OnceLock::new();
    V.get_or_init(|| CString::new(env!("CARGO_PKG_VERSION")).expect("version has no NUL"))
        .as_ptr()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn init_then_shutdown_is_ok() {
        assert_eq!(wlshm_backend_init(), WLSHM_OK);
        wlshm_backend_shutdown();
    }

    #[test]
    fn version_is_nonempty_cstr() {
        let p = wlshm_backend_version();
        assert!(!p.is_null());
        let s = unsafe { CStr::from_ptr(p) }.to_str().unwrap();
        assert!(!s.is_empty());
        // Stable pointer across calls (interned).
        assert_eq!(p, wlshm_backend_version());
    }
}
