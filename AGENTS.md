# AGENTS.md — Emacs `wlshm` backend (branch `wlshm-backend`)

Project guidance for the **wlshm** window-system backend: raw Wayland +
CPU Cairo rendering onto a `wl_shm` buffer. Extends the user's global
`~/AGENTS.md` (Guix, signed Conventional Commits, search tools, etc.); this
file adds repo-specific rules. Keep it concise and durable.

## What this is

Emacs Wayland backend forked from the old GPU `wgpu-backend`. **CPU, not GPU:**
Emacs draws with Cairo onto a persistent image-surface "canvas"; a Rust layer
(`rust/wlshm-backend/`, smithay-client-toolkit 0.19, no wgpu) presents it via a
`wl_shm` double buffer. Reuses Emacs's `ftcrfont` (Cairo+FreeType) glyph driver.
Tree symbols: `output_wlshm`, `FRAME_WLSHM_P`, `--with-wlshm`, window-system
`wlshm`, `src/wlshmterm.c` / `wlshmfns.c` / `wlshmgui.h` / `wlshm_ffi.h`,
`lisp/term/wlshm-win.el`, `WLSHM_DEBUG`. "wgpu" no longer exists in-tree.

## Build

```sh
guix shell -m manifest.scm -- ./configure --with-wlshm && make -j
```

- Rust staticlib `libwlshm_backend.a` builds via a cargo FORCE rule.
- Regen the FFI header after any `winsys.rs` FFI change:
  `cd rust/wlshm-backend && guix shell rust-cbindgen -- cbindgen --config
  cbindgen.toml --output ../../src/wlshm_ffi.h`
- **BUILD GOTCHA (costs hours): Rust-only changes need a forced relink.** In
  `src/Makefile`, `temacs: | $(WLSHM_STATICLIB)` is order-only (`|`) on purpose,
  so a plain `make` after editing `rust/wlshm-backend/src/*.rs` rebuilds the
  `.a` but does NOT relink emacs — you silently run the OLD Rust code.
  `rm -f src/temacs src/emacs` before `make` for any Rust change. (C changes
  relink fine.) Verify with `strings src/emacs | grep <new-symbol>`.
- Commit-message lines must be ≤78 chars (Emacs `commit-msg` hook rejects longer).

## Testing — and the rule that matters most

**NEVER test on the user's live Wayland session.** The user develops this
backend live (their running niri + their interactive build-tree Emacs).

**CATASTROPHIC — process cleanup (a fork once killed the user's live niri):**
NEVER kill processes by name/pattern. `pkill niri`/`sway`/`weston`,
`pkill -x .emacs-*`, any `pkill -f <pat>` match the user's compositor and their
live Emacs and kill them. ONLY kill exact PIDs you spawned (capture `$!`), or
rely on the harness's PID-tracked cleanup. Put this rule in every fork/subagent
prompt that starts a compositor or Emacs.

Harness: `test/manual/wlshm/run-tests.sh [all|1|2|3]` (Tier 1 render smoke,
Tier 2 golden PNGs under headless weston, Tier 3 nested-niri interaction —
SKIPs unless `WLSHM_PARENT_WL` set). `lib.sh` kills only `$WLSHM_PIDS`.
Goldens in `test/manual/wlshm/golden/`; regen with `WLSHM_REGEN=1`.

Harness gotchas:
- `wlshm_eval` callers MUST capture output (`r=$(wlshm_eval …)`), never
  `wlshm_eval … >/dev/null` — the subshell delay lets redisplay+present finish;
  the redirected form races it and the dump is dropped.
- The interactive shell is **zsh** (no word-split of unquoted `$VAR` in
  `for f in $FILES`) — run such loops under `bash -c`.
- `wlshm-dump-canvas` needs `WLSHM_DEBUG=1`.

## Fractional-scale rendering (the user runs niri at 1.5×)

Most rendering bugs only manifest at fractional scale and CANNOT be reproduced
at integer scale. Two hard-won invariants — do not regress them:

- **Font AA must be GRAYSCALE, never subpixel.** Glyphs raster into a wl_shm
  buffer the compositor composites and (fractional path) RESAMPLES via
  `wp_viewport`; subpixel/LCD triads smear into red/blue/cyan edge fringes.
  `wlshm_font_options_from_pattern` forces `CAIRO_ANTIALIAS_GRAY` (NONE if AA
  off) and ignores fontconfig `rgba`. (This overrides the user's
  `~/.config/fontconfig/conf.d/90-subpixel-rendering.conf` for wlshm.)
- **No copy-based scrolling at fractional scale.** `scroll_run_hook` memmoves
  rasterized glyphs by an integer offset; at fractional scale a text row isn't
  an integer # of physical px, so moved glyphs land at a sub-pixel phase a fresh
  render never makes, leaving stale fringes until C-l. `FRAME_SCROLL_COPY_UNSAFE`
  (frame.h, true only for a fractional-scale wlshm frame) gates the three
  copy-scroll paths — `scrolling_window` (dispnew.c), and
  `try_window_reusing_current_matrix` + `try_window_id` (xdisp.c) — so rows are
  redrawn fresh instead. Costs a full-window redraw per scroll (acceptable).
- 1px lines (overline/underline/divider/box edges) use physical-snapped
  AA-none helpers (`wlshm_fill_phys` / `wlshm_fill_rect_phys`), not the
  antialiased path, so they don't ramp across two physical rows.

### Headless fractional-scale repro (agent-safe, no live session)

Headless weston is integer-scale only. To force a fractional scale for canvas
inspection, add a TEMP env hook `WLSHM_FORCE_SCALE120` and apply it in BOTH
`wlshm_ensure_canvas` (canvas device scale) AND `wlshm_frame_scale_factor` (so
`FRAME_SCALE_FACTOR` / `FRAME_SCROLL_COPY_UNSAFE` see it too — canvas-only is not
enough). Render, then `wlshm-dump-canvas`. Remove the hook before committing.

Isolate stale/copy artifacts by diffing an INCREMENTAL action against the same
state after `(redraw-display)` (the C-l equivalent): nonzero diff = what
redisplay left stale. Use ImageMagick `-compose difference` + per-row `awk` on
`txt:` output to localize. For true on-screen blur (compositor resampling), the
canvas dump can't show it — use `grim` against a nested niri and compare vs pgtk
(`/home/trev/.guix-home/profile/bin/emacs` is the pgtk baseline).

## Where things live

- Drawing / redisplay interface / event loop: `src/wlshmterm.c`
- Frame & GUI primitives (create-frame, geometry, selections): `src/wlshmfns.c`
- Wayland client (globals, surfaces, present/throttle, subsurfaces, viewport):
  `rust/wlshm-backend/src/winsys.rs`
- Window-system Lisp: `lisp/term/wlshm-win.el`

The git history on `wlshm-backend` has detailed, self-contained commit messages
for every fix — read them before re-investigating a symptom.
