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

/* Runs the M1/M2 conform suite against `volume` (e.g. "SYS:" or
 * "Work:"). If `selftest_hang` is nonzero, runs ONLY the watchdog
 * hang-detection self-test (see packet.h's stressfs_watchdog_selftest)
 * instead of the real table, printing one line and returning
 * immediately -- this is the demonstration vehicle for M1's "a hang
 * produces inconclusive and an abort" exit criterion, kept as an
 * explicit, opt-in mode so a normal run is never slowed down by it and
 * so CI can invoke it on its own.
 *
 * `tier` is the proposal's run-identity "tier" field (e.g. "volamos",
 * "copperline", "hardware") -- the binary cannot always know which
 * tier it is running under, so the invoking script states it (see
 * main.c's TIER/K ReadArgs option); pass NULL or "" for "unknown".
 * `seed` is the proposal's run-identity "seed" field (M1's SEED/N/K
 * option, now threaded all the way into the report header).
 *
 * M2 EXIT CODE SCHEME (docs/implementation-plan.md M2 step 4 --
 * "0 all-pass, distinct codes for fail/inconclusive", exact numbers
 * left to this implementation; chosen to match AmigaDOS's own
 * conventional return-code ladder, RETURN_OK=0 < RETURN_WARN=5 <
 * RETURN_ERROR=10 < RETURN_FAIL=20, which most Shell scripts and
 * `IF WARN`/`IF ERROR` already understand):
 *
 *   RETURN_OK (0)     -- every EXECUTED test passed. Unsupported
 *                         (version-gated) tests never count against
 *                         this, per the proposal's "always listed,
 *                         never silently skipped, but not a failure"
 *                         treatment of `unsupported`.
 *   RETURN_WARN (5)   -- at least one test reported `fail`, and NONE
 *                         reported `inconclusive`. A real but bounded
 *                         problem: the run completed, every result is
 *                         trustworthy, only the handler's behaviour
 *                         was wrong somewhere.
 *   RETURN_ERROR (10) -- at least one test reported `inconclusive`
 *                         (a watchdog-detected hang, or a harness-side
 *                         setup failure that aborted the run). This is
 *                         DISTINCT from and takes PRIORITY over a mere
 *                         fail, and is a strictly higher number, because
 *                         `inconclusive` signals a possibly-damaged
 *                         handler and/or an untrustworthy remainder of
 *                         the run -- exactly the signal
 *                         docs/proposal.md's "What a pass does not
 *                         establish" section warns a reader never to
 *                         confuse with an ordinary failure. A caller
 *                         scripting on exit code can therefore treat
 *                         "ERROR or worse" as "do not trust this run's
 *                         image/handler state", a stronger statement
 *                         than "WARN or worse" (ordinary failures).
 *   RETURN_FAIL (20)  -- the run could not even start (no handler port,
 *                         dos.library too old, watchdog device
 *                         unavailable, run-identity assembly itself
 *                         failed stressfs_run_identity_is_complete() --
 *                         a harness bug by the proposal's own
 *                         definition). Reserved for "nothing ran at
 *                         all", the most severe case.
 *
 * If both a fail and an inconclusive occur in the same run,
 * RETURN_ERROR wins (priority rule above) -- this is the one place M1's
 * ad-hoc RETURN_WARN+1=6 scheme (never matching any AmigaDOS
 * convention) has been replaced. */
int stressfs_conform_run(const char *volume, const char *tier, long seed, int selftest_hang);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_CONFORM_RUN_H */
