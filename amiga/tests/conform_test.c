/* amiga/tests/conform_test.c -- host unit test for the portable
 * conform engine (amiga/lib/conform.c). Exercises verdict computation
 * and line formatting in isolation from any Amiga API, including the
 * two behaviours M1's exit criteria call out explicitly:
 *
 *   - a deliberately-wrong expectation against a real/plausible packet
 *     reply produces `fail`, not `pass` -- the engine's own negative
 *     test, proven here at the unit level (amiga/src/conform.c's table
 *     carries the Amiga-side twin of this, a real DoPkt() call
 *     checked against a wrong expectation);
 *   - version gating: a test entry whose min_version exceeds the
 *     handler's reports `unsupported` without the predicate/exact
 *     match logic ever running.
 */

#include "stressfs/conform.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_exact_match_pass(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got;
    memset(&e, 0, sizeof(e));
    e.name = "t";
    e.section = "s";
    e.expect_res1 = -1; /* DOSTRUE */
    e.expect_res2 = 0;
    got.res1 = -1;
    got.res2 = 0;
    CHECK(stressfs_conform_evaluate(&e, got, 37) == STRESSFS_VERDICT_PASS,
          "exact match should pass");
}

static void test_exact_match_fail(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got;
    memset(&e, 0, sizeof(e));
    e.name = "engine self-test: deliberately wrong expectation";
    e.section = "s";
    e.expect_res1 = 0; /* DOSFALSE -- deliberately wrong: the handler
                        * actually returned DOSTRUE below. This is the
                        * negative test of the engine at the unit
                        * level: proves fail is reachable, not just
                        * pass. */
    e.expect_res2 = 0;
    got.res1 = -1; /* DOSTRUE: what a real ACTION_IS_FILESYSTEM reply
                    * looks like on a real file system. */
    got.res2 = 0;
    CHECK(stressfs_conform_evaluate(&e, got, 37) == STRESSFS_VERDICT_FAIL,
          "deliberately wrong expectation must produce fail, not pass");
}

static void test_res2_any(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got;
    memset(&e, 0, sizeof(e));
    e.expect_res1 = -1;
    e.expect_res2_any = 1; /* don't care what dp_Res2 is on success */
    got.res1 = -1;
    got.res2 = 12345;
    CHECK(stressfs_conform_evaluate(&e, got, 37) == STRESSFS_VERDICT_PASS,
          "expect_res2_any should ignore res2");
}

static int predicate_nonzero_or_zero_at_root(stressfs_pkt_result_t got, void *ctx)
{
    (void)ctx;
    /* Models ACTION_PARENT on the root: either a nonzero lock, or
     * res1==0 && res2==0 (root has no parent, not a failure). */
    if (got.res1 != 0) {
        return 1;
    }
    return got.res2 == 0;
}

static void test_predicate_multi_valid_outcome(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got_lock, got_root;
    memset(&e, 0, sizeof(e));
    e.predicate = predicate_nonzero_or_zero_at_root;

    got_lock.res1 = 12345;
    got_lock.res2 = 0;
    CHECK(stressfs_conform_evaluate(&e, got_lock, 37) == STRESSFS_VERDICT_PASS,
          "predicate: nonzero lock should pass");

    got_root.res1 = 0;
    got_root.res2 = 0;
    CHECK(stressfs_conform_evaluate(&e, got_root, 37) == STRESSFS_VERDICT_PASS,
          "predicate: zero lock at root (no error) should pass");

    got_root.res1 = 0;
    got_root.res2 = 999; /* a real error this time */
    CHECK(stressfs_conform_evaluate(&e, got_root, 37) == STRESSFS_VERDICT_FAIL,
          "predicate: zero lock WITH an error code should fail");
}

static void test_version_gating(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got;
    memset(&e, 0, sizeof(e));
    e.min_version = 40; /* e.g. a V40-only packet */
    e.expect_res1 = -1;
    got.res1 = -1;
    got.res2 = 0;

    /* Handler is V37: below the floor -- must be unsupported, and must
     * not even consult the expectation. */
    CHECK(stressfs_conform_evaluate(&e, got, 37) == STRESSFS_VERDICT_UNSUPPORTED,
          "test above handler's DOS version should be unsupported");

    /* Handler is V40: at the floor -- runs normally. */
    CHECK(stressfs_conform_evaluate(&e, got, 40) == STRESSFS_VERDICT_PASS,
          "test at handler's DOS version floor should run normally");

    /* Unknown handler version (0): cannot gate; evaluate() runs the
     * entry normally and leaves the "run only baseline" decision to
     * the caller, see conform.h's header comment. */
    CHECK(stressfs_conform_evaluate(&e, got, 0) == STRESSFS_VERDICT_PASS,
          "unknown handler version should not gate (caller's job)");
}

static void test_null_entry_is_inconclusive(void)
{
    stressfs_pkt_result_t got = {0, 0};
    CHECK(stressfs_conform_evaluate(NULL, got, 37) == STRESSFS_VERDICT_INCONCLUSIVE,
          "NULL entry should be treated as inconclusive, never pass");
}

static void test_format_line_contains_fields(void)
{
    stressfs_test_entry_t e;
    stressfs_pkt_result_t got;
    char buf[256];
    memset(&e, 0, sizeof(e));
    e.name = "ACTION_NIL";
    e.section = "packet-documentation.md#ACTION_NIL";
    e.oracle = STRESSFS_ORACLE_INDEPENDENT;
    got.res1 = -1;
    got.res2 = 0;

    stressfs_conform_format_line(buf, sizeof(buf), &e, STRESSFS_VERDICT_PASS, got, NULL);

    CHECK(strstr(buf, "pass") != NULL, "formatted line should contain the verdict");
    CHECK(strstr(buf, "independent") != NULL, "formatted line should contain the oracle class");
    CHECK(strstr(buf, "ACTION_NIL") != NULL, "formatted line should contain the test name");
    CHECK(strstr(buf, e.section) != NULL, "formatted line should contain the manual section");
}

static void test_format_line_handles_null_and_small_buffer(void)
{
    char tiny[4];
    stressfs_pkt_result_t got = {0, 0};
    /* Must not crash on a too-small buffer or a NULL entry. */
    stressfs_conform_format_line(tiny, sizeof(tiny), NULL, STRESSFS_VERDICT_FAIL, got, NULL);
    CHECK(tiny[sizeof(tiny) - 1] == '\0', "truncated line must stay NUL-terminated");
    stressfs_conform_format_line(NULL, 0, NULL, STRESSFS_VERDICT_FAIL, got, NULL); /* must not crash */
}

int main(void)
{
    test_exact_match_pass();
    test_exact_match_fail();
    test_res2_any();
    test_predicate_multi_valid_outcome();
    test_version_gating();
    test_null_entry_is_inconclusive();
    test_format_line_contains_fields();
    test_format_line_handles_null_and_small_buffer();

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("conform_test: OK\n");
    return 0;
}
