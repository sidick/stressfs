# Documented handler quirks

**GENERATED FILE — do not edit.** Produced by `ci/quirks-doc.sh` from
[`data/stressfs.quirks`](../data/stressfs.quirks), which is the single source
of truth: the `stressfs` binary reads that same file to decide what to skip,
downgrade and expect, so this page can never drift from what the tool actually
does (docs/proposal.md, "Deliverables and relationships"). CI regenerates it
and fails on any difference. To change anything here, edit the quirks file and
re-run `ci/quirks-doc.sh --write`.

Each entry lists the handler and version range it matches, the conform-table
test (or packet) it applies to, the behaviour observed, and what stressfs does
about it. Entries are grouped by **confidence tier**, which decides whether an
entry applies on its own:

| Tier | Applied | Meaning |
| --- | --- | --- |
| `confirmed` | automatically, whenever its match conditions hold | reproduced by stressfs itself against a real handler under an emulator or on hardware |
| `reported` | only with `FORCEQUIRK=<id>` | documented or seen elsewhere, not reproduced by this project |
| `suspected` | only with `FORCEQUIRK=<id>` | a guess, or a single unexplained failure stressfs hit once |

Nothing is promoted to `confirmed` on the strength of a document alone.
`NOQUIRKS` (the `-K` switch) ignores the whole file, confirmed entries
included, so a quirk-covered deviation shows up as the raw finding.

## confirmed

### `ffs-is-filesystem-res2-garbage`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | version>=40 |
| Test / packet | `baseline-is-filesystem` |
| Confidence | `confirmed` |
| What stressfs does | `baseline-is-filesystem`: reported as `deviation` only when the handler returns dp_Res1=-1, dp_Res2=any (any other failure stays a `fail`) |

**Behaviour.** ACTION_IS_FILESYSTEM answers correctly in dp_Res1 (DOSTRUE) but does not zero dp_Res2: FFS 47.4 returned dp_Res2 = 1027 where the manual's packet documentation says the second result is unused for this packet and callers that read IoErr() after a successful DoPkt() therefore see stale rubbish. This is the proposal's "ACTION_IS_FILESYSTEM dp_Res2-not-zeroed" quirk (docs/proposal.md, "Documented quirks checked explicitly"). The declared pair pins dp_Res1 to the CORRECT value (-1) and leaves dp_Res2 as '*' deliberately: the exact leftover value is whatever happened to be in the packet, so pinning it would make the entry stop applying for no good reason, while pinning dp_Res1 keeps the entry from ever covering a handler that got the actual answer wrong.

### `ffs-nil-action-not-known`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | version>=40 |
| Test / packet | `baseline-nil` |
| Confidence | `confirmed` |
| What stressfs does | `baseline-nil`: reported as `deviation` only when the handler returns dp_Res1=0, dp_Res2=209 (any other failure stays a `fail`) |

**Behaviour.** ACTION_NIL (packet 0) is documented as a no-op a handler should reply to successfully; FFS 47.4 instead returns DOSFALSE with dp_Res2 = ERROR_ACTION_NOT_KNOWN (209), i.e. its packet switch has no case for it. Harmless in itself -- nothing in AmigaDOS sends ACTION_NIL -- but it is a real, reproducible protocol deviation and the baseline ladder sends it deliberately (docs/proposal.md, "Safe probing" point 1), so it must be declared rather than counted as a handler bug every run.

### `ffs-exclusive-root-lock-granted`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | version>=40 |
| Test / packet | `locate-exclusive-root` |
| Confidence | `confirmed` |
| What stressfs does | `locate-exclusive-root`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** The AmigaDOS Manual's packet chapter states an exclusive lock on the root directory is refused (docs/proposal.md lists "exclusive lock on the root refused" among the documented quirks to check explicitly). FFS 47.4 GRANTS it: the ACTION_LOCATE_OBJECT returns a real lock in dp_Res1 with dp_Res2 = 0. stressfs frees that lock again immediately -- see amiga/src/conform.c's run_locate_exclusive_root_refused(), which learned that the hard way -- so the rest of the run is unaffected. dp_Res1 is '*' because the value is a lock pointer and differs every run; dp_Res2 is pinned to 0 so that a genuine error return (which is what the manual describes) would NOT be covered by this entry.

