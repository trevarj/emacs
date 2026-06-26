;;; redisplay-bench.el --- wlshm redisplay throughput benchmark  -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;;; Commentary:

;; Graphical redisplay-throughput benchmark inspired by the emacs-gpu
;; benchmark harness discussed on emacs-devel in June 2026.
;;
;; This file is intentionally backend-neutral.  The shell runner starts Emacs
;; under wlshm, PGTK, or X11, then passes labels through environment variables.

;;; Code:

(require 'cl-lib)

(defvar wlshm-redisplay-bench--results nil)

(defun wlshm-redisplay-bench--env (name &optional default)
  "Return environment variable NAME, or DEFAULT if unset/empty."
  (let ((value (getenv name)))
    (if (and value (> (length value) 0)) value default)))

(defun wlshm-redisplay-bench--env-int (name default)
  "Return environment variable NAME parsed as an integer, or DEFAULT."
  (let ((value (wlshm-redisplay-bench--env name)))
    (if value (string-to-number value) default)))

(defun wlshm-redisplay-bench--out-file ()
  "Return the benchmark output file."
  (or (wlshm-redisplay-bench--env "WLSHM_BENCH_OUT")
      (expand-file-name "wlshm-redisplay-bench.tsv" temporary-file-directory)))

(defun wlshm-redisplay-bench--progress (stage)
  "Append diagnostic progress STAGE if a progress file is configured."
  (when-let* ((file (wlshm-redisplay-bench--env "WLSHM_BENCH_PROGRESS")))
    (let ((backend (wlshm-redisplay-bench--env "WLSHM_BENCH_BACKEND" "unknown"))
          (run (wlshm-redisplay-bench--env "WLSHM_BENCH_RUN" "0")))
      (write-region
       (format "%s\t%s\t%s\t%s\n"
               (format-time-string "%Y-%m-%dT%H:%M:%S%z")
               backend run stage)
       nil file 'append 'silent))))

(defun wlshm-redisplay-bench--log (workload frames seconds)
  "Append one result row for WORKLOAD with FRAMES completed in SECONDS."
  (let* ((backend (wlshm-redisplay-bench--env "WLSHM_BENCH_BACKEND" "unknown"))
         (scenario (wlshm-redisplay-bench--env "WLSHM_BENCH_SCENARIO" "unknown"))
         (run (wlshm-redisplay-bench--env "WLSHM_BENCH_RUN" "0"))
         (fps (/ frames seconds))
         (ms (* 1000.0 (/ seconds frames)))
         (f (selected-frame))
         (scale (wlshm-redisplay-bench--monitor-scale f))
         (row (mapconcat
               #'identity
               (list backend scenario run workload
                     (number-to-string frames)
                     (format "%.6f" seconds)
                     (format "%.3f" fps)
                     (format "%.3f" ms)
                     (number-to-string (frame-pixel-width f))
                     (number-to-string (frame-pixel-height f))
                     (if scale (format "%.3f" scale) "")
                     (symbol-name window-system)
                     emacs-version)
               "\t")))
    (push row wlshm-redisplay-bench--results)
    (write-region (concat row "\n") nil
                  (wlshm-redisplay-bench--out-file)
                  'append 'silent)))

(defun wlshm-redisplay-bench--monitor-scale (frame)
  "Return FRAME's monitor scale factor if Emacs reports one."
  (ignore-errors
    (let ((attrs (frame-monitor-attributes frame)))
      (or (alist-get 'scale-factor attrs)
          (alist-get 'backing-scale-factor attrs)))))

(defun wlshm-redisplay-bench--code-buffer ()
  "Return a buffer with many lines of font-locked Emacs Lisp."
  (let ((buf (get-buffer-create "*wlshm-redisplay-bench-code*")))
    (with-current-buffer buf
      (emacs-lisp-mode)
      (erase-buffer)
      (dotimes (i 8000)
        (insert (format "(defun wlshm-bench-func-%d (a b c) ; line %d lorem ipsum dolor sit\n"
                        i i))
        (insert (format "  (let ((x (+ a b)) (y \"a string value %d\")) (* x c %d)))\n"
                        i i)))
      (font-lock-ensure)
      (goto-char (point-min)))
    buf))

(defun wlshm-redisplay-bench--pbm-image ()
  "Return a generated photo-like PBM/PPM image object."
  (let* ((width 240)
         (height 160)
         (rows nil))
    (dotimes (y height)
      (let ((cols nil))
        (dotimes (x width)
          (let* ((r (mod (+ (* x 5) (* y 2) (/ (* x y) 211)) 256))
                 (g (mod (+ (* x 2) (* y 4) (/ (* x x) 173)) 256))
                 (b (mod (+ (* x 3) (* y 3) (/ (* y y) 97)) 256)))
            (push (format "%d %d %d" r g b) cols)))
        (push (mapconcat #'identity (nreverse cols) " ") rows)))
    (create-image
     (concat (format "P3\n%d %d\n255\n" width height)
             (mapconcat #'identity (nreverse rows) "\n")
             "\n")
     'pbm t :ascent 'center)))

(defun wlshm-redisplay-bench--image-buffer ()
  "Return a buffer with generated images interleaved with text."
  (let ((buf (get-buffer-create "*wlshm-redisplay-bench-image*"))
        (img (wlshm-redisplay-bench--pbm-image)))
    (with-current-buffer buf
      (fundamental-mode)
      (erase-buffer)
      (dotimes (i 60)
        (insert (format "image block %d of the scrolling benchmark\n" i))
        (insert-image img)
        (insert "\n\n"))
      (goto-char (point-min)))
    buf))

(defun wlshm-redisplay-bench--top ()
  "Move selected window to the top of the current buffer and redisplay."
  (goto-char (point-min))
  (set-window-start (selected-window) (point-min))
  (redisplay t))

(defun wlshm-redisplay-bench--frames (n thunk &optional workload)
  "Call THUNK N times, forcing redisplay after each call.
Return elapsed seconds."
  (let ((start (float-time))
        (progress-every
         (wlshm-redisplay-bench--env-int "WLSHM_BENCH_PROGRESS_EVERY" 0)))
    (dotimes (i n)
      (funcall thunk)
      (redisplay t)
      (when (and workload
                 (> progress-every 0)
                 (zerop (mod (1+ i) progress-every)))
        (wlshm-redisplay-bench--progress
         (format "%s:frame:%d/%d" workload (1+ i) n))))
    (- (float-time) start)))

(defun wlshm-redisplay-bench--line-scroll ()
  "Scroll one line, wrapping to the top at end of buffer."
  (condition-case nil
      (scroll-up 1)
    (end-of-buffer
     (wlshm-redisplay-bench--top))))

(defun wlshm-redisplay-bench--page-scroll ()
  "Scroll one window, wrapping to the top at end of buffer."
  (condition-case nil
      (scroll-up)
    (end-of-buffer
     (wlshm-redisplay-bench--top))))

(defun wlshm-redisplay-bench--frame-count (name full smoke)
  "Return workload NAME's frame count.
FULL and SMOKE are the normal and smoke-test counts."
  (let ((scale (wlshm-redisplay-bench--env "WLSHM_BENCH_FRAME_SCALE" "full")))
    (cond
     ((string= scale "smoke") smoke)
     ((string= scale "quick") smoke)
     (t full))))

(defun wlshm-redisplay-bench--settle-frame (frame)
  "Configure FRAME, then wait briefly for compositor-driven size changes."
  (menu-bar-mode -1)
  (when (fboundp 'tool-bar-mode)
    (tool-bar-mode -1))
  (when (fboundp 'scroll-bar-mode)
    (scroll-bar-mode -1))
  (blink-cursor-mode -1)
  (setq inhibit-message t
        message-log-max nil
        redisplay-skip-fontification-on-input nil)
  (let ((cols (wlshm-redisplay-bench--env-int "WLSHM_BENCH_COLS" 160))
        (rows (wlshm-redisplay-bench--env-int "WLSHM_BENCH_ROWS" 48)))
    (set-frame-size frame cols rows))
  (when (string= (wlshm-redisplay-bench--env "WLSHM_BENCH_FULLSCREEN" "0") "1")
    (set-frame-parameter frame 'fullscreen 'fullboth))
  ;; Let Wayland/GTK/X settle configure events.  Repeated redisplay calls also
  ;; ensure the first measured frame does not include initial map work.
  (dotimes (_ 12)
    (redisplay t)
    (sit-for 0.1))
  ;; Some backends ignore fullscreen requests made before the initial map.  The
  ;; benchmark's fullscreen mode is part of the scenario, so assert it again
  ;; after the frame has had a chance to become visible.  Clear first so this
  ;; second request is not optimized away as an unchanged frame parameter.
  (when (string= (wlshm-redisplay-bench--env "WLSHM_BENCH_FULLSCREEN" "0") "1")
    (set-frame-parameter frame 'fullscreen nil)
    (redisplay t)
    (sit-for 0.1)
    (set-frame-parameter frame 'fullscreen 'fullboth))
  (dotimes (_ 12)
    (redisplay t)
    (sit-for 0.1)))

(defun wlshm-redisplay-bench-run ()
  "Run all redisplay benchmark workloads and exit Emacs."
  (condition-case err
      (let ((frame (selected-frame)))
        (wlshm-redisplay-bench--progress "settle:start")
        (wlshm-redisplay-bench--settle-frame frame)
        (wlshm-redisplay-bench--progress "settle:done")
        (wlshm-redisplay-bench--progress "code-buffer:start")
        (switch-to-buffer (wlshm-redisplay-bench--code-buffer))
        (delete-other-windows)
        (wlshm-redisplay-bench--top)
        (sit-for 0.2)
        ;; Warm up font/image/canvas caches before measuring.
        (wlshm-redisplay-bench--progress "warmup:start")
        (wlshm-redisplay-bench--frames
         (wlshm-redisplay-bench--frame-count "warmup" 80 8)
         #'wlshm-redisplay-bench--line-scroll
         "warmup")
        (wlshm-redisplay-bench--progress "warmup:done")
        (wlshm-redisplay-bench--top)

        (wlshm-redisplay-bench--progress "line-scroll:start")
        (let* ((n (wlshm-redisplay-bench--frame-count "line-scroll" 800 20))
               (s (wlshm-redisplay-bench--frames
                   n #'wlshm-redisplay-bench--line-scroll
                   "line-scroll")))
          (wlshm-redisplay-bench--log "line-scroll" n s))
        (wlshm-redisplay-bench--progress "line-scroll:done")

        (wlshm-redisplay-bench--top)
        (wlshm-redisplay-bench--progress "page-scroll:start")
        (let* ((n (wlshm-redisplay-bench--frame-count "page-scroll" 400 12))
               (s (wlshm-redisplay-bench--frames
                   n #'wlshm-redisplay-bench--page-scroll
                   "page-scroll")))
          (wlshm-redisplay-bench--log "page-scroll" n s))
        (wlshm-redisplay-bench--progress "page-scroll:done")

        (wlshm-redisplay-bench--top)
        (wlshm-redisplay-bench--progress "full-redraw:start")
        (let* ((n (wlshm-redisplay-bench--frame-count "full-redraw" 300 10))
               (s (wlshm-redisplay-bench--frames
                   n (lambda () (redraw-frame frame))
                   "full-redraw")))
          (wlshm-redisplay-bench--log "full-redraw" n s))
        (wlshm-redisplay-bench--progress "full-redraw:done")

        (let ((buf (get-buffer-create "*wlshm-redisplay-bench-type*")))
          (wlshm-redisplay-bench--progress "typing:start")
          (switch-to-buffer buf)
          (erase-buffer)
          (emacs-lisp-mode)
          (redisplay t)
          (let* ((n (wlshm-redisplay-bench--frame-count "typing" 600 20))
                 (i 0)
                 (s (wlshm-redisplay-bench--frames
                     n (lambda ()
                         (insert (char-to-string (+ ?a (mod i 26))))
                         (setq i (1+ i))
                         (when (> (current-column) 150)
                           (insert "\n")))
                     "typing")))
            (wlshm-redisplay-bench--log "typing" n s)))
        (wlshm-redisplay-bench--progress "typing:done")

        (condition-case image-err
            (progn
              (wlshm-redisplay-bench--progress "image-scroll:start")
              (switch-to-buffer (wlshm-redisplay-bench--image-buffer))
              (delete-other-windows)
              (wlshm-redisplay-bench--top)
              (let* ((n (wlshm-redisplay-bench--frame-count "image-scroll" 500 12))
                     (s (wlshm-redisplay-bench--frames
                         n #'wlshm-redisplay-bench--line-scroll
                         "image-scroll")))
                (wlshm-redisplay-bench--log "image-scroll" n s))
              (wlshm-redisplay-bench--progress "image-scroll:done"))
          (error
           (wlshm-redisplay-bench--progress "image-scroll:error")
           (wlshm-redisplay-bench--log
            (format "image-scroll-skip:%S" image-err) 1 1.0))))
    (error
     (wlshm-redisplay-bench--progress "error")
     (write-region
      (format "ERR\t%s\t%s\t%s\t%S\n"
              (wlshm-redisplay-bench--env "WLSHM_BENCH_BACKEND" "unknown")
              (wlshm-redisplay-bench--env "WLSHM_BENCH_SCENARIO" "unknown")
              (wlshm-redisplay-bench--env "WLSHM_BENCH_RUN" "0")
              err)
      nil (wlshm-redisplay-bench--out-file) 'append 'silent)))
  (kill-emacs 0))

(unless (wlshm-redisplay-bench--env "WLSHM_BENCH_NO_AUTORUN")
  (run-with-timer 1.0 nil #'wlshm-redisplay-bench-run))

(provide 'redisplay-bench)

;;; redisplay-bench.el ends here
