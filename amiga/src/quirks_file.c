/* amiga/src/quirks_file.c -- M3: the Amiga-only half of the quirks
 * database, i.e. getting the file's bytes off the disk and into the
 * portable parser in amiga/lib/quirks.c.
 *
 * This file exists purely so that lib/quirks.c can stay portable and
 * host-unit-tested (see stressfs/quirks.h's top comment): everything
 * here is dos.library Open/Read/Close plus exec AllocMem/FreeMem, the
 * same split M1 used for handler_id.c and M2 for the report files.
 *
 * Search order for the default file, when the caller gave no explicit
 * path (QUIRKS/K). Mirrors devsoak's quirks_load() and adds the
 * repo-relative path the plan's step 7 names:
 *
 *   1. "stressfs.quirks"          -- the current directory. Under
 *                                    Copperline's `--run` that is
 *                                    RunProg:, i.e. the host directory
 *                                    holding the cross-built binary, so
 *                                    ci/copperline-m3.sh simply copies
 *                                    the file next to the binary.
 *   2. "PROGDIR:stressfs.quirks"  -- next to the executable however it
 *                                    was invoked (2.0+; harmless if the
 *                                    assign does not exist).
 *   3. "data/stressfs.quirks"     -- the repo layout, for a run started
 *                                    from the checkout root.
 *
 * A missing file is NOT an error: a stressfs with no quirks file
 * behaves exactly like one run with NOQUIRKS, and says so in the
 * report's applied-quirks section. A file that exists but is MALFORMED
 * is a different matter entirely -- the parser refuses the whole file
 * and this function propagates that, because "never half-applied"
 * (docs/implementation-plan.md M3) has to mean the run either gets the
 * whole file or none of it.
 */

#include "stressfs/quirks_file.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

/* Bounded like every other buffer in this project: a quirks file is a
 * hand-maintained text file of a few dozen entries, and a stressfs run
 * must not be able to exhaust an A1200's memory because someone
 * concatenated a log into it. A file that does not fit is refused
 * (not truncated and parsed, which could leave a half-entry looking
 * well-formed). */
/* Raised from 16384 at M5: the file crossed that bound once M5 added
 * its own reproduced-finding entries (data/stressfs.quirks, "---
 * reproduced by stressfs against FFS V40 under Copperline ---") on top
 * of M3's seed set. Still a small, fixed bound -- a hand-maintained
 * text file of a few dozen entries, nowhere near this -- just a larger
 * one, not an unbounded read. */
#define QUIRKS_FILEBUF_MAX 32768UL

static const char *const g_default_paths[] = {
    "stressfs.quirks",
    "PROGDIR:stressfs.quirks",
    "data/stressfs.quirks"
};

int stressfs_quirks_load_file(stressfs_quirks_db_t *db, const char *path,
                              stressfs_quirk_diag_fn diag, void *diag_ctx)
{
    BPTR fh = 0;
    const char *used = NULL;
    char msg[200];
    UBYTE *buf;
    LONG total = 0;
    int ok;

    if (db == NULL) {
        return 0;
    }

    if (db->suppressed) {
        /* -K/NOQUIRKS: do not even look for the file. Reported in the
         * applied-quirks summary (see report.c), not only here. */
        if (diag != NULL) {
            diag(diag_ctx, "stressfs: quirks: NOQUIRKS given -- the quirks file is not "
                           "read at all, so any covered deviation will be reported as "
                           "the raw finding");
        }
        return 1;
    }

    if (path != NULL && path[0] != '\0') {
        fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
        if (fh == 0) {
            /* An EXPLICITLY named file that cannot be opened is an
             * error: the user asked for that file by name, and
             * silently running without it would mean the run was not
             * the run they asked for. */
            snprintf(msg, sizeof(msg), "stressfs: quirks: cannot open %s", path);
            if (diag != NULL) {
                diag(diag_ctx, msg);
            }
            return 0;
        }
        used = path;
    } else {
        size_t i;
        for (i = 0; i < sizeof(g_default_paths) / sizeof(g_default_paths[0]); i++) {
            fh = Open((CONST_STRPTR)g_default_paths[i], MODE_OLDFILE);
            if (fh != 0) {
                used = g_default_paths[i];
                break;
            }
        }
        if (fh == 0) {
            if (diag != NULL) {
                diag(diag_ctx, "stressfs: quirks: no quirks file found "
                               "(stressfs.quirks, PROGDIR:stressfs.quirks or "
                               "data/stressfs.quirks) -- running with no quirks");
            }
            /* Not an error; leaves db->loaded == 0. */
            return 1;
        }
    }

    buf = (UBYTE *)AllocMem(QUIRKS_FILEBUF_MAX + 1, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        Close(fh);
        if (diag != NULL) {
            diag(diag_ctx, "stressfs: quirks: AllocMem failed -- quirks file not loaded");
        }
        return 0;
    }

    for (;;) {
        LONG got = Read(fh, buf + total, (LONG)(QUIRKS_FILEBUF_MAX - (ULONG)total));
        if (got <= 0) {
            break;
        }
        total += got;
        if ((ULONG)total >= QUIRKS_FILEBUF_MAX) {
            break;
        }
    }
    Close(fh);
    buf[total] = '\0';

    if ((ULONG)total >= QUIRKS_FILEBUF_MAX) {
        FreeMem(buf, QUIRKS_FILEBUF_MAX + 1);
        if (diag != NULL) {
            diag(diag_ctx, "stressfs: quirks: file is larger than this build's buffer "
                           "-- REFUSING THE WHOLE FILE rather than parsing a truncated "
                           "copy");
        }
        return 0;
    }

    ok = stressfs_quirks_parse(db, used, (const char *)buf, diag, diag_ctx);
    FreeMem(buf, QUIRKS_FILEBUF_MAX + 1);
    return ok;
}
