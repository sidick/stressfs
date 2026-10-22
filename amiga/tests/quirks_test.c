/* amiga/tests/quirks_test.c -- M3 host ctest for the stressfs.quirks
 * parser, matcher and application stage (docs/implementation-plan.md
 * M3 exit criterion: "host tests cover the parser including malformed
 * files (refused loudly, never half-applied)").
 *
 * Structure follows the existing conform_test.c/report_test.c
 * conventions exactly: a CHECK macro counting failures, one function
 * per behaviour, main() returning nonzero if anything failed. Fixture
 * files live in amiga/tests/quirks/ and are passed in as
 * STRESSFS_QUIRKS_FIXTURE_DIR; the golden renderings of a
 * quirks-affected report live in amiga/tests/golden/ alongside M2's,
 * passed in as STRESSFS_GOLDEN_DIR; and the real shipped file
 * (data/stressfs.quirks) is parsed too, as STRESSFS_SHIPPED_QUIRKS, so
 * a typo in it fails CI on the host without needing an emulator.
 */

#include "stressfs/conform.h"
#include "stressfs/quirks.h"
#include "stressfs/report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#ifndef STRESSFS_GOLDEN_DIR
#define STRESSFS_GOLDEN_DIR "golden"
#endif
#ifndef STRESSFS_QUIRKS_FIXTURE_DIR
#define STRESSFS_QUIRKS_FIXTURE_DIR "quirks"
#endif
#ifndef STRESSFS_SHIPPED_QUIRKS
#define STRESSFS_SHIPPED_QUIRKS "../../data/stressfs.quirks"
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (f == NULL) {
        fprintf(stderr, "quirks_test: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static char *read_fixture(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STRESSFS_QUIRKS_FIXTURE_DIR, name);
    return read_whole_file(path);
}

/* Golden-file comparison. STRESSFS_REGEN_GOLDEN=1 in the environment
 * rewrites the golden file from what the formatters produce instead of
 * comparing -- how the two M3 goldens in amiga/tests/golden/ were
 * produced in the first place, kept available (and documented here)
 * because the alternative is pasting 2KB of JSON into a file by hand
 * and getting a byte wrong. Regeneration is NEVER the default: an
 * ordinary `ctest` run always compares. */
static void compare_golden(const char *rendered, const char *golden_path,
                            const char *what)
{
    char *golden;
    char msg[256];

    if (getenv("STRESSFS_REGEN_GOLDEN") != NULL) {
        FILE *f = fopen(golden_path, "wb");
        if (f != NULL) {
            fwrite(rendered, 1, strlen(rendered), f);
            fclose(f);
            fprintf(stderr, "quirks_test: REGENERATED %s\n", golden_path);
        } else {
            snprintf(msg, sizeof(msg), "could not rewrite %s", golden_path);
            CHECK(0, msg);
        }
        return;
    }

    golden = read_whole_file(golden_path);
    snprintf(msg, sizeof(msg), "%s must be readable", golden_path);
    CHECK(golden != NULL, msg);
    if (golden == NULL) {
        return;
    }
    snprintf(msg, sizeof(msg), "%s must match its golden file byte-for-byte", what);
    CHECK(strcmp(rendered, golden) == 0, msg);
    if (strcmp(rendered, golden) != 0) {
        fprintf(stderr, "---- rendered ----\n%s---- golden ----\n%s", rendered, golden);
    }
    free(golden);
}

/* Diagnostics sink: concatenates every message so a test can assert
 * both that something WAS said (the "refused loudly" half of the exit
 * criterion) and what it said. */
struct diag_sink {
    char text[4096];
    int count;
};

static void diag_collect(void *ctx, const char *msg)
{
    struct diag_sink *s = (struct diag_sink *)ctx;
    size_t len = strlen(s->text);
    snprintf(s->text + len, sizeof(s->text) - len, "%s\n", msg);
    s->count++;
}

/* The FFS 47.4 identity the Copperline tier actually reports (see
 * data/stressfs.quirks's confirmed entries and
 * ci/copperline-m3.sh). */
static void ident_ffs47(stressfs_quirk_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    stressfs_quirks_identity_from_ver(id, "$VER: fs 47.4 (16.6.2020)");
    id->kick_version = 47;
    id->dos_version = 47;
    id->identified = 1;
}

static stressfs_test_entry_t make_entry(const char *id, const char *name)
{
    stressfs_test_entry_t e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.name = name;
    e.section = "packet-documentation.md#TEST";
    e.oracle = STRESSFS_ORACLE_INDEPENDENT;
    return e;
}

static stressfs_pkt_result_t res(long r1, long r2)
{
    stressfs_pkt_result_t r;
    r.res1 = r1;
    r.res2 = r2;
    return r;
}

/* ------------------------------------------------------------------ */
/* 1. "$VER:" identity parsing                                        */
/* ------------------------------------------------------------------ */

static void test_identity_from_ver(void)
{
    stressfs_quirk_identity_t id;

    memset(&id, 0, sizeof(id));
    CHECK(stressfs_quirks_identity_from_ver(&id, "$VER: fs 47.4 (16.6.2020)") == 1,
          "a real FFS 47.4 $VER: string must parse");
    CHECK(strcmp(id.name, "fs") == 0, "handler short name must be \"fs\"");
    CHECK(id.version == 47, "version must be 47");
    CHECK(id.revision == 4, "revision must be 4");
    CHECK(strcmp(id.idstring, "$VER: fs 47.4 (16.6.2020)") == 0,
          "the full $VER: string must be kept for id~ matching");

    memset(&id, 0, sizeof(id));
    CHECK(stressfs_quirks_identity_from_ver(&id, "FastFileSystem 45.1") == 1,
          "a bare \"name version.revision\" form must parse too");
    CHECK(strcmp(id.name, "FastFileSystem") == 0, "bare form name");
    CHECK(id.version == 45 && id.revision == 1, "bare form version/revision");

    memset(&id, 0, sizeof(id));
    CHECK(stressfs_quirks_identity_from_ver(&id, "") == 0,
          "an empty version string must be reported unparseable");
    CHECK(id.name[0] == '\0' && id.version == 0,
          "an unparseable version string must leave the fields empty, not partial");

    memset(&id, 0, sizeof(id));
    CHECK(stressfs_quirks_identity_from_ver(&id, "$VER: justaname") == 0,
          "a $VER: string with no version number must be reported unparseable");
}

/* ------------------------------------------------------------------ */
/* 2. A well-formed file: parsing, the confidence gate, FORCEQUIRK     */
/* ------------------------------------------------------------------ */

static void test_good_file_and_confidence_gate(void)
{
    stressfs_quirks_db_t db;
    stressfs_quirk_identity_t id;
    struct diag_sink sink;
    char *text = read_fixture("good-minimal.quirks");
    stressfs_test_entry_t demo_test = make_entry("demo-test", "demo");
    stressfs_test_entry_t demo_other = make_entry("demo-other", "demo other");
    stressfs_test_entry_t demo_dangerous = make_entry("demo-dangerous", "demo dangerous");
    stressfs_quirk_applied_t applied;
    char reason[STRESSFS_QUIRK_REASON_LEN];

    CHECK(text != NULL, "good-minimal.quirks must be readable");
    if (text == NULL) {
        return;
    }
    ident_ffs47(&id);

    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);
    CHECK(stressfs_quirks_parse(&db, "good-minimal.quirks", text, diag_collect, &sink) == 1,
          "a well-formed file must parse");
    CHECK(sink.count == 0, "a well-formed file must produce no diagnostics at all");
    CHECK(db.n_entries == 3, "three entries must be parsed");
    CHECK(db.loaded == 1, "db->loaded must be set after a successful parse");

    /* Confirmed entry auto-applies; its expect-deviation pins 7/0. */
    CHECK(stressfs_quirks_entry_active(&db, 0, &id, reason, sizeof(reason)) == 1,
          "the confirmed entry must be active against FFS 47.4");
    CHECK(strstr(reason, "auto (confirmed)") != NULL,
          "the match reason must say the entry auto-applied on confirmed");
    CHECK(strstr(reason, "name=fs") != NULL && strstr(reason, "version>=40") != NULL,
          "the match reason must name the conditions that held");

    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                         res(7, 0), &applied) == STRESSFS_VERDICT_DEVIATION,
          "a fail matching the declared res pair must become a deviation");
    CHECK(applied.applied == 1, "the applied-quirk record must be filled in");
    CHECK(strcmp(applied.id, "demo-confirmed") == 0, "applied.id");
    CHECK(strcmp(applied.confidence, "confirmed") == 0, "applied.confidence");

    /* A failure of a DIFFERENT shape is NOT covered. */
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                         res(8, 0), &applied) == STRESSFS_VERDICT_FAIL,
          "expect-deviation must not cover a failure whose result differs from the "
          "declared pair (this is what stops a quirk hiding a regression)");
    CHECK(applied.applied == 0, "no quirk record when nothing applied");

    /* A pass stays a pass. */
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_PASS,
                                         res(7, 0), &applied) == STRESSFS_VERDICT_PASS,
          "a pass must never be turned into a deviation");

    /* An inconclusive is untouchable. */
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_INCONCLUSIVE,
                                         res(7, 0), &applied) == STRESSFS_VERDICT_INCONCLUSIVE,
          "an inconclusive (hang) must never be softened by a quirk");

    /* The reported entry is inert until forced. */
    CHECK(stressfs_quirks_entry_active(&db, 1, &id, NULL, 0) == 0,
          "a `reported` entry must be inert without FORCEQUIRK");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_other, STRESSFS_VERDICT_FAIL,
                                         res(0, 205), &applied) == STRESSFS_VERDICT_FAIL,
          "a `reported` entry must not change a verdict on its own");

    CHECK(stressfs_quirks_force(&db, "demo-reported", diag_collect, &sink) == 1,
          "FORCEQUIRK must accept an id");
    CHECK(stressfs_quirks_entry_active(&db, 1, &id, reason, sizeof(reason)) == 1,
          "a forced `reported` entry must become active");
    CHECK(strstr(reason, "forced by id") != NULL,
          "the match reason must say the entry was forced");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_other, STRESSFS_VERDICT_FAIL,
                                         res(0, 205), &applied) == STRESSFS_VERDICT_DEVIATION,
          "a forced `reported` downgrade entry must turn a fail into a deviation");
    CHECK(applied.verb == STRESSFS_QUIRK_DOWNGRADE, "the recorded verb must be downgrade");

    /* skip is a pre-execution decision and produces `unsupported`. */
    memset(&applied, 0, sizeof(applied));
    CHECK(stressfs_quirks_check_skip(&db, &id, &demo_dangerous, &applied) == 1,
          "a confirmed `skip` entry must be reported by check_skip");
    CHECK(strcmp(applied.id, "demo-skip") == 0, "the skip entry's id");
    CHECK(stressfs_quirks_check_skip(&db, &id, &demo_test, &applied) == 0,
          "check_skip must not fire for a test no skip action targets");

    /* -K suppresses everything, confirmed included. */
    db.suppressed = 1;
    CHECK(stressfs_quirks_entry_active(&db, 0, &id, NULL, 0) == 0,
          "NOQUIRKS (-K) must deactivate even a confirmed entry");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                         res(7, 0), &applied) == STRESSFS_VERDICT_FAIL,
          "under -K a quirk-covered deviation must show up as the raw fail");
    CHECK(stressfs_quirks_check_skip(&db, &id, &demo_dangerous, &applied) == 0,
          "under -K a skip entry must not keep a test from running");
    db.suppressed = 0;

    /* A different handler identity must not match. */
    {
        stressfs_quirk_identity_t other;
        memset(&other, 0, sizeof(other));
        stressfs_quirks_identity_from_ver(&other, "$VER: pfs3 19.2 (1.1.2018)");
        other.kick_version = 47;
        other.dos_version = 47;
        other.identified = 1;
        CHECK(stressfs_quirks_entry_active(&db, 0, &other, NULL, 0) == 0,
              "name=fs must not match a handler called pfs3");
        CHECK(stressfs_quirks_apply_verdict(&db, &other, &demo_test, STRESSFS_VERDICT_FAIL,
                                             res(7, 0), &applied) == STRESSFS_VERDICT_FAIL,
              "an FFS-only quirk must leave another handler's fail alone");
    }

    /* A version range that excludes this handler must not match. */
    {
        stressfs_quirk_identity_t old;
        ident_ffs47(&old);
        old.version = 39; /* below the entry's version>=40 */
        CHECK(stressfs_quirks_entry_active(&db, 0, &old, NULL, 0) == 0,
              "version>=40 must not match version 39");
    }

    /* Kickstart-version matching (the third condition type the plan
     * names) is exercised by the skip entry's kick>=37. */
    {
        stressfs_quirk_identity_t oldkick;
        ident_ffs47(&oldkick);
        oldkick.kick_version = 34; /* Kickstart 1.3 */
        CHECK(stressfs_quirks_entry_active(&db, 2, &oldkick, NULL, 0) == 0,
              "kick>=37 must not match a Kickstart 1.3 machine");
    }

    free(text);
}

