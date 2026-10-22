#!/usr/bin/env bash
# ci/report-checker.sh
#
# M2 (docs/implementation-plan.md section 3's M2 entry): "a trivial
# ci/ checker script parses a real report and computes the verdict
# counts (the seed of phase 4's summariser)". Deliberately minimal per
# that wording -- this is NOT phase 4's campaign summariser (pass
# rates, first-failure seeds, shrunk sequences); it just counts
# verdicts in a real stressfs-conform-report.json file and prints
# them, which is exactly the bar M2 sets.
#
# The JSON-lines format (one object per line, no multi-line objects --
# see amiga/lib/conform.c's stressfs_conform_format_json and
# amiga/lib/report.c's stressfs_report_format_header_json) is simple
# enough that a portable grep/sed pipeline is a defensible choice over
# pulling in a JSON library for a "trivial" checker; the one thing it
# depends on is that stressfs never emits a literal `"verdict":"...”`
# substring inside a *different* field's string value for a verdict
# word, which is true by construction (verdict is a closed enum, see
# stressfs_verdict_name() in amiga/include/stressfs/conform.h) and is
# exactly the kind of "mostly numeric/enum-controlled strings, so keep
# it simple" tradeoff the task's own JSON-escaping guidance calls out.
#
# Usage:
#   ci/report-checker.sh path/to/stressfs-conform-report.json
#
# Output: one "verdict=N" line per verdict seen, plus a run_identity
# line and a total line. Exit code: 0 if the file parsed and contained
# at least one run_identity header line and at least one test_result
# line; 1 otherwise (empty/missing file, or a header-less report --
# which would itself indicate the harness-bug path in report.c fired).
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <report.json>" >&2
  exit 1
fi

REPORT="$1"

if [ ! -f "$REPORT" ]; then
  echo "error: report file not found: $REPORT" >&2
  exit 1
fi

HEADER_LINES=$(grep -c '"type":"run_identity"' "$REPORT" || true)
BUG_LINES=$(grep -c '"type":"harness_bug"' "$REPORT" || true)
TEST_LINES=$(grep -c '"type":"test_result"' "$REPORT" || true)

echo "== stressfs report-checker: $REPORT =="
echo "run_identity headers: $HEADER_LINES"
if [ "$BUG_LINES" -gt 0 ]; then
  echo "HARNESS BUG headers (run identity was incomplete): $BUG_LINES"
fi
echo "test_result lines: $TEST_LINES"

TOTAL=0
for v in pass fail deviation unsupported inconclusive; do
  COUNT=$(grep -c "\"type\":\"test_result\",\"verdict\":\"$v\"" "$REPORT" || true)
  echo "verdict=$v count=$COUNT"
  TOTAL=$((TOTAL + COUNT))
done
echo "verdict=total count=$TOTAL"

if [ "$HEADER_LINES" -lt 1 ] || [ "$TEST_LINES" -lt 1 ]; then
  echo "error: report is missing a run_identity header or has no test_result lines" >&2
  exit 1
fi

exit 0
