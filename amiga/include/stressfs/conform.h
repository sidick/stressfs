/* stressfs/conform.h
 *
 * The table-driven conformance engine's PORTABLE core (M1,
 * docs/implementation-plan.md section 3's M1 entry and the step-1
 * instruction: "a static array of test entries ... the table
 * structure, expectation-matching logic and report-line formatting
 * almost certainly have no Amiga dependencies even though the actual
 * DoPkt() calls do"). This header and its .c file in amiga/lib build
 * and unit-test on the host (see amiga/tests/conform_test.c); they
 * never include an Amiga system header.
 *
 * The trick that keeps this portable: a test entry's "run" step is a
 * function pointer with a fully generic signature (void *ctx in,
 * {res1,res2} out). The *body* of that function is free to call
 * DoPkt() and friends — it lives in amiga/src/ and only compiles
 * cross — but the pointer type itself, and everything that consumes
 * its result (expectation matching, verdict computation, line
 * formatting), has no Amiga dependency and is exactly what this file
 * declares.
 *
 * Verdict computation and line formatting are deliberately two
 * separate functions (stressfs_conform_evaluate vs.
 * stressfs_conform_format_line) rather than one combined step, per
 * the task's instruction not to paint M2's machine-readable report
 * writer into a corner: M2 can call stressfs_conform_evaluate exactly
 * as-is and substitute its own (JSON) formatter.
 */
