#!/usr/bin/env bash
# ci/quirks-check.sh
#
# Cross-references every action target in data/stressfs.quirks against
# the conform table's test ids in amiga/src/conform.c --
# docs/implementation-plan.md section 5's "Quirks-file drift from the
# packet matrix" risk: "if entries reference packets or tests the table
# no longer contains (or vice versa), quirks silently stop applying and
# findings get mislabelled."
#
# WARN-ONLY by default (exit 0), --strict to fail. That is a deliberate
# M3 decision, not laziness: the plan's own M3 wording asks the seeded
# file to carry the FFS <= V40 ACTION_FLUSH entry from the proposal,
# and ACTION_FLUSH is NOT in phase 1's ~20-packet matrix -- it is
# phase-2/phase-5 work (see the plan's section 4). The same is true of
# ACTION_MORE_CACHE. Those two entries are therefore deliberate forward
# references: inert today, pointing at test ids that will exist when
# the durability and cache tests land. Making the cross-reference a
# hard CI gate now would mean either deleting the entries the plan asks
# for, or inventing stub table rows to satisfy a checker, both worse
# than printing the dangling list. The strict gate belongs with phase
# 2's full matrix; `--strict` is there so it can be switched on in one
# line when that happens.
#
# Usage:
#   ci/quirks-check.sh [--strict]
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIRKS="$ROOT/data/stressfs.quirks"
TABLE="$ROOT/amiga/src/conform.c"

STRICT=0
if [ "${1:-}" = "--strict" ]; then
  STRICT=1
fi

for f in "$QUIRKS" "$TABLE"; do
  if [ ! -f "$f" ]; then
    echo "error: not found: $f" >&2
    exit 1
  fi
done

# The table's test ids are the first argument of each ENTRY_* macro
# invocation (see amiga/src/conform.c's ENTRY_EXACT/ENTRY_RES1_ANY/
# ENTRY_PRED definitions, whose leading parameter is `tid`).
TABLE_IDS="$(grep -oE 'ENTRY_(EXACT|RES1_ANY|PRED)\("[a-z0-9-]+"' "$TABLE" \
             | sed -E 's/.*"([a-z0-9-]+)"/\1/' | sort)"

# Every action target in the quirks file, with any trailing '*' kept so
# wildcard entries can be reported as such.
TARGETS="$(awk '$1 == "action" { print $3 }' "$QUIRKS" | sort -u)"

echo "== stressfs quirks/table cross-reference =="
echo "conform table test ids: $(echo "$TABLE_IDS" | grep -c . || true)"
echo "quirks action targets:  $(echo "$TARGETS" | grep -c . || true)"

# Duplicate table ids would make a quirks target ambiguous.
DUPES="$(echo "$TABLE_IDS" | uniq -d || true)"
if [ -n "$DUPES" ]; then
  echo "error: duplicate conform-table test ids (a quirks target would be ambiguous):" >&2
  echo "$DUPES" >&2
  exit 1
fi

DANGLING=""
for t in $TARGETS; do
  case "$t" in
    *\*)
      prefix="${t%\*}"
      if ! echo "$TABLE_IDS" | grep -q "^$prefix"; then
        DANGLING="$DANGLING $t"
      fi
      ;;
    *)
      if ! echo "$TABLE_IDS" | grep -qx "$t"; then
        DANGLING="$DANGLING $t"
      fi
      ;;
  esac
done

if [ -z "$DANGLING" ]; then
  echo "every quirks action target names an existing conform-table test id"
  exit 0
fi

echo ""
echo "the following quirks action targets match no conform-table test id:"
for t in $DANGLING; do
  ids="$(awk -v tgt="$t" '$1 == "quirk" { id = $2 } $1 == "action" && $3 == tgt { print id }' "$QUIRKS" | sort -u | tr '\n' ' ')"
  echo "  $t   (from: $ids)"
done
echo ""
echo "These entries are INERT: nothing in the table can ever match them, so they"
echo "cannot change a verdict. At M3 that is expected for the deliberate forward"
echo "references (ACTION_FLUSH durability, ACTION_MORE_CACHE, the PFS3 placeholder);"
echo "see this script's header comment."

if [ "$STRICT" = 1 ]; then
  echo "error: --strict given and dangling targets exist" >&2
  exit 1
fi
exit 0
