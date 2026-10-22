/* amiga/lib/conform.c -- portable verdict/format logic for the M1
 * conform engine. See amiga/include/stressfs/conform.h for the design
 * rationale (why this file has no Amiga dependency and compiles/tests
 * on the host).
 */

#include "stressfs/conform.h"

#include <stdio.h>
#include <string.h>

const char *stressfs_verdict_name(stressfs_verdict_t v)
{
    switch (v) {
    case STRESSFS_VERDICT_PASS:         return "pass";
    case STRESSFS_VERDICT_FAIL:         return "fail";
    case STRESSFS_VERDICT_DEVIATION:    return "deviation";
    case STRESSFS_VERDICT_UNSUPPORTED:  return "unsupported";
    case STRESSFS_VERDICT_INCONCLUSIVE: return "inconclusive";
    default:                            return "?";
    }
}

const char *stressfs_oracle_name(stressfs_oracle_t o)
{
    switch (o) {
    case STRESSFS_ORACLE_INDEPENDENT: return "independent";
    case STRESSFS_ORACLE_MODEL:       return "model";
    case STRESSFS_ORACLE_CORRELATED:  return "correlated";
    default:                          return "?";
    }
}

stressfs_verdict_t stressfs_conform_evaluate(const stressfs_test_entry_t *entry,
                                              stressfs_pkt_result_t got,
                                              int handler_version)
{
    if (entry == NULL) {
        return STRESSFS_VERDICT_INCONCLUSIVE;
    }

    /* Version gating comes first and is unconditional: a test below
     * the running handler's floor is unsupported regardless of
     * anything else, per M1's exit criterion ("tests annotated below
     * the running handler's DOS version report unsupported"). Only
     * applies when the handler's version was actually established
     * (handler_version != 0) -- if identification failed we cannot
     * gate, and the caller (amiga/src/conform.c) is responsible for
     * the proposal's fallback ("if identity cannot be established the
     * suite runs only the baseline and reports the rest as
     * unsupported"), not this function. */
    if (handler_version != 0 && entry->min_version > handler_version) {
        return STRESSFS_VERDICT_UNSUPPORTED;
    }

    if (entry->predicate != NULL) {
        return entry->predicate(got, entry->run_ctx)
                   ? STRESSFS_VERDICT_PASS
                   : STRESSFS_VERDICT_FAIL;
    }

    if (!entry->expect_res1_any && got.res1 != entry->expect_res1) {
        return STRESSFS_VERDICT_FAIL;
    }
    if (!entry->expect_res2_any && got.res2 != entry->expect_res2) {
        return STRESSFS_VERDICT_FAIL;
    }
    return STRESSFS_VERDICT_PASS;
}

void stressfs_conform_format_line(char *buf, size_t bufsz,
                                   const stressfs_test_entry_t *entry,
                                   stressfs_verdict_t verdict,
                                   stressfs_pkt_result_t got)
{
    const char *name = (entry != NULL && entry->name != NULL) ? entry->name : "?";
    const char *section = (entry != NULL && entry->section != NULL) ? entry->section : "?";
    const char *oracle = stressfs_oracle_name(entry != NULL ? entry->oracle : STRESSFS_ORACLE_MODEL);
    const char *verdict_name = stressfs_verdict_name(verdict);

    if (buf == NULL || bufsz == 0) {
        return;
    }

    /* One line, readable by eye (M1's own exit bar -- full JSON-lines
     * is M2's job, see conform.h's header comment): verdict first so
     * `grep` for fail/inconclusive is trivial, then oracle class and
     * manual section per the proposal's "Output" paragraph, then the
     * test name and the raw dp_Res1/dp_Res2 for debugging. */
    snprintf(buf, bufsz, "%-12s oracle=%-11s section=%-48s res1=%-6ld res2=%-4ld  %s",
             verdict_name, oracle, section, got.res1, got.res2, name);
}
