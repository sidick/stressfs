#!/usr/bin/env bash
# ci/copperline-m1.sh
#
# M1 (docs/implementation-plan.md): run `stressfs conform` under
# Copperline against a real FFS volume, headlessly and reproducibly --
# the same established pattern as ci/copperline-m0.sh (a scripted
# --run/--exit-on-return invocation, never a manual windowed session,
# per the project's standing rule). Two modes:
#
#   ci/copperline-m1.sh            -- runs the real conform table
#   ci/copperline-m1.sh --selftest-hang
#                                   -- runs the watchdog hang-detection
#                                      self-test (a stub MsgPort nobody
#                                      services, racing a timer.device
#                                      deadline -- see amiga/src/packet.c's
#                                      header comment); exits 5 when the
#                                      watchdog correctly detects the
#                                      hang, nonzero-but-not-5 otherwise
#
# M1's own watchdog story has two tiers (see amiga/src/conform.c's
# header comment): the real table talks to the handler via plain
# DoPkt() (so it works under volamos too, which only fakes that LVO,
# not SendPkt()/WaitPkt()) and has no per-packet timeout; the "second
# process with a timeout" for a REAL hang is this script's own
# `timeout` wrapper around the whole invocation below -- a genuinely
# hung handler makes the whole `stressfs conform` process (and this
# script) hang, and `timeout` kills it and this script reports
# inconclusive and a nonzero exit, exactly matching the proposal's
# "never continue past a possibly damaged handler" rule at the
# granularity M1 actually needs it. The per-packet mechanism
# (SendPkt()+timer.device) exists separately and is only exercised by
# --selftest-hang, which deliberately targets a stub port, never a
# real volume.
#
# `stressfs conform`'s own report lines go to a plain file
# (stressfs-conform-report.txt) in the current guest directory, which
# under `--run` is RunProg: -- the host directory holding the
# cross-built binary -- because a headless Copperline run has no
# console window to capture Printf() output from. This script reads
# that file back off the host filesystem after the run and prints it,
# then checks the process's own exit code.
#
# Needs a real AmigaOS Kickstart ROM and an FFS-formatted HDF image,
# same env vars and same caveat as ci/copperline-m0.sh (licensed/
# personal assets this repo cannot ship).
#
# Usage:
#   COPPERLINE_ROM=/path/to/kickstart.rom \
#   COPPERLINE_HDF=/path/to/ffs-volume.hdf \
#   COPPERLINE_VOLUME=Work: \
#     ci/copperline-m1.sh [--selftest-hang]
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build-m68k/stressfs"
REPORT="$ROOT/build-m68k/stressfs-conform-report.txt"

ROM="${COPPERLINE_ROM:-$HOME/kick32.a1200.rom}"
HDF="${COPPERLINE_HDF:-$HOME/os_drive.hdf}"
VOLUME="${COPPERLINE_VOLUME:-Work:}"
TIMEOUT_SECS="${COPPERLINE_M1_TIMEOUT:-120}"

MODE="conform"
if [ "${1:-}" = "--selftest-hang" ]; then
  MODE="selftest-hang"
fi

if [ ! -f "$BINARY" ]; then
  echo "error: $BINARY not found -- run ci/cross-build.sh first" >&2
  exit 1
fi
if [ ! -f "$ROM" ]; then
  echo "error: Kickstart ROM not found at $ROM (set COPPERLINE_ROM)" >&2
  exit 1
fi
if [ ! -f "$HDF" ]; then
  echo "error: FFS HDF not found at $HDF (set COPPERLINE_HDF, or build one with ~/src/amibake)" >&2
  exit 1
fi

CONFIG="$(mktemp -t copperline-m1-XXXXXX.toml)"
trap 'rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<EOF
rom = "$ROM"

[machine]
profile = "A1200"

[ide]
master = "$HDF"
EOF

rm -f "$REPORT"

if [ "$MODE" = "selftest-hang" ]; then
  RUN_ARGS="CONFORM SELFTESTHANG"
  echo "== running stressfs conform's watchdog hang-detection self-test under Copperline =="
else
  RUN_ARGS="VOLUME $VOLUME CONFORM"
  echo "== running stressfs conform under Copperline against $VOLUME on $HDF =="
fi

rc=0
timeout "$TIMEOUT_SECS" copperline --config "$CONFIG" --noaudio \
  --run "$BINARY" --run-args "$RUN_ARGS" \
  --exit-on-return || rc=$?

if [ "$rc" = 124 ]; then
  echo "== stressfs conform did not return within ${TIMEOUT_SECS}s -- the script-level watchdog" \
       "fired. Treating as inconclusive; the handler/image may be in an unknown state. =="
  exit 2
fi

echo "== stressfs conform exit code under Copperline: $rc =="

if [ -f "$REPORT" ]; then
  echo "== report (RunProg:stressfs-conform-report.txt, read back from $REPORT) =="
  cat "$REPORT"
else
  echo "== no report file was produced =="
fi

if [ "$MODE" = "selftest-hang" ]; then
  # RETURN_WARN (5): the watchdog correctly detected the hang -- the
  # expected, successful demonstration. Anything else is a real
  # failure of the mechanism itself.
  if [ "$rc" = 5 ]; then
    echo "== watchdog self-test: hang correctly detected =="
    exit 0
  fi
  echo "== watchdog self-test: hang was NOT detected (or crashed) -- rc=$rc =="
  exit 1
fi

exit "$rc"
