# wlshm backend architecture

`wlshm` is an Emacs window-system backend for raw Wayland.  Emacs renders with
CPU Cairo into a persistent per-frame image surface; a Rust static library owns
Wayland objects and presents those pixels through a `wl_shm` double buffer.

The name is historical.  There is no GPU renderer, `wgpu`, Vulkan path, or
external `wlshm` rendering crate in the current design.

## Identity and build

- Configure with `--with-wlshm`; the window system symbol is `wlshm`.
- Core identifiers are `output_wlshm`, `FRAME_WLSHM_P`, `HAVE_WLSHM`,
  `src/wlshmterm.c`, `src/wlshmfns.c`, `src/wlshmgui.h`,
  `src/wlshmterm.h`, `src/wlshm_ffi.h`, and `lisp/term/wlshm-win.el`.
- The Rust crate is `rust/wlshm-backend/`.  It builds
  `libwlshm_backend.a`, linked into `src/emacs`.
- `src/wlshm_ffi.h` is generated from Rust by `cbindgen`.
- The Guix environment is `manifest.scm`.

Rust-only edits need a forced relink before running the binary:

```sh
rm -f src/temacs src/emacs
make
```

`src/Makefile.in` deliberately builds the Rust archive through a FORCE rule.
Without removing the linked binary, a plain `make` can rebuild the archive while
leaving `src/emacs` linked to old Rust code.

## Runtime model

The backend is single-threaded from Emacs's point of view:

```text
Emacs redisplay
  -> wlshm RIF hooks in src/wlshmterm.c
  -> Cairo draws into FRAME_X_OUTPUT (f)->canvas
  -> frame_up_to_date / flush_display
  -> wlshm_window_present (...)
  -> Rust copies pixels into a free wl_shm buffer
  -> wl_surface.attach + damage + commit
```

Important state boundaries:

- C owns redisplay integration, frame state, Cairo drawing, colors, glyph
  drawing via `ftcrfont`, scroll bars, menus, tooltips, and test PNG dumps.
- Rust owns the Wayland connection, xdg-shell and popup surfaces,
  subsurfaces, `wl_shm` pools, fractional-scale objects, viewports, input
  decoding, key repeat timers, clipboard, primary selection, DnD, and IME
  protocol handling.
- Lisp registers the `wlshm` window system, frame creation, selections, DnD,
  IME preedit display, and themed tool-bar icon lookup.

The Wayland fd, key-repeat timerfd, and present-deadline timerfd are added as
keyboard wait descriptors.  When any of them wakes Emacs, `read_socket_hook`
calls into Rust, drains pending Wayland events, converts them into Emacs input
events, and presents hover/scroll-bar updates when needed.

## Rendering

Each frame has a persistent Cairo image-surface canvas in `struct wlshm_output`.
The canvas is reused across redisplay passes so normal incremental redisplay
updates only the regions Emacs chooses to repaint.

The canvas uses `CAIRO_FORMAT_RGB24`, which matches Wayland `Xrgb8888` on the
presentation side.  Presenting a frame flushes the Cairo surface and hands Rust
the raw pixel pointer, physical width/height, and stride.

Text is not a custom backend font path.  `wlshm` reuses Emacs's Cairo/FreeType
glyph machinery through `ftcrfont`, preserving existing shaping, fontset, face,
bidi, and composition behavior.

The redisplay interface in `src/wlshmterm.c` covers the current GUI surface:

- glyphs, backgrounds, boxes, underlines, overlines, strike-through, cursor
- fringes and cached fringe masks
- internal borders, dividers, tab bar, tool bar, scroll bars
- images through the Cairo image path
- child frames, menus, tooltips, and popup surfaces

## Scaling invariants

Emacs works in logical pixels.  The backend allocates the Cairo canvas in
physical pixels:

```text
physical_width  = ceil (logical_width  * scale120 / 120)
physical_height = ceil (logical_height * scale120 / 120)
```

Cairo gets a matching device scale so drawing code still uses logical
coordinates.  Rust maps the physical buffer back to the logical Wayland surface
with `wp_viewporter`; the precise fractional scale comes from
`wp_fractional_scale_v1` as `scale * 120`.

The fractional-scale rules are part of the architecture:

- Font antialiasing is grayscale, never subpixel/LCD.  The compositor may
  resample the final `wl_shm` buffer, and RGB subpixel masks smear into color
  fringes when resampled.
- Copy-based scrolling is disabled for fractional-scale `wlshm` frames.
  Moving already-rasterized glyph rows can place them at a sub-pixel phase a
  fresh render would never produce, leaving stale fringes.
- One-pixel lines and box/divider edges are snapped to physical pixels and
  drawn with antialiasing disabled.  They must not be routed through the
  antialiased logical-rectangle path.

The generic gates for these rules are `FRAME_SCALE_FACTOR` and
`FRAME_SCROLL_COPY_UNSAFE` in `src/frame.h`.

## Wayland surface model

Rust keeps one `Backend` per thread-local connection.  Windows are addressed
from C as `uint64_t` handles.

Surface roles:

- Toplevel Emacs frames are xdg toplevels.
- Menus and tooltips are xdg popups.
- Child frames with a parent frame become `wl_subsurface` children.

Subsurface placement applies on the parent commit, so movement and initial
mapping must commit the parent when required.  Subsurfaces also use the same
fractional-scale and viewport treatment as toplevels so child frames stay crisp
at non-integer scale.

Rust intentionally releases explicit Wayland protocol objects such as
viewports, fractional-scale objects, subsurfaces, and surfaces on frame close.
The process-global connection is otherwise kept simple and effectively lives
until Emacs exits.

## Input and desktop integration

Rust decodes Wayland input into POD `WlshmEvent` values, and C converts those
to Emacs input events.

Currently covered:

- keyboard input, modifiers, focus, and client-side key repeat
- pointer motion, enter/leave, buttons, wheel and horizontal wheel events
- mouse-face/help-echo, tab-bar, tool-bar, and Emacs-drawn scroll-bar handling
- clipboard and primary selection
- drag-and-drop receive for text and URI lists
- `zwp_text_input_v3` preedit and committed text
- cursor-shape updates where the compositor supports the protocol

Some Wayland limitations are deliberate no-ops: pointer warping, many
X-specific frame-positioning requests, override-redirect semantics, and several
legacy X frame parameters.

## Tests and diagnostics

Never test against the user's live Wayland session.  Use the harness:

```sh
test/manual/wlshm/run-tests.sh [all|1|2|3]
```

Tiers:

- Tier 1: render smoke under private headless weston.
- Tier 2: golden PNG scenes under private headless weston.
- Tier 3: nested-niri interaction tests; skipped unless `WLSHM_PARENT_WL` is
  set.

The harness tracks exact PIDs it spawns.  Do not replace that with `pkill`,
`killall`, or any process-name cleanup.

Useful diagnostics:

- Set `WLSHM_DEBUG=1` to enable `/tmp/wlshm-debug.log` and debug-only Lisp
  helpers.
- `(wlshm-dump-canvas PATH)` writes the selected frame's current Cairo canvas
  to a PNG when `WLSHM_DEBUG=1`.
- `test/manual/wlshm/capture-freeze.sh` captures repeated gdb snapshots of the
  build-tree `wlshm` Emacs only.
- Regenerate render goldens intentionally with `WLSHM_REGEN=1`, then inspect
  the resulting PNG diffs before committing.

For fractional-scale rendering bugs, compare an incremental action against the
same final state after `(redraw-display)`.  A nonzero image diff is usually
redisplay leaving stale pixels, not compositor blur.  True compositor-resample
blur requires an on-screen capture from a nested compositor.
