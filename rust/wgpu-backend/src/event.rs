//! Input events handed from the Wayland event thread to Emacs.
//!
//! In the full design (M3) the Wayland thread decodes keyboard/pointer events
//! via xkbcommon and enqueues them here; Emacs drains them from its
//! `read_socket_hook` over FFI. M0 ships the ABI-stable types plus a simple
//! queue with tests, so the FFI shape and the drain semantics are pinned early.

use std::collections::VecDeque;

/// Kind of input event. ABI-stable: values must not be reordered.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WgpuEventKind {
    Key = 0,
    Resize = 1,
    FocusIn = 2,
    FocusOut = 3,
}

/// A backend input event, ABI-stable for the C side.
///
/// `a`/`b` are kind-dependent payload slots (kept generic so the C struct stays
/// fixed-size as event kinds gain fields):
/// - `Key`:    `a` = keysym, `b` = modifier mask
/// - `Resize`: `a` = width px, `b` = height px
/// - focus:    unused
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WgpuEvent {
    pub kind: WgpuEventKind,
    pub a: u32,
    pub b: u32,
}

impl WgpuEvent {
    pub fn key(keysym: u32, modifiers: u32) -> Self {
        Self { kind: WgpuEventKind::Key, a: keysym, b: modifiers }
    }
    pub fn resize(width: u32, height: u32) -> Self {
        Self { kind: WgpuEventKind::Resize, a: width, b: height }
    }
}

/// FIFO of pending input events. Producer = Wayland thread, consumer = Emacs
/// main thread via `read_socket_hook`. (M0: not yet wrapped in a lock; the
/// lock-free/mutex handoff lands with the real thread in M3.)
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
    /// Returns the number actually written.
    pub fn drain_into(&mut self, out: &mut [WgpuEvent]) -> usize {
        let n = out.len().min(self.q.len());
        for slot in out.iter_mut().take(n) {
            // pop_front cannot fail: n <= self.q.len().
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
    fn drain_respects_capacity_and_preserves_order() {
        let mut q = EventQueue::new();
        for i in 0..5 {
            q.push(WgpuEvent::key(i, 0));
        }
        let mut out = [WgpuEvent::key(99, 0); 3];
        assert_eq!(q.drain_into(&mut out), 3);
        assert_eq!(out[0].a, 0);
        assert_eq!(out[1].a, 1);
        assert_eq!(out[2].a, 2);
        assert_eq!(q.len(), 2);
    }

    #[test]
    fn drain_into_empty_queue_returns_zero() {
        let mut q = EventQueue::new();
        let mut out = [WgpuEvent::resize(0, 0); 2];
        assert_eq!(q.drain_into(&mut out), 0);
        assert!(q.is_empty());
    }

    #[test]
    fn drain_partial_then_rest() {
        let mut q = EventQueue::new();
        for i in 0..3 {
            q.push(WgpuEvent::key(i, 0));
        }
        let mut out = [WgpuEvent::key(0, 0); 2];
        assert_eq!(q.drain_into(&mut out), 2);
        assert_eq!(out[0].a, 0);
        assert_eq!(out[1].a, 1);
        let mut rest = [WgpuEvent::key(0, 0); 4];
        assert_eq!(q.drain_into(&mut rest), 1);
        assert_eq!(rest[0].a, 2);
    }
}
