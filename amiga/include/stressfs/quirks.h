/* stressfs/quirks.h -- M3 (docs/implementation-plan.md section 3's M3
 * entry): the stressfs.quirks database.
 *
 * PORTABLE, same rule as conform.h/report.h: this file and
 * amiga/lib/quirks.c parse already-read text and evaluate matches over
 * an already-collected identity struct. No Amiga header, no file I/O,
 * no allocation -- every table here is fixed-size storage inside
 * stressfs_quirks_db_t, which the caller owns (amiga/src/conform.c
 * keeps one static instance). Reading the file off disk is Amiga-only
 * and lives in amiga/src/quirks_file.c; the host unit tests
 * (amiga/tests/quirks_test.c) feed the same parser from string
 * literals and from fixture files read with stdio.
 *
 * ---------------------------------------------------------------------
 * FILE FORMAT
 * ---------------------------------------------------------------------
 * Deliberately the same grammar and the same semantics as
 * ~/src/devsoak/devsoak.quirks, per docs/implementation-plan.md
 * section 0's decision table ("stressfs.quirks, same format and
 * semantics as devsoak.quirks"). Keywords at column 0, '#' comments
 * and blank lines ignored, a leading-whitespace line is a continuation
 * of the preceding `because` free text and is skipped:
 *
 *     quirk   <id>
 *     match   key=value key<value key>=value ...   (repeatable, AND'd
 *                                                   across ALL match
 *                                                   lines in the entry)
 *     because <free text, may wrap onto further indented lines>
 *     action  <verb> <args...>                     (repeatable)
 *     status  confirmed|reported|suspected
 *
 * MATCH KEYS (the implementation plan asks for "handler name, version
 * range, Kickstart version"; the set below is devsoak's own key set
 * re-pointed at *handler* identity instead of *driver* identity, so
 * the two files stay readable by the same eyes):
 *
 *     name=STR        case-insensitive exact match on the handler's
 *                     short name, as derived from its "$VER:" string
 *                     (e.g. "fs" for AmigaOS 3.2's FastFileSystem,
 *                     whose embedded string is "$VER: fs 47.4 (...)").
 *     version{<,<=,>,>=,=}N
 *     revision{<,<=,>,>=,=}N
 *                     the handler's own version/revision, parsed out of
 *                     that same "$VER:" string ("fs 47.4" -> 47 / 4).
 *                     This is the plan's "version range" condition.
 *     id~STR          case-insensitive substring of the FULL "$VER:"
 *                     string (devsoak's id~ equivalent).
 *     kick{<,<=,>,>=,=}N
 *                     the running Kickstart's exec.library lib_Version.
 *                     This is the plan's "Kickstart version" condition.
 *     dos{<,<=,>,>=,=}N
 *                     the running dos.library version. Included because
 *                     stressfs already establishes it for the conform
 *                     table's own min_version gating (handler_id.h) and
 *                     several AmigaDOS-level behaviours are keyed to it
 *                     rather than to the handler's own revision.
 *     dostype=STR     case-insensitive exact match on the volume's
 *                     on-disk DosType ("DOS1", "DOS3", "PFS3", ...).
 *                     Added at M5: a ROM-resident handler (FastFileSystem
 *                     supplied as a Kickstart resident module rather than
 *                     loaded from L:, which is how FFS V40 runs on an
 *                     A3000/A4000-profile Kickstart 3.1 ROM) has no
 *                     classic hunk seglist to scan for a "$VER:" string,
 *                     so name=/version= can never match it even though
 *                     the handler is perfectly identifiable by DosType.
 *                     This is the key the pfs3-aio-identity-unexpressible
 *                     placeholder entry (seeded at M3) asked for.
 *
 * Any other key is WELL-FORMED BUT UNEVALUABLE (e.g. a machine type, a
 * DosType nobody exposes, "the medium is removable"). Exactly as in
 * devsoak, such an entry is not a file error: it is flagged
 * `unmatchable` at load time, warned about once, and can then only ever
 * be applied by naming it explicitly with FORCEQUIRK (the plan's
 * `-k ID`). See the MALFORMED vs. UNEVALUABLE note below -- that
 * distinction is the whole reason both behaviours exist.
 *
 * ACTION VERBS -- exactly the three the implementation plan names
 * ("actions (skip a packet, downgrade a test, expect a deviation)"),
 * no more:
 *
 *     action skip <target>
 *         The test is NOT EXECUTED AT ALL -- no packet is sent. Final
 *         verdict `unsupported`. This is the verb that exists to keep
 *         stressfs away from a packet that wedges a known handler, so
 *         it has to be consulted *before* the run function is called
 *         (stressfs_quirks_check_skip(), below), unlike the other two.
 *         Why `unsupported` and not a new verdict: docs/proposal.md
 *         defines `unsupported` as "the tier, OS version or handler
 *         cannot run the test; always listed, never silently skipped",
 *         which is precisely what a skip quirk asserts about this
 *         handler. It must not be `pass` (that would manufacture
 *         evidence from nothing) and must not be `deviation` (nothing
 *         was observed to deviate -- we declined to look).
 *
 *     action downgrade <target>
 *         The test IS executed. A raw `fail` becomes `deviation`,
 *         WHATEVER shape the failure took. The blanket form: "any
 *         failure of this test against this handler is known
 *         behaviour, and we have not pinned down what it returns."
 *
 *     action expect-deviation <target> <res1>/<res2>
 *         The test IS executed. A raw `fail` becomes `deviation` ONLY
 *         IF the observed dp_Res1/dp_Res2 pair matches the declared
 *         one; either half may be `*` for "any value". A failure of a
 *         DIFFERENT shape stays a raw `fail`. This is the precise form
 *         and the one every seeded entry in data/stressfs.quirks uses,
 *         because it is the only one that cannot hide a regression:
 *         docs/proposal.md's "a quirk can never silently hide a
 *         regression" is a property of pinning the result, not of
 *         declaring the test interesting. The res pair is REQUIRED --
 *         omitting it is a file error, not a silent fallback to
 *         `downgrade`; that is what keeps the two verbs genuinely
 *         distinct rather than one being a typo of the other.
 *
 * In all three cases a `pass` stays `pass` (a handler that has been
 * fixed is reported as fixed, and the quirk simply does not fire), and
 * an `inconclusive` is NEVER touched: a watchdog-detected hang means
 * the handler may be damaged and the proposal aborts the run on it, so
 * no quirks entry is allowed to soften that signal.
 *
 * <target> is a conform-table TEST ID (stressfs_test_entry_t.id, added
 * in M3 exactly so quirks entries have a short stable name to point at
 * instead of a 70-character human-readable test name), with an optional
 * trailing '*' as a prefix wildcard, same convention as devsoak's
 * `skip NSCMD_*`.
 *
 * CONFIDENCE TIERS (docs/implementation-plan.md M3: "confidence tiers
 * where only `confirmed` applies automatically and `reported`/
 * `suspected` need `-k ID`"; devsoak.quirks's own header states the
 * same policy and adds "nothing may be promoted to confirmed on the
 * strength of a document alone"):
 *
 *     status confirmed  -- auto-applies when its match conditions hold.
 *     status reported   -- inert unless its id is named via FORCEQUIRK.
 *     status suspected  -- same.
 *
 * and the whole file is ignored, confirmed entries included, when the
 * caller sets `suppressed` (the plan's `-K`), so a would-be-covered
 * deviation shows up as the raw `fail`/`inconclusive` finding it is.
 *
 * Precisely, for one entry:
 *
 *     eligible = (status == confirmed) || forced-by-id
 *     matched  = every evaluable condition holds
 *                && (entry is not `unmatchable` || forced-by-id)
 *     active   = matched && eligible && !db->suppressed
 *
 * The `|| forced-by-id` on the second line is a DELIBERATE divergence
 * from devsoak's quirks.c as currently written: devsoak's eval_match()
 * returns 0 unconditionally for an unmatchable entry, so such an entry
 * can never apply even with -k -- which contradicts its own file
 * header ("an entry using one is flagged unmatchable at load time and
 * printed as a warning, and can then only ever be applied via -k").
 * stressfs implements the documented intent, since that is the
 * behaviour the format's own documentation promises and the behaviour
 * the plan's text relies on. Entries whose *evaluable* conditions
 * simply do not hold are NOT force-applicable (that part matches
 * devsoak exactly): FORCEQUIRK lifts the confidence gate always, and
 * the match gate only for an unevaluable key.
 *
 * ---------------------------------------------------------------------
 * MALFORMED vs. UNEVALUABLE -- the parser's two failure modes
 * ---------------------------------------------------------------------
 * docs/implementation-plan.md M3's exit criterion wants malformed
 * files "refused loudly, never half-applied", while devsoak's format
 * documentation wants an unknown match key merely warned about. Those
 * are not in tension; they are two different defects:
 *
 *   (a) MALFORMED GRAMMAR -- the file is not in the format at all, so
 *       nothing in it can be trusted: an unknown keyword at column 0,
 *       a keyword before its `quirk` line, a missing/duplicate/oversized
 *       id, a match token with no operator, a numeric key with a
 *       non-numeric value, `id` with an operator other than `~`, an
 *       unknown action verb, an action missing its arguments,
 *       `expect-deviation` without a res pair, an unknown `status`
 *       word, an entry with no `match`/`action`/`status` line, or any
 *       table overflow. stressfs_quirks_parse() reports the first such
 *       error through the diag callback, discards EVERY entry it had
 *       parsed so far (the db is re-initialised, not left holding the
 *       prefix it managed to read) and returns 0. The caller must treat
 *       that as fatal. This is stricter than devsoak, which warns and
 *       skips individual bad lines; the plan's wording for stressfs is
 *       explicit, and "refuse the whole file" is the only rule that
 *       makes "never half-applied" true by construction.
 *
 *   (b) WELL-FORMED BUT UNEVALUABLE MATCH KEY -- the file IS in the
 *       format; it simply names a condition this build cannot test.
 *       Warned once through the diag callback, the entry is marked
 *       `unmatchable`, parsing CONTINUES, and the rest of the file
 *       applies normally. Same leniency as devsoak, for the same
 *       reason: a newer stressfs.quirks shipped with a newer harness
 *       must not brick an older binary, and several genuinely useful
 *       real-world conditions (devsoak's A3000 SDMAC entry, this
 *       file's own machine-dependent cases) have no expressible key.
 */