/* ------------------------------------------------------------------ */
/* 3. Malformed files: refused loudly, never half-applied             */
/* ------------------------------------------------------------------ */

static void check_refused(const char *fixture, const char *expect_substr)
{
    stressfs_quirks_db_t db;
    struct diag_sink sink;
    char *text = read_fixture(fixture);
    /* Large enough to hold the full diag_sink.text (char[4096], see
     * above) embedded in the "should mention" message below without
     * GCC's -Wformat-truncation= flagging a possible truncation under
     * -Werror (reproduces on CI's gcc; not every local toolchain warns
     * here, which is why this didn't surface until a real CI run). */
    char msg[4096 + 256];

    if (text == NULL) {
        snprintf(msg, sizeof(msg), "%s must be readable", fixture);
        CHECK(0, msg);
        return;
    }

    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);

    snprintf(msg, sizeof(msg), "%s must be REFUSED (parse returns 0)", fixture);
    CHECK(stressfs_quirks_parse(&db, fixture, text, diag_collect, &sink) == 0, msg);

    snprintf(msg, sizeof(msg), "%s: nothing may be left applied after a refusal", fixture);
    CHECK(db.n_entries == 0 && db.loaded == 0, msg);

    snprintf(msg, sizeof(msg), "%s must be refused LOUDLY (at least one diagnostic)", fixture);
    CHECK(sink.count >= 1, msg);

    snprintf(msg, sizeof(msg), "%s: the diagnostic must say the whole file was refused", fixture);
    CHECK(strstr(sink.text, "REFUSING THE WHOLE FILE") != NULL, msg);

    if (expect_substr != NULL) {
        snprintf(msg, sizeof(msg), "%s: diagnostic should mention \"%s\"; got: %s",
                 fixture, expect_substr, sink.text);
        CHECK(strstr(sink.text, expect_substr) != NULL, msg);
    }

    free(text);
}

