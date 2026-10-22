/* amiga/src/main.c — the M0 hello-world stressfs binary.
 *
 * docs/implementation-plan.md milestone M0: "proving startup code,
 * library linkage, BPTR/BSTR handling and packet plumbing in one
 * shot." This is deliberately minimal — not the conform engine (that's
 * M1) — just enough real plumbing to prove the toolchain and the
 * packet path end to end:
 *
 *   1. Parse a ReadArgs template skeleton for the subcommands and
 *      options the later milestones will actually use.
 *   2. Open dos.library explicitly, with an explicit version check
 *      (V37 — this project's documented minimum OS, 2.04).
 *
 *      Decision not spelled out by the implementation plan: this
 *      explicit OpenLibrary() is a SEPARATE handle used only to assert
 *      the version floor, stored in a local variable rather than in
 *      the global `DOSBase` the toolchain's own startup code
 *      (`-noixemul` libnix) already manages. libnix's ncrt0 always
 *      auto-opens dos.library (any version) into its own global
 *      `DOSBase` before main() runs and auto-closes it at exit — that
 *      auto-opened handle is what every dos.library proto call in this
 *      file (Printf, ReadArgs, Lock, DoPkt, ...) actually uses under
 *      the hood, via the extern `DOSBase` proto/dos.h declares.
 *      Redefining that same global ourselves (`struct DosLibrary
 *      *DOSBase;` at file scope) was tried first and crashes under
 *      volamos — the duplicate definition corrupts libnix's own
 *      startup state (observed as a jump through a bad address
 *      register shortly after libnix's own init-list processing,
 *      before main() is ever reached). Keeping our explicit,
 *      version-checked open as its own local handle, immediately
 *      closed again, gets the "opens dos.library explicitly with a
 *      version check" proof M0 asks for without fighting the startup
 *      code that already owns the global.
 *   3. Resolve a named volume's filesystem handler port. There is no
 *      single library call for "give me a volume's handler port"; the
 *      RKRM's own NameOfVolume example walks the DosList rather than
 *      asking a lock for it, and that is the approach used here:
 *      LockDosList(LDF_VOLUMES|LDF_READ), FindDosEntry() by name, read
 *      dol_Task out of the returned struct DosList, UnLockDosList().
 *
 *      Decision not spelled out by the implementation plan: an earlier
 *      version of this file used Lock()'s own fl_Task (BADDR(lock)'s
 *      FileLock, a one-call shortcut that is also BPTR/BSTR handling
 *      and is valid on real AmigaOS — a lock's handler task and its
 *      volume's DosList handler task are the same port there). It
 *      does not work against volamos: volamos's Lock() hands back a
 *      shared, generic sentinel task id for every lock regardless of
 *      volume (see volamos's crate::dosfile module docs,
 *      DEFAULT_FILE_SYS_TASK), while DoPkt()'s ACTION_IS_FILESYSTEM
 *      handling only recognizes the per-volume ids LockDosList/
 *      FindDosEntry hand out (crate::dosdevlist::task_id_for_volume) —
 *      sending to the generic sentinel gets back ERROR_ACTION_NOT_KNOWN
 *      (209). Going through the DosList, which is both the documented
 *      convention and what every tier (volamos and real hardware
 *      alike) agrees on, is correct either way and was the actual fix.
 *   4. Send ACTION_IS_FILESYSTEM to that port with DoPkt() and print
 *      dp_Res1 (DoPkt()'s own return value) and dp_Res2 (IoErr(), by
 *      the documented DoPkt() convention).
 *
 * The ReadArgs template matches docs/implementation-plan.md's M0
 * wording exactly: CONFORM/S,RANDOM/S,SEED/N/K,VOLUME/K. Only VOLUME
 * (defaulting to SYS: when absent) is actually acted on here; CONFORM/
 * RANDOM/SEED are parsed and echoed so the template itself is proven,
 * but the subcommand engines they will eventually select are M1 (conform)
 * and M4 (random) work, not M0's.
 */

#include <stddef.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "stressfs/prng.h"
#include "stressfs/conform_run.h"

/* M1 (docs/implementation-plan.md): CONFORM now dispatches to the real
 * table-driven engine (amiga/src/conform.c) instead of being echoed
 * and ignored. SELFTESTHANG is a new, hidden-from-the-plan-template
 * switch added for M1's watchdog demonstration (see conform.c and
 * packet.h's header comments) -- it is not part of the proposal's
 * public CLI surface, just a developer/CI knob to prove the hang path
 * on demand without needing a real misbehaving handler. */
/* M2: TIER/K added -- the proposal's run-identity "tier" field
 * (docs/implementation-plan.md M2 step 1: "a TIER/K ReadArgs option is
 * a reasonable way to let the invoking script state it, default to
 * 'unknown'"). The binary itself cannot always know which tier it is
 * running under (the same cross-built executable runs unmodified under
 * volamos, Copperline and real hardware), so the invoking script
 * (ci/cross-build.sh's callers, ci/copperline-m2.sh, a future hardware
 * runner) states it explicitly. */
#define STRESSFS_TEMPLATE "CONFORM/S,RANDOM/S,SEED/N/K,VOLUME/K,TIER/K,SELFTESTHANG/S"
#define STRESSFS_DOS_VERSION 37L /* V37 = AmigaOS 2.04, this project's floor */

