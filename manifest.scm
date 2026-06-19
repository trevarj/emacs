;; Dev environment for building Emacs with the experimental wgpu backend.
;;
;;   guix shell -m manifest.scm        ; interactive dev shell
;;   guix shell -m manifest.scm -- ... ; one-off command
;;   (direnv loads this automatically via .envrc -> `use guix;`)
;;
;; It combines two things:
;;   1. the *build dependencies* of the emacs-next package (autotools, gcc,
;;      texinfo, pkg-config, the usual image/font/X libs, ...), i.e. the
;;      equivalent of `guix shell -D emacs-next`, so the C core configures and
;;      builds; and
;;   2. the extra deps the Rust Wayland + wgpu backend needs (rust toolchain,
;;      cbindgen, raw Wayland + xkbcommon, the Vulkan/GBM stack, freetype +
;;      harfbuzz for the glyph atlas).

(use-modules (gnu packages)
             (guix profiles))

(concatenate-manifests
 (list
  ;; (1) Everything needed to build Emacs itself, tracking emacs-next.
  (package->development-manifest
   (specification->package "emacs-next"))

  ;; (2) wgpu backend extras.
  (specifications->manifest
   (list "rust"            ; rustc + std
         "rust:cargo"      ; cargo (separate output of the rust package)
         "rust-cbindgen"   ; generate src/wgpu_ffi.h from the crate
         "pkg-config"
         ;; Wayland + input
         "wayland"
         "wayland-protocols"
         "libxkbcommon"
         ;; GPU stack for wgpu (Vulkan + GBM/EGL via mesa)
         "vulkan-loader"
         "vulkan-headers"
         "mesa"
         ;; Text stack reused from Emacs's font driver
         "freetype"
         "harfbuzz"
         ;; A font so fontconfig can resolve the default frame font.
         "font-dejavu"))))