### `ffs-set-file-size-returns-size`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | version>=40 |
| Test / packet | `set-file-size-grow`, `set-file-size-truncate` |
| Confidence | `confirmed` |
| What stressfs does | `set-file-size-grow`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`); `set-file-size-truncate`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** ACTION_SET_FILE_SIZE is documented to return DOSTRUE/DOSFALSE in dp_Res1; FFS 47.4 returns the file's NEW SIZE there instead (20 after growing stressfs's 15-byte fixture by 5, 5 after truncating it to 5), with dp_Res2 = 0. This is the proposal's "ACTION_SET_FILE_SIZE return-value" quirk. dp_Res1 is '*' because the value is a file size that depends on the fixture; dp_Res2 pinned to 0 keeps a real failure (nonzero dp_Res2) out of this entry's scope. Note the dos.library wrapper hides this: SetFileSize() reports success either way, which is exactly the proposal's reason for testing packets and wrappers as twins.

### `ffs-v40-is-filesystem-res2-garbage`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | dos>=40 |
| Other match conditions | `dostype=DOS1` |
| Test / packet | `baseline-is-filesystem` |
| Confidence | `confirmed` |
| What stressfs does | `baseline-is-filesystem`: reported as `deviation` only when the handler returns dp_Res1=-1, dp_Res2=any (any other failure stays a `fail`) |

**Behaviour.** Same finding as ffs-is-filesystem-res2-garbage above, reproduced directly against FFS V40 (ROM-resident, no "$VER:" string): ACTION_IS_FILESYSTEM answers dp_Res1 = DOSTRUE correctly but leaves dp_Res2 = 1027 un-zeroed.

### `ffs-v40-nil-action-not-known`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | dos>=40 |
| Other match conditions | `dostype=DOS1` |
| Test / packet | `baseline-nil` |
| Confidence | `confirmed` |
| What stressfs does | `baseline-nil`: reported as `deviation` only when the handler returns dp_Res1=0, dp_Res2=209 (any other failure stays a `fail`) |

**Behaviour.** Same finding as ffs-nil-action-not-known above, reproduced directly against FFS V40: ACTION_NIL returns DOSFALSE with dp_Res2 = ERROR_ACTION_NOT_KNOWN (209).

### `ffs-v40-exclusive-root-lock-granted`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | dos>=40 |
| Other match conditions | `dostype=DOS1` |
| Test / packet | `locate-exclusive-root` |
| Confidence | `confirmed` |
| What stressfs does | `locate-exclusive-root`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** Same finding as ffs-exclusive-root-lock-granted above, reproduced directly against FFS V40: an exclusive ACTION_LOCATE_OBJECT on the root grants a real lock (dp_Res2 = 0) instead of being refused.

### `ffs-v40-set-file-size-returns-size`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | dos>=40 |
| Other match conditions | `dostype=DOS1` |
| Test / packet | `set-file-size-grow`, `set-file-size-truncate` |
| Confidence | `confirmed` |
| What stressfs does | `set-file-size-grow`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`); `set-file-size-truncate`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** Same finding as ffs-set-file-size-returns-size above, reproduced directly against FFS V40: ACTION_SET_FILE_SIZE returns the new file size in dp_Res1 (20 after growing the 15-byte fixture by 5, 5 after truncating it back) rather than DOSTRUE/DOSFALSE.

