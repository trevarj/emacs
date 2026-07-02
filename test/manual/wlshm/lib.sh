# lib.sh --- shared primitives for the wlshm graphical test harness.
#
# Sourced by run-tests.sh.  Provides: CPU/Cairo env, a build-tree Emacs
# runner, headless-weston and nested-niri launchers, a hang/crash watchdog
# that captures a gdb backtrace, virtual input injection, emacsclient-driven
# state evaluation, PNG golden comparison, and cleanup that only kills PIDs we
# spawned (never the user's session).
#
# Design notes learned the hard way:
#   * The build-tree ./src/emacs must run with EMACSLOADPATH (and friends)
#     UNSET, otherwise it picks up the packaged emacs-next lisp and fails to
#     load cl-lib etc.
#   * Rendering is CPU/Cairo (no GPU), so frames are deterministic on a given
#     machine without any GPU setup.

set -u

# Repo root = two levels up from this file (test/manual/wlshm/lib.sh).
WLSHM_HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
WLSHM_ROOT="$(cd "$WLSHM_HERE/../../.." && pwd)"
WLSHM_EMACS="$WLSHM_ROOT/src/emacs"
WLSHM_TMP="${WLSHM_TMP:-/tmp/wlshm-tests}"
WLSHM_GOLDEN="$WLSHM_HERE/golden"
WLSHM_PIDS=""
WLSHM_FAILS=0
WLSHM_PASSES=0

mkdir -p "$WLSHM_TMP"

# --- environment ----------------------------------------------------------

wlshm_setup_env () {
  # Rendering is CPU/Cairo now (no GPU/Vulkan), so output is deterministic
  # without any GPU setup.
  # The build-tree binary must use ITS OWN lisp/, not the packaged emacs's.
  unset EMACSLOADPATH EMACSDATA EMACSDOC EMACSPATH 2>/dev/null || true
  echo "[env] CPU/Cairo backend (no Vulkan)"
}

# --- process tracking / cleanup -------------------------------------------

wlshm_track () { WLSHM_PIDS="$WLSHM_PIDS $1"; }

wlshm_cleanup () {
  for p in $WLSHM_PIDS; do kill -9 "$p" 2>/dev/null || true; done
  WLSHM_PIDS=""
}
trap wlshm_cleanup EXIT INT TERM

# --- pass/fail accounting --------------------------------------------------

wlshm_pass () { WLSHM_PASSES=$((WLSHM_PASSES + 1)); echo "  PASS: $*"; }
wlshm_fail () { WLSHM_FAILS=$((WLSHM_FAILS + 1));  echo "  FAIL: $*"; }

# --- gdb backtrace on hang/crash ------------------------------------------

# wlshm_gdb_bt PID OUTFILE  -- snapshot all-thread backtrace + debug-log tail.
wlshm_gdb_bt () {
  pid=$1 out=$2
  {
    echo "=== gdb backtrace of pid $pid ($(date)) ==="
    timeout 40 gdb -p "$pid" -batch \
      -ex 'set pagination off' -ex 'set auto-load safe-path /' \
      -ex 'thread apply all bt' 2>/dev/null
    echo "=== tail /tmp/wlshm-debug.log ==="
    tail -60 /tmp/wlshm-debug.log 2>/dev/null
  } >"$out" 2>&1
  echo "    (backtrace saved to $out)"
}

# --- the build-tree Emacs --------------------------------------------------

# wlshm_emacs WAYLAND_DISPLAY ARGS...  -- run the build-tree emacs,
# clean load path, WLSHM_DEBUG on.  Runs in the FOREGROUND.
wlshm_emacs () {
  wd=$1; shift
  WAYLAND_DISPLAY="$wd" WLSHM_DEBUG=1 "$WLSHM_EMACS" -Q "$@"
}

# wlshm_emacs_batch ARGS...  -- headless --batch emacs (no display).
wlshm_emacs_batch () {
  "$WLSHM_EMACS" -Q --batch "$@"
}

# --- compositors -----------------------------------------------------------

# wlshm_start_weston SOCKET W H  -- headless weston; waits for the socket.
wlshm_start_weston () {
  sock=$1 w=$2 h=$3
  weston --backend=headless-backend.so --socket="$sock" \
    --width="$w" --height="$h" --idle-time=0 \
    >"$WLSHM_TMP/weston-$sock.log" 2>&1 &
  wlshm_track $!
  for _ in $(seq 1 40); do
    [ -S "$XDG_RUNTIME_DIR/$sock" ] && return 0
    sleep 0.25
  done
  echo "weston ($sock) failed to start"; return 1
}

# wlshm_start_niri  -- nested niri in the parent session; echoes its wayland-N.
# Requires WLSHM_PARENT_WL (the user's session display, e.g. wayland-1).
wlshm_start_niri () {
  : "${WLSHM_PARENT_WL:?set WLSHM_PARENT_WL to the parent session display}"
  log="$WLSHM_TMP/niri.log"; : >"$log"
  WAYLAND_DISPLAY="$WLSHM_PARENT_WL" niri -c "$WLSHM_HERE/niri-test.kdl" \
    >"$log" 2>&1 &
  wlshm_track $!
  for _ in $(seq 1 40); do
    s=$(grep -oE "Wayland socket: wayland-[0-9]+" "$log" | head -1 \
          | grep -oE "wayland-[0-9]+$")
    [ -n "$s" ] && [ -S "$XDG_RUNTIME_DIR/$s" ] && { echo "$s"; return 0; }
    sleep 0.25
  done
  echo "nested niri failed to start" >&2; return 1
}

# --- Emacs server (for interaction tests) ----------------------------------

WLSHM_SERVER_PID=""

