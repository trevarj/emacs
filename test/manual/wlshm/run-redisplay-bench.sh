#!/usr/bin/env bash
# run-redisplay-bench.sh --- wlshm/pgtk/x11 redisplay benchmark.
#
# Primary scenario: Wayland clients inside a private nested niri configured for
# a 3840x2160 output at 1.5x scale.  This is deliberately not run against the
# user's live compositor.  Cleanup only kills exact PIDs spawned here.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"
cd "$WLSHM_ROOT" || exit 2
wlshm_setup_env

RUNS=3
FRAME_SCALE=full
COLS=160
ROWS=48
NIRI_WIDTH=3840
NIRI_HEIGHT=2160
NIRI_SCALE=1.5
NIRI_OUTPUT="${WLSHM_BENCH_NIRI_OUTPUT:-}"
RUN_X11=1
RUN_WAYLAND=1
BENCH_NIRI_WL=
BENCH_NIRI_PID=
BENCH_PARENT_FOCUS_ID=

usage () {
  cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --runs N              repetitions per backend (default: 3)
  --frames-scale NAME   full or smoke (default: full)
  --cols N              initial frame columns before fullscreen (default: 160)
  --rows N              initial frame rows before fullscreen (default: 48)
  --niri-scale N        nested niri output scale (default: 1.5)
  --niri-size WxH       nested niri physical output size (default: 3840x2160)
  --niri-output NAME    output name to configure (default: common nested names)
  --x11-only            only run the Xvfb/X11 baseline
  --wayland-only        only run wlshm and pgtk in nested niri
  -h, --help            show this help

Set WLSHM_PARENT_WL to the parent Wayland socket for Wayland runs, e.g.
WLSHM_PARENT_WL=wayland-1 $0 --runs 3
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --runs) RUNS=$2; shift 2 ;;
    --frames-scale) FRAME_SCALE=$2; shift 2 ;;
    --cols) COLS=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --niri-scale) NIRI_SCALE=$2; shift 2 ;;
    --niri-size)
      NIRI_WIDTH=${2%x*}
      NIRI_HEIGHT=${2#*x}
      shift 2 ;;
    --niri-output) NIRI_OUTPUT=$2; shift 2 ;;
    --x11-only) RUN_WAYLAND=0; RUN_X11=1; shift ;;
    --wayland-only) RUN_WAYLAND=1; RUN_X11=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

case "$RUNS" in (*[!0-9]*|"") echo "invalid --runs: $RUNS" >&2; exit 2 ;; esac

EL="$WLSHM_HERE/redisplay-bench.el"
OUT_DIR="$WLSHM_TMP/redisplay-bench-$(date +%Y%m%d-%H%M%S)"
OUT="$OUT_DIR/results.tsv"
mkdir -p "$OUT_DIR"
printf '# backend\tscenario\trun\tworkload\tframes\tseconds\tfps\tms_per_frame\tframe_px_w\tframe_px_h\tmonitor_scale\twindow_system\temacs_version\n' >"$OUT"

write_niri_config () {
  conf=$1
  cat >"$conf" <<EOF
input {
    keyboard {
        repeat-delay 250
        repeat-rate 50
    }
}

layout {
    gaps 0
    center-focused-column "never"
    default-column-width { proportion 1.0; }
    preset-column-widths { proportion 1.0; }
}

prefer-no-csd

EOF

  if [ -n "$NIRI_OUTPUT" ]; then
    cat >>"$conf" <<EOF
output "$NIRI_OUTPUT" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
EOF
    return
  fi

  cat >>"$conf" <<EOF
// Nested/winit output names can vary.  Niri ignores unmatched output blocks.
output "winit" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
output "WL-1" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
output "HEADLESS-1" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
output "eDP-1" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
output "HDMI-A-1" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
output "Virtual-1" {
    mode "${NIRI_WIDTH}x${NIRI_HEIGHT}"
    scale ${NIRI_SCALE}
    focus-at-startup
}
EOF
}

start_bench_niri () {
  [ -n "${WLSHM_PARENT_WL:-}" ] || {
    echo "WLSHM_PARENT_WL must be set for nested niri Wayland benchmarks" >&2
    return 1
  }
  command -v niri >/dev/null 2>&1 || { echo "niri not found" >&2; return 1; }
  conf="$OUT_DIR/niri-bench.kdl"
  log="$OUT_DIR/niri.log"
  write_niri_config "$conf"
  WAYLAND_DISPLAY="$WLSHM_PARENT_WL" niri -c "$conf" >"$log" 2>&1 &
  pid=$!
  BENCH_NIRI_PID=$pid
  wlshm_track "$pid"
  for _ in $(seq 1 80); do
    sock=$(grep -oE "Wayland socket: wayland-[0-9]+" "$log" | head -1 \
             | grep -oE "wayland-[0-9]+$" || true)
    if [ -n "$sock" ] && [ -S "$XDG_RUNTIME_DIR/$sock" ]; then
      BENCH_NIRI_WL=$sock
      return 0
    fi
    kill -0 "$pid" 2>/dev/null || {
      echo "nested niri exited early; see $log" >&2
      return 1
    }
    sleep 0.25
  done
  echo "nested niri socket did not appear; see $log" >&2
  return 1
}

