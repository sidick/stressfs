/* stressfs/conform_run.h -- entry point for `stressfs conform`, called
 * from main.c's ReadArgs dispatch. See amiga/src/conform.c for the
 * engine itself (the safe-probing ladder, the fixture, the table, the
 * watchdog self-test).
 */
#ifndef STRESSFS_CONFORM_RUN_H
#define STRESSFS_CONFORM_RUN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the M1 conform suite against `volume` (e.g. "SYS:" or "Work:").
 * If `selftest_hang` is nonzero, runs ONLY the watchdog hang-detection
 * self-test (see packet.h's stressfs_watchdog_selftest) instead of the
 * real table, printing one line and returning immediately -- this is
 * the demonstration vehicle for M1's "a hang produces inconclusive and
 * an abort" exit criterion, kept as an explicit, opt-in mode so a
 * normal run is never slowed down by it and so CI can invoke it on
 * its own.
 *
 * Returns a process return code: RETURN_OK if every executed test
 * passed (unsupported/gated tests do not count against this),
 * RETURN_WARN if at least one test failed or was inconclusive. */
int stressfs_conform_run(const char *volume, int selftest_hang);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_CONFORM_RUN_H */
