# wlshm backend for Emacs — design & execution plan

A Wayland + GPU-accelerated graphical backend for Emacs, written in Rust and
hooked into the C core via FFI. An alternative to PGTK, not a modification of
it.

## Decisions (locked)

- **Goals:** kill input latency, smooth scrolling/animation + correct HiDPI
  (fractional) scaling, low CPU/power. (All three.)
- **Distribution:** personal fork. No upstream/GNU-policy constraints, free to
  use Rust and new build deps.
- **Windowing:** raw Wayland via `smithay-client-toolkit` (xdg-shell, input via
  `libxkbcommon`, IME later via `text-input-v3`). Truest "not PGTK".
- **Render API:** `wlshm` (Vulkan/GL/Metal backends).
- **Loop/threading:** Rust owns a dedicated render+Wayland thread; exposes a
  wakeup fd Emacs adds to its existing `select` set. `read_socket_hook` drains a
  lock-free queue over FFI. Decouples redisplay from Wayland dispatch.
- **Text:** reuse Emacs shaping/metrics (FreeType+HarfBuzz font driver); add a
  GPU glyph-atlas `draw`. Correct bidi/composition/faces for free.
- **Validation:** offscreen wlshm render + pixel readback, diffed against golden
  PNGs. Deterministic, no compositor, CI-able. Triggered from Lisp via
  `(wlshm-dump-frame PATH)`.
- **Scale/present:** native-pixel render via `wp-fractional-scale-v1` +
  `wp-viewporter`; adaptive present (Mailbox/Immediate, Fifo fallback) with
  `wp-presentation` feedback.
- **Build/link:** `cargo` staticlib (`libwlshm_backend.a`) + `cbindgen`-generated
  header, linked into the `emacs` binary. `configure` flag `--with-wlshm`.
- **Deps:** managed via `guix shell` + `manifest.scm`; crates vendored
  (`cargo vendor`) for offline/reproducible builds.
- **First slice (MVP):** Wayland window + text frame + cursor + keyboard input,
  validated by screenshot. Defer scrollbars, menus, mouse, IME, toolbars,
  images.

## Identity & layout

- **Backend id:** `wlshm`. Macro `HAVE_WLSHM`, `window_system == "wlshm"`, output
  method `output_wlshm`, Lisp symbol `Qwlshm`.
- **C side (thin shims):** `src/wlshmterm.c`, `src/wlshmterm.h`, `src/wlshmfns.c`
  (frame/Lisp glue), `src/wlshmfont.c` (font driver). Hold no drawing logic —
  only translation to FFI calls. Structurally modeled on `pgtkterm.c`
  (`pgtk_create_terminal` at `pgtkterm.c:4812`) but ~10x thinner.
- **Rust side:** crate `rust/wlshm-backend/` → staticlib `libwlshm_backend.a`.
  Owns Wayland, wlshm, glyph atlas, render thread, input decode.
- **Generated header:** `src/wlshm_ffi.h` (cbindgen, Rust→C exports). Hand-written
  `wlshm_callbacks.h` for the small set of C functions Rust calls back into.

## Architecture & data flow

```
  ┌──────────────────────── Emacs main thread ─────────────────────────┐
  │ keyboard.c loop → select(+wakeup fd) → read_socket_hook             │
  │   redisplay → RIF hooks → record into FrameCmdBuffer (main-owned)   │
  │   frame_up_to_date / flush_display → publish cmd buffer + signal    │
  └──────────────┬─────────────────────────────────▲───────────────────┘
       publish (mutex swap + eventfd)    drain queue │ (read_socket)
                 ▼                                   │
  ┌──── Render thread (Rust) ────┐   ┌──── Wayland event thread (Rust) ───┐
  │ owns wlshm device/queue/      │   │ smithay loop: keyboard/pointer →    │
  │  surface + persistent frame  │   │  decode (xkbcommon) → lock-free     │
  │  texture + glyph atlas       │   │  input queue → write wakeup pipe fd │
  │ replays cmds → present       │◄──┤ frame callbacks / presentation-time │
  └──────────────────────────────┘   └──────────────────────────────────────┘
```

Invariants:
- All wlshm state lives on the render thread (no `Surface`/`Device` across
  threads).
