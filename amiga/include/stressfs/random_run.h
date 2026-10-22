/* stressfs/random_run.h -- entry point for `stressfs random`, called
 * from main.c's ReadArgs dispatch. See amiga/src/random.c for the
 * engine itself (generator+model loop, real volume execution via
 * dos.library wrappers, the working-set refusal, divergence
 * detection and the report lines).
 */
#ifndef STRESSFS_RANDOM_RUN_H
#define STRESSFS_RANDOM_RUN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors stressfs_conform_run()'s signature shape (conform_run.h) --
 * same tier/seed/report plumbing and the same M2 exit-code scheme
 * (RETURN_OK/WARN/ERROR/FAIL; see conform_run.h's header comment),
 * extended with the knobs M4 needs:
 *
 *   `ops`          -- OPS/N/K: how many operations to generate and
 *                     apply (the exit criterion's "ops >= 10000").
 *                     Defaults to 1000 when <= 0.
 *   `verify_every` -- VERIFYN/N/K: how often, in steps, to additionally
 *                     run a full-tree compare on top of the per-step
 *                     checks (docs/implementation-plan.md M4: "full
 *                     tree compare every N steps"). Defaults to 200
 *                     when <= 0 -- see random.c's header comment for
 *                     why 200.
 *   `break_model`  -- RANDOMBREAKMODEL/S: the M4 self-test switch,
 *                     mirroring M1's SELFTESTHANG: runs one scripted
 *                     create+write and deliberately reports a wrong
 *                     model expectation for it, proving the
 *                     comparison logic detects a divergence end to
 *                     end without needing a real misbehaving handler.
 *                     Ignores `ops`/`stop_after` entirely.
 *   `stop_after`   -- `stressfs replay`'s own knob: if > 0, the run
 *                     stops (as RETURN_OK, not a harness failure) once
 *                     exactly this many steps have been generated,
 *                     regardless of `ops` -- so a replay can reproduce
 *                     "the stream up to the step that diverged"
 *                     without its own copy of the engine. 0 means
 *                     "run the full `ops` count", the normal case.
 *
 * On a divergence, the run STOPS at that step (an fsx-style tester's
 * "the first divergence is the bug", docs/proposal.md's own framing)
 * and the already-written report (JSON lines, one per step, same file
 * as every other run) is the operation log replay needs: the run
 * identity header carries the seed, and the last JSON line carries
 * `"diverged":true` at the step where it happened -- see
 * stressfs/replay.h for how `stressfs replay` uses that file. */
int stressfs_random_run(const char *volume, const char *tier, long seed,
                         long ops, long verify_every, int break_model,
                         long stop_after);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_RANDOM_RUN_H */