static void test_malformed_files_are_refused(void)
{
    /* The first two fixtures put a VALID entry before the broken line
     * on purpose: that is what makes "never half-applied" a real
     * assertion rather than a tautology about an empty file. */
    check_refused("bad-unknown-keyword.quirks", "unknown keyword");
    check_refused("bad-match-no-op.quirks", "malformed match token");
    check_refused("bad-unknown-verb.quirks", "unknown action verb");
    check_refused("bad-expect-no-pair.quirks", "res1>/<res2");
    check_refused("bad-unknown-status.quirks", "unknown status");
    check_refused("bad-no-status.quirks", "no 'status' line");
    check_refused("bad-orphan-match.quirks", "before any 'quirk'");
    check_refused("bad-dup-id.quirks", "duplicate quirk id");
    check_refused("bad-nonnumeric-version.quirks", "decimal number");

    /* Refusal must also be total in the sense that a db which HAD a
     * good file loaded and is then handed a bad one ends up empty,
     * rather than keeping the old contents and silently mixing the
     * two notions of "what is applied". */
    {
        stressfs_quirks_db_t db;
        struct diag_sink sink;
        char *good = read_fixture("good-minimal.quirks");
        char *bad = read_fixture("bad-unknown-verb.quirks");

        if (good != NULL && bad != NULL) {
            memset(&sink, 0, sizeof(sink));
            stressfs_quirks_init(&db);
            CHECK(stressfs_quirks_parse(&db, "good", good, diag_collect, &sink) == 1,
                  "the good file must load first");
            CHECK(db.n_entries == 3, "three entries loaded");
            CHECK(stressfs_quirks_parse(&db, "bad", bad, diag_collect, &sink) == 0,
                  "the bad file must be refused");
            CHECK(db.n_entries == 0 && db.loaded == 0,
                  "a refusal must leave the db empty even if it previously held a good file");
        }
        free(good);
        free(bad);
    }

    /* Inline malformed cases that do not warrant their own fixture
     * file -- all of them structural, all of them fatal. */
    {
        static const struct {
            const char *text;
            const char *what;
        } cases[] = {
            { "quirk\n", "a bare `quirk` with no id" },
            { "quirk  x\nmatch  \nbecause y\naction downgrade t\nstatus confirmed\n",
              "an empty match line" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction\nstatus confirmed\n",
              "an empty action line" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction downgrade\nstatus confirmed\n",
              "an action with no target" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction downgrade t extra\nstatus confirmed\n",
              "an action with an extra argument" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction expect-deviation t 1-2\nstatus confirmed\n",
              "a res pair with no slash" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction expect-deviation t x/0\nstatus confirmed\n",
              "a non-numeric dp_Res1" },
            { "quirk  x\nmatch  name=fs\nbecause y\naction expect-deviation t 0/x\nstatus confirmed\n",
              "a non-numeric dp_Res2" },
            { "quirk  x\nmatch  name<40\nbecause y\naction downgrade t\nstatus confirmed\n",
              "a string key with a numeric operator" },
            { "quirk  x\nmatch  version~40\nbecause y\naction downgrade t\nstatus confirmed\n",
              "a numeric key with the substring operator" },
            { "quirk  x\nmatch  id=fs\nbecause y\naction downgrade t\nstatus confirmed\n",
              "id with '=' instead of '~'" },
            { "quirk  x\nmatch  name=\nbecause y\naction downgrade t\nstatus confirmed\n",
              "a match token with no value" },
            { "quirk  x\nbecause y\naction downgrade t\nstatus confirmed\n",
              "an entry with no match line" },
            { "quirk  x\nmatch name=fs\nbecause y\nstatus confirmed\n",
              "an entry with no action line" },
            { "quirk  x\nmatch name=fs\nbecause\naction downgrade t\nstatus confirmed\n",
              "an empty because line" },
            { "quirk  x\nmatch name=fs\nbecause y\naction downgrade *\nstatus confirmed\n",
              "a bare '*' target that would match every test" },
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            stressfs_quirks_db_t db;
            struct diag_sink sink;
            char msg[200];

            memset(&sink, 0, sizeof(sink));
            stressfs_quirks_init(&db);
            snprintf(msg, sizeof(msg), "%s must refuse the file", cases[i].what);
            CHECK(stressfs_quirks_parse(&db, "inline", cases[i].text,
                                         diag_collect, &sink) == 0, msg);
            snprintf(msg, sizeof(msg), "%s must leave nothing applied", cases[i].what);
            CHECK(db.n_entries == 0 && db.loaded == 0, msg);
            snprintf(msg, sizeof(msg), "%s must be diagnosed", cases[i].what);
            CHECK(sink.count >= 1, msg);
        }
    }

    /* Over-long ids/targets are refused rather than truncated into
     * something that matches the wrong test. */
    {
        stressfs_quirks_db_t db;
        struct diag_sink sink;
        char text[512];
        char longid[STRESSFS_QUIRK_ID_LEN + 20];
        size_t i;

        for (i = 0; i < sizeof(longid) - 1; i++) {
            longid[i] = 'a';
        }
        longid[sizeof(longid) - 1] = '\0';
        snprintf(text, sizeof(text),
                 "quirk %s\nmatch name=fs\nbecause y\naction downgrade t\nstatus confirmed\n",
                 longid);

        memset(&sink, 0, sizeof(sink));
        stressfs_quirks_init(&db);
        CHECK(stressfs_quirks_parse(&db, "inline", text, diag_collect, &sink) == 0,
              "an over-long quirk id must refuse the file, not be truncated");
        CHECK(db.n_entries == 0, "nothing applied after an over-long id");
    }

    /* Too many entries is also a refusal, not a silent truncation --
     * "anything past the limit" must never be quietly dropped the way
     * devsoak's parser drops it. */
    {
        stressfs_quirks_db_t db;
        struct diag_sink sink;
        char text[8192];
        size_t off = 0;
        int i;

        text[0] = '\0';
        for (i = 0; i < STRESSFS_QUIRK_MAX_ENTRIES + 2; i++) {
            off += (size_t)snprintf(text + off, sizeof(text) - off,
                                    "quirk e%d\nmatch name=fs\nbecause y\n"
                                    "action downgrade t%d\nstatus confirmed\n\n", i, i);
        }
        memset(&sink, 0, sizeof(sink));
        stressfs_quirks_init(&db);
        CHECK(stressfs_quirks_parse(&db, "inline", text, diag_collect, &sink) == 0,
              "more entries than the build's limit must refuse the file");
        CHECK(db.n_entries == 0, "nothing applied after an entry-table overflow");
    }
}