#ifndef STRESSFS_QUIRKS_H
#define STRESSFS_QUIRKS_H

#include <stddef.h>

#include "stressfs/conform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed limits. Overflowing any of them is a MALFORMED-file error
 * (case (a) above), not a silent truncation: a quirks file bigger than
 * the harness can hold must not be half-applied either. */
#define STRESSFS_QUIRK_MAX_ENTRIES   32
#define STRESSFS_QUIRK_MAX_CONDS      6
#define STRESSFS_QUIRK_MAX_ACTIONS    6
#define STRESSFS_QUIRK_MAX_FORCED     8
#define STRESSFS_QUIRK_ID_LEN        48
#define STRESSFS_QUIRK_TARGET_LEN    48
#define STRESSFS_QUIRK_STR_LEN       48
#define STRESSFS_QUIRK_REASON_LEN   200

/* Confidence tiers. */
typedef enum {
    STRESSFS_QUIRK_CONFIRMED = 0,
    STRESSFS_QUIRK_REPORTED,
    STRESSFS_QUIRK_SUSPECTED
} stressfs_quirk_status_t;

/* Action verbs -- see the header comment for the exact verdict each
 * one produces. */
typedef enum {
    STRESSFS_QUIRK_SKIP = 0,
    STRESSFS_QUIRK_DOWNGRADE,
    STRESSFS_QUIRK_EXPECT_DEVIATION
} stressfs_quirk_verb_t;

