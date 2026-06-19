;;; golden-test.el --- wgpu backend offscreen golden-image tests  -*- lexical-binding: t; -*-

;; Drives the wgpu backend's deterministic offscreen render path and checks it
;; against committed golden images.  Needs a Vulkan/GL device (software
;; lavapipe is fine) but no display, so it runs headless / in CI.
;;
;; Usage:
;;   emacs -Q --batch -l test/manual/wgpu/golden-test.el -f wgpu-golden-run
;;   emacs -Q --batch -l test/manual/wgpu/golden-test.el -f wgpu-golden-regenerate
;;
;; M1d: the only "frame" content is a solid clear color, so the golden is a
;; solid field.  M2 makes this glyph-aware and the goldens gain real content.

(require 'cl-lib)

(defconst wgpu-golden-dir
  (expand-file-name "golden/"
                    (file-name-directory (or load-file-name buffer-file-name)))
  "Directory holding committed golden images.")

(defconst wgpu-golden-width 64)
(defconst wgpu-golden-height 64)
(defconst wgpu-golden-tolerance 2
  "Maximum allowed absolute per-byte difference (sRGB rounding slack).")

(defun wgpu-golden--rgba-file ()
  (expand-file-name "clear-64.rgba" wgpu-golden-dir))

(defun wgpu-golden--read-bytes (file)
  (with-temp-buffer
    (set-buffer-multibyte nil)
    (insert-file-contents-literally file)
    (buffer-string)))

(defun wgpu-golden--require-backend ()
  (unless (fboundp 'wgpu--frame-rgba)
    (error "This build lacks the wgpu backend (configure --with-wgpu)")))

(defun wgpu-golden-regenerate ()
  "Render the offscreen clear and (over)write the golden files."
  (wgpu-golden--require-backend)
  (make-directory wgpu-golden-dir t)
  (let ((rgba (wgpu--frame-rgba wgpu-golden-width wgpu-golden-height)))
    (with-temp-file (wgpu-golden--rgba-file)
      (set-buffer-multibyte nil)
      (insert rgba)))
  (wgpu-dump-frame (expand-file-name "clear-64.png" wgpu-golden-dir)
                   wgpu-golden-width wgpu-golden-height)
  (message "Regenerated goldens in %s" wgpu-golden-dir))

(defun wgpu-golden-run ()
  "Render the offscreen clear and assert it matches the golden.
Signals an error (non-zero batch exit) on mismatch."
  (wgpu-golden--require-backend)
  (let* ((golden-file (wgpu-golden--rgba-file))
         (_ (unless (file-exists-p golden-file)
              (error "No golden at %s; run wgpu-golden-regenerate first"
                     golden-file)))
         (golden (wgpu-golden--read-bytes golden-file))
         (actual (wgpu--frame-rgba wgpu-golden-width wgpu-golden-height)))
    (unless (= (length golden) (length actual))
      (error "Size mismatch: golden %d bytes, actual %d bytes"
             (length golden) (length actual)))
    (let ((maxdiff 0) (nbad 0))
      (dotimes (i (length golden))
        (let ((d (abs (- (aref golden i) (aref actual i)))))
          (setq maxdiff (max maxdiff d))
          (when (> d wgpu-golden-tolerance) (cl-incf nbad))))
      (when (> nbad 0)
        (error "Golden mismatch: %d/%d bytes differ (max diff %d > tol %d)"
               nbad (length golden) maxdiff wgpu-golden-tolerance))
      ;; Also exercise the PNG writer end to end.
      (let ((png (make-temp-file "wgpu-golden-" nil ".png")))
        (wgpu-dump-frame png wgpu-golden-width wgpu-golden-height)
        (let ((sig (wgpu-golden--read-bytes png)))
          (unless (and (>= (length sig) 8)
                       (equal (substring sig 0 8)
                              (unibyte-string #x89 #x50 #x4e #x47
                                              #x0d #x0a #x1a #x0a)))
            (error "wgpu-dump-frame did not write a valid PNG")))
        (delete-file png))
      (message "wgpu golden OK (%d bytes, max diff %d)"
               (length golden) maxdiff))))

(provide 'golden-test)
;;; golden-test.el ends here