/* ------------------------------------------------------------------ */
/* 4. Well-formed but unevaluable match key: warn and continue         */
/* ------------------------------------------------------------------ */

static void test_unevaluable_key_warns_and_continues(void)
{
    stressfs_quirks_db_t db;
    stressfs_quirk_identity_t id;
    struct diag_sink sink;
    char *text = read_fixture("unevaluable-key.quirks");
    stressfs_test_entry_t a3000 = make_entry("a3000-only-test", "a3000 only");
    stressfs_test_entry_t ordinary = make_entry("ordinary-test", "ordinary");
    stressfs_quirk_applied_t applied;
    char reason[STRESSFS_QUIRK_REASON_LEN];

    CHECK(text != NULL, "unevaluable-key.quirks must be readable");
    if (text == NULL) {
        return;
    }
    ident_ffs47(&id);

    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);

    /* The whole point: this is NOT a malformed file. */
    CHECK(stressfs_quirks_parse(&db, "unevaluable-key.quirks", text,
                                 diag_collect, &sink) == 1,
          "a well-formed file with an unevaluable match key must still LOAD");
    CHECK(db.loaded == 1, "db->loaded must be set");
    CHECK(db.n_entries == 2, "both entries must be parsed");
    CHECK(sink.count == 1, "exactly one warning must be emitted (once per entry)");
    CHECK(strstr(sink.text, "cannot be evaluated") != NULL,
          "the warning must say the key cannot be evaluated");
    CHECK(strstr(sink.text, "FORCEQUIRK=needs-a3000") != NULL,
          "the warning must tell the user how to force the entry");
    CHECK(strstr(sink.text, "REFUSING") == NULL,
          "an unevaluable key must NOT refuse the file");

    /* Force-only, even though its status is `confirmed`. */
    CHECK(db.entries[0].unevaluable == 1, "the entry must be flagged unevaluable");
    CHECK(stressfs_quirks_entry_active(&db, 0, &id, NULL, 0) == 0,
          "an unevaluable entry must never auto-apply, confirmed or not");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &a3000, STRESSFS_VERDICT_FAIL,
                                         res(0, 1), &applied) == STRESSFS_VERDICT_FAIL,
          "an unevaluable entry must not change a verdict on its own");

    /* The rest of the file applies normally -- "parsing continues". */
    CHECK(stressfs_quirks_entry_active(&db, 1, &id, NULL, 0) == 1,
          "the ordinary entry in the same file must still be active");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &ordinary, STRESSFS_VERDICT_FAIL,
                                         res(0, 1), &applied) == STRESSFS_VERDICT_DEVIATION,
          "the ordinary entry in the same file must still apply");

    /* FORCEQUIRK waives the unevaluable key (and only it). */
    CHECK(stressfs_quirks_force(&db, "needs-a3000", diag_collect, &sink) == 1, "force ok");
    CHECK(stressfs_quirks_entry_active(&db, 0, &id, reason, sizeof(reason)) == 1,
          "FORCEQUIRK must make an unevaluable entry applicable");
    CHECK(strstr(reason, "unevaluable, waived by FORCEQUIRK") != NULL,
          "the match reason must say which condition was waived");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &a3000, STRESSFS_VERDICT_FAIL,
                                         res(0, 1), &applied) == STRESSFS_VERDICT_DEVIATION,
          "a forced unevaluable entry must apply");

    /* ... but the entry's OTHER, evaluable condition (name=fs) is
     * still enforced even when forced. */
    {
        stressfs_quirk_identity_t other;
        memset(&other, 0, sizeof(other));
        stressfs_quirks_identity_from_ver(&other, "$VER: pfs3 19.2");
        other.kick_version = 47;
        other.dos_version = 47;
        CHECK(stressfs_quirks_entry_active(&db, 0, &other, NULL, 0) == 0,
              "FORCEQUIRK waives only the unevaluable key, not the evaluable ones");
    }

    free(text);
}

