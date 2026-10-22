/* amiga/src/handler_id.c -- see amiga/include/stressfs/handler_id.h
 * for the design rationale.
 */

#include "stressfs/handler_id.h"
#include "stressfs/packet.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#define STRESSFS_DOS_MIN_VERSION 37L

int stressfs_dos_version(void)
{
    struct Library *dos;
    int version;

    dos = OpenLibrary((CONST_STRPTR)"dos.library", STRESSFS_DOS_MIN_VERSION);
    if (dos == NULL) {
        return 0;
    }
    version = (int)dos->lib_Version;
    CloseLibrary(dos);
    return version;
}

/* Scans one loaded code/data segment for an embedded "$VER:" marker.
 * `seg` is a BPTR as stored in a DosList's dol_SegList chain (classic
 * hunk loader convention: BADDR(seg) is the segment's "pointer to
 * next segment" long, the four bytes immediately before it hold the
 * segment's total byte size including that long, and the segment's
 * own data starts immediately after it). Bounded and defensive: a
 * corrupt or wildly implausible size aborts the scan for this segment
 * rather than reading off into nowhere.
 *
 * Returns nonzero and fills `out` (NUL-terminated, truncated to
 * outsz-1) if a marker was found. */
static int scan_segment_for_version(BPTR seg, char *out, size_t outsz)
{
    ULONG *base;
    ULONG size_field;
    UBYTE *data;
    LONG datalen;
    LONG i;

    if (seg == 0) {
        return 0;
    }

    base = (ULONG *)BADDR(seg);
    size_field = *(base - 1);

    /* size_field includes the "next segment" long itself; sanity
     * bound it generously but refuse anything implausible (a real
     * handler's segments are at most a few hundred KB). */
    if (size_field < 8 || size_field > (1024UL * 1024UL)) {
        return 0;
    }

    data = (UBYTE *)base + sizeof(ULONG);
    datalen = (LONG)size_field - (LONG)sizeof(ULONG);
    if (datalen <= 0) {
        return 0;
    }

    for (i = 0; i + 5 <= datalen; i++) {
        if (data[i] == '$' && data[i + 1] == 'V' && data[i + 2] == 'E' &&
            data[i + 3] == 'R' && data[i + 4] == ':') {
            size_t j = 0;
            LONG k = i;
            /* Copy from "$VER:" up to the first non-printable byte
             * (the linker null-terminates these, but some tools pad
             * with other control bytes) or the segment's end. */
            while (k < datalen && j + 1 < outsz) {
                UBYTE c = data[k];
                if (c < 0x20 || c >= 0x7f) {
                    break;
                }
                out[j++] = (char)c;
                k++;
            }
            out[j] = '\0';
            return (j > 0);
        }
    }

    return 0;
}

static void scan_seglist_for_version(BPTR seglist, char *out, size_t outsz)
{
    BPTR seg = seglist;
    int guard;

    out[0] = '\0';

    /* Bounded walk: a real segment chain is short (a handler is
     * typically a handful of hunks); cap iterations so a corrupt or
     * cyclic chain cannot hang this step -- it is still a packet-free,
     * local memory walk, so there is nothing for the watchdog to
     * catch here, but a defensive bound costs nothing. */
    for (guard = 0; seg != 0 && guard < 64; guard++) {
        if (scan_segment_for_version(seg, out, outsz)) {
            return;
        }
        seg = *(BPTR *)BADDR(seg);
    }
}

void stressfs_identify_handler(const char *volume_name,
                                stressfs_handler_info_t *info,
                                unsigned timeout_secs)
{
    char name_buf[108];
    size_t name_len = 0;
    struct DosList *dlist;

    memset(info, 0, sizeof(*info));

    /* Identification itself currently needs no packet (DosType and
     * the segment list both come straight from the DosList, see
     * below), so there is nothing here for a watchdog to time out on
     * yet. `timeout_secs` stays part of the signature -- callers
     * already pass the same deadline they use everywhere else, and a
     * future revision that adds e.g. a capability-query packet here
     * (QUERY_CAPS/ACTION_FILESYSTEM_ATTR, phase 2) should not need an
     * API change to get a watched call. */
    (void)timeout_secs;

    if (volume_name == NULL) {
        return;
    }

    while (volume_name[name_len] != '\0' && volume_name[name_len] != ':' &&
           name_len < sizeof(name_buf) - 1) {
        name_buf[name_len] = volume_name[name_len];
        name_len++;
    }
    name_buf[name_len] = '\0';

    dlist = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dlist != NULL) {
        dlist = FindDosEntry(dlist, (CONST_STRPTR)name_buf, LDF_VOLUMES);
    }

    if (dlist == NULL) {
        UnLockDosList(LDF_VOLUMES | LDF_READ);
        return;
    }

    info->port = dlist->dol_Task;
    info->identified = (info->port != NULL);

    /* DosType lives directly on the VOLUME DosList entry
     * (dol_misc.dol_volume.dol_DiskType, e.g. 'DOS\1' for FFS) -- no
     * packet needed at all, exactly the "DosType from the DosList
     * entry" the proposal asks for. */
    {
        ULONG t = (ULONG)dlist->dol_misc.dol_volume.dol_DiskType;
        info->dos_type[0] = (char)((t >> 24) & 0xff);
        info->dos_type[1] = (char)((t >> 16) & 0xff);
        info->dos_type[2] = (char)((t >> 8) & 0xff);
        info->dos_type[3] = (char)(t & 0xff);
        info->dos_type[4] = '\0';
    }

    UnLockDosList(LDF_VOLUMES | LDF_READ);

    /* The segment list, in contrast, lives on the DEVICE node (the
     * dol_handler union member), not the volume node -- a volume's
     * dol_misc is the dol_volume member instead, so dol_SegList simply
     * is not there to read. A volume and the device that spawned its
     * handler process share the same dol_Task once mounted, so find
     * the device entry with a matching task and read its dol_SegList
     * from there. This is the "needs extra plumbing beyond what
     * main.c already does" case flagged in the task brief. */
    if (info->port != NULL) {
        struct DosList *ddl = LockDosList(LDF_DEVICES | LDF_READ);
        for (;;) {
            ddl = NextDosEntry(ddl, LDF_DEVICES);
            if (ddl == NULL) {
                break;
            }
            if (ddl->dol_Task == info->port) {
                scan_seglist_for_version(ddl->dol_misc.dol_handler.dol_SegList,
                                          info->version, sizeof(info->version));
                break;
            }
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
    }
}
