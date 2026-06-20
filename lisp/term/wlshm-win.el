;;; wlshm-win.el --- Wayland + wlshm window system support  -*- lexical-binding: t -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or (at
;; your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; Window-system glue for the experimental "wlshm" backend (raw Wayland +
;; GPU rendering via Rust/FFI).  Mirrors term/pgtk-win.el: it registers the
;; `wlshm' window system's initialization, argument handling and frame
;; creation.  See wlshm-backend-plan.md.

;;; Code:

(eval-when-compile (require 'cl-lib))
(require 'frame)
(require 'mouse)
(require 'faces)

(defvar x-command-line-resources)

;; Reuse the shared X-style command-line handling (geometry, font, fg/bg, ...).
(declare-function x-handle-args "common-win" (args))
(declare-function x-open-connection "wlshmfns.c"
                  (display &optional xrm-string must-succeed))
(declare-function x-create-frame-with-faces "faces" (&optional parameters))

(defvar wlshm-initialized nil
  "Non-nil if the wlshm backend has been initialized.")

(cl-defmethod handle-args-function (args &context (window-system wlshm))
  (x-handle-args args))

(cl-defmethod frame-creation-function (params &context (window-system wlshm))
  (x-create-frame-with-faces params))

(cl-defmethod window-system-initialization (&context (window-system wlshm)
                                            &optional display)
  "Initialize the wlshm backend; DISPLAY is ignored (single Wayland display)."
  (cl-assert (not wlshm-initialized))
  (create-default-fontset)
  (x-open-connection (or display "wayland") x-command-line-resources t)
  ;; Normalize font size for HiDPI: pick a pixel size scaled by the output
  ;; scale factor, and pin it (with an explicit pixelsize so the face system
  ;; doesn't collapse the size).  Only if the user hasn't set a font.
  (let ((scale (if (fboundp 'wlshm-scale-factor) (wlshm-scale-factor) 1)))
    (unless (or (assq 'font default-frame-alist)
                (assq 'font initial-frame-alist))
      (push (cons 'font (format "Monospace:pixelsize=%d" (* 14 (max 1 scale))))
            default-frame-alist)))
  (setq wlshm-initialized t))

;;; Selection / clipboard.
;; Backed by the Wayland data-device (system CLIPBOARD) and the
;; primary-selection protocol (PRIMARY).  The internal functions dispatch on
;; the selection symbol; SECONDARY is unsupported (kept in the kill ring).

(declare-function wlshm-own-selection-internal "wlshmfns.c"
                  (selection value &optional frame))
(declare-function wlshm-disown-selection-internal "wlshmfns.c"
                  (selection &optional time-object terminal))
(declare-function wlshm-get-selection-internal "wlshmfns.c"
                  (selection-symbol target-type &optional time-stamp terminal))
(declare-function wlshm-selection-owner-p "wlshmfns.c"
                  (&optional selection terminal))
(declare-function wlshm-selection-exists-p "wlshmfns.c"
                  (&optional selection terminal))

;; Selections handled natively by the wlshm backend.
(defconst wlshm--native-selections '(CLIPBOARD PRIMARY))

(cl-defmethod gui-backend-set-selection (selection value
                                         &context (window-system wlshm))
  (if (not (memq selection wlshm--native-selections))
      ;; Let the default (kill-ring) handling stand for SECONDARY etc.
      'foreign-selection
    (if value
        (wlshm-own-selection-internal selection value)
      (wlshm-disown-selection-internal selection))))

(cl-defmethod gui-backend-get-selection (selection-symbol target-type
                                         &context (window-system wlshm))
  (when (memq selection-symbol wlshm--native-selections)
    (wlshm-get-selection-internal selection-symbol target-type)))

(cl-defmethod gui-backend-selection-owner-p (selection
                                             &context (window-system wlshm))
  (and (memq selection wlshm--native-selections)
       (wlshm-selection-owner-p selection)))

(cl-defmethod gui-backend-selection-exists-p (selection
                                              &context (window-system wlshm))
  (and (memq selection wlshm--native-selections)
       (wlshm-selection-exists-p selection)))

;; Any display name maps to the wlshm backend.
(add-to-list 'display-format-alist '(".*" . wlshm))

(provide 'wlshm-win)
(provide 'term/wlshm-win)
;;; wlshm-win.el ends here
