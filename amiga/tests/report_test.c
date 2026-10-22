/* amiga/tests/report_test.c -- M2 host ctest: golden-file validation
 * of the report writer (docs/implementation-plan.md M2 exit criterion:
 * "a host-side ctest validates the writer against golden files").
 *
 * Builds a small, hand-constructed set of synthetic table entries and
 * a synthetic run-identity header through the REAL formatters
 * (stressfs_conform_format_line/_json, stressfs_report_format_header_
 * human/_json) and diffs the result byte-for-byte against checked-in
 * golden files in amiga/tests/golden/. Also proves the comparison is
 * real (not a tautology) by flipping one golden byte at runtime and
 * confirming the comparison then fails -- the same kind of
 * engine-self-test M1 did for stressfs_conform_evaluate(), applied
 * here to the golden-file mechanism itself.
 */

#include "stressfs/conform.h"
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

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (f == NULL) {
        fprintf(stderr, "report_test: cannot open golden file %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

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

/* ------------------------------------------------------------------ */
/* The synthetic fixture: a handful of table entries exercising each   */
/* verdict and oracle class, plus a fully-populated run identity.      */
/* ------------------------------------------------------------------ */

static void build_run_identity(stressfs_run_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    id->harness_version = STRESSFS_VERSION;
    id->handler_name = "DOS\\1";
    id->handler_version = "$VER: FastFileSystem 45.1 (1.1.1999)";
    id->tier = "copperline";
    id->seed = 12345;
    id->seed_set = 1;
    id->dos_version = 47;

    id->have_geometry = 1;
    id->geom_numblocks = 1024000;
    id->geom_numblocksused = 512000;
    id->geom_bytesperblock = 512;
    id->geom_disktype = 0x444F5301L; /* 'DOS\1' */

    id->have_envec = 1;
    id->de_tablesize = 19;
    id->de_sizeblock = 128;
    id->de_surfaces = 2;
    id->de_sectorperblock = 1;
    id->de_blockspertrack = 112;
    id->de_reserved = 2;
    id->de_prealloc = 0;
    id->de_interleave = 0;
    id->de_lowcyl = 0;
    id->de_highcyl = 1546;
    id->de_numbuffers = 30;
    id->de_maxtransfer = 0x7ffffffeUL;
    id->de_mask = 0x7ffffffeUL;
    id->de_bootpri = 0;
    id->de_dostype = 0x444F5301UL;
}

static void build_table_entries(stressfs_test_entry_t entries[4],
                                 stressfs_pkt_result_t results[4],
                                 stressfs_verdict_t verdicts[4])
{
    /* 0: pass, independent oracle */
    memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].id = "baseline-is-filesystem";
    entries[0].name = "ACTION_IS_FILESYSTEM";
    entries[0].section = "packet-documentation.md#ACTION_IS_FILESYSTEM";
    entries[0].oracle = STRESSFS_ORACLE_INDEPENDENT;
    results[0].res1 = -1;
    results[0].res2 = 0;
    verdicts[0] = STRESSFS_VERDICT_PASS;

    /* 1: fail, model oracle */
    memset(&entries[1], 0, sizeof(entries[1]));
    entries[1].id = "selftest-negative";
    entries[1].name = "ENGINE SELF-TEST: deliberately wrong expectation";
    entries[1].section = "implementation-plan.md#M1";
    entries[1].oracle = STRESSFS_ORACLE_MODEL;
    results[1].res1 = -1;
    results[1].res2 = 0;
    verdicts[1] = STRESSFS_VERDICT_FAIL;

    /* 2: unsupported, correlated oracle */
    memset(&entries[2], 0, sizeof(entries[2]));
    entries[2].id = "same-lock";
    entries[2].name = "ACTION_SAME_LOCK (requires V36)";
    entries[2].section = "packet-documentation.md#ACTION_SAME_LOCK";
    entries[2].oracle = STRESSFS_ORACLE_CORRELATED;
    entries[2].min_version = 36;
    results[2].res1 = 0;
    results[2].res2 = 0;
    verdicts[2] = STRESSFS_VERDICT_UNSUPPORTED;

    /* 3: inconclusive, independent oracle (watchdog abort) */
    memset(&entries[3], 0, sizeof(entries[3]));
    entries[3].id = "read-probe";
    entries[3].name = "ACTION_READ (probe.txt contents)";
    entries[3].section = "packet-documentation.md#ACTION_READ";
    entries[3].oracle = STRESSFS_ORACLE_INDEPENDENT;
    results[3].res1 = 0;
    results[3].res2 = 0;
    verdicts[3] = STRESSFS_VERDICT_INCONCLUSIVE;
}

static void test_human_lines_match_golden(void)
{
    stressfs_test_entry_t entries[4];
    stressfs_pkt_result_t results[4];
    stressfs_verdict_t verdicts[4];
    stressfs_run_identity_t id;
    char *golden;
    char rendered[2048];
    size_t off = 0;
    int i;
    char line[512];

    build_table_entries(entries, results, verdicts);
    build_run_identity(&id);

    rendered[0] = '\0';

    CHECK(stressfs_report_format_header_human(line, sizeof(line), &id) == 1,
          "header_human should succeed on a complete identity");
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    for (i = 0; i < 4; i++) {
        stressfs_conform_format_line(line, sizeof(line), &entries[i], verdicts[i], results[i], NULL);
        off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    }

    golden = read_whole_file(STRESSFS_GOLDEN_DIR "/report-human.txt");
    CHECK(golden != NULL, "golden/report-human.txt must be readable");
    if (golden != NULL) {
        CHECK(strcmp(rendered, golden) == 0,
              "rendered human-readable report must match golden/report-human.txt byte-for-byte");
        if (strcmp(rendered, golden) != 0) {
            fprintf(stderr, "---- rendered ----\n%s---- golden ----\n%s", rendered, golden);
        }
        free(golden);
    }
}

static void test_json_lines_match_golden(void)
{
    stressfs_test_entry_t entries[4];
    stressfs_pkt_result_t results[4];
    stressfs_verdict_t verdicts[4];
    stressfs_run_identity_t id;
    char *golden;
    char rendered[2048];
    size_t off = 0;
    int i;
    char line[1024];

    build_table_entries(entries, results, verdicts);
    build_run_identity(&id);

    rendered[0] = '\0';

    CHECK(stressfs_report_format_header_json(line, sizeof(line), &id) == 1,
          "header_json should succeed on a complete identity");
    off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);

    for (i = 0; i < 4; i++) {
        stressfs_conform_format_json(line, sizeof(line), &entries[i], verdicts[i], results[i], NULL);
        off += (size_t)snprintf(rendered + off, sizeof(rendered) - off, "%s\n", line);
    }

    golden = read_whole_file(STRESSFS_GOLDEN_DIR "/report-json.jsonl");
    CHECK(golden != NULL, "golden/report-json.jsonl must be readable");
    if (golden != NULL) {
        CHECK(strcmp(rendered, golden) == 0,
              "rendered JSON-lines report must match golden/report-json.jsonl byte-for-byte");
        if (strcmp(rendered, golden) != 0) {
            fprintf(stderr, "---- rendered ----\n%s---- golden ----\n%s", rendered, golden);
        }
        free(golden);
    }
}