parent_niri_msg () {
  WAYLAND_DISPLAY="$WLSHM_PARENT_WL" niri msg "$@"
}

parent_niri_focused_id () {
  parent_niri_msg focused-window 2>/dev/null \
    | sed -n 's/^Window ID \([0-9][0-9]*\):.*/\1/p' \
    | head -1
}

parent_niri_window_id_for_pid () {
  pid=$1
  parent_niri_msg -j windows 2>/dev/null \
    | tr '{' '\n' \
    | awk -v pid="$pid" '
        index($0, "\"pid\":" pid) {
          if (match($0, /"id":[0-9]+/)) {
            id = substr($0, RSTART + 5, RLENGTH - 5)
            print id
            exit
          }
        }
      '
}

fullscreen_parent_niri_window () {
  BENCH_PARENT_FOCUS_ID=$(parent_niri_focused_id || true)
  for _ in $(seq 1 80); do
    id=$(parent_niri_window_id_for_pid "$BENCH_NIRI_PID")
    if [ -n "$id" ]; then
      parent_niri_msg action focus-window --id "$id" >/dev/null
      sleep 0.2
      parent_niri_msg action fullscreen-window --id "$id" >/dev/null
      sleep 1
      return 0
    fi
    sleep 0.25
  done
  echo "could not find nested niri parent window for pid $BENCH_NIRI_PID" >&2
  return 1
}

stop_bench_niri () {
  if [ -n "$BENCH_NIRI_PID" ]; then
    kill "$BENCH_NIRI_PID" 2>/dev/null || true
    wait "$BENCH_NIRI_PID" 2>/dev/null || true
  fi
  if [ -n "$BENCH_PARENT_FOCUS_ID" ]; then
    parent_niri_msg action focus-window --id "$BENCH_PARENT_FOCUS_ID" \
      >/dev/null 2>&1 || true
  fi
  BENCH_NIRI_PID=
  BENCH_NIRI_WL=
  BENCH_PARENT_FOCUS_ID=
}

run_wlshm () {
  wl=$1 run=$2
  scenario="niri-${NIRI_WIDTH}x${NIRI_HEIGHT}-scale-${NIRI_SCALE}"
  echo "  wlshm run $run/$RUNS ($scenario)"
  backend=wlshm
  WLSHM_BENCH_BACKEND=$backend \
  WLSHM_BENCH_SCENARIO=$scenario \
  WLSHM_BENCH_RUN=$run \
  WLSHM_BENCH_OUT=$OUT \
  WLSHM_BENCH_PROGRESS="$OUT_DIR/wlshm-$run.progress" \
  WLSHM_BENCH_FRAME_SCALE=$FRAME_SCALE \
  WLSHM_BENCH_COLS=$COLS \
  WLSHM_BENCH_ROWS=$ROWS \
  WLSHM_BENCH_FULLSCREEN=1 \
  WLSHM_BENCH_NO_AUTORUN=1 \
  WAYLAND_DISPLAY="$wl" WLSHM_DEBUG=1 \
    "$WLSHM_EMACS" -Q -l "$EL" --eval '(wlshm-redisplay-bench-run)' \
    >"$OUT_DIR/wlshm-$run.log" 2>&1
}

run_pgtk () {
  wl=$1 run=$2
  scenario="niri-${NIRI_WIDTH}x${NIRI_HEIGHT}-scale-${NIRI_SCALE}"
  echo "  pgtk run $run/$RUNS ($scenario)"
  WLSHM_BENCH_BACKEND=pgtk \
  WLSHM_BENCH_SCENARIO=$scenario \
  WLSHM_BENCH_RUN=$run \
  WLSHM_BENCH_OUT=$OUT \
  WLSHM_BENCH_PROGRESS="$OUT_DIR/pgtk-$run.progress" \
  WLSHM_BENCH_FRAME_SCALE=$FRAME_SCALE \
  WLSHM_BENCH_COLS=$COLS \
  WLSHM_BENCH_ROWS=$ROWS \
  WLSHM_BENCH_FULLSCREEN=1 \
  WLSHM_BENCH_NO_AUTORUN=1 \
  WAYLAND_DISPLAY="$wl" GDK_BACKEND=wayland \
    guix shell emacs-next-pgtk -- emacs -Q -l "$EL" \
      --eval '(wlshm-redisplay-bench-run)' >"$OUT_DIR/pgtk-$run.log" 2>&1
}

