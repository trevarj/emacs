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
  ;; Pin a LOGICAL default pixel size (explicit pixelsize so the face system
  ;; doesn't collapse the size), only if the user hasn't set a font.  HiDPI
  ;; crispness is handled by the Cairo device scale on the canvas, so the font
  ;; stays logical -- scaling it here too would double-scale the text.
  (unless (or (assq 'font default-frame-alist)
              (assq 'font initial-frame-alist))
    (push (cons 'font "Monospace:pixelsize=14") default-frame-alist))
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

;;; Drag-and-drop (receive).
;; A drop from another Wayland client arrives as a DRAG_N_DROP_EVENT built in
;; wlshmterm.c, whose .arg is `(uri-list . STRING)' (file drops) or
;; `(text . STRING)' (plain text).  We route it through dnd.el just like the X
;; and pgtk backends do.

(require 'dnd)

(defun wlshm-dnd-handle-drag-n-drop-event (event)
  "Receive a drag-and-drop EVENT on a wlshm frame.
EVENT is `(drag-n-drop POSITION (TAG . DATA))'.  When TAG is `uri-list'
the URIs are opened via `dnd-handle-multiple-urls'; when TAG is `text'
the data is inserted via `dnd-insert-text'."
  (interactive "e")
  (let* ((payload (nth 2 event))
         (tag (car-safe payload))
         (data (cdr-safe payload))
         (posn (event-start event))
         (window (posn-window posn))
         (frame (cond ((framep window) window)
                      ((windowp window) (window-frame window)))))
    (when frame
      (raise-frame frame)
      (select-frame frame))
    (when (windowp window)
      (select-window window))
    (cond
     ((eq tag 'uri-list)
      ;; text/uri-list: CRLF-separated URIs; skip blank and comment lines.
      (let ((urls (seq-remove
                   (lambda (s) (or (string-empty-p s)
                                   (string-prefix-p "#" s)))
                   (split-string (string-trim-right data) "[\r\n]+"))))
        (when urls
          (dnd-handle-multiple-urls window urls 'copy))))
     ((eq tag 'text)
      (dnd-insert-text window 'copy data)))))

(define-key special-event-map [drag-n-drop]
            #'wlshm-dnd-handle-drag-n-drop-event)


;;; Input method (preedit display).
;; A Wayland IME (zwp_text_input_v3) drives composition through wlshmterm.c,
;; which posts committed text as ordinary keystrokes and the in-progress
;; preedit as a PREEDIT_TEXT_EVENT.  We display the preedit with a zero-width
;; overlay at point, exactly as the pgtk/x/android backends do.  EVENT's
;; payload `(nth 1 event)' is a list of parts `(STRING . ATTRS)'; ATTRS may
;; carry `ul'/`fg'/`bg'.  text-input-v3 sends a single underlined segment.

(defvar wlshm-preedit-overlay nil
  "Overlay showing the IME preedit (composition) text, or nil.")

(defun wlshm-preedit-text (event)
  "Display IME preedit text carried by EVENT.
EVENT is a `preedit-text' event whose payload is a list of parts
\(STRING . ATTRS); a nil payload clears the preedit."
  (interactive "e")
  (when wlshm-preedit-overlay
    (delete-overlay wlshm-preedit-overlay))
  (setq wlshm-preedit-overlay nil)
  (let ((ovstr "")
        (idx 0)
        atts str color face-name)
    (dolist (part (nth 1 event))
      (setq str (car part))
      (setq face-name (intern (format "wlshm-im-%d" idx)))
      (eval `(defface ,face-name nil "Face of input method preedit." :group 'wlshm))
      (setq atts nil)
      (when (setq color (cdr-safe (assq 'fg (cdr part))))
        (setq atts (append atts `(:foreground ,color))))
      (when (setq color (cdr-safe (assq 'bg (cdr part))))
        (setq atts (append atts `(:background ,color))))
      (when (setq color (cdr-safe (assq 'ul (cdr part))))
        ;; `ul' may be t (use the face's default underline) or a color.
        (setq atts (append atts `(:underline ,color))))
      (face-spec-set face-name `((t . ,atts)))
      (add-text-properties 0 (length str) `(face ,face-name) str)
      (setq ovstr (concat ovstr str))
      (setq idx (1+ idx)))
    (when (> (length ovstr) 0)
      (setq wlshm-preedit-overlay (make-overlay (point) (point)))
      (overlay-put wlshm-preedit-overlay 'before-string ovstr))))

(define-key special-event-map [preedit-text] #'wlshm-preedit-text)

;;; Themed tool-bar icons (freedesktop icon themes, e.g. Adwaita).
;; The internal (no-toolkit) tool bar otherwise draws Emacs's built-in
;; etc/images bitmaps.  When a freedesktop icon theme is installed we resolve
;; each tool-bar item to its scalable SVG and let `find-image' use that; SVGs
;; rasterize crisply at the device scale (see FRAME_SCALE_FACTOR).  Resolution
;; falls back to the built-in bitmaps when no themed icon is found, so
;; `emacs -Q' still works with no theme present.

(defgroup wlshm nil
  "Raw-Wayland (wlshm) backend."
  :group 'environment)

(defvar wlshm--icon-base-dirs nil
  "Cached list of <data-dir>/icons base directories.")
(defvar wlshm--icon-theme-info (make-hash-table :test 'equal)
  "Cache mapping a theme name to (SUBDIRS . INHERITS) from its index.theme.")
(defvar wlshm--icon-file-cache (make-hash-table :test 'equal)
  "Cache mapping a freedesktop icon name to its SVG path, or the symbol `none'.")

(defun wlshm--icon-clear-caches (&rest _)
  "Flush themed-icon caches and the tool-bar keymap cache."
  (setq wlshm--icon-base-dirs nil)
  (clrhash wlshm--icon-theme-info)
  (clrhash wlshm--icon-file-cache)
  ;; The image / tool-bar caches only exist once a GUI frame does; skip them
  ;; at load/dump time (and tolerate their absence).
  (when (display-graphic-p)
    (when (fboundp 'tool-bar--flush-cache) (tool-bar--flush-cache))
    (ignore-errors (clear-image-cache))))

(defcustom wlshm-tool-bar-use-system-icons t
  "If non-nil, draw tool-bar icons from a freedesktop icon theme.
See `wlshm-icon-theme'.  Falls back to Emacs's built-in icons when a
themed icon cannot be found."
  :type 'boolean
  :group 'wlshm
  :version "31.1"
  :set (lambda (sym val) (set-default sym val) (wlshm--icon-clear-caches)))

(defcustom wlshm-icon-theme "Adwaita"
  "Name of the freedesktop icon theme used for tool-bar icons."
  :type 'string
  :group 'wlshm
  :version "31.1"
  :set (lambda (sym val) (set-default sym val) (wlshm--icon-clear-caches)))

(defcustom wlshm-tool-bar-icon-size 24
  "Logical pixel size for themed tool-bar icons.
Scalable icons render at this size so the tool bar stays uniform; the
device scale rasterizes them crisply."
  :type 'natnum
  :group 'wlshm
  :version "31.1")

(defconst wlshm--icon-name-map
  '(("new" . "document-new") ("open" . "document-open")
    ("diropen" . "folder-open") ("close" . "window-close")
    ("save" . "document-save") ("saveas" . "document-save-as")
    ("undo" . "edit-undo") ("redo" . "edit-redo")
    ("cut" . "edit-cut") ("copy" . "edit-copy") ("paste" . "edit-paste")
    ("search" . "edit-find") ("search-replace" . "edit-find-replace")
    ("print" . "document-print") ("preferences" . "preferences-system")
    ("help" . "help-browser") ("left-arrow" . "go-previous")
    ("right-arrow" . "go-next") ("home" . "go-home") ("jump-to" . "go-jump")
    ("exit" . "application-exit") ("info" . "dialog-information")
    ("delete" . "edit-delete") ("refresh" . "view-refresh")
    ("spell" . "tools-check-spelling") ("describe" . "document-properties")
    ("attach" . "mail-attachment") ("connect" . "network-connect")
    ("sort-ascending" . "view-sort-ascending")
    ("sort-descending" . "view-sort-descending")
    ("bookmark_add" . "bookmark-new") ("cancel" . "process-stop"))
  "Map Emacs tool-bar icon base names to freedesktop icon names.")

(defun wlshm--icon-base-dirs ()
  "Return the freedesktop icon search directories, in priority order."
  (or wlshm--icon-base-dirs
      (setq wlshm--icon-base-dirs
            (let (dirs)
              (dolist (d (append
                          (list (or (getenv "XDG_DATA_HOME")
                                    (expand-file-name "~/.local/share")))
                          (split-string (or (getenv "XDG_DATA_DIRS") "") ":" t)
                          (list "/usr/share" "/usr/local/share")))
                (let ((id (expand-file-name "icons" d)))
                  (when (file-directory-p id) (push id dirs))))
              (let ((h (expand-file-name "~/.icons")))
                (when (file-directory-p h) (push h dirs)))
              (delete-dups (nreverse dirs))))))

(defun wlshm--icon-theme-info (theme)
  "Return (SUBDIRS . INHERITS) parsed from THEME's index.theme, cached."
  (or (gethash theme wlshm--icon-theme-info)
      (puthash
       theme
       (let (subdirs inherits)
         (catch 'done
           (dolist (base (wlshm--icon-base-dirs))
             (let ((idx (expand-file-name (format "%s/index.theme" theme) base)))
               (when (file-readable-p idx)
                 (with-temp-buffer
                   (insert-file-contents idx)
                   (goto-char (point-min))
                   (when (re-search-forward "^Directories=\\(.*\\)$" nil t)
                     (setq subdirs (split-string (match-string 1) "," t)))
                   (goto-char (point-min))
                   (when (re-search-forward "^Inherits=\\(.*\\)$" nil t)
                     (setq inherits (split-string (match-string 1) "," t))))
                 (throw 'done nil)))))
         (cons subdirs inherits))
       wlshm--icon-theme-info)))

(defun wlshm--find-themed-icon-1 (name theme seen)
  "Search THEME (and its inherited themes) for SVG icon NAME.
SEEN guards against inheritance cycles."
  (unless (member theme seen)
    (push theme seen)
    (let* ((info (wlshm--icon-theme-info theme))
           (subdirs (car info))
           (inherits (cdr info)))
      (or
       ;; Prefer symbolic (monochrome, uniform) SVGs, then scalable color ones;
       ;; within each, prefer the NAME-symbolic spelling.
       (catch 'hit
         (dolist (want '("symbolic" "scalable"))
           (dolist (subdir subdirs)
             (when (string-search want subdir)
               (dolist (base (wlshm--icon-base-dirs))
                 (dolist (cand
                          (list (format "%s/%s/%s/%s-symbolic.svg"
                                        base theme subdir name)
                                (format "%s/%s/%s/%s.svg" base theme subdir name)))
                   (when (file-readable-p cand) (throw 'hit cand)))))))
         nil)
       (catch 'hit
         (dolist (parent inherits)
           (let ((r (wlshm--find-themed-icon-1 name parent seen)))
             (when r (throw 'hit r))))
         nil)))))

(defun wlshm--find-themed-icon (name)
  "Resolve freedesktop icon NAME to an SVG file path (cached), or nil."
  (let ((hit (gethash name wlshm--icon-file-cache 'miss)))
    (if (not (eq hit 'miss))
        (and (stringp hit) hit)
      (let ((path (or (wlshm--find-themed-icon-1 name wlshm-icon-theme nil)
                      (wlshm--find-themed-icon-1 name "hicolor" nil))))
        (puthash name (or path 'none) wlshm--icon-file-cache)
        path))))

(defun wlshm--icon-svg-spec (path)
  "Build an SVG image spec for the themed icon at PATH.
The `tool-bar' face foreground recolors symbolic (monochrome) icons."
  (let ((fg (face-attribute 'tool-bar :foreground nil 'default)))
    (append (list :type 'svg :file path
                  :width wlshm-tool-bar-icon-size
                  :height wlshm-tool-bar-icon-size)
            (unless (eq fg 'unspecified) (list :foreground fg)))))

(defun wlshm--find-image-advice (orig specs &optional cache)
  "Around `find-image': prefer a themed SVG for known tool-bar icons.
Only active on wlshm frames with SVG support; otherwise a no-op.  The
themed spec is prepended, so a load failure falls back to SPECS."
  (let ((svg (and wlshm-tool-bar-use-system-icons
                  (eq (window-system) 'wlshm)
                  (image-type-available-p 'svg)
                  (consp specs)
                  (ignore-errors
                    (let* ((file (plist-get (car specs) :file))
                           (base (and (stringp file) (file-name-base file)))
                           (fd (and base (cdr (assoc base wlshm--icon-name-map)))))
                      (and fd (wlshm--find-themed-icon fd)))))))
    (funcall orig (if svg (cons (wlshm--icon-svg-spec svg) specs) specs)
             cache)))

(advice-add 'find-image :around #'wlshm--find-image-advice)

;; Any display name maps to the wlshm backend.
(add-to-list 'display-format-alist '(".*" . wlshm))

(provide 'wlshm-win)
(provide 'term/wlshm-win)
;;; wlshm-win.el ends here
