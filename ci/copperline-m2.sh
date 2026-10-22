#!/usr/bin/env bash
# ci/copperline-m2.sh
#
# M2 (docs/implementation-plan.md section 3's M2 entry): run `stressfs
# conform` under Copperline against a real FFS volume, headlessly and
# reproducibly -- same established pattern as ci/copperline-m0.sh and
# ci/copperline-m1.sh (a scripted --run/--exit-on-return invocation,
# never a manual windowed session; this is a hard project rule, not a
# suggestion). This script's own job is narrower than M1's: confirm
# that the new run-identity header and the new JSON-lines report come
# back SANE from a real handler, not just from volamos (M2's exit
# criterion explicitly calls this out: "run the whole thing under
# Copperline headlessly against the real FFS volume ... and confirm
# the run-identity header and JSON lines come back sane from a real
# handler too").
#
# Passes TIER=copperline and a fixed SEED so the run-identity header's
# mandatory fields are populated the way a real campaign would (see
# amiga/src/main.c's TIER/K option and amiga/src/conform.c's
# emit_run_identity()).
#
# Two report files are read back off the host filesystem after the
# run (see amiga/src/conform.c's dual-output decision comment for why
# there are two, and amiga/src/conform.c's existing header comment on
# why under `--run` the current guest directory is RunProg: -- the
# host directory holding the cross-built binary):
#   stressfs-conform-report.txt  -- human-readable
#   stressfs-conform-report.json -- JSON-lines (fed to ci/report-checker.sh)
#
# Needs a real AmigaOS Kickstart ROM and an FFS-formatted HDF image,
# same env vars and same caveat as ci/copperline-m0.sh/-m1.sh
# (licensed/personal assets this repo cannot ship).
#
# Usage:
#   COPPERLINE_ROM=/path/to/kickstart.rom \
#   COPPERLINE_HDF=/path/to/ffs-volume.hdf \
#   COPPERLINE_VOLUME=Work: \
#     ci/copperline-m2.sh
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build-m68k/stressfs"
REPORT_TXT="$ROOT/build-m68k/stressfs-conform-report.txt"
REPORT_JSON="$ROOT/build-m68k/stressfs-conform-report.json"
CHECKER="$ROOT/ci/report-checker.sh"

ROM="${COPPERLINE_ROM:-$HOME/kick32.a1200.rom}"
HDF="${COPPERLINE_HDF:-$HOME/os_drive.hdf}"
VOLUME="${COPPERLINE_VOLUME:-Work:}"
SEED="${COPPERLINE_M2_SEED:-20261022}"
TIMEOUT_SECS="${COPPERLINE_M2_TIMEOUT:-120}"

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

CONFIG="$(mktemp -t copperline-m2-XXXXXX.toml)"
trap 'rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<EOF
rom = "$ROM"

[machine]
profile = "A1200"

[ide]
master = "$HDF"
EOF

rm -f "$REPORT_TXT" "$REPORT_JSON"

RUN_ARGS="VOLUME $VOLUME TIER copperline SEED $SEED CONFORM"
echo "== running stressfs conform under Copperline against $VOLUME on $HDF (TIER=copperline SEED=$SEED) =="

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

if [ -f "$REPORT_TXT" ]; then
  echo "== human-readable report (RunProg:stressfs-conform-report.txt) =="
  cat "$REPORT_TXT"
else
  echo "== no human-readable report file was produced =="
fi

if [ -f "$REPORT_JSON" ]; then
  echo "== JSON-lines report (RunProg:stressfs-conform-report.json) =="
  cat "$REPORT_JSON"
  echo "== running ci/report-checker.sh against it =="
  "$CHECKER" "$REPORT_JSON"
else
  echo "error: no JSON report file was produced -- M2 exit criterion not met" >&2
  exit 1
fi

exit "$rc"