/* Match keys and comparison operators. */
typedef enum {
    STRESSFS_QUIRK_KEY_NAME = 0,
    STRESSFS_QUIRK_KEY_VERSION,
    STRESSFS_QUIRK_KEY_REVISION,
    STRESSFS_QUIRK_KEY_ID,
    STRESSFS_QUIRK_KEY_KICK,
    STRESSFS_QUIRK_KEY_DOS,
    /* M5: the volume's on-disk DosType ("DOS1", "PFS3", ...), e.g.
     * `dostype=PFS3`. Added because ROM-resident handlers (FFS on a
     * machine profile whose Kickstart carries FastFileSystem as a
     * resident module rather than an L:-loaded segment, e.g. FFS V40
     * under an A3000 Kickstart 3.1 ROM) have no classic hunk seglist
     * to scan for a "$VER:" string, so `name=`/`version=` can never
     * match them -- handler_id.c's own comment flags this, and the
     * quirks file's `pfs3-aio-identity-unexpressible` placeholder
     * entry asked for exactly this key to be added once M5 needed it.
     * DosType is always available (it comes from ACTION_DISK_INFO /
     * the mount DosEnvec, not the segment list), so it identifies a
     * handler family even when name/version cannot. */
    STRESSFS_QUIRK_KEY_DOSTYPE,
    STRESSFS_QUIRK_KEY_UNEVALUABLE
} stressfs_quirk_key_t;