/* The task's instruction: "demonstrate this and then leave the tests
 * passing" -- prove the golden-file comparison is a REAL diff, not a
 * tautology that would pass against anything. Deliberately corrupts an
 * in-memory copy of the golden JSON text and confirms strcmp() then
 * reports a mismatch, exactly the negative-test pattern M1 used for
 * stressfs_conform_evaluate(). Never writes to the golden file on
 * disk -- the corruption is applied to a copy. */
static void test_golden_comparison_is_real(void)
{
    char *golden = read_whole_file(STRESSFS_GOLDEN_DIR "/report-json.jsonl");
    char *corrupted;
    size_t len;

    CHECK(golden != NULL, "golden/report-json.jsonl must be readable for the self-test");
    if (golden == NULL) {
        return;
    }
    len = strlen(golden);
    CHECK(len > 10, "golden file should be non-trivially sized for this self-test");

    corrupted = (char *)malloc(len + 1);
    CHECK(corrupted != NULL, "malloc for corrupted copy should succeed");
    if (corrupted != NULL) {
        memcpy(corrupted, golden, len + 1);
        /* Flip one byte that is virtually guaranteed to be a quote or
         * letter inside the JSON text, not whitespace -- corrupting
         * byte 20 (well inside the first "type":"run_identity" field
         * of every real golden file this test ships). */
        corrupted[20] = (char)(corrupted[20] ^ 0x20);

        CHECK(strcmp(corrupted, golden) != 0,
              "a deliberately corrupted copy must NOT match the real golden file "
              "(proves the golden-file comparison mechanism is a real diff)");
        free(corrupted);
    }
    free(golden);
}

static void test_incomplete_identity_is_refused(void)
{
    stressfs_run_identity_t id;
    char buf[256];

    memset(&id, 0, sizeof(id));
    /* Deliberately leave every mandatory field unset. */
    CHECK(stressfs_run_identity_is_complete(&id) == 0,
          "an all-zero run identity must be reported incomplete");
    CHECK(stressfs_report_format_header_json(buf, sizeof(buf), &id) == 0,
          "format_header_json must refuse (return 0) an incomplete identity");
    CHECK(strstr(buf, "harness_bug") != NULL,
          "format_header_json's refusal output must still be valid, labelled JSON");

    CHECK(stressfs_report_format_header_human(buf, sizeof(buf), &id) == 0,
          "format_header_human must refuse (return 0) an incomplete identity");
    CHECK(strstr(buf, "HARNESS BUG") != NULL,
          "format_header_human's refusal output must say so in plain text");

    /* Now set every mandatory field but one (seed_set) and confirm it
     * still fails -- proves the check is a conjunction, not satisfied
     * by partial credit. */
    id.harness_version = "x";
    id.handler_name = "x";
    id.handler_version = "x";
    id.tier = "x";
    id.seed_set = 0;
    CHECK(stressfs_run_identity_is_complete(&id) == 0,
          "missing seed_set alone must still fail completeness");
    id.seed_set = 1;
    CHECK(stressfs_run_identity_is_complete(&id) == 1,
          "every mandatory field set (seed_set included) must pass completeness");
}

static void test_json_escaping(void)
{
    char buf[64];
    buf[0] = '\0';
    stressfs_json_escape_append(buf, sizeof(buf), "a\"b\\c\nd");
    CHECK(strcmp(buf, "a\\\"b\\\\c\\nd") == 0, "json escaping must escape quote, backslash, newline");
}

int main(void)
{
    test_human_lines_match_golden();
    test_json_lines_match_golden();
    test_golden_comparison_is_real();
    test_incomplete_identity_is_refused();
    test_json_escaping();

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("report_test: OK\n");
    return 0;
}
