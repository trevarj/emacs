#!/bin/sh
# ci-build.sh --- reproducible build + tier-1/2 test of the wlshm backend.
#
# Runs everything inside the project's Guix dev environment:
#
#   guix shell -m manifest.scm -- <build+test>
#
# so the toolchain and deps come from manifest.scm against the runner's current
# guix channel.  Intended to be invoked by the CI workflow (which is
# responsible for the rebase onto origin/master *before* calling this), but it
# is self-contained and can be run by hand from the repo root.
#
# Tiers (see test/manual/wlshm/run-tests.sh):
#   tier 1  render smoke         -- HARD gate: failure fails the build.
#   tier 2  on-screen goldens    -- SOFT for now: reported, never fatal, until
#                                   the golden PNGs are version-pinned in-tree.
#   tier 3  interaction (niri)   -- not run in CI (needs a parent Wayland
#                                   session; nested niri is a developer-only
#                                   workflow).
#
# Requirements:
#   * guix on PATH (the runner must be Guix-capable).
#   * A working /tmp (weston + test artifacts land in $WLSHM_TMP).
#
# IMPORTANT build gotcha (project-specific): never pipe configure's stdout/stderr
# through grep/head/tail.  Early SIGPIPE from those tools has been observed to
# truncate the generated config.h on this tree.  We capture config.log on
# failure instead of filtering live output.

set -eu

# Skip the tier-3 interaction tools (wtype/wlrctl) when resolving manifest.scm:
# they come from personal channels and CI uses core Guix only (tier-1/2 here).
export WLSHM_CI=1

# --- locate the repo root (script lives in test/manual/wlshm/) -------------
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT"

MANIFEST="$ROOT/manifest.scm"

[ -f "$MANIFEST" ] || { echo "ci-build: missing $MANIFEST" >&2; exit 2; }

command -v guix >/dev/null 2>&1 || {
  echo "ci-build: guix not found on PATH (need a Guix-capable runner)" >&2
  exit 2
}

NPROC="$(nproc 2>/dev/null || echo 1)"

# --- the actual build + test, run *inside* the Guix dev environment --------
# This here-doc becomes the argv of `sh -euc` under guix shell.  Keep it POSIX.
# We pass ROOT and NPROC in as positional args to the inner shell.
run_inside () {
  guix shell -m "$MANIFEST" -- \
    /bin/sh -euc '
      ROOT="$1"; NPROC="$2"
      cd "$ROOT"

      # The harness requires the build-tree emacs to start clean; a leaked
      # EMACSLOADPATH (etc.) from the dev shell breaks -Q.  Unset before build
      # and test so both see a pristine environment.
      unset EMACSLOADPATH EMACSDATA EMACSDOC EMACSPATH 2>/dev/null || true

      echo "== autogen =="
      ./autogen.sh

      echo "== configure (--with-wlshm) =="
      # Do NOT pipe configure through grep/head/tail: SIGPIPE can truncate
      # config.h on this tree.  Capture config.log on failure instead.
      if ! ./configure --with-wlshm; then
        echo "configure failed; tail of config.log:" >&2
        [ -f config.log ] && tail -n 200 config.log >&2 || true
        exit 1
      fi

      echo "== make -j$NPROC =="
      make -j"$NPROC"

      echo "== tier 1 (render smoke) -- HARD gate =="
      test/manual/wlshm/run-tests.sh 1

      echo "== tier 2 (on-screen goldens) -- SOFT (allow-fail) =="
      if test/manual/wlshm/run-tests.sh 2; then
        echo "tier 2: PASS"
      else
        echo "tier 2: FAILED (non-fatal until goldens are version-pinned)" >&2
      fi
    ' _ "$ROOT" "$NPROC"
}

run_inside
echo "ci-build: done (tier 1 passed; tier 2 advisory)"