typedef enum {
    STRESSFS_QUIRK_OP_EQ = 0,
    STRESSFS_QUIRK_OP_LT,
    STRESSFS_QUIRK_OP_LE,
    STRESSFS_QUIRK_OP_GT,
    STRESSFS_QUIRK_OP_GE,
    STRESSFS_QUIRK_OP_SUBSTR
} stressfs_quirk_op_t;

typedef struct {
    unsigned char key;   /* stressfs_quirk_key_t */
    unsigned char op;    /* stressfs_quirk_op_t */
    long num;
    char str[STRESSFS_QUIRK_STR_LEN];
    char raw[STRESSFS_QUIRK_STR_LEN * 2]; /* the condition as written, for the match reason */
} stressfs_quirk_cond_t;

typedef struct {
    unsigned char verb;  /* stressfs_quirk_verb_t */
    char target[STRESSFS_QUIRK_TARGET_LEN];
    int  target_wild;    /* nonzero if `target` ended in '*' (prefix match) */
    long res1;
    int  res1_any;
    long res2;
    int  res2_any;
} stressfs_quirk_action_t;

typedef struct {
    char id[STRESSFS_QUIRK_ID_LEN];
    stressfs_quirk_cond_t conds[STRESSFS_QUIRK_MAX_CONDS];
    int  n_conds;
    int  unevaluable;    /* saw a well-formed but unevaluable match key */
    stressfs_quirk_action_t actions[STRESSFS_QUIRK_MAX_ACTIONS];
    int  n_actions;
    unsigned char status; /* stressfs_quirk_status_t */
} stressfs_quirk_entry_t;

/* Everything the match conditions can be evaluated against. Collected
 * by the Amiga side (handler_id.h + exec/dos library bases) and by the
 * host tests by hand; stressfs_quirks_identity_from_ver() below fills
 * the name/version/revision triple from a raw "$VER:" string so that
 * parsing lives here (portable, unit-tested) rather than in
 * amiga/src/. */
typedef struct {
    char name[STRESSFS_QUIRK_STR_LEN];  /* short handler name, e.g. "fs" */
    char idstring[96];                  /* the full "$VER:" string */
    long version;
    long revision;
    long kick_version;                  /* exec.library lib_Version */
    long dos_version;                   /* dos.library lib_Version */
    int  identified;                    /* 0 = handler identity unknown */
    char dostype[5];                    /* on-disk DosType, e.g. "DOS1",
                                          * "PFS3"; empty if unavailable */
} stressfs_quirk_identity_t;

typedef struct {
    stressfs_quirk_entry_t entries[STRESSFS_QUIRK_MAX_ENTRIES];
    int n_entries;

    char forced[STRESSFS_QUIRK_MAX_FORCED][STRESSFS_QUIRK_ID_LEN];
    int n_forced;

    int loaded;      /* 1 once a file has been parsed successfully */
    int suppressed;  /* 1 = the plan's -K: never consult this db at all */
    char path[128];  /* where the file came from, for the report header */
} stressfs_quirks_db_t;

/* What fired on one report line. `id`/`confidence` point into the db
 * (or at string literals) and stay valid as long as the db does. */
typedef struct stressfs_quirk_applied {
    int applied;
    const char *id;
    const char *confidence;
    unsigned char verb;
    char match_reason[STRESSFS_QUIRK_REASON_LEN];
} stressfs_quirk_applied_t;

