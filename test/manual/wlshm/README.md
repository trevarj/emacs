# wlshm backend manual tests

Golden-image and bring-up tests for the experimental wlshm backend: raw
Wayland plus CPU Cairo rendering presented through `wl_shm`.  See
`../../../wlshm-architecture.md`.

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

## Render smoke and golden images

The main harness runs Emacs under a private compositor and captures the current
Cairo canvas via the debug-only dump primitive:

```sh
test/manual/wlshm/run-tests.sh [all|1|2|3]
```

Tier 1 is a render smoke test.  Tier 2 compares committed PNG goldens in
`golden/`.  Tier 3 uses nested niri and skips unless `WLSHM_PARENT_WL` is set.

From a test Emacs started with `WLSHM_DEBUG=1`:

```elisp
(wlshm-dump-canvas "/tmp/frame.png") ; write the current canvas to PNG
```

To run only the golden PNG tier:

```sh
test/manual/wlshm/run-tests.sh 2
```

To (re)generate goldens after an intentional visual change:

```sh
WLSHM_REGEN=1 test/manual/wlshm/run-tests.sh 2
```
