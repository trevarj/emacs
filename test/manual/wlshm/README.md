# wlshm backend manual tests

Golden-image and bring-up tests for the experimental wlshm (Wayland + GPU)
backend.  See `../../../wlshm-backend-plan.md`.

## Standalone Rust window (M1c/M2)

Open a Wayland window that renders demo content (a row of atlas glyphs + a
cursor fill, over a clear background) via the same `Renderer` the offscreen
golden path uses (needs `WAYLAND_DISPLAY`):

```sh
cd rust/wlshm-backend
guix shell -m ../../manifest.scm -- cargo run --example clear_color        # until closed
guix shell -m ../../manifest.scm -- cargo run --example clear_color -- 90   # 90 frames
```

## Windowed tests without disturbing your session

Do NOT run windowed tests (or inject keys) against your live `WAYLAND_DISPLAY`
-- the window competes for focus while you're working.  Run a private headless
compositor on a separate socket and point Emacs at it:

```sh
guix shell weston -- weston --backend=headless-backend.so \
  --socket=wlshm-test --width=2560 --height=1440 --idle-time=0 &
WAYLAND_DISPLAY=wlshm-test guix shell -m manifest.scm -- \
  env -u EMACSLOADPATH ./src/emacs -Q
```

Drive editing with `--eval` (not key injection) and read back state, e.g.:

```sh
... ./src/emacs -Q --eval '(run-with-timer 1.5 nil (lambda ()
  (switch-to-buffer "*scratch*") (erase-buffer)
  (dotimes (i 50) (insert (format "line %d\n" i)) (redisplay))
  (kill-emacs 0)))'
```

## Offscreen golden images (M1d)

The offscreen render path is deterministic and needs only a Vulkan/GL device
(software lavapipe is fine), no display, so it runs in CI.  From Emacs:

```elisp
(wlshm-dump-frame "/tmp/frame.png")   ; render offscreen, write PNG
```

`golden-test.el` drives this and compares against committed goldens in
`golden/` with a per-pixel tolerance.

```sh
guix shell -m manifest.scm -- ./src/emacs -Q --batch \
  -l test/manual/wlshm/golden-test.el -f wlshm-golden-run
```

To (re)generate goldens after an intentional visual change:

```sh
... -l test/manual/wlshm/golden-test.el -f wlshm-golden-regenerate
```