/* ReadArgs() argument slots, in template order. */
enum {
    ARG_CONFORM,
    ARG_RANDOM,
    ARG_SEED,
    ARG_VOLUME,
    ARG_TIER,
    ARG_SELFTESTHANG,
    ARG_COUNT
};

/* proto/dos.h declares DOSBase extern; the toolchain's own -noixemul
 * startup code (libnix's ncrt0) supplies the definition and manages it
 * automatically (auto-open before main(), auto-close at exit) — see
 * the header comment above for why this file does not redefine it. */

int main(void)
{
    LONG args[ARG_COUNT];
    struct RDArgs *rdargs;
    struct Library *dos_version_check;
    LONG seed = 0;
    CONST_STRPTR volume = (CONST_STRPTR)"SYS:";
    CONST_STRPTR tier = (CONST_STRPTR)"unknown";
    int rc = RETURN_OK;
    int i;

    for (i = 0; i < ARG_COUNT; i++) {
        args[i] = 0;
    }

    dos_version_check = OpenLibrary((CONST_STRPTR)"dos.library", STRESSFS_DOS_VERSION);
    if (dos_version_check == NULL) {
        /* Explicit version check failed: dos.library is open (libnix's
         * startup already did that for us, any version), but not at
         * our required floor. Report via the process return code. */
        return RETURN_FAIL;
    }
    CloseLibrary(dos_version_check);

    rdargs = ReadArgs((STRPTR)STRESSFS_TEMPLATE, args, NULL);
    if (rdargs == NULL) {
        PrintFault(IoErr(), (CONST_STRPTR)"stressfs");
        return RETURN_FAIL;
    }

    if (args[ARG_SEED] != 0) {
        seed = *(LONG *)args[ARG_SEED];
    }
    if (args[ARG_VOLUME] != 0) {
        volume = (CONST_STRPTR)args[ARG_VOLUME];
    }
    if (args[ARG_TIER] != 0) {
        tier = (CONST_STRPTR)args[ARG_TIER];
    }

    Printf((CONST_STRPTR)"stressfs: CONFORM=%ld RANDOM=%ld SEED=%ld VOLUME=%s\n",
           (LONG)(args[ARG_CONFORM] != 0),
           (LONG)(args[ARG_RANDOM] != 0),
           seed,
           volume);

    /* M1: CONFORM now runs the real table-driven engine instead of
     * just the M0 hello-world ACTION_IS_FILESYSTEM demo below. It owns
     * its own exit code and returns directly -- the rest of main()'s
     * body (the PRNG smoke print and the bare ACTION_IS_FILESYSTEM
     * call) stays exactly as M0 left it for the non-CONFORM case, so
     * `stressfs VOLUME=...` with no switches is still the M0 proof
     * unchanged. */
    if (args[ARG_CONFORM] != 0) {
        rc = stressfs_conform_run((const char *)volume, (const char *)tier, seed,
                                   args[ARG_SELFTESTHANG] != 0);
        FreeArgs(rdargs);
        return rc;
    }

    /* Prove the portable lib/ PRNG is linked in and runnable on the
     * real target too, not just under the host ctest build. */
    {
        stressfs_prng_t prng;
        stressfs_prng_seed(&prng, (uint32_t)seed);
        Printf((CONST_STRPTR)"stressfs: prng first value = 0x%08lx\n",
               (ULONG)stressfs_prng_next(&prng));
    }

    /* The one real packet: ACTION_IS_FILESYSTEM to the named volume's
     * handler port, found by walking the DosList (see the header
     * comment's step 3 for why this, not Lock()'s FileLock, is what's
     * used here). FindDosEntry()/dol_Name convention (and the RKRM's
     * NameOfVolume example) wants the bare volume name, no trailing
     * ':'. */
    {
        char name_buf[108]; /* MAXFILENAMELENGTH-ish; plenty for a volume name */
        LONG name_len = 0;
        struct DosList *dlist;

        while (volume[name_len] != '\0' && volume[name_len] != ':' &&
               (size_t)name_len < sizeof(name_buf) - 1) {
            name_buf[name_len] = volume[name_len];
            name_len++;
        }
        name_buf[name_len] = '\0';

        dlist = LockDosList(LDF_VOLUMES | LDF_READ);
        if (dlist != NULL) {
            dlist = FindDosEntry(dlist, (CONST_STRPTR)name_buf, LDF_VOLUMES);
        }

        if (dlist == NULL) {
            Printf((CONST_STRPTR)"stressfs: no such volume \"%s\"\n", volume);
            rc = RETURN_WARN;
        } else {
            struct MsgPort *handler = dlist->dol_Task;
            LONG res1;

            SetIoErr(0);
            res1 = DoPkt(handler, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0);

            Printf((CONST_STRPTR)"stressfs: ACTION_IS_FILESYSTEM on \"%s\" "
                   "-> dp_Res1=%ld dp_Res2=%ld\n",
                   volume, res1, IoErr());

            if (res1 != DOSTRUE) {
                rc = RETURN_WARN;
            }
        }

        UnLockDosList(LDF_VOLUMES | LDF_READ);
    }

    FreeArgs(rdargs);
    return rc;
}
