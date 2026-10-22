#!/usr/bin/env bash
# ci/copperline-m3.sh
#
# M3 (docs/implementation-plan.md section 3's M3 entry) EXIT CRITERION:
# "the same conform table run against FFS V40 with and without `-K`
# demonstrably changes verdicts (a quirk-covered deviation vs. a raw
# finding)".
#
# This script runs `stressfs conform` under Copperline against a real
# FFS volume TWICE -- once normally (the quirks file's `confirmed`
# entries auto-apply) and once with NOQUIRKS (the plan's `-K`, which
# makes stressfs ignore the file entirely) -- and then diffs the
# verdict counts. It fails if the two runs produce the SAME counts,
# because that would mean the quirks machinery had no observable
# effect, which is exactly what this milestone has to demonstrate.
#
# Headless and scripted, same established pattern as
# ci/copperline-m0.sh / -m1.sh / -m2.sh: a generated config with a
# top-level `rom` and an `[ide] master = <hdf>`, plus
# `--run`/`--exit-on-return` and an outer `timeout`. Never a manual
# windowed session -- that is a hard project rule, not a suggestion:
# a headless deterministic run is the only way to verify anything
# under Copperline reliably.
#
# The quirks file is copied next to the cross-built binary before the
# run, because under `--run` the guest's current directory is RunProg:
# (the host directory holding the binary), which is the first entry in
# amiga/src/quirks_file.c's default search order.
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
#     ci/copperline-m3.sh
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build-m68k/stressfs"
QUIRKS_SRC="$ROOT/data/stressfs.quirks"
QUIRKS_DST="$ROOT/build-m68k/stressfs.quirks"
REPORT_TXT="$ROOT/build-m68k/stressfs-conform-report.txt"
REPORT_JSON="$ROOT/build-m68k/stressfs-conform-report.json"
OUT="$ROOT/build-m68k/m3"
CHECKER="$ROOT/ci/report-checker.sh"

ROM="${COPPERLINE_ROM:-$HOME/kick32.a1200.rom}"
HDF="${COPPERLINE_HDF:-$HOME/os_drive.hdf}"
VOLUME="${COPPERLINE_VOLUME:-Work:}"
SEED="${COPPERLINE_M3_SEED:-20261022}"
TIMEOUT_SECS="${COPPERLINE_M3_TIMEOUT:-180}"

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

cp "$QUIRKS_SRC" "$QUIRKS_DST"
mkdir -p "$OUT"

CONFIG="$(mktemp -t copperline-m3-XXXXXX.toml)"
trap 'rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<EOF
rom = "$ROM"

[machine]
profile = "A1200"

[ide]
master = "$HDF"
EOF

# Runs one pass and stashes its two report files under build-m68k/m3/.
# $1 = label ("quirks" / "noquirks"), $2.. = extra stressfs arguments.
run_pass() {
  label="$1"
  shift
  rm -f "$REPORT_TXT" "$REPORT_JSON"

  RUN_ARGS="VOLUME $VOLUME TIER copperline SEED $SEED CONFORM $*"
  echo ""
  echo "== pass '$label': stressfs $RUN_ARGS =="

  rc=0
  timeout "$TIMEOUT_SECS" copperline --config "$CONFIG" --noaudio \
    --run "$BINARY" --run-args "$RUN_ARGS" \
    --exit-on-return || rc=$?

  if [ "$rc" = 124 ]; then
    echo "== pass '$label' did not return within ${TIMEOUT_SECS}s -- the script-level" \
         "watchdog fired. Treating as inconclusive; the handler/image may be in an" \
         "unknown state. ==" >&2
    exit 2
  fi
  echo "== pass '$label' exit code: $rc =="
  eval "rc_$label=$rc"

  if [ ! -f "$REPORT_JSON" ]; then
    echo "error: pass '$label' produced no JSON report" >&2
    exit 1
  fi
  cp "$REPORT_TXT" "$OUT/report-$label.txt"
  cp "$REPORT_JSON" "$OUT/report-$label.json"
}

# Pass 1: quirks applied (the default -- data/stressfs.quirks sits next
# to the binary, so the first entry of the default search order finds
# it, and its `confirmed` entries auto-apply).
run_pass quirks

# Pass 2: the plan's -K. Same binary, same table, same volume, same
# seed; the ONLY difference is that the quirks file is not consulted.
run_pass noquirks -K

echo ""
echo "=============================================================="
echo " M3 exit criterion: verdict counts WITH vs WITHOUT the quirks"
echo "=============================================================="
echo ""
echo "--- with quirks (confirmed entries auto-applied) ---"
"$CHECKER" "$OUT/report-quirks.json"
echo ""
echo "--- with NOQUIRKS (-K) ---"
"$CHECKER" "$OUT/report-noquirks.json"

counts() {
  for v in pass fail deviation unsupported inconclusive; do
    printf '%s=%s ' "$v" "$(grep -c "\"verdict\":\"$v\"" "$1" || true)"
  done
  echo ""
}

WITH="$(counts "$OUT/report-quirks.json")"
WITHOUT="$(counts "$OUT/report-noquirks.json")"

echo ""
echo "with quirks:    $WITH"
echo "with NOQUIRKS:  $WITHOUT"
echo ""
echo "--- the lines whose verdict the quirks changed ---"
# The `test` field (added in M3) is what makes this join possible at
# all: it is a short stable id, unlike the human-readable test name.
for tid in $(sed -E 's/.*"test":"([a-z0-9-]+)".*/\1/' "$OUT/report-quirks.json" \
             | grep -E '^[a-z0-9-]+$' | sort -u); do
  a="$(grep "\"test\":\"$tid\"" "$OUT/report-quirks.json" | sed -E 's/.*"verdict":"([a-z]+)".*/\1/' | head -1)"
  b="$(grep "\"test\":\"$tid\"" "$OUT/report-noquirks.json" | sed -E 's/.*"verdict":"([a-z]+)".*/\1/' | head -1)"
  if [ "$a" != "$b" ]; then
    q="$(grep "\"test\":\"$tid\"" "$OUT/report-quirks.json" \
         | sed -E 's/.*"applied_quirks":\[\{"id":"([^"]+)","confidence":"([^"]+)".*/\1 (\2)/')"
    printf '  %-32s %-12s -> %-12s   quirk: %s\n' "$tid" "$b" "$a" "$q"
  fi
done

if [ "$WITH" = "$WITHOUT" ]; then
  echo ""
  echo "error: the two passes produced IDENTICAL verdict counts. The M3 exit criterion" \
       "requires the quirks file to demonstrably change verdicts against this handler;" \
       "either no confirmed entry matched it, or the quirks file was not found next to" \
       "the binary. Reports are in $OUT/." >&2
  exit 1
fi

echo ""
echo "== M3 exit criterion met: the same conform table against the same FFS volume" \
     "produced different verdicts with and without NOQUIRKS. Reports kept in $OUT/. =="
exit 0
