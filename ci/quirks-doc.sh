#!/usr/bin/env bash
# ci/quirks-doc.sh
#
# M3 (docs/implementation-plan.md section 3's M3 entry): "The
# docs/quirks.md generator script", satisfying the proposal's promise
# that "the build generates a documentation page from it (handler,
# version, packet, behaviour, workaround) so the published list can
# never drift from what the tool actually does".
#
# Reads data/stressfs.quirks and writes the documentation page to
# stdout. docs/quirks.md is that output, checked in, and CI regenerates
# it and diffs byte-for-byte (see .github/workflows/ci.yml's
# quirks-doc job and the --check mode below), which is the M3 exit
# criterion "docs/quirks.md regenerates byte-identically from the
# shipped file in CI". The page is GENERATED -- never hand-edited; the
# header it emits says so.
#
# Usage:
#   ci/quirks-doc.sh                 # write the page to stdout
#   ci/quirks-doc.sh --write         # overwrite docs/quirks.md
#   ci/quirks-doc.sh --check         # regenerate and diff against the
#                                    #   checked-in copy; exit 1 on drift
#
# Implementation note: awk, not a C tool or a Python script. The format
# is line-oriented with keywords at column 0 (that is the whole point
# of it being "hand-parseable on a stock AmigaDOS machine"), awk is in
# every CI image this repo uses, and keeping the generator outside the
# Amiga binary is deliberate -- the harness reads the file to decide
# what to do, the docs page is a host-side build product, and nothing
# on the Amiga side should carry documentation-formatting code. The
# AUTHORITATIVE parser is still amiga/lib/quirks.c: this script does not
# validate the grammar (the host ctest `quirks_test` parses the same
# shipped file with the real parser on every CI run, which is a
# stronger check than anything awk could do), it only renders it.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIRKS="${STRESSFS_QUIRKS_FILE:-$ROOT/data/stressfs.quirks}"
DOC="${STRESSFS_QUIRKS_DOC:-$ROOT/docs/quirks.md}"

if [ ! -f "$QUIRKS" ]; then
  echo "error: quirks file not found: $QUIRKS" >&2
  exit 1
fi

