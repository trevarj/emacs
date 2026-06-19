//! Standalone bring-up: open a Wayland window and present a clear color.
//!
//!   cargo run --example clear_color           # run until the window closes
//!   cargo run --example clear_color -- 120     # present 120 frames then exit
//!
//! Requires a running Wayland compositor (WAYLAND_DISPLAY).

fn main() {
    let max_frames: u32 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);

    // Emacs-ish dark teal so it's obviously "ours".
    let color = [0.07, 0.15, 0.18, 1.0];
    if let Err(e) = wgpu_backend::window::run_windowed_clear(color, max_frames) {
        eprintln!("clear_color: {e}");
        std::process::exit(1);
    }
}
