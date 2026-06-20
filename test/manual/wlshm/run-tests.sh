#!/usr/bin/env bash
# run-tests.sh --- wlshm backend graphical test harness.
#
# Three tiers, all CPU/Cairo (no GPU):
#   1. render smoke      (headless weston: open a window, dump a non-empty frame)
#   2. on-screen goldens (headless weston, real frame, no input)
#   3. interaction tests (nested niri, virtual input, state asserts)
#
# Usage (inside the dev shell):
#   guix shell -m manifest.scm weston wtype wlrctl -- \
#     env WLSHM_PARENT_WL=wayland-1 test/manual/wlshm/run-tests.sh [all|1|2|3]
#
#   WLSHM_REGEN=1   regenerate golden images instead of comparing (review!).
#   WLSHM_PARENT_WL the parent Wayland display for nested niri (tier 3 skips if unset).
#
# Exit non-zero if any test fails.  Hangs/crashes are caught by a watchdog that
# writes a gdb backtrace to $WLSHM_TMP/<name>.bt instead of hanging forever.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"
cd "$WLSHM_ROOT" || exit 2
wlshm_setup_env

TIER="${1:-all}"

# --- Tier 1: render smoke (headless weston) -------------------------------
# CPU/Cairo frames need a real wl_surface, so there is no compositor-less
# offscreen tier anymore.  Instead: open a window, render text, dump a frame,
# and assert it is a non-trivial PNG (catches a totally-broken render path
# fast, before the per-scene golden compares in tier 2).
run_tier1 () {
  echo "== Tier 1: render smoke (weston) =="
  command -v weston >/dev/null 2>&1 || { echo "  SKIP (weston not found)"; return; }
  wlshm_start_weston wlshm-t1 1000 700 || { wlshm_fail "tier1 weston"; return; }
  out="$WLSHM_TMP/smoke.png"; rm -f "$out"
  WAYLAND_DISPLAY=wlshm-t1 WLSHM_DEBUG=1 "$WLSHM_EMACS" -Q \
    --eval "(progn (insert \"render smoke\\n\") (dotimes (i 20) (insert (format \"line %d\\n\" i))) (redisplay t) (redisplay t) (wlshm-dump-canvas \"$out\") (kill-emacs 0))" \
    >"$WLSHM_TMP/tier1.log" 2>&1 || true
  # A blank/failed render is a few hundred bytes; a real text frame is several KB.
  if [ -f "$out" ] && [ "$(stat -c%s "$out" 2>/dev/null || echo 0)" -gt 2000 ]
  then wlshm_pass "render smoke ($out)"
  else wlshm_fail "render smoke (no/empty dump -- see $WLSHM_TMP/tier1.log)"; fi
}

# --- Tier 2: on-screen rendering goldens (weston) -------------------------
run_tier2 () {
  echo "== Tier 2: on-screen rendering goldens (weston) =="
  command -v weston >/dev/null 2>&1 || { echo "  SKIP (weston not found)"; return; }
  wlshm_start_weston wlshm-t2 1200 800 || { wlshm_fail "tier2 weston"; return; }
  wlshm_start_server wlshmt2 wlshm-t2 || { wlshm_fail "tier2 emacs server"; return; }
  for scene in plain region fringe faces dividers image scrollbar menu; do
    out="$WLSHM_TMP/scene-$scene.png"; rm -f "$out"
    _=$(wlshm_eval wlshmt2 25 "(wlshm-test-render \"$scene\" \"$out\")")
    wlshm_compare_png "$out" "$WLSHM_GOLDEN/scene-$scene.png" "render:$scene"
  done
  # Tooltip: x-show-tip renders a tip frame (verified visually) and x-hide-tip
  # reports it was open.  Assert show doesn't error and hide returns t.
  tip=$(wlshm_eval wlshmt2 15 "(progn (x-show-tip \"tip line one\nsecond line\" (selected-frame) nil 30 10 10) (x-hide-tip))")
  [ "$tip" = "t" ] && wlshm_pass "tooltip show/hide" \
    || wlshm_fail "tooltip show/hide (got: $tip)"
  emacsclient -s wlshmt2 --eval '(kill-emacs 0)' >/dev/null 2>&1 || true
}