/* ------------------------------------------------------------------ */
/* 5. FORCEQUIRK list parsing                                          */
/* ------------------------------------------------------------------ */

static void test_force_list(void)
{
    stressfs_quirks_db_t db;
    struct diag_sink sink;

    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);
    CHECK(stressfs_quirks_force(&db, "a,b , c", diag_collect, &sink) == 1,
          "a comma-separated FORCEQUIRK list must parse");
    CHECK(db.n_forced == 3, "three forced ids");
    CHECK(strcmp(db.forced[0], "a") == 0 && strcmp(db.forced[1], "b") == 0 &&
          strcmp(db.forced[2], "c") == 0, "forced ids must be split and trimmed");

    /* Repeated calls accumulate, which is what makes the CLI option
     * effectively repeatable. */
    CHECK(stressfs_quirks_force(&db, "d", diag_collect, &sink) == 1, "a second call must add");
    CHECK(db.n_forced == 4, "forced ids accumulate across calls");

    /* Forced ids survive a file refusal: they came from the command
     * line, not the file. */
    {
        char *bad = read_fixture("bad-unknown-verb.quirks");
        if (bad != NULL) {
            CHECK(stressfs_quirks_parse(&db, "bad", bad, diag_collect, &sink) == 0,
                  "the bad file must be refused");
            CHECK(db.n_forced == 4, "FORCEQUIRK ids must survive a file refusal");
            free(bad);
        }
    }

    CHECK(stressfs_quirks_force(&db, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                 diag_collect, &sink) == 0,
          "an over-long forced id must be rejected, not truncated");
}