generate() {
  awk '
    function flush_entry() {
      if (id == "") return
      n_entries[status]++
      order[status, n_entries[status]] = id
      e_handler[id] = (handler == "") ? "(any handler)" : handler
      e_version[id] = (version == "") ? "(any version)" : version
      e_other[id]   = other
      e_packet[id]  = packets
      e_because[id] = because
      e_work[id]    = workaround
      id = ""; handler = ""; version = ""; other = ""
      packets = ""; because = ""; workaround = ""; status = ""
    }

    # Keywords are at column 0; a leading-whitespace line continues the
    # preceding `because` free text. Comments and blanks are ignored.
    /^[ \t]/ {
      line = $0
      sub(/^[ \t]+/, "", line)
      if (in_because && line != "") because = because " " line
      next
    }
    /^#/ { next }
    /^[ \t]*$/ { next }

    {
      keyword = $1
      rest = $0
      sub(/^[^ \t]+[ \t]+/, "", rest)
      if (rest == $0) rest = ""
      in_because = 0
    }

    keyword == "quirk" {
      flush_entry()
      id = rest
      next
    }

    keyword == "match" {
      for (i = 2; i <= NF; i++) {
        tok = $i
        if (tok ~ /^[Nn][Aa][Mm][Ee]=/) {
          v = tok; sub(/^[^=]*=/, "", v)
          handler = (handler == "") ? v : handler ", " v
        } else if (tok ~ /^([Vv][Ee][Rr][Ss][Ii][Oo][Nn]|[Rr][Ee][Vv][Ii][Ss][Ii][Oo][Nn]|[Kk][Ii][Cc][Kk]|[Dd][Oo][Ss])[<>=]/) {
          version = (version == "") ? tok : version " and " tok
        } else {
          other = (other == "") ? tok : other ", " tok
        }
      }
      next
    }

    keyword == "because" {
      because = rest
      in_because = 1
      next
    }

    keyword == "action" {
      verb = $2
      target = $3
      if (verb == "skip") {
        w = "not executed at all against this handler (verdict `unsupported`)"
      } else if (verb == "downgrade") {
        w = "any failure of this test is reported as `deviation`, not `fail`"
      } else if (verb == "expect-deviation") {
        split($4, rp, "/")
        if (rp[1] == "*") rp[1] = "any"
        if (rp[2] == "*") rp[2] = "any"
        w = "reported as `deviation` only when the handler returns dp_Res1=" rp[1] \
            ", dp_Res2=" rp[2] " (any other failure stays a `fail`)"
      } else {
        w = verb " " target
      }
      e = "`" target "`"
      packets = (packets == "") ? e : packets ", " e
      w = e ": " w
      workaround = (workaround == "") ? w : workaround "; " w
      next
    }

    keyword == "status" {
      status = rest
      next
    }

    END {
      flush_entry()

      print "# Documented handler quirks"
      print ""
      print "**GENERATED FILE — do not edit.** Produced by `ci/quirks-doc.sh` from"
      print "[`data/stressfs.quirks`](../data/stressfs.quirks), which is the single source"
      print "of truth: the `stressfs` binary reads that same file to decide what to skip,"
      print "downgrade and expect, so this page can never drift from what the tool actually"
      print "does (docs/proposal.md, \"Deliverables and relationships\"). CI regenerates it"
      print "and fails on any difference. To change anything here, edit the quirks file and"
      print "re-run `ci/quirks-doc.sh --write`."
      print ""
      print "Each entry lists the handler and version range it matches, the conform-table"
      print "test (or packet) it applies to, the behaviour observed, and what stressfs does"
      print "about it. Entries are grouped by **confidence tier**, which decides whether an"
      print "entry applies on its own:"
      print ""
      print "| Tier | Applied | Meaning |"
      print "| --- | --- | --- |"
      print "| `confirmed` | automatically, whenever its match conditions hold | reproduced by stressfs itself against a real handler under an emulator or on hardware |"
      print "| `reported` | only with `FORCEQUIRK=<id>` | documented or seen elsewhere, not reproduced by this project |"
      print "| `suspected` | only with `FORCEQUIRK=<id>` | a guess, or a single unexplained failure stressfs hit once |"
      print ""
      print "Nothing is promoted to `confirmed` on the strength of a document alone."
      print "`NOQUIRKS` (the `-K` switch) ignores the whole file, confirmed entries"
      print "included, so a quirk-covered deviation shows up as the raw finding."
      print ""

      ntiers = split("confirmed reported suspected", tiers, " ")
      for (t = 1; t <= ntiers; t++) {
        tier = tiers[t]
        print "## " tier
        print ""
        if (n_entries[tier] + 0 == 0) {
          print "_No entries at this confidence tier._"
          print ""
          continue
        }
        for (k = 1; k <= n_entries[tier]; k++) {
          qid = order[tier, k]
          print "### `" qid "`"
          print ""
          print "| Field | Value |"
          print "| --- | --- |"
          print "| Handler | " e_handler[qid] " |"
          print "| Version | " e_version[qid] " |"
          if (e_other[qid] != "") {
            print "| Other match conditions | `" e_other[qid] "` |"
          }
          print "| Test / packet | " e_packet[qid] " |"
          print "| Confidence | `" tier "` |"
          print "| What stressfs does | " e_work[qid] " |"
          print ""
          print "**Behaviour.** " e_because[qid]
          print ""
        }
      }
    }
  ' "$QUIRKS"
}

case "${1:-}" in
  --write)
    generate > "$DOC"
    echo "wrote $DOC"
    ;;
  --check)
    TMP="$(mktemp -t stressfs-quirks-doc-XXXXXX.md)"
    trap 'rm -f "$TMP"' EXIT
    generate > "$TMP"
    if [ ! -f "$DOC" ]; then
      echo "error: $DOC does not exist -- run ci/quirks-doc.sh --write" >&2
      exit 1
    fi
    if ! diff -u "$DOC" "$TMP"; then
      echo "" >&2
      echo "error: docs/quirks.md is not byte-identical to what ci/quirks-doc.sh" \
           "generates from data/stressfs.quirks (diff above). Re-run" \
           "ci/quirks-doc.sh --write and commit the result; never hand-edit the page." >&2
      exit 1
    fi
    echo "docs/quirks.md is byte-identical to the generated page"
    ;;
  "")
    generate
    ;;
  *)
    echo "usage: $0 [--write|--check]" >&2
    exit 1
    ;;
esac