### `ffs-v40-end-close-returns-32`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | dos>=40 |
| Other match conditions | `dostype=DOS1` |
| Test / packet | `end-close-output`, `end-close-input` |
| Confidence | `confirmed` |
| What stressfs does | `end-close-output`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`); `end-close-input`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** NEW finding, not present against FFS 47.4: ACTION_END (the packet dos.library's Close() sends) is documented to return DOSTRUE/DOSFALSE in dp_Res1; FFS V40 instead returns 32 there, with dp_Res2 = 0, both for a file opened for output (ACTION_FINDOUTPUT) and one opened for input (ACTION_FINDINPUT). 32 is neither DOSTRUE (-1) nor a documented AmigaDOS error code, and it is not simply the fixture's byte count (the output fixture is 15 bytes, later grown to 20 and truncated to 5 by the SET_FILE_SIZE tests above; the input fixture, probe.txt, is a separate small file) -- it does not correlate with the ACTION_END packet number either (32 is not ACTION_END's own `dp_Type`). stressfs has not determined what 32 actually encodes; it is recorded here as observed fact, not explained. dos.library's Close() (RKRM "Input and Output") reports success based on this same dp_Res1 the same way it treats every other packet's DOSTRUE, so a close() caller relying on the wrapper sees ordinary success either way -- again the packet/wrapper-twin split the proposal calls for is what surfaces this at all. dp_Res1 is pinned to '*' (the value is a fixed constant here, 32, but pinning it narrowly risks the entry going stale if a different fixture size ever changes whatever 32 actually represents); dp_Res2 pinned to 0 keeps a real failure out of this entry's scope.

## reported

### `ffs-v40-flush-acks-early`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | version<=40 |
| Test / packet | `flush-durability-*` |
| Confidence | `reported` |
| What stressfs does | `flush-durability-*`: any failure of this test is reported as `deviation`, not `fail` |

**Behaviour.** docs/proposal.md, "Durability boundary for crash tests": "FFS <= V40 replied before the write finished, which the quirks file records". ACTION_FLUSH is supposed to be the acknowledgement point -- "data is acknowledged only when ACTION_FLUSH has returned" -- so a handler that replies to it before the data is on the medium breaks the durability boundary every layer-4 crash test is judged against, and a power cut after a successful Flush() can lose acknowledged data on these versions. The action targets a durability test id (flush-durability-*) that the M1/M2 conform table does NOT yet contain: ACTION_FLUSH is not in phase 1's ~20-packet matrix, and proving this quirk needs the layer-4 fault machinery (Copperline's disk.fault `ignore` policy), which is phase 5. The entry is seeded now, inert, so that the durability test inherits a declared expectation the day it lands rather than being written against a handler whose behaviour nobody wrote down. Status is `reported`: the proposal states it as fact about real FFS versions, but this project has not reproduced it, and "nothing may be promoted to confirmed on the strength of a document alone".

## suspected

### `ffs-more-cache-count-in-res1`

| Field | Value |
| --- | --- |
| Handler | fs |
| Version | (any version) |
| Test / packet | `more-cache` |
| Confidence | `suspected` |
| What stressfs does | `more-cache`: reported as `deviation` only when the handler returns dp_Res1=any, dp_Res2=0 (any other failure stays a `fail`) |

**Behaviour.** docs/proposal.md, "Documented quirks checked explicitly": "ACTION_MORE_CACHE returning the count in dp_Res1 as AddBuffers expects". The packet is documented as returning DOSTRUE/DOSFALSE, but AddBuffers reads the resulting buffer count out of dp_Res1 and real handlers oblige, so a strict reading of the manual would fail every handler in existence. ACTION_MORE_CACHE is not in phase 1's matrix (it is on the phase-2 list in the implementation plan's section 4), so this entry is inert and forward-looking, exactly like the flush entry above. Status `suspected` rather than `reported`: the proposal describes the convention, but which of dp_Res1's possible meanings a given FFS version actually implements is precisely what the phase-2 test will establish.

### `pfs3-aio-identity-unexpressible`

| Field | Value |
| --- | --- |
| Handler | (any handler) |
| Version | (any version) |
| Other match conditions | `handler-dostype=PFS3` |
| Test / packet | `pfs3-placeholder-never-matches` |
| Confidence | `suspected` |
| What stressfs does | `pfs3-placeholder-never-matches`: not executed at all against this handler (verdict `unsupported`) |

**Behaviour.** Placeholder kept deliberately, and deliberately UNMATCHABLE, to exercise and document the parser's second failure mode (see amiga/include/stressfs/quirks.h): `handler-dostype` is a well-formed match key this build cannot evaluate, so this entry is flagged unmatchable at load, warned about once, and can only ever apply via FORCEQUIRK=pfs3-aio-identity-unexpressible. PFS3 identity really is not expressible with the current key set: pfs3_handler's "$VER:" string is not reliably present in its segment list, so name=/version= cannot key on it, and the DosType that WOULD identify it ("PFS3") is not one of the keys the parser evaluates. M5 runs stressfs against PFS3-AIO and will either add a key or record the finding; until then this entry documents the gap in the file itself rather than in a comment nobody greps. No real action is attached beyond a skip of a test id that does not exist, so forcing it does nothing harmful. UPDATE at M5: the general gap this entry anticipated IS now closed -- `dostype=` (note: not `handler-dostype`, this entry's own made-up key name) is a real, evaluable match key as of M5 (see the `dostype=DOS1` entries above, and amiga/include/stressfs/quirks.h). What did NOT happen is PFS3 identification itself: PFS3-AIO never mounted long enough under Copperline this session to read ITS DosType back (see ci/m5/pfs3-aio-status.md for the full account), so there is still no reproduced PFS3 entry to add, and this placeholder is left exactly as M3 wrote it -- still deliberately using a key name (`handler-dostype`) the parser was never going to evaluate, still documenting the SPECIFIC gap of "PFS3 never got identified", which remains true even though the KIND of gap M3 guessed at (no evaluable key at all) no longer does.