# --- Tier 3: interaction (nested niri) ------------------------------------
run_tier3 () {
  echo "== Tier 3: interaction (nested niri) =="
  command -v niri  >/dev/null 2>&1 || { echo "  SKIP (niri not found)"; return; }
  command -v wtype >/dev/null 2>&1 || { echo "  SKIP (wtype not found)"; return; }
  command -v wlrctl >/dev/null 2>&1 || { echo "  SKIP (wlrctl not found)"; return; }
  [ -n "${WLSHM_PARENT_WL:-}" ] || { echo "  SKIP (set WLSHM_PARENT_WL, e.g. wayland-1)"; return; }

  wl=$(wlshm_start_niri) || { wlshm_fail "nested niri"; return; }
  echo "  nested niri on $wl"
  wlshm_start_server wlshmt3 "$wl" || { wlshm_fail "tier3 emacs server"; return; }
  _=$(wlshm_eval wlshmt3 15 "(wlshm-test-setup-buffer)")

  # Pointer motion + buffer click: exercises buffer_posn_from_coords (the
  # font-metrics freeze) and down-mouse-1 drag tracking.  Coords need not be
  # exact -- any motion/click over the window hits the same code paths.
  wlshm_pointer_move "$wl" 150 80
  sleep 0.3
  if [ "$(wlshm_eval wlshmt3 10 "(wlshm-test-tick)")" = "t" ]; then
    wlshm_pass "pointer motion: responsive"
  else wlshm_fail "pointer motion: FROZE (see $WLSHM_TMP/wlshmt3-hang.bt)"; fi

  wlshm_click "$wl" left; sleep 0.3
  wlshm_pointer_move "$wl" 0 600; wlshm_click "$wl" left   # toward the mode line
  sleep 0.5
  if [ "$(wlshm_eval wlshmt3 10 "(wlshm-test-tick)")" = "t" ]; then
    wlshm_pass "click (buffer + mode-line area): no freeze"
  else wlshm_fail "click: FROZE (see $WLSHM_TMP/wlshmt3-hang.bt)"; fi

  # Right-click (would have hit the NULL menu_show_hook crash).
  wlshm_click "$wl" right; sleep 0.3
  if [ "$(wlshm_eval wlshmt3 10 "(wlshm-test-tick)")" = "t" ]; then
    wlshm_pass "right-click: no crash"
  else wlshm_fail "right-click: crashed/froze (see $WLSHM_TMP/wlshmt3-hang.bt)"; fi

  # Key repeat: hold a key ~1s, expect several chars inserted.
  _=$(wlshm_eval wlshmt3 10 "(progn (switch-to-buffer \"*interact*\")(goto-char (point-max))(insert \"\n\"))")
  wlshm_key_press "$wl" a; sleep 1; wlshm_key_release "$wl" a; sleep 0.3
  n=$(wlshm_eval wlshmt3 10 "(- (line-end-position) (line-beginning-position))")
  if [ "${n:-0}" -gt 1 ] 2>/dev/null; then
    wlshm_pass "key repeat: $n chars from held key"
  else wlshm_fail "key repeat: only ${n:-0} char(s) (repeat not working?)"; fi

  emacsclient -s wlshmt3 --eval '(kill-emacs 0)' >/dev/null 2>&1 || true
}

case "$TIER" in
  all) run_tier1; run_tier2; run_tier3 ;;
  1) run_tier1 ;;  2) run_tier2 ;;  3) run_tier3 ;;
  *) echo "usage: $0 [all|1|2|3]"; exit 2 ;;
esac

echo "==========================================="
echo "wlshm tests: $WLSHM_PASSES passed, $WLSHM_FAILS failed"
[ "$WLSHM_FAILS" -eq 0 ]