/* ------------------------------------------------------------------ */
/* 6. The shipped data/stressfs.quirks file must parse                */
/* ------------------------------------------------------------------ */

static void test_shipped_file_parses(void)
{
    stressfs_quirks_db_t db;
    stressfs_quirk_identity_t id;
    struct diag_sink sink;
    char *text = read_whole_file(STRESSFS_SHIPPED_QUIRKS);
    stressfs_quirk_applied_t applied;
    stressfs_test_entry_t isfs = make_entry("baseline-is-filesystem", "baseline: ACTION_IS_FILESYSTEM");
    stressfs_test_entry_t excl = make_entry("locate-exclusive-root", "exclusive root lock");
    stressfs_test_entry_t sfs_grow = make_entry("set-file-size-grow", "set file size grow");

    CHECK(text != NULL, "data/stressfs.quirks must be readable from the test");
    if (text == NULL) {
        return;
    }

    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);
    CHECK(stressfs_quirks_parse(&db, "data/stressfs.quirks", text,
                                 diag_collect, &sink) == 1,
          "the SHIPPED quirks file must parse (a typo in it fails here, on the host, "
          "without needing an emulator)");
    CHECK(db.n_entries > 0, "the shipped file must contain entries");

    /* It deliberately contains one entry with an unevaluable key, so
     * exactly one warning is expected -- no more, no fewer. */
    CHECK(sink.count == 1,
          "the shipped file must produce exactly one load-time warning (its one "
          "deliberately-unmatchable PFS3 placeholder entry)");

    /* The confirmed FFS 47.4 entries must actually fire against the
     * identity the Copperline tier reports -- this is the host-side
     * mirror of ci/copperline-m3.sh's live proof. */
    ident_ffs47(&id);
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &isfs, STRESSFS_VERDICT_FAIL,
                                         res(-1, 1027), &applied) == STRESSFS_VERDICT_DEVIATION,
          "the shipped ffs-is-filesystem-res2-garbage entry must cover the observed "
          "dp_Res1=-1/dp_Res2=1027 result");
    CHECK(strcmp(applied.id, "ffs-is-filesystem-res2-garbage") == 0,
          "the right shipped entry must be the one that fired");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &excl, STRESSFS_VERDICT_FAIL,
                                         res(77210, 0), &applied) == STRESSFS_VERDICT_DEVIATION,
          "the shipped ffs-exclusive-root-lock-granted entry must cover a granted lock");
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &sfs_grow, STRESSFS_VERDICT_FAIL,
                                         res(20, 0), &applied) == STRESSFS_VERDICT_DEVIATION,
          "the shipped ffs-set-file-size-returns-size entry must cover a size return");

    /* ... and must NOT fire for a real error return on the same test. */
    CHECK(stressfs_quirks_apply_verdict(&db, &id, &sfs_grow, STRESSFS_VERDICT_FAIL,
                                         res(0, 226), &applied) == STRESSFS_VERDICT_FAIL,
          "the set-file-size entry must not cover a genuine error return");

    /* The FFS<=V40 flush entry is `reported`: inert by default. */
    {
        stressfs_test_entry_t flush = make_entry("flush-durability-acknowledges-early",
                                                  "flush durability");
        stressfs_quirk_identity_t v40;
        ident_ffs47(&v40);
        v40.version = 40;
        v40.revision = 1;
        CHECK(stressfs_quirks_apply_verdict(&db, &v40, &flush, STRESSFS_VERDICT_FAIL,
                                             res(0, 0), &applied) == STRESSFS_VERDICT_FAIL,
              "the proposal-sourced FFS<=V40 flush entry must be inert by default");
        CHECK(stressfs_quirks_force(&db, "ffs-v40-flush-acks-early", diag_collect, &sink) == 1,
              "force the flush entry");
        CHECK(stressfs_quirks_apply_verdict(&db, &v40, &flush, STRESSFS_VERDICT_FAIL,
                                             res(0, 0), &applied) == STRESSFS_VERDICT_DEVIATION,
              "forced, the flush entry must apply to flush-durability-* via its wildcard");
        CHECK(stressfs_quirks_apply_verdict(&db, &id, &flush, STRESSFS_VERDICT_FAIL,
                                             res(0, 0), &applied) == STRESSFS_VERDICT_FAIL,
              "even forced, version<=40 must not match FFS 47.4");
    }

    free(text);
}

