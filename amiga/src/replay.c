/* amiga/src/replay.c -- `stressfs replay`. See
 * amiga/include/stressfs/replay.h for why re-reading two numbers out
 * of the JSON-lines report `stressfs random` already writes is all
 * this subcommand needs to do ("thin", per the plan's own wording).
 */

#include "stressfs/replay.h"
#include "stressfs/random_run.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RETURN_ERROR
#define RETURN_ERROR 10
#endif

/* Bounded like every other file this project reads into memory
 * (quirks_file.c's QUIRKS_FILEBUF_MAX) -- a report from a run of a
 * few thousand steps is at most a few hundred KB of JSON lines, well
 * under this. A longer run's report is still readable: only the
 * TAIL matters (the last random_step line), and this cap is generous
 * enough that truncating the front of a genuinely huge log still
 * leaves that tail intact for any run this milestone's exit criterion
 * actually asks for (10k-100k steps). */
#define REPLAY_FILEBUF_MAX (512UL * 1024UL)

static long parse_long_after(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    if (p == NULL) {
        return -1;
    }
    p += strlen(key);
    return strtol(p, NULL, 10);
}

static int parse_bool_after(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    if (p == NULL) {
        return 0;
    }
    p += strlen(key);
    return strncmp(p, "true", 4) == 0;
}

/* Finds the LAST occurrence of `needle` in `hay`, or NULL. */
static const char *last_occurrence(const char *hay, const char *needle)
{
    const char *last = NULL;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        last = p;
        p += 1;
    }
    return last;
}

int stressfs_replay_run(const char *logpath, const char *volume, const char *tier)
{
    BPTR fh;
    UBYTE *buf;
    LONG total = 0;
    long seed, orig_step;
    int orig_diverged;
    const char *ident_line;
    const char *last_step_line;
    int rc;

    if (logpath == NULL || logpath[0] == '\0') {
        Printf((CONST_STRPTR)"stressfs replay: no log file given (REPLAYLOG=path)\n");
        return RETURN_FAIL;
    }

    fh = Open((CONST_STRPTR)logpath, MODE_OLDFILE);
    if (fh == 0) {
        Printf((CONST_STRPTR)"stressfs replay: cannot open \"%s\" (IoErr=%ld)\n",
               (CONST_STRPTR)logpath, (long)IoErr());
        return RETURN_FAIL;
    }

    buf = (UBYTE *)AllocMem(REPLAY_FILEBUF_MAX + 1, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        Close(fh);
        Printf((CONST_STRPTR)"stressfs replay: AllocMem failed\n");
        return RETURN_FAIL;
    }

    for (;;) {
        LONG got = Read(fh, buf + total, (LONG)(REPLAY_FILEBUF_MAX - (ULONG)total));
        if (got <= 0) {
            break;
        }
        total += got;
        if ((ULONG)total >= REPLAY_FILEBUF_MAX) {
            break;
        }
    }
    Close(fh);
    buf[total] = '\0';

    ident_line = strstr((const char *)buf, "\"type\":\"run_identity\"");
    if (ident_line == NULL) {
        FreeMem(buf, REPLAY_FILEBUF_MAX + 1);
        Printf((CONST_STRPTR)"stressfs replay: \"%s\" has no run_identity line -- not a "
               "`stressfs random` report\n", (CONST_STRPTR)logpath);
        return RETURN_FAIL;
    }
    seed = parse_long_after(ident_line, "\"seed\":");

    last_step_line = last_occurrence((const char *)buf, "\"type\":\"random_step\"");
    if (last_step_line == NULL) {
        FreeMem(buf, REPLAY_FILEBUF_MAX + 1);
        Printf((CONST_STRPTR)"stressfs replay: \"%s\" has no random_step lines -- nothing to replay\n",
               (CONST_STRPTR)logpath);
        return RETURN_FAIL;
    }
    orig_step = parse_long_after(last_step_line, "\"step\":");
    orig_diverged = parse_bool_after(last_step_line, "\"diverged\":");

    FreeMem(buf, REPLAY_FILEBUF_MAX + 1);

    if (orig_step < 1) {
        Printf((CONST_STRPTR)"stressfs replay: could not parse a usable step count from \"%s\"\n",
               (CONST_STRPTR)logpath);
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"stressfs replay: re-running SEED=%ld for %ld step(s) against %s "
           "(original run %s at step %ld)\n",
           seed, orig_step, (CONST_STRPTR)volume,
           orig_diverged ? "diverged" : "did not diverge", orig_step);

    rc = stressfs_random_run(volume, tier, seed, orig_step, 0, 0, orig_step);

    /* stressfs_random_run() returns RETURN_WARN exactly when IT saw a
     * divergence (see random_run.h); reproducing the ORIGINAL outcome
     * means both runs agree on whether a divergence happened at all --
     * "the same sequence, re-run, behaves the same way", which is what
     * a replay promises. A replay that does NOT reproduce an original
     * divergence (or produces one where the original had none) is
     * itself a reportable finding (non-determinism, or an environment
     * difference between the two runs), not quietly treated as either
     * outcome. */
    {
        int replay_diverged = (rc == RETURN_WARN);
        if (replay_diverged == orig_diverged) {
            Printf((CONST_STRPTR)"stressfs replay: REPRODUCED the original outcome (diverged=%s)\n",
                   orig_diverged ? "true" : "false");
            return RETURN_OK;
        }
        Printf((CONST_STRPTR)"stressfs replay: DID NOT reproduce the original outcome "
               "(original diverged=%s, replay diverged=%s) -- see the fresh report just "
               "written for details\n",
               orig_diverged ? "true" : "false", replay_diverged ? "true" : "false");
        return RETURN_WARN;
    }
}
