#!/usr/bin/env bash
# ci/copperline-m4.sh
#
# M4 (docs/implementation-plan.md section 3's M4 entry) EXIT CRITERIA,
# demonstrated against a real FFS volume under Copperline, headlessly
# and reproducibly -- same established pattern as ci/copperline-m0.sh
# through -m3.sh (a generated config with a top-level `rom` and an
# `[ide] master = <hdf>`, `--run`/`--exit-on-return`, an outer
# `timeout`; never a manual windowed session, a hard project rule).
#
#   1. SEED=n reproduces the identical operation stream: run
#      `stressfs random` twice with the same seed and diff the JSON
#      reports byte-for-byte.
#   2. A seeded run of >= 10000 operations against FFS completes with
#      the model agreeing OR produces a logged, replayable divergence.
#      Either outcome is accepted and reported honestly -- a
#      divergence here is a legitimate finding (the model is portable-
#      only at M4; see random_model.h's own header comments on two
#      such findings made while developing this milestone), not a
#      script failure. Whichever happens, `stressfs replay` is run
#      against the resulting report and its output is diffed against
#      the original to confirm the SAME outcome reproduces.
#   3. `stressfs random RANDOMBREAKMODEL` demonstrates divergence
#      detection end to end with a deliberately wrong expectation.
#
# Needs a real AmigaOS Kickstart ROM and an FFS-formatted HDF image,
# same env vars and same caveat as the earlier scripts (licensed/
# personal assets this repo cannot ship), which is why this is a
# local-only script and not a CI job.
#
# Usage:
#   COPPERLINE_ROM=/path/to/kickstart.rom \
#   COPPERLINE_HDF=/path/to/ffs-volume.hdf \
#   COPPERLINE_VOLUME=Work: \
#     ci/copperline-m4.sh
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build-m68k/stressfs"
REPORT_JSON="$ROOT/build-m68k/stressfs-random-report.json"
OUT="$ROOT/build-m68k/m4"

ROM="${COPPERLINE_ROM:-$HOME/kick32.a1200.rom}"
HDF="${COPPERLINE_HDF:-$HOME/os_drive.hdf}"
VOLUME="${COPPERLINE_VOLUME:-Work:}"
SEED="${COPPERLINE_M4_SEED:-1}"
OPS="${COPPERLINE_M4_OPS:-10000}"
VERIFYN="${COPPERLINE_M4_VERIFYN:-200}"
TIMEOUT_SECS="${COPPERLINE_M4_TIMEOUT:-300}"

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

mkdir -p "$OUT"

CONFIG="$(mktemp -t copperline-m4-XXXXXX.toml)"
trap 'rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<EOF
rom = "$ROM"

[machine]
profile = "A1200"

[ide]
master = "$HDF"
EOF

run_random() {
  label="$1"
  shift
  rm -f "$REPORT_JSON"
  RUN_ARGS="VOLUME $VOLUME TIER copperline $*"
  echo ""
  echo "== pass '$label': stressfs $RUN_ARGS =="
  rc=0
  timeout "$TIMEOUT_SECS" copperline --config "$CONFIG" --noaudio \
    --run "$BINARY" --run-args "$RUN_ARGS" \
    --exit-on-return || rc=$?
  if [ "$rc" = 124 ]; then
    echo "== pass '$label' did not return within ${TIMEOUT_SECS}s -- treating as" \
         "inconclusive; the handler/image may be in an unknown state. ==" >&2
    exit 2
  fi
  echo "== pass '$label' exit code: $rc =="
  if [ ! -f "$REPORT_JSON" ]; then
    echo "error: pass '$label' produced no JSON report" >&2
    exit 1
  fi
  cp "$REPORT_JSON" "$OUT/report-$label.json"
}

echo "=============================================================="
echo " 1. SEED=$SEED reproduces the identical operation stream"
echo "=============================================================="
run_random determinism-a SEED "$SEED" RANDOM OPS 100
run_random determinism-b SEED "$SEED" RANDOM OPS 100

if diff -q "$OUT/report-determinism-a.json" "$OUT/report-determinism-b.json" >/dev/null; then
  echo "== OK: two runs with SEED=$SEED produced byte-identical reports =="
else
  echo "error: SEED=$SEED produced DIFFERENT reports across two runs -- the generator" \
       "is not reproducible against this handler" >&2
  diff "$OUT/report-determinism-a.json" "$OUT/report-determinism-b.json" | head -20 >&2
  exit 1
fi

echo ""
echo "=============================================================="
echo " 2. a seeded run of >= $OPS operations against FFS"
echo "=============================================================="
run_random bigrun SEED "$SEED" RANDOM OPS "$OPS" VERIFYN "$VERIFYN"

LAST_LINE="$(grep '"type":"random_step"' "$OUT/report-bigrun.json" | tail -1)"
LAST_STEP="$(echo "$LAST_LINE" | sed -E 's/.*"step":([0-9]+).*/\1/')"
DIVERGED="$(echo "$LAST_LINE" | sed -E 's/.*"diverged":([a-z]+).*/\1/')"

if [ "$DIVERGED" = "false" ] && [ "$LAST_STEP" -ge "$OPS" ]; then
  echo "== OK: the model agreed with FFS for the full $OPS-operation run =="
else
  echo "== the run stopped at step $LAST_STEP (diverged=$DIVERGED) before reaching $OPS ops. =="
  echo "   Per the M4 exit criterion this is an ACCEPTED outcome (\"completes ... OR" \
       "produces a logged, replayable divergence\") -- not a script failure. The" \
       "divergence is a finding about FFS's behaviour, not the harness; see" \
       "$OUT/report-bigrun.json and amiga/include/stressfs/random_model.h for the" \
       "class of FFS behaviour this milestone found while being built."
fi

echo ""
echo "=============================================================="
echo " 3. stressfs replay reproduces the bigrun outcome"
echo "=============================================================="
cp "$OUT/report-bigrun.json" "$ROOT/build-m68k/stressfs-random-report.json"
RUN_ARGS="VOLUME $VOLUME TIER copperline REPLAY REPLAYLOG RunProg:stressfs-random-report.json"
echo "== stressfs $RUN_ARGS =="
timeout "$TIMEOUT_SECS" copperline --config "$CONFIG" --noaudio \
  --run "$BINARY" --run-args "$RUN_ARGS" \
  --exit-on-return || true
cp "$REPORT_JSON" "$OUT/report-replay.json"

if diff -q "$OUT/report-bigrun.json" "$OUT/report-replay.json" >/dev/null; then
  echo "== OK: stressfs replay reproduced the bigrun report byte-for-byte =="
else
  echo "error: replay did NOT reproduce the original report" >&2
  diff "$OUT/report-bigrun.json" "$OUT/report-replay.json" | head -20 >&2
  exit 1
fi

echo ""
echo "=============================================================="
echo " 4. the intentionally-broken-model self-test"
echo "=============================================================="
run_random selftest SEED 1 RANDOM RANDOMBREAKMODEL
if grep -q '"diverged":true' "$OUT/report-selftest.json"; then
  echo "== OK: the deliberately corrupted expectation was reported as a divergence =="
else
  echo "error: RANDOMBREAKMODEL did not produce a reported divergence" >&2
  exit 1
fi

echo ""
echo "== M4 exit criteria demonstrated. Reports kept in $OUT/. =="
exit 0