/* Diagnostics channel. Called once per warning or per fatal parse
 * error with a complete, already-formatted one-line message (no
 * trailing newline). On the Amiga side this routes to the report's
 * note channel so a warning lands in the report file as well as the
 * console; in tests it collects into a buffer. May be NULL. */
typedef void (*stressfs_quirk_diag_fn)(void *ctx, const char *msg);

void stressfs_quirks_init(stressfs_quirks_db_t *db);

/* Parses `text` (a whole stressfs.quirks file, NUL-terminated; the
 * buffer is NOT modified) into `db`. Returns 1 on success, 0 if the
 * file was MALFORMED -- in which case `db` has been re-initialised and
 * holds nothing (never a half-applied prefix), `db->loaded` is 0, and
 * the diag callback has been told why. `path` is remembered only for
 * reporting and may be NULL. */
int stressfs_quirks_parse(stressfs_quirks_db_t *db, const char *path,
                          const char *text,
                          stressfs_quirk_diag_fn diag, void *diag_ctx);

/* Adds one or more forced ids (the plan's `-k ID`, repeatable; a
 * single comma-separated string is accepted, exactly as devsoak's own
 * `-k ID[,ID]` does -- see amiga/src/main.c for why the CLI spells it
 * FORCEQUIRK). May be called before or after parsing. Returns 1 on
 * success, 0 if the forced-id table overflowed (diagnosed through
 * `diag`). */
int stressfs_quirks_force(stressfs_quirks_db_t *db, const char *ids,
                          stressfs_quirk_diag_fn diag, void *diag_ctx);

/* Fills the name/version/revision/idstring fields of `ident` from a
 * handler's embedded version string. Accepts both the raw marker form
 * ("$VER: fs 47.4 (16.6.2020)") and a bare "name version.revision".
 * Leaves the fields empty/zero and returns 0 if nothing usable could
 * be parsed; the kick/dos fields are the caller's to set. */
int stressfs_quirks_identity_from_ver(stressfs_quirk_identity_t *ident,
                                      const char *verstring);

/* Is entry `idx` active for this run (matched && eligible &&
 * !suppressed, per the header comment's three-line definition)? When
 * it is and `reason`/`reasonsz` are non-NULL, fills `reason` with the
 * human-readable match reason that goes in the report (which
 * conditions held, and whether the entry auto-applied on `confirmed`
 * or was forced by id). */
int stressfs_quirks_entry_active(const stressfs_quirks_db_t *db, int idx,
                                 const stressfs_quirk_identity_t *ident,
                                 char *reason, size_t reasonsz);

/* Pre-execution consultation: does an active entry say this test must
 * not be run at all (`action skip`)? Returns 1 and fills `out` if so.
 * The caller must then NOT call entry->run and must report
 * STRESSFS_VERDICT_UNSUPPORTED. */
int stressfs_quirks_check_skip(const stressfs_quirks_db_t *db,
                               const stressfs_quirk_identity_t *ident,
                               const stressfs_test_entry_t *entry,
                               stressfs_quirk_applied_t *out);

/* The quirks-application STAGE (docs/implementation-plan.md M3 step 5:
 * its own step, after stressfs_conform_evaluate() has computed the raw
 * verdict and separate from both formatters). Given the db, the
 * handler identity, the table entry, the raw verdict and the raw
 * {res1,res2} the run produced, returns the FINAL verdict and fills
 * `out` with the quirk that changed it, if any. Pure: touches no
 * global state, so the host tests drive it directly.
 *
 * `out` is always initialised (out->applied == 0 when no quirk fired),
 * so a caller can pass it straight to the formatters unconditionally. */
stressfs_verdict_t stressfs_quirks_apply_verdict(const stressfs_quirks_db_t *db,
                                                 const stressfs_quirk_identity_t *ident,
                                                 const stressfs_test_entry_t *entry,
                                                 stressfs_verdict_t raw,
                                                 stressfs_pkt_result_t got,
                                                 stressfs_quirk_applied_t *out);

const char *stressfs_quirk_status_name(stressfs_quirk_status_t st);
const char *stressfs_quirk_verb_name(stressfs_quirk_verb_t v);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_QUIRKS_H */
