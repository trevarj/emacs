#!/usr/bin/env bash
# Diagnose a freeze in the BUILD-TREE (wlshm) Emacs ONLY (never the packaged
# pgtk editor).  Run while it's frozen (leave it frozen).
#
# Does N SEPARATE gdb attach/detach cycles (the process RUNS between them), so
# comparing charpos across cycles tells an infinite loop (charpos stays put)
# from slow-but-progressing (charpos creeps).  Also prints pixel_width (0 ->
# zero-width-glyph loop), the char, method, and the mouse coords/buffer.
# Output: /tmp/wlshm-freeze.bt
#
# Usage:  bash test/manual/wlshm/capture-freeze.sh
set -u
ROOT=/home/trev/Workspace/emacs
pid=""
for p in $(pgrep emacs 2>/dev/null); do
  case "$(readlink /proc/$p/exe 2>/dev/null)" in
    */Workspace/emacs/src/*) pid="$p"; break;;
  esac
done
if [ -z "$pid" ]; then
  echo "No build-tree (Workspace/emacs/src) Emacs running. Processes:"
  for p in $(pgrep emacs 2>/dev/null); do echo "  $p -> $(readlink /proc/$p/exe 2>/dev/null)"; done
  exit 1
fi
echo "Tracing wlshm Emacs PID $pid"

cat > /tmp/wlshm-probe.py <<'PYEOF'
import gdb
def fr(name):
    f = gdb.newest_frame()
    while f is not None:
        if (f.name() or "") == name: return f
        f = f.older()
    return None
m = fr("move_it_to") or fr("move_it_in_display_line_to")
if m:
    m.select()
    for ex in ["it->current.pos.charpos","it->pixel_width","it->current_x","it->current_y",
               "it->vpos","it->c","it->method","it->len","it->nglyphs",
               "it->glyph_not_available_p","it->face_id"]:
        try: print("MIT", ex, "=", gdb.parse_and_eval(ex))
        except Exception as e: print("MIT", ex, "ERR")
b = fr("buffer_posn_from_coords")
if b:
    b.select()
    for ex in ["*x","*y","w->pixel_width","w->pixel_height"]:
        try: print("BPC", ex, "=", gdb.parse_and_eval(ex))
        except Exception as e: print("BPC", ex, "ERR")
PYEOF

GDB="gdb"; command -v gdb >/dev/null 2>&1 || GDB="guix shell gdb -- gdb"
: > /tmp/wlshm-freeze.bt
for i in $(seq 1 6); do
  echo "===== CYCLE $i (separate attach; process ran since last) =====" >> /tmp/wlshm-freeze.bt
  $GDB -p "$pid" -batch -ex 'set pagination off' -ex 'bt 4' \
       -ex 'source /tmp/wlshm-probe.py' >> /tmp/wlshm-freeze.bt 2>&1
  sleep 0.6   # let the inferior run between attaches
done
echo "Wrote /tmp/wlshm-freeze.bt ($(wc -l < /tmp/wlshm-freeze.bt) lines)"
echo "charpos across cycles (same => infinite loop; creeping => slow):"
grep 'charpos =' /tmp/wlshm-freeze.bt
echo "pixel_width / char:"; grep -E 'pixel_width =|->c =|method =' /tmp/wlshm-freeze.bt | head
