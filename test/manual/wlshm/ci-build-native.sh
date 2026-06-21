#!/bin/sh
# ci-build-native.sh --- build the wlshm backend on a stock Ubuntu CI runner.
#
# This is the GitHub-hosted-runner path: it assumes the workflow already
# installed the C build dependencies (apt) and a current Rust toolchain
# (rustup -> cargo on PATH).  It does NOT use Guix.  ci-build.sh remains the
# Guix path for local / self-hosted use.
#
# The build (`make`) is the HARD gate -- the "does it still compile on master"
# check.  The render tests need a headless weston and are treated as advisory
# so a runtime/weston quirk on the runner can't mask a green compile.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT"

# A leaked EMACSLOADPATH (etc.) breaks the build-tree emacs under -Q.
unset EMACSLOADPATH EMACSDATA EMACSDOC EMACSPATH 2>/dev/null || true

NPROC="$(nproc 2>/dev/null || echo 2)"

echo "== toolchain =="
cargo --version || { echo "ci-build-native: cargo not on PATH" >&2; exit 2; }
cc --version | head -1 || true

echo "== autogen =="
./autogen.sh

echo "== configure (--with-wlshm) =="
# Do NOT pipe configure through grep/head/tail: SIGPIPE can truncate config.h
# on this tree.  Capture config.log on failure instead.
if ! ./configure --with-wlshm; then
  echo "configure failed; tail of config.log:" >&2
  [ -f config.log ] && tail -n 200 config.log >&2 || true
  exit 1
fi

echo "== make -j$NPROC (HARD gate) =="
make -j"$NPROC"

# Give the render tests a private XDG runtime dir for the headless weston.
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/wlshm-xdg}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export XDG_RUNTIME_DIR

echo "== tier 1 (render smoke) -- advisory =="
if test/manual/wlshm/run-tests.sh 1; then
  echo "tier 1: PASS"
else
  echo "tier 1: FAILED (advisory; build above is the gate)" >&2
fi

echo "== tier 2 (on-screen goldens) -- advisory =="
if test/manual/wlshm/run-tests.sh 2; then
  echo "tier 2: PASS"
else
  echo "tier 2: FAILED (advisory; goldens are version-sensitive)" >&2
fi

echo "ci-build-native: build OK"
