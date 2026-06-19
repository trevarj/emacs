;;; golden-test.el --- wgpu backend offscreen golden-image tests  -*- lexical-binding: t; -*-

;; Drives the wgpu backend's deterministic offscreen render path and checks it
;; against committed golden images.  Needs a Vulkan/GL device (software
;; lavapipe is fine) but no display, so it runs headless / in CI.
;;
;; Usage:
;;   emacs -Q --batch -l test/manual/wgpu/golden-test.el -f wgpu-golden-run
;;   emacs -Q --batch -l test/manual/wgpu/golden-test.el -f wgpu-golden-regenerate
;;
;; Cases:
;;   clear-64     a 64x64 solid clear (the M1d frame background).
;;   demo-128x48  the M2 frame-command demo: a row of synthetic glyphs drawn
;;                from the atlas plus a cursor fill.

(require 'cl-lib)

(defconst wgpu-golden-dir
  (expand-file-name "golden/"
                    (file-name-directory (or load-file-name buffer-file-name)))
  "Directory holding committed golden images.")

(defconst wgpu-golden-tolerance 2
  "Maximum allowed absolute per-byte difference (sRGB rounding slack).")

(defun wgpu-golden--cases ()
  "Alist of (NAME . THUNK); THUNK returns an RGBA8 unibyte string."
  (list (cons "clear-64"    (lambda () (wgpu--frame-rgba 64 64)))
        (cons "demo-128x48" (lambda () (wgpu--demo-rgba)))))

(defun wgpu-golden--file (name) (expand-file-name (concat name ".rgba") wgpu-golden-dir))

(defun wgpu-golden--read-bytes (file)
  (with-temp-buffer
    (set-buffer-multibyte nil)
    (insert-file-contents-literally file)
    (buffer-string)))

(defun wgpu-golden--require-backend ()
  (unless (and (fboundp 'wgpu--frame-rgba) (fboundp 'wgpu--demo-rgba))
    (error "This build lacks the wgpu backend (configure --with-wgpu)")))

(defun wgpu-golden-regenerate ()
  "Render each case and (over)write its golden RGBA + a viewable PNG."
  (wgpu-golden--require-backend)
  (make-directory wgpu-golden-dir t)
  (pcase-dolist (`(,name . ,thunk) (wgpu-golden--cases))
    (let ((rgba (funcall thunk)))
      (with-temp-file (wgpu-golden--file name)
        (set-buffer-multibyte nil)
        (insert rgba))))
  ;; Viewable PNGs.
  (wgpu-dump-frame (expand-file-name "clear-64.png" wgpu-golden-dir) 64 64)
  (wgpu--draw-demo (expand-file-name "demo-128x48.png" wgpu-golden-dir))
  (message "Regenerated goldens in %s" wgpu-golden-dir))

(defun wgpu-golden--compare (name thunk)
  (let ((file (wgpu-golden--file name)))
    (unless (file-exists-p file)
      (error "No golden %s; run wgpu-golden-regenerate first" file))
    (let ((golden (wgpu-golden--read-bytes file))
          (actual (funcall thunk)))
      (unless (= (length golden) (length actual))
        (error "%s: size mismatch (golden %d, actual %d)"
               name (length golden) (length actual)))
      (let ((maxdiff 0) (nbad 0))
        (dotimes (i (length golden))
          (let ((d (abs (- (aref golden i) (aref actual i)))))
            (setq maxdiff (max maxdiff d))
            (when (> d wgpu-golden-tolerance) (cl-incf nbad))))
        (when (> nbad 0)
          (error "%s: %d/%d bytes differ (max diff %d > tol %d)"
                 name nbad (length golden) maxdiff wgpu-golden-tolerance))
        (message "  %-12s OK (%d bytes, max diff %d)" name (length golden) maxdiff)))))

(defun wgpu-golden-run ()
  "Render each case and assert it matches its golden.
Signals an error (non-zero batch exit) on mismatch."
  (wgpu-golden--require-backend)
  (pcase-dolist (`(,name . ,thunk) (wgpu-golden--cases))
    (wgpu-golden--compare name thunk))
  ;; Also exercise the PNG writer end to end.
  (let ((png (make-temp-file "wgpu-golden-" nil ".png")))
    (wgpu--draw-demo png)
    (let ((sig (wgpu-golden--read-bytes png)))
      (unless (and (>= (length sig) 8)
                   (equal (substring sig 0 8)
                          (unibyte-string #x89 #x50 #x4e #x47 #x0d #x0a #x1a #x0a)))
        (error "wgpu--draw-demo did not write a valid PNG")))
    (delete-file png))
  (message "wgpu goldens OK"))

(provide 'golden-test)
;;; golden-test.el ends here
