# wgpu backend manual tests

Golden-image and bring-up tests for the experimental wgpu (Wayland + GPU)
backend.  See `../../../wgpu-backend-plan.md`.

## Standalone Rust window (M1c/M2)

Open a Wayland window that renders demo content (a row of atlas glyphs + a
cursor fill, over a clear background) via the same `Renderer` the offscreen
golden path uses (needs `WAYLAND_DISPLAY`):

```sh
cd rust/wgpu-backend
guix shell -m ../../manifest.scm -- cargo run --example clear_color        # until closed
guix shell -m ../../manifest.scm -- cargo run --example clear_color -- 90   # 90 frames
```

## Offscreen golden images (M1d)

The offscreen render path is deterministic and needs only a Vulkan/GL device
(software lavapipe is fine), no display, so it runs in CI.  From Emacs:

```elisp
(wgpu-dump-frame "/tmp/frame.png")   ; render offscreen, write PNG
```

`golden-test.el` drives this and compares against committed goldens in
`golden/` with a per-pixel tolerance.

```sh
guix shell -m manifest.scm -- ./src/emacs -Q --batch \
  -l test/manual/wgpu/golden-test.el -f wgpu-golden-run
```

To (re)generate goldens after an intentional visual change:

```sh
... -l test/manual/wgpu/golden-test.el -f wgpu-golden-regenerate
```