- **Persistent frame texture** kept across frames. Emacs updates incrementally
  (dirty glyph rows only); RIF ops mutate regions, present flips. This is the
  low-CPU win vs PGTK's full-frame `cairo_paint`.
- Input never waits on redisplay: event thread decodes + enqueues + pokes the
  wakeup fd; Emacs `select` wakes and drains. Structurally removes the
  `pgtk_read_socket` GLib-dispatch stall measured in `emacs-pgtk-perf.md`.

## FFI surface (the contract)

The RIF (`dispextern.h:3068`) and terminal hooks (`termhooks.h:475`) are
populated in `wlshm_create_terminal` (analog of `pgtkterm.c:4812`); each hook body
is a 1–3 line forwarder to a `wlshm_*` FFI call.

Rust → C (cbindgen):
- Lifecycle: `wlshm_backend_init`, `wlshm_terminal_create`, `wlshm_frame_create`,
  `wlshm_frame_destroy`, `wlshm_shutdown`.
- Cmd recording (one per RIF hook): `wlshm_clear_area`, `wlshm_draw_glyphs`,
  `wlshm_fill_rect`, `wlshm_draw_rect`, `wlshm_scroll`, `wlshm_draw_cursor`,
  `wlshm_draw_fringe_bitmap`, `wlshm_draw_window_divider`, …
- Glyph atlas: `wlshm_atlas_get_or_raster(font_key, glyph, subpixel)`.
- Present: `wlshm_frame_present`.
- Input: `wlshm_backend_poll_events(buf, max)`, `wlshm_backend_wakeup_fd`.
- Misc: `wlshm_set_scale`, `wlshm_resize`, `wlshm_dump_frame(path)` (test hook).

C → Rust callbacks: glyph raster bridge (`wlshm_cb_raster_glyph`), color/resource
queries. Kept minimal.

## Rendering pipeline (Rust)

- Command buffer: `enum WlshmCmd { Glyphs, FillRect, Rect, Scroll, Cursor,
  Fringe, Divider, ClearArea }`. Main thread appends; published by
  mutex-protected double-buffer swap + eventfd signal.
- Replay onto persistent texture: glyphs/fills/cursor → instanced quads in few
  draw calls; `Scroll` → intra-texture GPU blit + overdraw; `ClearArea` →
  scissored clear.
- Present: native-pixel surface (`wp-fractional-scale-v1` + `wp-viewporter`);
  prefer Mailbox/Immediate, Fifo fallback; pace with frame callbacks +
  `wp-presentation`. Report `wl_surface.damage_buffer` for partial updates.

## Text / glyph atlas

- Reuse Emacs shaping: `wlshmfont` driver layers on existing FreeType+HarfBuzz
  machinery (like `ftcrhbfont`) for `list`/`match`/`open`/`shape`/`text_extents`
  /`encode_char`. Replace only `draw`.
- `draw`: key `(font, glyph-id, subpixel-bucket)` into a dynamic GPU atlas
  (rect-packed). Miss → `FT_Render_Glyph` → upload subregion → cache. Emit quad
  (atlas UV, pen pos, fg color). Backgrounds/boxes/underlines → colored quads.
- Color: backend owns pixel encoding — pack RGBA into the `unsigned long` pixel;
  `defined_color_hook`/`query_colors` become bit ops, no round-trips.

## Input

- smithay keyboard → `xkbcommon` keymap → keysym + mods. Port modifier/keysym →
  Emacs event translation from `pgtkterm.c` `key_press_event`. Enqueue to
  lock-free queue.
- `read_socket_hook` calls `wlshm_backend_poll_events`, converts to Emacs input
  events. Wakeup fd integrated into the `xg_select` analog (`src/xgselect.c:188`
  is the reference seam).
- MVP keyboard only; pointer/scrollbars next; IME (`text-input-v3`)/menus later.

## Validation harness (first-class)

- Primary gate (deterministic, no compositor): render thread can target an
  offscreen texture; `(wlshm-dump-frame PATH)` forces redisplay → offscreen
  replay → readback → PNG.
