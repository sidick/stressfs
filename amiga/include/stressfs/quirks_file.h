/* stressfs/quirks_file.h -- the Amiga-only file loader for the quirks
 * database (M3). See amiga/src/quirks_file.c for the search order and
 * the missing-vs-malformed distinction, and stressfs/quirks.h for the
 * parser this hands the bytes to.
 *
 * Deliberately a separate header from quirks.h: quirks.h is portable
 * and host-unit-tested, this one is not, and the split is what keeps
 * the host build from ever seeing dos.library.
 */
#ifndef STRESSFS_QUIRKS_FILE_H
#define STRESSFS_QUIRKS_FILE_H

#include "stressfs/quirks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the quirks file into `db` (which the caller must have
 * initialised, and on which it must already have set `suppressed` for
 * NOQUIRKS and any FORCEQUIRK ids). `path` may be NULL/empty for the
 * default search order.
 *
 * Returns 1 when the run may proceed -- including the "no quirks file
 * anywhere" case, which is normal -- and 0 when it may not: an
 * explicitly-named file that would not open, an allocation failure, an
 * over-large file, or a MALFORMED file the parser refused. The caller
 * treats 0 as fatal (see amiga/src/conform.c), because a run that
 * silently continued without the quirks the operator expected would
 * report findings that are not comparable with any other run. */
int stressfs_quirks_load_file(stressfs_quirks_db_t *db, const char *path,
                              stressfs_quirk_diag_fn diag, void *diag_ctx);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_QUIRKS_FILE_H */