#ifndef STRESSFS_CONFORM_H
#define STRESSFS_CONFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The proposal's full verdict vocabulary (docs/proposal.md, "What a
 * pass means"). `deviation` is listed per the proposal but M1 does not
 * populate it (no quirks file yet; that's M3) -- it exists here so M3
 * does not have to touch this enum or any switch over it. */
typedef enum {
    STRESSFS_VERDICT_PASS = 0,
    STRESSFS_VERDICT_FAIL,
    STRESSFS_VERDICT_DEVIATION,
    STRESSFS_VERDICT_UNSUPPORTED,
    STRESSFS_VERDICT_INCONCLUSIVE
} stressfs_verdict_t;

/* docs/proposal.md, "Oracle classes". Every test entry names its
 * oracle class as a property of the table entry, not a parameter
 * decided later at the call site -- the proposal calls this out
 * explicitly as a risk to avoid ("the oracle class is a property of
 * the oracle object ... not a parameter of the check call"). */
typedef enum {
    STRESSFS_ORACLE_INDEPENDENT = 0, /* the AmigaDOS Manual itself */
    STRESSFS_ORACLE_MODEL,           /* the harness's own expectation */
    STRESSFS_ORACLE_CORRELATED       /* a host implementation sharing code */
} stressfs_oracle_t;

/* The raw dp_Res1/dp_Res2 pair a packet send (or dos.library wrapper
 * call) produced. Plain longs -- no Amiga type needed to represent
 * this, which is exactly why it can cross the portable/Amiga-only
 * boundary in a function pointer's signature. */
typedef struct {
    long res1;
    long res2;
} stressfs_pkt_result_t;

/* Runs one test's action (sends the packet, or calls its dos.library
 * wrapper twin) and returns what came back. `ctx` is an opaque,
 * entry-specific context (e.g. "the handler port and a BPTR to the
 * fixture file's lock") allocated and owned by amiga/src/ code.
 *
 * May also be NULL for a table entry representing a pure watchdog
 * self-test or a placeholder; stressfs_conform_run (amiga/src/) treats
 * a NULL run function as "do not execute, this entry is a label only"
 * -- not currently used by the M1 table, but kept available so a
 * future milestone's "the previous entry's inconclusive aborted the
 * run" bookkeeping has somewhere to attach a line without re-running
 * anything.
 */
typedef stressfs_pkt_result_t (*stressfs_test_run_fn)(void *ctx);

/* A predicate for the (several) packets in the table whose *shape* of
 * success is correct but which may validly succeed in more than one
 * way depending on handler state (decision not fully specified by the
 * implementation plan -- see e.g. ACTION_PARENT on the root directory,
 * which legitimately returns dp_Res1==0/dp_Res2==0 *or* a non-zero
 * lock depending on whether dp_Arg1 already was the root). Returns
 * nonzero for "this result counts as a pass". */
typedef int (*stressfs_test_predicate_fn)(stressfs_pkt_result_t got, void *ctx);

/* Opaque to this header: see stressfs/quirks.h, which includes this
 * file (so the dependency only goes one way). The formatters below
 * take a pointer to one of these so a report line can carry the quirk
 * that changed its verdict without lib/conform.c needing the whole
 * quirks database. */
struct stressfs_quirk_applied;

/* One row of the conform table. */
typedef struct {
    /* M3: a short, stable machine name for this test -- lowercase,
     * hyphenated, e.g. "locate-exclusive-root". The quirks file's
     * action targets are these ids (see stressfs/quirks.h), which is
     * why they exist: pointing a quirks entry at the 70-character
     * human-readable `name` below would make the file unreadable and
     * would break every time the prose was reworded. The id is also
     * emitted in both report renderings so a reader can map a line
     * back to a quirks entry (and so ci/quirks-check.sh can
     * cross-reference the two files -- docs/implementation-plan.md
     * section 5's "quirks-file drift from the packet matrix" risk).
     * Must be unique across the table. */
    const char *id;
    const char *name;     /* e.g. "ACTION_LOCATE_OBJECT (shared, success)" */
    const char *section;  /* manual section / packet reference, e.g. "packet-documentation.md#ACTION_LOCATE_OBJECT" */
    int min_version;      /* dos.library version (33, 36, 37, ...) this
                            * packet requires; 0 means "always applicable". */
    stressfs_oracle_t oracle;

    stressfs_test_run_fn run;
    void *run_ctx;

    /* Expectation. If `predicate` is non-NULL it is authoritative and
     * the four exact-match fields below are ignored entirely -- this
     * is the "multi-valid-outcome packet" escape hatch. Otherwise each
     * of res1/res2 is checked for exact equality unless its
     * corresponding `_any` flag is set (meaning "don't care, any value
     * passes for this half of the result"). */
    long expect_res1;
    int  expect_res1_any;
    long expect_res2;
    int  expect_res2_any;
    stressfs_test_predicate_fn predicate;
} stressfs_test_entry_t;

/* Pure function: given the entry's expectation (or predicate) and what
 * the packet/wrapper actually returned, plus the handler's identified
 * dos.library version (0 if unknown -- see stressfs_conform_handler_info_t
 * in handler_id.h for how that gets established), decide the verdict.
 *
 * Version gating happens here, first: if entry->min_version is above
 * handler_version (and handler_version is known, i.e. nonzero), the
 * verdict is STRESSFS_VERDICT_UNSUPPORTED regardless of `got` -- the
 * caller must not have executed the packet at all in that case (see
 * the header comment on stressfs_test_run_fn), `got` is only
 * meaningful for the executed case and is ignored for the gated one. */
stressfs_verdict_t stressfs_conform_evaluate(const stressfs_test_entry_t *entry,
                                              stressfs_pkt_result_t got,
                                              int handler_version);

/* Formats one human-readable report line into `buf` (NUL-terminated,
 * truncated to fit `bufsz`). Does not append a trailing newline --
 * callers (amiga/src/conform.c's Printf-based writer, or a host test
 * harness) own line termination. Kept deliberately simple text at M1
 * (full JSON-lines is M2's job, see the header comment above) but
 * still packs every field the proposal's "Output" paragraph asks for:
 * verdict, oracle class, manual section, and the test name/packet.
 */
/* M3 added the `quirk` parameter: the quirks entry that changed this
 * line's verdict, or NULL (or an entry with .applied == 0) when none
 * did. docs/proposal.md's Layer 1 "Output" paragraph requires every
 * line to carry "every quirk that was applied (its id, confidence and
 * the match reason)", so this is not an optional extra -- a line whose
 * verdict a quirk changed and which does not say so would be exactly
 * the "a quirk silently hides a regression" failure the proposal
 * forbids. */
void stressfs_conform_format_line(char *buf, size_t bufsz,
                                   const stressfs_test_entry_t *entry,
                                   stressfs_verdict_t verdict,
                                   stressfs_pkt_result_t got,
                                   const struct stressfs_quirk_applied *quirk);

/* M2: the machine-readable twin of stressfs_conform_format_line(),
 * living alongside it per this header's top comment -- M1 deliberately
 * split evaluate()/format_line() apart so M2 could add this without
 * touching either. One JSON object per line (no trailing newline;
 * same convention as format_line above), schema stable from M2
 * onward: verdict, oracle class, manual section, test name, the raw
 * dp_Res1/dp_Res2 pair, and `applied_quirks` -- empty at M2, and
 * populated by M3 with one {"id","confidence","match_reason"} object
 * when a quirk changed this line's verdict (an array, not a single
 * object, because the schema was fixed as an array at M2 and because
 * later phases may want to record several contributing entries; M3
 * never emits more than one, see stressfs_quirks_apply_verdict()).
 * M3 also added the `test` field (the table entry's stable id). Truncates into `buf`/`bufsz` like every other
 * formatter here; never crashes on a NULL entry or a too-small buffer. */
void stressfs_conform_format_json(char *buf, size_t bufsz,
                                   const stressfs_test_entry_t *entry,
                                   stressfs_verdict_t verdict,
                                   stressfs_pkt_result_t got,
                                   const struct stressfs_quirk_applied *quirk);

/* Stable, short strings for each verdict/oracle value, used by both
 * the formatter above and anything else (host tests, a future M2
 * summariser) that wants the canonical spelling. */
const char *stressfs_verdict_name(stressfs_verdict_t v);
const char *stressfs_oracle_name(stressfs_oracle_t o);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_CONFORM_H */
