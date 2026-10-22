# data/

Holds [`stressfs.quirks`](stressfs.quirks), the single-source quirks file
(match conditions, actions, confidence tiers) that
[`docs/quirks.md`](../docs/quirks.md) is generated from — seeded at M3
(docs/implementation-plan.md section 1 and milestone M3), once the conform
table (M1) and the report/verdict machinery (M2) it refers to actually
existed; a quirks file with nothing to apply to would have been fiction.

- The file's own header documents the grammar; the authoritative parser is
  [`amiga/lib/quirks.c`](../amiga/lib/quirks.c), whose header
  ([`amiga/include/stressfs/quirks.h`](../amiga/include/stressfs/quirks.h))
  documents the action verbs, the verdict each produces, the confidence tiers
  and the malformed-vs-unevaluable distinction.
- `ci/quirks-doc.sh --write` regenerates `docs/quirks.md` from this file;
  CI regenerates it and fails on any byte of drift, so the published page can
  never disagree with the file the tool reads.
- `ci/quirks-check.sh` cross-references every action target here against the
  conform table's test ids in `amiga/src/conform.c`.
- The file is shipped in the archive alongside the binary: `stressfs` looks
  for `stressfs.quirks` in the current directory, then `PROGDIR:`, then
  `data/` (see `amiga/src/quirks_file.c`), or wherever `QUIRKS=<path>` says.
