;;; wgpu-win.el --- Wayland + wgpu window system support  -*- lexical-binding: t -*-

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

;; Window-system glue for the experimental "wgpu" backend (raw Wayland +
;; GPU rendering via Rust/FFI).  Mirrors term/pgtk-win.el: it registers the
;; `wgpu' window system's initialization, argument handling and frame
;; creation.  See wgpu-backend-plan.md.

;;; Code:

(eval-when-compile (require 'cl-lib))
(require 'frame)
(require 'mouse)
(require 'faces)

(defvar x-command-line-resources)

;; Reuse the shared X-style command-line handling (geometry, font, fg/bg, ...).
(declare-function x-handle-args "common-win" (args))
(declare-function x-open-connection "wgpufns.c"
                  (display &optional xrm-string must-succeed))
(declare-function x-create-frame-with-faces "faces" (&optional parameters))

(defvar wgpu-initialized nil
  "Non-nil if the wgpu backend has been initialized.")

(cl-defmethod handle-args-function (args &context (window-system wgpu))
  (x-handle-args args))

(cl-defmethod frame-creation-function (params &context (window-system wgpu))
  (x-create-frame-with-faces params))

(cl-defmethod window-system-initialization (&context (window-system wgpu)
                                            &optional display)
  "Initialize the wgpu backend; DISPLAY is ignored (single Wayland display)."
  (cl-assert (not wgpu-initialized))
  (create-default-fontset)
  (x-open-connection (or display "wayland") x-command-line-resources t)
  (setq wgpu-initialized t))

;; Any display name maps to the wgpu backend.
(add-to-list 'display-format-alist '(".*" . wgpu))

(provide 'term/wgpu-win)
;;; wgpu-win.el ends here