/* ------------------------------------------------------------------ */
/* 7. Golden-file renderings of a quirks-affected report               */
/* ------------------------------------------------------------------ */

static void test_quirked_report_matches_golden(void)
{
    stressfs_quirks_db_t db;
    stressfs_quirk_identity_t id;
    struct diag_sink sink;
    char *text = read_fixture("good-minimal.quirks");
    char rendered[4096];
    char line[1024];
    size_t off = 0;
    stressfs_test_entry_t demo_test = make_entry("demo-test", "demo test (expect-deviation)");
    stressfs_test_entry_t demo_other = make_entry("demo-other", "demo other (downgrade)");
    stressfs_test_entry_t demo_dangerous = make_entry("demo-dangerous", "demo dangerous (skip)");
    stressfs_quirk_applied_t applied;
    stressfs_verdict_t v;

    CHECK(text != NULL, "good-minimal.quirks must be readable");
    if (text == NULL) {
        return;
    }
    memset(&sink, 0, sizeof(sink));
    stressfs_quirks_init(&db);
    stressfs_quirks_parse(&db, "data/stressfs.quirks", text, diag_collect, &sink);
    stressfs_quirks_force(&db, "demo-reported", NULL, NULL);
    ident_ffs47(&id);

    /* --- human-readable --- */
    rendered[0] = '\0';
    stressfs_report_format_quirks_human(line, sizeof(line), &db, &id);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    v = stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                       res(7, 0), &applied);
    stressfs_conform_format_line(line, sizeof(line), &demo_test, v, res(7, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    v = stressfs_quirks_apply_verdict(&db, &id, &demo_other, STRESSFS_VERDICT_FAIL,
                                       res(0, 205), &applied);
    stressfs_conform_format_line(line, sizeof(line), &demo_other, v, res(0, 205), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    stressfs_quirks_check_skip(&db, &id, &demo_dangerous, &applied);
    stressfs_conform_format_line(line, sizeof(line), &demo_dangerous,
                                  STRESSFS_VERDICT_UNSUPPORTED, res(0, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    /* ... and the same three lines with -K, which is the exit
     * criterion's "with and without -K demonstrably changes verdicts"
     * made into a byte-exact fixture. */
    db.suppressed = 1;
    stressfs_report_format_quirks_human(line, sizeof(line), &db, &id);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    v = stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                       res(7, 0), &applied);
    stressfs_conform_format_line(line, sizeof(line), &demo_test, v, res(7, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    db.suppressed = 0;

    compare_golden(rendered, STRESSFS_GOLDEN_DIR "/report-quirks-human.txt",
                    "the quirks-affected human-readable report");

    /* --- JSON-lines --- */
    off = 0;
    rendered[0] = '\0';
    stressfs_report_format_quirks_json(line, sizeof(line), &db, &id);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    v = stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                       res(7, 0), &applied);
    stressfs_conform_format_json(line, sizeof(line), &demo_test, v, res(7, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    v = stressfs_quirks_apply_verdict(&db, &id, &demo_other, STRESSFS_VERDICT_FAIL,
                                       res(0, 205), &applied);
    stressfs_conform_format_json(line, sizeof(line), &demo_other, v, res(0, 205), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    stressfs_quirks_check_skip(&db, &id, &demo_dangerous, &applied);
    stressfs_conform_format_json(line, sizeof(line), &demo_dangerous,
                                  STRESSFS_VERDICT_UNSUPPORTED, res(0, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    db.suppressed = 1;
    stressfs_report_format_quirks_json(line, sizeof(line), &db, &id);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    v = stressfs_quirks_apply_verdict(&db, &id, &demo_test, STRESSFS_VERDICT_FAIL,
                                       res(7, 0), &applied);
    stressfs_conform_format_json(line, sizeof(line), &demo_test, v, res(7, 0), &applied);
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    db.suppressed = 0;

    compare_golden(rendered, STRESSFS_GOLDEN_DIR "/report-quirks-json.jsonl",
                    "the quirks-affected JSON-lines report");

    free(text);
}

/* ------------------------------------------------------------------ */
/* 8. Defensive: NULL/empty inputs                                     */
/* ------------------------------------------------------------------ */

static void test_defensive(void)
{
    stressfs_quirks_db_t db;
    stressfs_quirk_identity_t id;
    stressfs_quirk_applied_t applied;
    stressfs_test_entry_t e = make_entry("t", "t");
    char buf[256];

    stressfs_quirks_init(&db);
    ident_ffs47(&id);

    CHECK(stressfs_quirks_parse(&db, "p", "", NULL, NULL) == 1,
          "an empty file is well-formed (zero entries), not malformed");
    CHECK(db.n_entries == 0, "an empty file yields no entries");
    CHECK(stressfs_quirks_parse(&db, "p", "# only a comment\n\n", NULL, NULL) == 1,
          "a comments-only file is well-formed");

    CHECK(stressfs_quirks_parse(NULL, "p", "x", NULL, NULL) == 0, "NULL db is rejected");
    CHECK(stressfs_quirks_parse(&db, "p", NULL, NULL, NULL) == 0, "NULL text is rejected");

    CHECK(stressfs_quirks_apply_verdict(NULL, &id, &e, STRESSFS_VERDICT_FAIL,
                                         res(0, 0), &applied) == STRESSFS_VERDICT_FAIL,
          "a NULL db must pass the raw verdict through unchanged");
    CHECK(applied.applied == 0, "and must still initialise the applied record");

    CHECK(stressfs_quirks_entry_active(&db, -1, &id, NULL, 0) == 0, "negative index");
    CHECK(stressfs_quirks_entry_active(&db, 999, &id, NULL, 0) == 0, "out-of-range index");

    stressfs_report_format_quirks_human(buf, sizeof(buf), NULL, &id);
    CHECK(strstr(buf, "none") != NULL,
          "the human summary must say so explicitly when there is no database");
    stressfs_report_format_quirks_json(buf, sizeof(buf), NULL, &id);
    CHECK(strstr(buf, "\"active_count\":0") != NULL,
          "the JSON summary must still be a complete object with no database");

    /* Tiny buffers must truncate, never overflow (ASan is watching). */
    {
        char tiny[8];
        stressfs_report_format_quirks_human(tiny, sizeof(tiny), &db, &id);
        stressfs_report_format_quirks_json(tiny, sizeof(tiny), &db, &id);
        stressfs_conform_format_line(tiny, sizeof(tiny), &e, STRESSFS_VERDICT_FAIL,
                                      res(0, 0), &applied);
        stressfs_conform_format_json(tiny, sizeof(tiny), &e, STRESSFS_VERDICT_FAIL,
                                      res(0, 0), &applied);
        CHECK(strlen(tiny) < sizeof(tiny), "every formatter must stay inside its buffer");
    }
}

int main(void)
{
    test_identity_from_ver();
    test_good_file_and_confidence_gate();
    test_malformed_files_are_refused();
    test_unevaluable_key_warns_and_continues();
    test_force_list();
    test_shipped_file_parses();
    test_quirked_report_matches_golden();
    test_defensive();

    if (failures != 0) {
        fprintf(stderr, "quirks_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("quirks_test: all checks passed\n");
    return 0;
}