run_x11 () {
  run=$1
  scenario="xvfb-${NIRI_WIDTH}x${NIRI_HEIGHT}-scale-1.0"
  echo "  x11 run $run/$RUNS ($scenario)"
  display=":$((100 + (($$ + run) % 800)))"
  WLSHM_BENCH_BACKEND=x11 \
  WLSHM_BENCH_SCENARIO=$scenario \
  WLSHM_BENCH_RUN=$run \
  WLSHM_BENCH_OUT=$OUT \
  WLSHM_BENCH_PROGRESS="$OUT_DIR/x11-$run.progress" \
  WLSHM_BENCH_FRAME_SCALE=$FRAME_SCALE \
  WLSHM_BENCH_COLS=$COLS \
  WLSHM_BENCH_ROWS=$ROWS \
  WLSHM_BENCH_FULLSCREEN=1 \
  WLSHM_BENCH_NO_AUTORUN=1 \
    guix shell emacs-next xorg-server -- bash -c '
      set -u
      display=$1
      screen=$2
      el=$3
      xvfb_log=$4
      Xvfb "$display" -screen 0 "$screen" -nolisten tcp -ac >"$xvfb_log" 2>&1 &
      pid=$!
      trap '"'"'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true'"'"' EXIT
      sleep 1
      kill -0 "$pid" 2>/dev/null || {
        cat "$xvfb_log" >&2
        exit 1
      }
      DISPLAY="$display" GDK_BACKEND=x11 emacs -Q -l "$el" \
        --eval "(wlshm-redisplay-bench-run)"
    ' sh "$display" "${NIRI_WIDTH}x${NIRI_HEIGHT}x24" "$EL" \
      "$OUT_DIR/xvfb-$run.log" >"$OUT_DIR/x11-$run.log" 2>&1
}

median_for () {
  backend=$1 workload=$2
  awk -F '\t' -v b="$backend" -v w="$workload" \
    'NF >= 13 && $1 == b && $4 == w { print $7 }' "$OUT" \
    | sort -n \
    | awk '{ a[++n] = $1 } END { if (n) { mid = int((n + 1) / 2); if (n % 2) print a[mid]; else printf "%.3f\n", (a[mid] + a[mid + 1]) / 2 } }'
}

latest_size_for () {
  backend=$1
  awk -F '\t' -v b="$backend" \
    'NF >= 13 && $1 == b { w=$9; h=$10; s=$11; ws=$12 } END { if (w) printf "%sx%s scale=%s ws=%s", w, h, s, ws; }' "$OUT"
}

print_summary () {
  echo
  echo "== Raw results =="
  cat "$OUT"
  echo
  echo "== Median FPS =="
  printf '%-14s %-12s %-12s %-12s\n' workload wlshm pgtk x11
  printf '%-14s %-12s %-12s %-12s\n' "--------------" "------------" "------------" "------------"
  for workload in line-scroll page-scroll full-redraw typing image-scroll; do
    printf '%-14s %-12s %-12s %-12s\n' \
      "$workload" \
      "$(median_for wlshm "$workload")" \
      "$(median_for pgtk "$workload")" \
      "$(median_for x11 "$workload")"
  done
  echo
  echo "== Last observed frame size =="
  for backend in wlshm pgtk x11; do
    size=$(latest_size_for "$backend")
    [ -n "$size" ] && echo "  $backend: $size"
  done
  echo
  echo "Results: $OUT"
}

if [ "$RUN_WAYLAND" -eq 1 ]; then
  for run in $(seq 1 "$RUNS"); do
    echo "== Starting private nested niri ${NIRI_WIDTH}x${NIRI_HEIGHT} scale ${NIRI_SCALE} for wlshm run $run =="
    start_bench_niri || exit 1
    echo "  nested niri on $BENCH_NIRI_WL"
    fullscreen_parent_niri_window || exit 1
    run_wlshm "$BENCH_NIRI_WL" "$run" || echo "  wlshm run $run failed (see $OUT_DIR/wlshm-$run.log)" >&2
    stop_bench_niri

    echo "== Starting private nested niri ${NIRI_WIDTH}x${NIRI_HEIGHT} scale ${NIRI_SCALE} for pgtk run $run =="
    start_bench_niri || exit 1
    echo "  nested niri on $BENCH_NIRI_WL"
    fullscreen_parent_niri_window || exit 1
    run_pgtk "$BENCH_NIRI_WL" "$run" || echo "  pgtk run $run failed (see $OUT_DIR/pgtk-$run.log)" >&2
    stop_bench_niri
  done
fi

if [ "$RUN_X11" -eq 1 ]; then
  echo "== Running X11/Xvfb ${NIRI_WIDTH}x${NIRI_HEIGHT} baseline =="
  for run in $(seq 1 "$RUNS"); do
    run_x11 "$run" || echo "  x11 run $run failed (see $OUT_DIR/x11-$run.log)" >&2
  done
fi

print_summary