- Golden tests: elisp scripts in `test/manual/wlshm/` set up known
  buffer/face/scale → `wlshm-dump-frame` → compare to committed golden PNG with
  per-pixel tolerance + diff image on failure. Goldens in
  `test/manual/wlshm/golden/`.
- Rust unit tests: atlas packing, cmd encoding, xkb mapping, scroll math —
  `cargo test`, no GPU.
- Latency regression: run `test/manual/pgtk-keybench.el` on `wlshm`; compare
  p50/p95/p99/max against PGTK.
- Optional E2E (separate, non-CI): headless compositor (`weston --backend=
  headless`/`cage`) + `wlr-screencopy`.

## Build integration

- `configure.ac`: `--with-wlshm` → `window_system=wlshm`, define `HAVE_WLSHM` +
  `HAVE_WINDOW_SYSTEM`, set `WLSHM_OBJ="wlshmterm.o wlshmfns.o wlshmfont.o"`, locate
  FreeType/HarfBuzz/wayland/xkbcommon, require `cargo`. (Seams:
  `configure.ac:2675` pgtk block, `:3875` PGTK_OBJ.)
- `src/Makefile.in`: rule runs `cargo build --release` (in `guix shell`) →
  `libwlshm_backend.a`; link it + `-lwayland-client -lxkbcommon` (+ wlshm system
  deps: vulkan-loader/libdrm/libgbm). cbindgen pre-build step → `wlshm_ffi.h`.
  (Seams: `Makefile.in:325` PGTK_OBJ, `:469` link list, `:573` LIBES.)
- Core: add `output_wlshm` (`termhooks.h:57`), `Qwlshm` (`frame.c:7258`),
  display_info union member (`termhooks.h:539`).
- Guix: `manifest.scm` adds rust/cargo/cbindgen/vulkan-loader/wayland/
  libxkbcommon/freetype/harfbuzz; `cargo vendor` for offline; `.envrc` =
  `use guix;`.

## Milestones (each gated by tests)

- **M0 — Scaffold/build.** Crate + staticlib + cbindgen + `--with-wlshm` +
  Makefile/Guix wiring; `wlshm_create_terminal` stub registers hooks. Gate:
  builds in guix shell; `cargo test` green; `emacs` links.
- **M1 — Window + clear color.** smithay window + wlshm surface; present solid
  color; offscreen path. Gate: solid-color golden via `wlshm-dump-frame`.
- **M2 — Text + cursor.** Persistent texture, glyph atlas, `wlshmfont` draw,
  cursor, `clear_frame_area`, minimal fringe/border. Gate: "hello world+cursor"
  golden.
- **M3 — Keyboard + latency path.** xkbcommon, event thread, wakeup fd,
  `read_socket_hook`. Gate: scripted keys → expected buffer + golden; keybench
  p50/p95/p99 vs PGTK.
- **M4 — Scroll, resize, HiDPI, present tuning.** `scroll_run` GPU blit, resize,
  fractional scaling, adaptive present + presentation feedback, damage. Gate:
  fractional-scale golden, scroll golden, latency re-measured.
- **M5 — Polish (post-MVP).** pointer/mouse-face, scrollbars; then IME, menus,
  images, selection/clipboard. Each with goldens.

## Top risks & mitigations

- Thread/cmd-buffer correctness → all GPU on render thread; double-buffered cmd
  lists; eventfd; fuzz encode/replay in Rust tests.
- Expose/repaint model → keep persistent texture as source of truth, re-present
  on expose. Decide vs repaint-from-matrix at M2 against resize/occlusion.
- Font shaping parity → layer on existing FreeType/HarfBuzz driver; override only
  `draw`.
- Keysym/modifier edge cases → port mapping from `pgtkterm.c`.
- Present latency / Mailbox availability varies → presentation-time + measure;
  Fifo fallback.
- Guix vendoring of wlshm's large crate tree → `cargo vendor`; pin versions.

## To confirm during execution (non-blocking)

- Persistent-texture vs repaint-from-matrix on expose (resolve at M2).
- Exact present trigger: `frame_up_to_date` vs `flush_display` vs
  `buffer_flipping_unblocked` (match Emacs double-buffer semantics).
- Whether `wlshmfont` can fully reuse `sfntfont`/`ftfont` open/shape or needs a
  thin fork.