# wlshm_start_server NAME WAYLAND_DISPLAY  -- launch a long-lived emacs server.
wlshm_start_server () {
  name=$1 wd=$2
  WAYLAND_DISPLAY="$wd" WLSHM_DEBUG=1 "$WLSHM_EMACS" -Q \
    -l "$WLSHM_HERE/interaction-tests.el" \
    --eval "(progn (setq server-name \"$name\") (server-start))" \
    >"$WLSHM_TMP/emacs-$name.log" 2>&1 &
  WLSHM_SERVER_PID=$!; wlshm_track "$WLSHM_SERVER_PID"
  for _ in $(seq 1 60); do
    [ -S "$XDG_RUNTIME_DIR/emacs/$name" ] && break
    kill -0 "$WLSHM_SERVER_PID" 2>/dev/null || { echo "emacs server died"; return 1; }
    sleep 0.25
  done
  [ -S "$XDG_RUNTIME_DIR/emacs/$name" ] \
    || { echo "emacs server ($name) socket never appeared"; return 1; }
  # The emacsclient socket can appear a touch before the GUI frame has opened
  # its Wayland window and presented; a short settle avoids racing the first
  # render/dump.  (Probing the server with emacsclient this early is itself
  # flaky, so just wait.)
  sleep 1
  return 0
}

# wlshm_eval NAME TIMEOUT FORM  -- emacsclient --eval with a watchdog.  On
# timeout (= Emacs hung), capture a gdb backtrace and return 124.  Prints the
# eval result on success.
# NOTE: callers must CAPTURE the output -- `r=$(wlshm_eval ...)` -- rather than
# redirect it (`wlshm_eval ... >/dev/null`).  Running this in a command
# substitution forks a subshell whose small startup delay reliably lets the
# server's redisplay+present complete before we move on; the redirected form
# races it and the render/dump is dropped.
wlshm_eval () {
  name=$1 tmo=$2 form=$3
  out=$(timeout "$tmo" emacsclient -s "$name" --eval "$form" 2>/dev/null)
  rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "    (emacs hung on eval; capturing backtrace)"
    [ -n "$WLSHM_SERVER_PID" ] && wlshm_gdb_bt "$WLSHM_SERVER_PID" "$WLSHM_TMP/$name-hang.bt"
    return 124
  fi
  printf '%s' "$out"
  return $rc
}

# --- virtual input (into the nested compositor) ----------------------------

# wlshm_key WAYLAND_DISPLAY TEXT      -- type via virtual-keyboard.
wlshm_key () { WAYLAND_DISPLAY="$1" wtype -- "$2" 2>/dev/null; }
# wlshm_key_press/release WAYLAND_DISPLAY KEY  -- hold/release a key.
wlshm_key_press   () { WAYLAND_DISPLAY="$1" wtype -P "$2" 2>/dev/null; }
wlshm_key_release () { WAYLAND_DISPLAY="$1" wtype -p "$2" 2>/dev/null; }
wlshm_wtype_modifier () {
  case "$1" in
    Control_L|Control_R|ctrl) printf '%s\n' ctrl ;;
    Shift_L|Shift_R|shift) printf '%s\n' shift ;;
    Super_L|Super_R|logo) printf '%s\n' logo ;;
    Alt_R|altgr) printf '%s\n' altgr ;;
    *) printf '%s\n' "$1" ;;
  esac
}
wlshm_key_chord () {
  wd=$1; shift
  [ "$#" -ge 1 ] || return 2
  local key i mod
  local -a cmd
  key="${!#}"
  cmd=(wtype)
  for ((i = 1; i < $#; i++)); do
    mod=$(wlshm_wtype_modifier "${!i}")
    cmd+=(-M "$mod")
  done
  cmd+=(-k "$key")
  for ((i = $# - 1; i >= 1; i--)); do
    mod=$(wlshm_wtype_modifier "${!i}")
    cmd+=(-m "$mod")
  done
  WAYLAND_DISPLAY="$wd" "${cmd[@]}" 2>/dev/null
}

# wlshm_pointer_move WAYLAND_DISPLAY DX DY  -- relative pointer move.
wlshm_pointer_move () { WAYLAND_DISPLAY="$1" wlrctl pointer move "$2" "$3" 2>/dev/null; }
# wlshm_click WAYLAND_DISPLAY [BUTTON]  -- click at the current position.
wlshm_click () { WAYLAND_DISPLAY="$1" wlrctl pointer click "${2:-left}" 2>/dev/null; }

# --- golden image comparison ----------------------------------------------

# wlshm_compare_png ACTUAL GOLDEN NAME  -- compare a window dump to its golden.
# Software lavapipe is deterministic on a given machine and the png encoder is
# byte-stable, so an exact compare is reliable here.  Regenerate (review before
# commit) with WLSHM_REGEN=1.  Also fails clearly if ACTUAL is missing (the dump
# never happened, e.g. the scene hung).
wlshm_compare_png () {
  actual=$1 golden=$2 name=$3
  # The dump file can land after emacsclient returns; wait so a benign timing
  # skew is not reported as a crash.
  for _ in $(seq 1 50); do [ -f "$actual" ] && break; sleep 0.1; done
  [ -f "$actual" ] || { wlshm_fail "$name (no dump produced -- scene crashed/hung?)"; return 1; }
  if [ "${WLSHM_REGEN:-0}" = "1" ]; then
    cp "$actual" "$golden"; echo "    (regenerated $golden)"; return 0
  fi
  [ -f "$golden" ] || { wlshm_fail "$name (no golden $golden -- run with WLSHM_REGEN=1)"; return 1; }
  if cmp -s "$actual" "$golden"
  then wlshm_pass "$name"; else wlshm_fail "$name (differs from $golden)"; fi
}
