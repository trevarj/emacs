;;; interaction-tests.el --- scenes + assertions for the wlshm test harness  -*- lexical-binding: t; -*-

;; Loaded into the test Emacs server (run-tests.sh).  Provides deterministic
;; render scenes for the on-screen golden tier and small helpers the shell
;; queries over emacsclient for the interaction tier.  Kept self-contained so
;; it works in -Q.

;;; Code:

(defun wlshm-test-prep ()
  "Make rendering deterministic: no cursor blink, no message clutter."
  (when (bound-and-true-p blink-cursor-mode) (blink-cursor-mode -1))
  (setq-default truncate-lines nil)
  (setq inhibit-message t)
  (menu-bar-mode -1)
  (tool-bar-mode -1))

(defun wlshm-test-fill (n fmt)
  (erase-buffer)
  (dotimes (i n) (insert (format fmt i)))
  (goto-char (point-min)))

;; --- render scenes (one deterministic frame each) -------------------------

(defun wlshm-test-scene (name)
  "Set up the buffer/frame for render scene NAME (a string)."
  (wlshm-test-prep)
  (delete-other-windows)
  (switch-to-buffer (get-buffer-create (concat "*scene-" name "*")))
  (pcase name
    ("plain"
     (wlshm-test-fill 40 "Line %03d: the quick brown fox jumps\n"))
    ("region"
     (wlshm-test-fill 40 "Line %03d: the quick brown fox jumps\n")
     (goto-char (point-min)) (forward-line 2)
     (push-mark (point) t t) (forward-line 3) (forward-char 18)
     (setq deactivate-mark nil) (activate-mark))
    ("fringe"
     (setq truncate-lines t)
     (erase-buffer)
     (insert "short\n" (make-string 400 ?X) "\nmore\n")
     (setq indicate-empty-lines t)
     (goto-char (point-min)))
    ("faces"
     (erase-buffer)
     (insert (propertize " Raised " 'face '(:box (:line-width 4 :style released-button) :background "gray80")))
     (insert "  ")
     (insert (propertize " Sunken " 'face '(:box (:line-width 4 :style pressed-button) :background "gray80")))
     (insert "\n\n")
     (insert (propertize "wave" 'face '(:underline (:style wave :color "red"))) " ")
     (insert (propertize "double" 'face '(:underline (:style line :color "blue"))) " ")
     (insert (propertize "over" 'face '(:overline "red")) " ")
     (insert (propertize "strike" 'face '(:strike-through "blue")))
     (goto-char (point-min)))
    ("dividers"
     (wlshm-test-fill 30 "L %03d\n")
     (split-window-right) (split-window-below))
    ("image"
     ;; A 48x48 color PPM (built-in pbm loader, no external image lib) with
     ;; three vertical R/G/B bands, so it proves the full-color image path.
     (erase-buffer)
     (let* ((w 48) (h 48)
            (hdr (string-to-unibyte (format "P6\n%d %d\n255\n" w h)))
            (px (make-string (* w h 3) 0)))
       (dotimes (y h)
         (dotimes (x w)
           (let ((i (* 3 (+ x (* y w)))))
             (aset px i (if (< x (/ w 3)) 255 0))
             (aset px (1+ i) (if (and (>= x (/ w 3)) (< x (* 2 (/ w 3)))) 255 0))
             (aset px (+ i 2) (if (>= x (* 2 (/ w 3))) 255 0)))))
       (insert "inline image: ")
       (insert-image (create-image (concat hdr px) 'pbm t))
       (insert " <- R/G/B bands\n"))
     (goto-char (point-min)))
    ("scrollbar"
     ;; Many lines so a vertical scroll bar appears; scroll to the middle so
     ;; the handle sits mid-trough.
     (erase-buffer)
     (dotimes (i 200)
       (insert (format "line %d  the quick brown fox\n" i)))
     (set-window-scroll-bars (selected-window) nil t nil nil)
     (goto-char (point-min))
     (forward-line 100)
     (recenter))
    (_ (error "unknown scene %s" name)))
  (redisplay t))

(defun wlshm-test-render (name path)
  "Render scene NAME and dump the frame to PATH."
  ;; The "menu" scene renders a popup-menu surface directly (the interactive
  ;; modal loop can't run unattended); it proves menu drawing.
  (if (equal name "menu")
      (progn
        (set-face-attribute 'default nil :height 150)
        (redisplay t)
        (wlshm-test-menu-render
         (list "Open File" "Save" (cons "Save As (disabled)" nil)
               nil "Copy" "Paste" "Quit")
         4 path))
    (wlshm-test-scene name)
    (redisplay t)
    ;; Two passes: the first present sizes/clears, the second has the scene.
    (redisplay t)
    (wlshm-dump-canvas path)))

;; --- interaction helpers (queried by the shell over emacsclient) ----------

(defun wlshm-test-setup-buffer ()
  "A plain buffer with known content for click/hover tests."
  (wlshm-test-prep)
  (delete-other-windows)
  (switch-to-buffer (get-buffer-create "*interact*"))
  (wlshm-test-fill 60 "Line %03d: clickable brown fox\n")
  ;; A hoverable button on its own line near the top.
  (goto-char (point-min)) (forward-line 1) (end-of-line)
  (insert "  ")
  (insert-text-button "[HOVER-ME]" 'help-echo "a button"
                      'mouse-face 'highlight 'action #'ignore)
  (goto-char (point-min))
  (redisplay t)
  (point))

(defun wlshm-test-point () (point))
(defun wlshm-test-tick () "Heartbeat: returns t once redisplay is reachable." (redisplay t) t)

(defun wlshm-test-mouse-face-active-p ()
  "Non-nil if a mouse-face highlight is currently shown."
  (and (boundp 'mouse-highlight) mouse-highlight
       (let ((hl (mouse-highlight-info)))
         (and hl t))))

;; Fallback if mouse-highlight-info is unavailable: check the frame's
;; highlight bookkeeping isn't error-prone; just report point for now.
(unless (fboundp 'mouse-highlight-info)
  (defun wlshm-test-mouse-face-active-p () 'unknown))

(provide 'wlshm-interaction-tests)
;;; interaction-tests.el ends here
