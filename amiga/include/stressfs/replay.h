/* stressfs/replay.h -- `stressfs replay`, "a thin subcommand that
 * re-executes a logged sequence" (docs/implementation-plan.md M4).
 *
 * DESIGN DECISION (not fully specified by the plan): because
 * stressfs_random_generate() is a pure function of (seed, prior model
 * state), and prior model state is itself fully determined by the
 * operations the SAME generator previously produced from that seed,
 * the entire operation stream for a run is reproducible from the seed
 * alone (plus the step count already run). There is therefore no need
 * for a separate, hand-rolled "operation log" format: the JSON-lines
 * report `stressfs random` already writes (one line per step, via
 * stressfs_report_format_step_json()) IS the log -- it carries the
 * seed in its run-identity header line and the step number of every
 * line, including whichever one first reported a divergence.
 * `stressfs replay LOGFILE VOLUME` just has to read those two numbers
 * back out of that file and re-invoke stressfs_random_run() with the
 * same seed, stopped at the same step, against (possibly) a different
 * volume -- which is exactly the "thin" the plan's wording asks for:
 * no second engine, no second file format, no state this header's
 * caller (amiga/src/replay.c) has to parse beyond two integers.
 */
#ifndef STRESSFS_REPLAY_H
#define STRESSFS_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reads `logpath` (a `stressfs random` JSON-lines report), extracts
 * the run's seed and the step number of its last line, and re-runs
 * `stressfs random` against `volume` stopped at exactly that step
 * (stressfs_random_run()'s `stop_after`), reporting whether the same
 * divergence reproduces. Returns the M2 exit-code scheme
 * (conform_run.h's header comment): RETURN_OK if the replayed run
 * reproduces the same outcome (diverged iff the original diverged),
 * RETURN_WARN if it diverged but not identically to the original (a
 * real, reportable finding about non-determinism -- NOT silently
 * treated as success), RETURN_FAIL if the log file could not be read
 * or parsed at all. */
int stressfs_replay_run(const char *logpath, const char *volume, const char *tier);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_REPLAY_H */
