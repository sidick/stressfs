/* stressfs/handler_id.h -- the safe-probing ladder's step 0, "identify
 * the handler before anything else" (docs/proposal.md, "Safe
 * probing", point 4; docs/implementation-plan.md M1: "handler
 * identification first (DosType from the DosList, version string from
 * the segment list where present)").
 *
 * Two different "versions" matter here and must not be confused (a
 * decision the implementation plan left open, see the final report):
 *
 *   - The table's min_version gating (amiga/include/stressfs/conform.h)
 *     is checked against the running SYSTEM's dos.library version --
 *     the floor at which a given packet type was *added to AmigaDOS
 *     at all* (e.g. ACTION_SAME_LOCK needs AmigaDOS 36 regardless of
 *     which file system answers it). That number comes from
 *     stressfs_dos_version(), a plain OpenLibrary() version query --
 *     nothing handler-specific about it, and it is exactly what
 *     amiga/src/main.c's header comment already documents using for
 *     the explicit, non-global version check.
 *   - The *handler's own* identity -- its DosType and its "$VER:"
 *     version string, if its code carries one -- is informational
 *     only: printed at the top of a conform run so a report is
 *     attributable to "FastFileSystem 45.1" rather than just "V40",
 *     and used for the proposal's "if identity cannot be established,
 *     run only the baseline" fallback. It does not feed the gating
 *     arithmetic.
 */
#ifndef STRESSFS_HANDLER_ID_H
#define STRESSFS_HANDLER_ID_H

#include <exec/types.h>
#include <exec/ports.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct MsgPort *port;      /* the handler's dol_Task; NULL if identification failed entirely */
    char dos_type[5];          /* 4-char DosType code + NUL, e.g. "DOS\1"; empty if unknown */
    char version[80];          /* the handler's own "$VER:" string if found, else empty */
    int identified;            /* nonzero iff `port` is usable at all */
} stressfs_handler_info_t;

/* The running system's dos.library version (e.g. 37, 40, 47) --
 * always available once dos.library is open at all, which M0 already
 * established happens unconditionally. Returns 0 only if dos.library
 * could not be opened (should not happen; main() already treats that
 * as a hard failure before conform ever runs). */
int stressfs_dos_version(void);

/* Walks the DosList to find `volume_name`'s handler port, queries its
 * DosType via a watched ACTION_DISK_INFO, and scans its segment list
 * for an embedded "$VER:" string. Fills `info` in all cases; if the
 * volume cannot be found at all, `info->identified` is 0 and every
 * other field is zeroed/empty -- the caller (conform.c) treats that as
 * "run only the baseline", per the proposal. `timeout_secs` is the
 * watchdog deadline for the identification packet itself -- even
 * identification is not exempt from the hang-detection rule. */
void stressfs_identify_handler(const char *volume_name,
                                stressfs_handler_info_t *info,
                                unsigned timeout_secs);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_HANDLER_ID_H */
