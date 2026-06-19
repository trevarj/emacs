//! Input events handed from the Wayland event handling to Emacs.
//!
//! The Wayland keyboard handler (sctk, which decodes via xkbcommon) fills these
//! and Emacs drains them from `read_socket_hook` over FFI, translating each to
//! a `struct input_event`.

use std::collections::VecDeque;

/// Kind of input event. ABI-stable: values must not be reordered.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WgpuEventKind {
    KeyPress = 0,
    FocusIn = 1,
    FocusOut = 2,
    /// Pointer moved; x/y carry the surface-relative position.
    PointerMotion = 3,
    /// Pointer button pressed; button/x/y are set.
    PointerPress = 4,
    /// Pointer button released; button/x/y are set.
    PointerRelease = 5,
    /// Scroll wheel/axis; axis_x/axis_y carry the (hi-res) deltas.
    PointerAxis = 6,
}

/// Modifier bits (our own encoding; translated to Emacs modifiers on the C
/// side).  Shift is reported but usually already baked into `unichar`.
pub const WGPU_MOD_SHIFT: u32 = 1 << 0;
pub const WGPU_MOD_CTRL: u32 = 1 << 1;
pub const WGPU_MOD_ALT: u32 = 1 << 2;
pub const WGPU_MOD_LOGO: u32 = 1 << 3;

/// A backend input event, ABI-stable for the C side.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WgpuEvent {
    pub kind: WgpuEventKind,
    /// xkb/X keysym (KeyPress).
    pub keysym: u32,
    /// Unicode codepoint, or 0 if the key produced no text (KeyPress).
    pub unichar: u32,
    /// Modifier mask (WGPU_MOD_*).
    pub modifiers: u32,
    /// Pointer position in surface pixels (PointerMotion/Press/Release).
    pub x: i32,
    pub y: i32,
    /// Emacs button number (0=left, 1=right, 2=middle, ...) for Press/Release.
    pub button: u32,
    /// Event timestamp in milliseconds (pointer events).
    pub time: u32,
    /// Hi-res scroll deltas (PointerAxis); +y scrolls down, +x scrolls right.
    pub axis_x: i32,
    pub axis_y: i32,
}

impl WgpuEvent {
    fn blank(kind: WgpuEventKind) -> Self {
        Self {
            kind,
            keysym: 0,
            unichar: 0,
            modifiers: 0,
            x: 0,
            y: 0,
            button: 0,
            time: 0,
            axis_x: 0,
            axis_y: 0,
        }
    }

    pub fn key(keysym: u32, unichar: u32, modifiers: u32) -> Self {
        Self { keysym, unichar, modifiers, ..Self::blank(WgpuEventKind::KeyPress) }
    }

    pub fn motion(x: i32, y: i32, modifiers: u32, time: u32) -> Self {
        Self { x, y, modifiers, time, ..Self::blank(WgpuEventKind::PointerMotion) }
    }

    pub fn button(press: bool, button: u32, x: i32, y: i32, modifiers: u32, time: u32) -> Self {
        let kind = if press {
            WgpuEventKind::PointerPress
        } else {
            WgpuEventKind::PointerRelease
        };
        Self { button, x, y, modifiers, time, ..Self::blank(kind) }
    }

    pub fn axis(axis_x: i32, axis_y: i32, x: i32, y: i32, modifiers: u32, time: u32) -> Self {
        Self {
            axis_x,
            axis_y,
            x,
            y,
            modifiers,
            time,
            ..Self::blank(WgpuEventKind::PointerAxis)
        }
    }
}

/// FIFO of pending input events (producer: Wayland handlers; consumer: Emacs
/// via read_socket_hook).
#[derive(Default)]
pub struct EventQueue {
    q: VecDeque<WgpuEvent>,
}

impl EventQueue {
    pub fn new() -> Self {
        Self { q: VecDeque::new() }
    }

    pub fn push(&mut self, e: WgpuEvent) {
        self.q.push_back(e);
    }

    /// Drain at most `out.len()` events into `out`, preserving FIFO order.
    /// Returns the number written.
    pub fn drain_into(&mut self, out: &mut [WgpuEvent]) -> usize {
        let n = out.len().min(self.q.len());
        for slot in out.iter_mut().take(n) {
            *slot = self.q.pop_front().expect("queue underflow");
        }
        n
    }

    pub fn len(&self) -> usize {
        self.q.len()
    }

    pub fn is_empty(&self) -> bool {
        self.q.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drain_respects_capacity_and_order() {
        let mut q = EventQueue::new();
        for i in 0..5 {
            q.push(WgpuEvent::key(i, i, 0));
        }
        let mut out = [WgpuEvent::key(99, 0, 0); 3];
        assert_eq!(q.drain_into(&mut out), 3);
        assert_eq!(out[0].keysym, 0);
        assert_eq!(out[2].keysym, 2);
        assert_eq!(q.len(), 2);
    }

    #[test]
    fn drain_empty_returns_zero() {
        let mut q = EventQueue::new();
        let mut out = [WgpuEvent::key(0, 0, 0); 2];
        assert_eq!(q.drain_into(&mut out), 0);
    }
}
