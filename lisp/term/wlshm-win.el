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

;; Window-system glue for the experimental "wlshm" backend (raw Wayland with
;; CPU Cairo rendering presented via wl_shm; window/event plumbing in Rust over
;; FFI).  Mirrors term/pgtk-win.el: it registers the
;; `wlshm' window system's initialization, argument handling and frame
;; creation.  See wlshm-architecture.md.

;;; Code:

(eval-when-compile (require 'cl-lib))
(require 'frame)
(require 'mouse)
(require 'faces)
(require 'fontset)

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
  ;; Create the standard fontset, like every other graphical backend, so
  ;; "fontset-standard" exists for set-frame-font / fontset selection.
  (condition-case err
      (create-fontset-from-fontset-spec standard-fontset-spec t)
    (error (display-warning
            'initialization
            (format "Creation of the standard fontset failed: %s" err)
            :error)))
  (x-open-connection (or display "wayland") x-command-line-resources t)
  ;; The default font (12pt, derived from the display resolution to ~16px at
  ;; 96 DPI, matching pgtk) is chosen in C by wlshm_default_font_parameter when
  ;; the user hasn't set one -- so it tracks the resolution rather than being a
  ;; fixed pixel count pinned here.  HiDPI crispness is handled by the Cairo
  ;; device scale on the canvas; the font stays logical (no double-scaling).
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
                      ((windowp window) (window-frame window))))
         ;; POSN-WINDOW is a frame (not a live window) for drops on the
         ;; tool/menu bar, internal border, etc.  `dnd-handle-multiple-urls'
         ;; wraps its body in `with-selected-window', which rejects a frame,
         ;; so resolve a live window to dispatch URI drops into.  (The `text'
         ;; branch keeps WINDOW: `dnd-insert-text' has its own non-window
         ;; kill-ring fallback.)
         (win (if (window-live-p window)
                  window
                (frame-selected-window (or frame (selected-frame))))))
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
          (dnd-handle-multiple-urls win urls 'copy))))
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

(defface wlshm-im-preedit
  '((t :underline t))
  "Face for input method preedit (composition) text.
text-input-v3 only ever sends a single underlined segment, so this
single reusable face covers the default styling.  Per-event color or
underline overrides are layered on top via the `face' text property."
  :group 'wlshm)

(defun wlshm-preedit-text (event)
  "Display IME preedit text carried by EVENT.
EVENT is a `preedit-text' event whose payload is a list of parts
\(STRING . ATTRS); a nil payload clears the preedit."
  (interactive "e")
  (when wlshm-preedit-overlay
    (delete-overlay wlshm-preedit-overlay))
  (setq wlshm-preedit-overlay nil)
  (let ((ovstr "")
        atts str color face)
    (dolist (part (nth 1 event))
      (setq str (car part))
      (setq atts nil)
      (when (setq color (cdr-safe (assq 'fg (cdr part))))
        (setq atts (append atts `(:foreground ,color))))
      (when (setq color (cdr-safe (assq 'bg (cdr part))))
        (setq atts (append atts `(:background ,color))))
      (when (setq color (cdr-safe (assq 'ul (cdr part))))
        ;; `ul' may be t (use the face's default underline) or a color.
        (setq atts (append atts `(:underline ,color))))
      ;; The `face' property accepts an attribute plist and a list of
      ;; faces/plists.  Layer any per-event overrides on top of the
      ;; reusable `wlshm-im-preedit' face instead of interning a fresh
      ;; symbol and `eval'ing a `defface' on every keystroke.
      (setq face (if atts (list atts 'wlshm-im-preedit) 'wlshm-im-preedit))
      (add-text-properties 0 (length str) (list 'face face) str)
      (setq ovstr (concat ovstr str)))
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
  :version "31.1"
  :set (lambda (sym val) (set-default sym val) (wlshm--icon-clear-caches)))

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

(defun wlshm--icon-theme-file (theme relpaths)
  "Return the first readable <base>/THEME/REL file across the icon base-dirs.
RELPATHS is a relative path string, or a list of them tried in order.
For each base directory (in priority order) every relpath is tried before
moving on to the next base, so base-dir priority dominates RELPATHS order."
  (let ((rels (if (listp relpaths) relpaths (list relpaths))))
    (catch 'hit
      (dolist (base (wlshm--icon-base-dirs))
        (dolist (rel rels)
          (let ((path (expand-file-name (format "%s/%s" theme rel) base)))
            (when (file-readable-p path) (throw 'hit path)))))
      nil)))

(defun wlshm--icon-theme-info (theme)
  "Return (SUBDIRS . INHERITS) parsed from THEME's index.theme, cached."
  (or (gethash theme wlshm--icon-theme-info)
      (puthash
       theme
       (let ((idx (wlshm--icon-theme-file theme "index.theme"))
             subdirs inherits)
         (when idx
           (with-temp-buffer
             (insert-file-contents idx)
             (goto-char (point-min))
             (when (re-search-forward "^Directories=\\(.*\\)$" nil t)
               (setq subdirs (split-string (match-string 1) "," t)))
             (goto-char (point-min))
             (when (re-search-forward "^Inherits=\\(.*\\)$" nil t)
               (setq inherits (split-string (match-string 1) "," t)))))
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
       ;; within each, prefer the NAME-symbolic spelling.  The candidate order
       ;; (preserved exactly) is, for each WANT then matching SUBDIR:
       ;;   base0/NAME-symbolic.svg, base0/NAME.svg,
       ;;   base1/NAME-symbolic.svg, base1/NAME.svg, ...
       ;; i.e. base-dir priority dominates, suffix spelling tie-breaks.
       (catch 'hit
         (dolist (want '("symbolic" "scalable"))
           (dolist (subdir subdirs)
             (when (string-search want subdir)
               (let ((path (wlshm--icon-theme-file
                            theme
                            (mapcar (lambda (suffix)
                                      (format "%s/%s%s" subdir name suffix))
                                    '("-symbolic.svg" ".svg")))))
                 (when path (throw 'hit path))))))
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

;; Modern scroll-bar UX: left-button drags the thumb.  wlshm draws its own bar
;; using the no-toolkit (xterm) event model, whose default bindings put
;; thumb-drag on the MIDDLE button and bind left drag-mouse-1 to
;; `scroll-bar-scroll-up' -- so dragging the thumb with the left button jumps
;; erratically to the release point instead of tracking.  Rebind the left
;; button to the drag-tracking commands so the bar behaves like a modern
;; toolkit scroll bar (press-and-drag the thumb; click off the thumb scrolls
;; there).  `scroll-bar-drag' already does the track-mouse loop and wlshm
;; reports the handle position via its mouse-position hook.
(global-set-key [vertical-scroll-bar down-mouse-1] 'scroll-bar-drag)
(global-set-key [horizontal-scroll-bar down-mouse-1] 'scroll-bar-horizontal-drag)

(provide 'wlshm-win)
(provide 'term/wlshm-win)
;;; wlshm-win.el ends here
