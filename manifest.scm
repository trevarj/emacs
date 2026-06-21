;; Dev environment for building Emacs with the experimental wlshm backend.
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
;;   2. the extra deps the raw-Wayland + CPU (Cairo-on-wl_shm) backend needs
;;      (rust toolchain, cbindgen, raw Wayland + xkbcommon, and the cairo +
;;      freetype + fontconfig + harfbuzz text stack).  No Vulkan/GPU anymore.

(use-modules (gnu packages)
             (guix profiles))

;; wtype/wlrctl drive the tier-3 nested-niri interaction tests and come from
;; personal channels.  CI builds against core Guix only and runs just tier-1/2,
;; so it sets WLSHM_CI=1 to drop them and keep the manifest resolvable with no
;; extra channels.
(define interactive-test-tools
  (if (getenv "WLSHM_CI")
      '()
      (list "wtype"           ; virtual-keyboard injection (nested-niri)
            "wlrctl")))       ; virtual-pointer injection

(concatenate-manifests
 (list
  ;; (1) Everything needed to build Emacs itself, tracking emacs-next.
  (package->development-manifest
   (specification->package "emacs-next"))

  ;; (2) wlshm backend extras.
  (specifications->manifest
   (list "rust"            ; rustc + std
         "rust:cargo"      ; cargo (separate output of the rust package)
         "rust-cbindgen"   ; generate src/wlshm_ffi.h from the crate
         "pkg-config"
         ;; Wayland + input
         "wayland"
         "wayland-protocols"
         "libxkbcommon"
         ;; CPU rendering + text stack (Cairo image surface -> wl_shm).
         "cairo"
         "freetype"
         "fontconfig"
         "harfbuzz"
         ;; A font so fontconfig can resolve the default frame font.
         "font-dejavu"
         ;; Render-test harness (tier-1/2, test/manual/wlshm/run-tests.sh):
         ;;   weston  - headless compositor for on-screen render goldens
         ;;   gdb     - watchdog backtraces on hang/crash
         "weston"
         "gdb"))

  ;; (3) Tier-3 interaction tools (wtype/wlrctl); skipped under WLSHM_CI.
  (specifications->manifest interactive-test-tools)))
