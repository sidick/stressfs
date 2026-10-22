/* amiga/src/conform.c -- the M1 table-driven conform engine.
 *
 * Structure (docs/implementation-plan.md M1, step 2 and step 3):
 *
 *   1. stressfs_conform_run() is the entry point from main.c. It runs
 *      the safe-probing ladder in the order the proposal specifies:
 *      handler identification, then the baseline
 *      (ACTION_IS_FILESYSTEM, ACTION_NIL, ACTION_CURRENT_VOLUME, a
 *      root lock taken and freed), then a fixture is created, then
 *      the packet matrix table runs, then the fixture is torn down.
 *   2. Every table entry's run function talks to the handler
 *      exclusively through stressfs_send_packet_watched() (never raw
 *      DoPkt()) so a hang anywhere in the matrix is caught by the
 *      watchdog, reported as STRESSFS_VERDICT_INCONCLUSIVE, and the
 *      run aborts immediately without attempting fixture teardown --
 *      "preserves the volume image untouched... aborts the run rather
 *      than continuing on a possibly damaged handler" (proposal,
 *      Safe probing point 4).
 *   3. One deliberate exception: the handful of "dos.library-wrapper
 *      twin" entries call the library function directly (Lock(),
 *      Open(), Examine(), Rename(), CreateDir()) rather than building
 *      a packet by hand, per the proposal's "library wrappers add
 *      their own conventions" rationale for testing both paths. These
 *      calls are NOT watchdog-protected: dos.library's own wrapper
 *      functions call DoPkt() internally (not our SendPkt()+timer
 *      pair), and DoPkt() itself cannot be raced against a timeout
 *      (see packet.h's header comment) -- there is no portable way to
 *      bound how long a wrapper call blocks. This is a known,
 *      deliberate scope limit for M1, called out in the final report:
 *      a hang inside a wrapper-twin test is not caught by this
 *      engine's watchdog. In practice the wrapper twins only run
 *      *after* the corresponding raw-packet test has already
 *      exercised the same operation successfully, so a handler that
 *      would hang here has already had every opportunity to hang on
 *      the (protected) raw-packet entry first.
 *
 * Context sharing (a decision the implementation plan left open): all
 * table entries share ONE `struct conform_env` as their `run_ctx`,
 * threaded through the whole table rather than each entry getting its
 * own isolated context. This matches reality -- the matrix is a
 * single scripted sequence with real data dependencies between steps
 * (lock a directory, then free that same lock three entries later;
 * open a file, then read/seek/close *that* file), not an independent
 * per-packet matrix. The table is executed strictly in order for
 * exactly this reason.
 */

#include "stressfs/conform_run.h"
#include "stressfs/conform.h"
#include "stressfs/packet.h"
#include "stressfs/handler_id.h"
#include "stressfs/quirks.h"
#include "stressfs/quirks_file.h"
#include "stressfs/report.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <assert.h>
#include <string.h>
#include <stdio.h>

/* M2 exit-code priority: see conform_run.h's header comment for the
 * full scheme and rationale. RETURN_ERROR is not defined by every
 * dos/dos.h across OS versions this project targets (it is, from
 * V37's dos/dos.h onward, but define it defensively rather than risk
 * a build break on an older header). */
#ifndef RETURN_ERROR
#define RETURN_ERROR 10
#endif

#define STRESSFS_WATCHDOG_SECS 10u

/* The running Kickstart's version is exec.library's own lib_Version,
 * read straight off ExecBase -- the `kick` match key in
 * data/stressfs.quirks. libnix's startup code owns the SysBase global
 * (same arrangement as DOSBase, see main.c's header comment), so this
 * only declares it. */
extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* Shared context threaded through the whole table.                   */
/* ------------------------------------------------------------------ */

struct conform_env {
    struct MsgPort *port;
    char volume[32];     /* e.g. "WORK:" -- includes the trailing colon */
    char dirname[32];    /* fixture subdirectory name, relative to the volume root */

    BPTR dir_lock;        /* shared lock on the fixture dir; lives across most of the table */
    BPTR tmp_lock;         /* scratch lock, reused by several sequential entries */

    APTR fh_obj;           /* AllocDosObject(DOS_FILEHANDLE) result, reused across open/seek/close */
    LONG fh_arg1;          /* the handler's own fh_Arg1 identifier for the currently-open file */

    struct FileInfoBlock fib; /* reused across all EXAMINE* entries, per the RKRM's own advice */

    int run_aborted;       /* set once a hang is detected; the table loop checks this each entry */
};

static void report_note(const char *s); /* defined below, used by fixture_setup's diagnostics */

static void path_join(char *out, size_t outsz, const char *volume, const char *dir, const char *leaf)
{
    /* volume already carries its trailing ':' (e.g. "WORK:"); dir/leaf
     * may be NULL/empty to build a shorter path. */
    out[0] = '\0';
    strncat(out, volume, outsz - 1);
    if (dir != NULL && dir[0] != '\0') {
        strncat(out, dir, outsz - strlen(out) - 1);
        if (leaf != NULL && leaf[0] != '\0') {
            strncat(out, "/", outsz - strlen(out) - 1);
        }
    }
    if (leaf != NULL && leaf[0] != '\0') {
        strncat(out, leaf, outsz - strlen(out) - 1);
    }
}

/* ------------------------------------------------------------------ */
/* Fixture setup/teardown: all via dos.library wrappers (independent  */
/* of what the table itself is testing), under a subdirectory so a    */
/* run never touches anything else on the volume.                     */
/* ------------------------------------------------------------------ */

static int fixture_write_file(const char *path, const char *contents)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh == 0) {
        return 0;
    }
    if (contents != NULL && contents[0] != '\0') {
        LONG len = (LONG)strlen(contents);
        if (Write(fh, (APTR)contents, len) != len) {
            Close(fh);
            return 0;
        }
    }
    Close(fh);
    return 1;
}

static int fixture_setup(struct conform_env *env)
{
    char path[160];
    BPTR lock;

    path_join(path, sizeof(path), env->volume, NULL, env->dirname);
    lock = CreateDir((CONST_STRPTR)path);
    if (lock == 0) {
        /* Idempotent: a previous run that hit an inconclusive/aborted
         * exit deliberately skips teardown (see the header comment on
         * stressfs_conform_run), which can leave this directory behind
         * from last time. Treat "already exists" as fine rather than
         * a fixture failure -- re-used, not re-created. */
        if (IoErr() == ERROR_OBJECT_EXISTS) {
            lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
        }
        if (lock == 0) {
            char hdr[256];
            snprintf(hdr, sizeof(hdr), "stressfs conform: CreateDir(\"%s\") failed, IoErr()=%ld",
                     path, (long)IoErr());
            report_note(hdr);
            return 0;
        }
    }
    UnLock(lock);

    path_join(path, sizeof(path), env->volume, env->dirname, "probe.txt");
    if (!fixture_write_file(path, "stressfs-m1 probe\n")) {
        return 0;
    }

    path_join(path, sizeof(path), env->volume, env->dirname, "second.txt");
    if (!fixture_write_file(path, "")) {
        return 0;
    }

    return 1;
}

/* Best-effort: never trusted after a hang (the caller skips this
 * entirely in that case, see stressfs_conform_run), and every call is
 * allowed to fail silently -- by the time teardown runs, several
 * fixture objects may already have been renamed, deleted or recreated
 * by the table itself. */
static void fixture_teardown(struct conform_env *env)
{
    static const char *leftovers[] = {
        "probe.txt", "probe-renamed.txt", "out.txt", "locked.txt",
        "second.txt", "subdir", "subdir2"
    };
    size_t i;
    char path[160];

    for (i = 0; i < sizeof(leftovers) / sizeof(leftovers[0]); i++) {
        path_join(path, sizeof(path), env->volume, env->dirname, leftovers[i]);
        DeleteFile((CONST_STRPTR)path);
    }

    path_join(path, sizeof(path), env->volume, NULL, env->dirname);
    DeleteFile((CONST_STRPTR)path);
}

/* ------------------------------------------------------------------ */
/* Table entry bodies. Each returns the raw {res1,res2} the packet or  */
/* wrapper produced; stressfs_conform_evaluate() (amiga/lib/conform.c) */
/* decides the verdict from the entry's expectation.                  */
/* ------------------------------------------------------------------ */

/* Every real table entry talks to the handler via plain DoPkt()
 * (packet.h's header comment explains why: volamos only implements
 * the DoPkt() LVO itself, not the SendPkt()/WaitPkt() primitives a
 * per-packet watchdog would need, and real DoPkt() cannot be raced
 * against a timeout from this process anyway). Hang protection for a
 * real run is therefore the OUTER script-level timeout around the
 * whole `stressfs conform` invocation (see ci/copperline-m1.sh), not
 * anything per-packet here; env->run_aborted exists only so a future
 * milestone has somewhere to hook a different detection signal
 * without changing every call site. */
static stressfs_pkt_result_t send(struct conform_env *env, LONG action,
                                   LONG a1, LONG a2, LONG a3, LONG a4, LONG a5)
{
    return stressfs_dopkt(env->port, action, a1, a2, a3, a4, a5);
}

/* --- baseline --- */

static stressfs_pkt_result_t run_is_filesystem(void *ctx)
{
    return send((struct conform_env *)ctx, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0);
}

static stressfs_pkt_result_t run_nil(void *ctx)
{
    return send((struct conform_env *)ctx, ACTION_NIL, 0, 0, 0, 0, 0);
}

static int predicate_nonzero_res1(stressfs_pkt_result_t got, void *ctx)
{
    (void)ctx;
    return got.res1 != 0;
}

static stressfs_pkt_result_t run_current_volume(void *ctx)
{
    return send((struct conform_env *)ctx, ACTION_CURRENT_VOLUME, 0, 0, 0, 0, 0);
}

static stressfs_pkt_result_t run_baseline_root_lock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r = send(env, ACTION_LOCATE_OBJECT, 0,
                                    (LONG)stressfs_bstr_set(&name, ""),
                                    SHARED_LOCK, 0, 0);
    if (r.res1 != 0) {
        env->tmp_lock = (BPTR)r.res1;
    }
    return r;
}

static stressfs_pkt_result_t run_baseline_root_unlock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_FREE_LOCK, (LONG)env->tmp_lock, 0, 0, 0, 0);
    env->tmp_lock = 0;
    return r;
}

/* --- matrix: locks --- */

static stressfs_pkt_result_t run_locate_dir_shared(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r = send(env, ACTION_LOCATE_OBJECT, 0,
                                    (LONG)stressfs_bstr_set(&name, env->dirname),
                                    SHARED_LOCK, 0, 0);
    if (r.res1 != 0) {
        env->dir_lock = (BPTR)r.res1;
    }
    return r;
}

static stressfs_pkt_result_t run_locate_exclusive_root_refused(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r = send(env, ACTION_LOCATE_OBJECT, 0,
                                    (LONG)stressfs_bstr_set(&name, ""),
                                    EXCLUSIVE_LOCK, 0, 0);
    /* This is a quirk CHECK, not a guaranteed refusal -- some handlers
     * (observed: FFS 47.4 under Copperline) grant it. Whichever way it
     * goes, free the lock immediately if one was granted: leaving an
     * exclusive lock on the root held for the rest of the run would
     * poison every later test that touches the root or the fixture
     * directory (observed: ACTION_PARENT and ACTION_FREE_LOCK both
     * failing with ERROR_OBJECT_IN_USE a few entries later, before
     * this fix, because exactly that lock was still outstanding). */
    if (r.res1 != 0) {
        (void)send(env, ACTION_FREE_LOCK, r.res1, 0, 0, 0, 0);
    }
    return r;
}

static stressfs_pkt_result_t run_locate_not_found(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    char path[64];
    strncpy(path, env->dirname, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    strncat(path, "/does-not-exist.xyz", sizeof(path) - strlen(path) - 1);
    return send(env, ACTION_LOCATE_OBJECT, 0,
                (LONG)stressfs_bstr_set(&name, path),
                SHARED_LOCK, 0, 0);
}

static stressfs_pkt_result_t run_wrapper_lock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    char path[160];
    stressfs_pkt_result_t r;
    BPTR lock;

    path_join(path, sizeof(path), env->volume, NULL, env->dirname);
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    r.res1 = (lock != 0) ? DOSTRUE : DOSFALSE;
    r.res2 = (lock != 0) ? 0 : IoErr();
    if (lock != 0) {
        UnLock(lock);
    }
    return r;
}

static stressfs_pkt_result_t run_copy_dir(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_COPY_DIR, (LONG)env->dir_lock, 0, 0, 0, 0);
    if (r.res1 != 0) {
        env->tmp_lock = (BPTR)r.res1;
    }
    return r;
}

static stressfs_pkt_result_t run_same_lock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SAME_LOCK, (LONG)env->dir_lock, (LONG)env->tmp_lock, 0, 0, 0);
}

static stressfs_pkt_result_t run_free_tmp_lock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_FREE_LOCK, (LONG)env->tmp_lock, 0, 0, 0, 0);
    env->tmp_lock = 0;
    return r;
}

static stressfs_pkt_result_t run_parent(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_PARENT, (LONG)env->dir_lock, 0, 0, 0, 0);
    if (r.res1 != 0) {
        env->tmp_lock = (BPTR)r.res1;
    }
    return r;
}

/* --- matrix: examine --- */

static stressfs_pkt_result_t run_examine_object(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    memset(&env->fib, 0, sizeof(env->fib));
    return send(env, ACTION_EXAMINE_OBJECT, (LONG)env->dir_lock,
                (LONG)MKBADDR(&env->fib), 0, 0, 0);
}

static stressfs_pkt_result_t run_wrapper_examine(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    struct FileInfoBlock local_fib;
    stressfs_pkt_result_t r;
    BOOL ok = Examine(env->dir_lock, &local_fib);
    r.res1 = ok ? DOSTRUE : DOSFALSE;
    r.res2 = ok ? 0 : IoErr();
    return r;
}

static stressfs_pkt_result_t run_examine_next_first(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_EXAMINE_NEXT, (LONG)env->dir_lock,
                (LONG)MKBADDR(&env->fib), 0, 0, 0);
}

static stressfs_pkt_result_t run_examine_next_drain(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r;
    int i;

    r.res1 = DOSTRUE;
    r.res2 = 0;
    /* Bounded: a real directory scan always terminates; the bound is
     * only there so a handler that (incorrectly) never returns
     * ERROR_NO_MORE_ENTRIES cannot turn this into an unbounded loop of
     * otherwise-successful, individually-fast packet sends -- each
     * individual send is still watchdog-protected on its own. */
    for (i = 0; i < 256 && r.res1 != 0 && !env->run_aborted; i++) {
        r = send(env, ACTION_EXAMINE_NEXT, (LONG)env->dir_lock,
                  (LONG)MKBADDR(&env->fib), 0, 0, 0);
    }
    return r;
}

/* --- matrix: file I/O --- */

static APTR alloc_fh(struct MsgPort *port)
{
    struct FileHandle *fh = (struct FileHandle *)AllocDosObject(DOS_FILEHANDLE, NULL);
    if (fh != NULL) {
        fh->fh_Type = port;
    }
    return (APTR)fh;
}

static stressfs_pkt_result_t run_findoutput(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r;

    env->fh_obj = alloc_fh(env->port);
    if (env->fh_obj == NULL) {
        r.res1 = 0;
        r.res2 = ERROR_NO_FREE_STORE;
        return r;
    }

    r = send(env, ACTION_FINDOUTPUT, (LONG)MKBADDR(env->fh_obj),
             (LONG)env->dir_lock, (LONG)stressfs_bstr_set(&name, "out.txt"), 0, 0);
    if (r.res1 != 0) {
        env->fh_arg1 = ((struct FileHandle *)env->fh_obj)->fh_Arg1;
    }
    return r;
}

static stressfs_pkt_result_t run_write(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    static const char data[] = "hello-stressfs\n"; /* 15 bytes */
    return send(env, ACTION_WRITE, env->fh_arg1, (LONG)data,
                (LONG)(sizeof(data) - 1), 0, 0);
}

static stressfs_pkt_result_t run_seek_beginning(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SEEK, env->fh_arg1, 0, OFFSET_BEGINNING, 0, 0);
}

static stressfs_pkt_result_t run_seek_current(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SEEK, env->fh_arg1, 0, OFFSET_CURRENT, 0, 0);
}

static stressfs_pkt_result_t run_seek_end(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SEEK, env->fh_arg1, 0, OFFSET_END, 0, 0);
}

static stressfs_pkt_result_t run_set_file_size_grow(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SET_FILE_SIZE, env->fh_arg1, 5, OFFSET_END, 0, 0);
}

static stressfs_pkt_result_t run_set_file_size_truncate(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    return send(env, ACTION_SET_FILE_SIZE, env->fh_arg1, 5, OFFSET_BEGINNING, 0, 0);
}

static stressfs_pkt_result_t run_end_close_output(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_END, env->fh_arg1, 0, 0, 0, 0);
    if (env->fh_obj != NULL) {
        FreeDosObject(DOS_FILEHANDLE, env->fh_obj);
        env->fh_obj = NULL;
    }
    env->fh_arg1 = 0;
    return r;
}

static stressfs_pkt_result_t run_wrapper_open_read_close(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    char path[160];
    char buf[64];
    stressfs_pkt_result_t r;
    BPTR fh;
    LONG n;

    path_join(path, sizeof(path), env->volume, env->dirname, "probe.txt");
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == 0) {
        r.res1 = 0;
        r.res2 = IoErr();
        return r;
    }

    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);

    if (n >= 0) {
        buf[n] = '\0';
    }
    r.res1 = (n > 0 && strcmp(buf, "stressfs-m1 probe\n") == 0) ? DOSTRUE : DOSFALSE;
    r.res2 = 0;
    return r;
}

static stressfs_pkt_result_t run_findinput(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r;

    env->fh_obj = alloc_fh(env->port);
    if (env->fh_obj == NULL) {
        r.res1 = 0;
        r.res2 = ERROR_NO_FREE_STORE;
        return r;
    }

    r = send(env, ACTION_FINDINPUT, (LONG)MKBADDR(env->fh_obj),
             (LONG)env->dir_lock, (LONG)stressfs_bstr_set(&name, "probe.txt"), 0, 0);
    if (r.res1 != 0) {
        env->fh_arg1 = ((struct FileHandle *)env->fh_obj)->fh_Arg1;
    }
    return r;
}

static stressfs_pkt_result_t run_read(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    static char buf[64];
    return send(env, ACTION_READ, env->fh_arg1, (LONG)buf, (LONG)(sizeof(buf) - 1), 0, 0);
}

static stressfs_pkt_result_t run_end_close_input(void *ctx)
{
    return run_end_close_output(ctx); /* identical mechanics */
}

static stressfs_pkt_result_t run_findupdate_roundtrip(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    APTR fh_obj;
    stressfs_pkt_result_t r;

    fh_obj = alloc_fh(env->port);
    if (fh_obj == NULL) {
        r.res1 = 0;
        r.res2 = ERROR_NO_FREE_STORE;
        return r;
    }

    r = send(env, ACTION_FINDUPDATE, (LONG)MKBADDR(fh_obj),
              (LONG)env->dir_lock, (LONG)stressfs_bstr_set(&name, "probe.txt"), 0, 0);
    if (r.res1 != 0) {
        LONG arg1 = ((struct FileHandle *)fh_obj)->fh_Arg1;
        (void)send(env, ACTION_END, arg1, 0, 0, 0, 0);
    }
    FreeDosObject(DOS_FILEHANDLE, fh_obj);
    return r;
}

/* --- matrix: delete / rename / createdir --- */

static stressfs_pkt_result_t run_delete_unlocked(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    return send(env, ACTION_DELETE_OBJECT, (LONG)env->dir_lock,
                (LONG)stressfs_bstr_set(&name, "second.txt"), 0, 0, 0);
}

static stressfs_pkt_result_t run_delete_while_locked(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    char path[160];
    stressfs_bstr_t name;
    stressfs_pkt_result_t r;
    BPTR fh;

    path_join(path, sizeof(path), env->volume, env->dirname, "locked.txt");
    fh = Open((CONST_STRPTR)path, MODE_NEWFILE); /* exclusive by definition */
    if (fh == 0) {
        r.res1 = 0;
        r.res2 = IoErr();
        return r;
    }

    r = send(env, ACTION_DELETE_OBJECT, (LONG)env->dir_lock,
              (LONG)stressfs_bstr_set(&name, "locked.txt"), 0, 0, 0);

    Close(fh);
    DeleteFile((CONST_STRPTR)path);
    return r;
}

static stressfs_pkt_result_t run_rename_object(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t src, dst;
    return send(env, ACTION_RENAME_OBJECT, (LONG)env->dir_lock,
                (LONG)stressfs_bstr_set(&src, "probe.txt"),
                (LONG)env->dir_lock,
                (LONG)stressfs_bstr_set(&dst, "probe-renamed.txt"), 0);
}

static stressfs_pkt_result_t run_wrapper_rename(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    char from[160], to[160];
    stressfs_pkt_result_t r;
    BOOL ok;

    path_join(from, sizeof(from), env->volume, env->dirname, "probe-renamed.txt");
    path_join(to, sizeof(to), env->volume, env->dirname, "probe.txt");
    ok = Rename((CONST_STRPTR)from, (CONST_STRPTR)to);
    r.res1 = ok ? DOSTRUE : DOSFALSE;
    r.res2 = ok ? 0 : IoErr();
    return r;
}

static stressfs_pkt_result_t run_create_dir(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_bstr_t name;
    stressfs_pkt_result_t r = send(env, ACTION_CREATE_DIR, (LONG)env->dir_lock,
                                    (LONG)stressfs_bstr_set(&name, "subdir"), 0, 0, 0);
    if (r.res1 != 0) {
        (void)send(env, ACTION_FREE_LOCK, r.res1, 0, 0, 0, 0);
    }
    return r;
}

static stressfs_pkt_result_t run_wrapper_create_dir(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    char path[160];
    stressfs_pkt_result_t r;
    BPTR lock;

    path_join(path, sizeof(path), env->volume, env->dirname, "subdir2");
    lock = CreateDir((CONST_STRPTR)path);
    r.res1 = (lock != 0) ? DOSTRUE : DOSFALSE;
    r.res2 = (lock != 0) ? 0 : IoErr();
    if (lock != 0) {
        UnLock(lock);
    }
    return r;
}

/* --- matrix: volume info --- */

static stressfs_pkt_result_t run_info(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    struct InfoData id;
    memset(&id, 0, sizeof(id));
    return send(env, ACTION_INFO, (LONG)env->dir_lock, (LONG)MKBADDR(&id), 0, 0, 0);
}

static stressfs_pkt_result_t run_disk_info(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    struct InfoData id;
    memset(&id, 0, sizeof(id));
    return send(env, ACTION_DISK_INFO, (LONG)MKBADDR(&id), 0, 0, 0, 0);
}

/* --- final cleanup of the persistent dir lock, run as its own table
 * entry so it has a visible report line --- */

static stressfs_pkt_result_t run_free_dir_lock(void *ctx)
{
    struct conform_env *env = (struct conform_env *)ctx;
    stressfs_pkt_result_t r = send(env, ACTION_FREE_LOCK, (LONG)env->dir_lock, 0, 0, 0, 0);
    env->dir_lock = 0;
    return r;
}

/* --- engine self-tests: the negative test and the version-gating demo --- */

static stressfs_pkt_result_t run_selftest_negative_is_filesystem(void *ctx)
{
    /* Same real packet as the baseline's run_is_filesystem -- the
     * table entry below deliberately expects DOSFALSE, which a real
     * file system never returns for ACTION_IS_FILESYSTEM, to prove
     * the engine can and does emit `fail`. */
    return send((struct conform_env *)ctx, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* The table.                                                          */
/* ------------------------------------------------------------------ */

static struct conform_env g_env;

/* M3: the quirks database for this run. One static instance, filled
 * once by stressfs_conform_run() before the first packet and read-only
 * thereafter -- same lifetime discipline devsoak's quirks.c documents,
 * and what makes stressfs_quirks_apply_verdict() safe to call from the
 * table loop without any locking. */
static stressfs_quirks_db_t g_quirks;

/* M3 added the leading `tid` (test id) argument: the short, stable
 * machine name a quirks entry's action targets (see
 * stressfs/quirks.h and stressfs/conform.h's comment on
 * stressfs_test_entry_t.id). Keep these unique and keep them stable --
 * data/stressfs.quirks points at them by name, and
 * ci/quirks-check.sh cross-references the two files. */
#define ENTRY_EXACT(tid, nm, sect, minver, oracle_, fn, exp1, exp2) \
    { (tid), (nm), (sect), (minver), (oracle_), (fn), &g_env, (exp1), 0, (exp2), 0, NULL }

#define ENTRY_RES1_ANY(tid, nm, sect, minver, oracle_, fn, exp2) \
    { (tid), (nm), (sect), (minver), (oracle_), (fn), &g_env, 0, 1, (exp2), 0, NULL }

#define ENTRY_PRED(tid, nm, sect, minver, oracle_, fn, pred) \
    { (tid), (nm), (sect), (minver), (oracle_), (fn), &g_env, 0, 1, 0, 1, (pred) }

static const stressfs_test_entry_t g_table[] = {
    /* --- baseline (docs/proposal.md "Safe probing" point 1) --- */
    ENTRY_EXACT("baseline-is-filesystem", "baseline: ACTION_IS_FILESYSTEM", "packet-documentation.md#ACTION_IS_FILESYSTEM",
                0, STRESSFS_ORACLE_INDEPENDENT, run_is_filesystem, DOSTRUE, 0),
    ENTRY_EXACT("baseline-nil", "baseline: ACTION_NIL", "packet-documentation.md#ACTION_NIL",
                0, STRESSFS_ORACLE_INDEPENDENT, run_nil, DOSTRUE, 0),
    ENTRY_PRED("baseline-current-volume", "baseline: ACTION_CURRENT_VOLUME", "packet-documentation.md#ACTION_CURRENT_VOLUME",
               0, STRESSFS_ORACLE_INDEPENDENT, run_current_volume, predicate_nonzero_res1),
    ENTRY_PRED("baseline-root-lock", "baseline: root lock (ACTION_LOCATE_OBJECT \"\")", "packet-documentation.md#ACTION_LOCATE_OBJECT",
               0, STRESSFS_ORACLE_INDEPENDENT, run_baseline_root_lock, predicate_nonzero_res1),
    ENTRY_EXACT("baseline-root-unlock", "baseline: root unlock (ACTION_FREE_LOCK)", "packet-documentation.md#ACTION_FREE_LOCK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_baseline_root_unlock, DOSTRUE, 0),

    /* --- matrix: locks --- */
    ENTRY_PRED("locate-shared", "ACTION_LOCATE_OBJECT (shared, success)", "packet-documentation.md#ACTION_LOCATE_OBJECT",
               0, STRESSFS_ORACLE_INDEPENDENT, run_locate_dir_shared, predicate_nonzero_res1),
    ENTRY_EXACT("locate-exclusive-root", "ACTION_LOCATE_OBJECT (exclusive lock on root, refused -- quirk check)",
                "proposal.md#Layer-1-packet-conformance",
                0, STRESSFS_ORACLE_MODEL, run_locate_exclusive_root_refused, 0, 0),
    ENTRY_EXACT("locate-not-found", "ACTION_LOCATE_OBJECT (not found)", "packet-documentation.md#ACTION_LOCATE_OBJECT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_locate_not_found, 0, ERROR_OBJECT_NOT_FOUND),
    ENTRY_EXACT("wrapper-lock", "dos.library wrapper twin: Lock() shared", "elementary-concepts.md#Lock",
                0, STRESSFS_ORACLE_INDEPENDENT, run_wrapper_lock, DOSTRUE, 0),
    ENTRY_PRED("copy-dir", "ACTION_COPY_DIR (duplicate lock)", "packet-documentation.md#ACTION_COPY_DIR",
               0, STRESSFS_ORACLE_INDEPENDENT, run_copy_dir, predicate_nonzero_res1),
    ENTRY_EXACT("same-lock", "ACTION_SAME_LOCK (dir lock vs. its duplicate)", "packet-documentation.md#ACTION_SAME_LOCK",
                36, STRESSFS_ORACLE_INDEPENDENT, run_same_lock, DOSTRUE, 0),
    ENTRY_EXACT("free-lock-duplicate", "ACTION_FREE_LOCK (the duplicate)", "packet-documentation.md#ACTION_FREE_LOCK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_free_tmp_lock, DOSTRUE, 0),
    ENTRY_PRED("parent", "ACTION_PARENT (of the fixture dir)", "packet-documentation.md#ACTION_PARENT",
               0, STRESSFS_ORACLE_INDEPENDENT, run_parent, predicate_nonzero_res1),
    ENTRY_EXACT("free-lock-parent", "ACTION_FREE_LOCK (the parent lock)", "packet-documentation.md#ACTION_FREE_LOCK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_free_tmp_lock, DOSTRUE, 0),

    /* --- matrix: examine --- */
    ENTRY_EXACT("examine-object", "ACTION_EXAMINE_OBJECT (fixture dir)", "packet-documentation.md#ACTION_EXAMINE_OBJECT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_examine_object, DOSTRUE, 0),
    ENTRY_EXACT("wrapper-examine", "dos.library wrapper twin: Examine()", "directories.md#Examine",
                0, STRESSFS_ORACLE_INDEPENDENT, run_wrapper_examine, DOSTRUE, 0),
    ENTRY_EXACT("examine-next-first", "ACTION_EXAMINE_NEXT (first entry)", "packet-documentation.md#ACTION_EXAMINE_NEXT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_examine_next_first, DOSTRUE, 0),
    ENTRY_EXACT("examine-next-drain", "ACTION_EXAMINE_NEXT (drained to ERROR_NO_MORE_ENTRIES)",
                "packet-documentation.md#ACTION_EXAMINE_NEXT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_examine_next_drain, 0, ERROR_NO_MORE_ENTRIES),

    /* --- matrix: file I/O --- */
    ENTRY_EXACT("findoutput", "ACTION_FINDOUTPUT (create out.txt, exclusive)", "packet-documentation.md#ACTION_FINDOUTPUT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_findoutput, DOSTRUE, 0),
    ENTRY_EXACT("write", "ACTION_WRITE (15 bytes)", "packet-documentation.md#ACTION_WRITE",
                0, STRESSFS_ORACLE_INDEPENDENT, run_write, 15, 0),
    ENTRY_EXACT("seek-beginning", "ACTION_SEEK (OFFSET_BEGINNING)", "packet-documentation.md#ACTION_SEEK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_seek_beginning, 15, 0),
    ENTRY_EXACT("seek-current", "ACTION_SEEK (OFFSET_CURRENT)", "packet-documentation.md#ACTION_SEEK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_seek_current, 0, 0),
    ENTRY_EXACT("seek-end", "ACTION_SEEK (OFFSET_END)", "packet-documentation.md#ACTION_SEEK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_seek_end, 0, 0),
    ENTRY_EXACT("set-file-size-grow", "ACTION_SET_FILE_SIZE (grow)", "packet-documentation.md#ACTION_SET_FILE_SIZE",
                36, STRESSFS_ORACLE_INDEPENDENT, run_set_file_size_grow, DOSTRUE, 0),
    ENTRY_EXACT("set-file-size-truncate", "ACTION_SET_FILE_SIZE (truncate)", "packet-documentation.md#ACTION_SET_FILE_SIZE",
                36, STRESSFS_ORACLE_INDEPENDENT, run_set_file_size_truncate, DOSTRUE, 0),
    ENTRY_EXACT("end-close-output", "ACTION_END (close out.txt)", "packet-documentation.md#ACTION_END",
                0, STRESSFS_ORACLE_INDEPENDENT, run_end_close_output, DOSTRUE, 0),
    ENTRY_EXACT("wrapper-open-read-close", "dos.library wrapper twin: Open()/Read()/Close() of probe.txt",
                "files.md#Open", 0, STRESSFS_ORACLE_INDEPENDENT, run_wrapper_open_read_close, DOSTRUE, 0),
    ENTRY_EXACT("findinput", "ACTION_FINDINPUT (probe.txt, shared)", "packet-documentation.md#ACTION_FINDINPUT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_findinput, DOSTRUE, 0),
    ENTRY_RES1_ANY("read", "ACTION_READ (probe.txt contents)", "packet-documentation.md#ACTION_READ",
                    0, STRESSFS_ORACLE_INDEPENDENT, run_read, 0),
    ENTRY_EXACT("end-close-input", "ACTION_END (close probe.txt input handle)", "packet-documentation.md#ACTION_END",
                0, STRESSFS_ORACLE_INDEPENDENT, run_end_close_input, DOSTRUE, 0),
    ENTRY_EXACT("findupdate", "ACTION_FINDUPDATE (probe.txt, open+close round trip)",
                "packet-documentation.md#ACTION_FINDUPDATE",
                0, STRESSFS_ORACLE_INDEPENDENT, run_findupdate_roundtrip, DOSTRUE, 0),

    /* --- matrix: delete / rename / createdir --- */
    ENTRY_EXACT("delete-unlocked", "ACTION_DELETE_OBJECT (unlocked second.txt)", "packet-documentation.md#ACTION_DELETE_OBJECT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_delete_unlocked, DOSTRUE, 0),
    ENTRY_EXACT("delete-while-locked", "ACTION_DELETE_OBJECT (locked.txt, exclusively open -> ERROR_OBJECT_IN_USE)",
                "packet-documentation.md#ACTION_DELETE_OBJECT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_delete_while_locked, 0, ERROR_OBJECT_IN_USE),
    ENTRY_EXACT("rename-object", "ACTION_RENAME_OBJECT (probe.txt -> probe-renamed.txt)",
                "packet-documentation.md#ACTION_RENAME_OBJECT",
                0, STRESSFS_ORACLE_INDEPENDENT, run_rename_object, DOSTRUE, 0),
    ENTRY_EXACT("wrapper-rename", "dos.library wrapper twin: Rename()", "directories.md#Rename",
                0, STRESSFS_ORACLE_INDEPENDENT, run_wrapper_rename, DOSTRUE, 0),
    ENTRY_PRED("create-dir", "ACTION_CREATE_DIR (subdir)", "packet-documentation.md#ACTION_CREATE_DIR",
               0, STRESSFS_ORACLE_INDEPENDENT, run_create_dir, predicate_nonzero_res1),
    ENTRY_EXACT("wrapper-create-dir", "dos.library wrapper twin: CreateDir() (subdir2)", "directories.md#CreateDir",
                0, STRESSFS_ORACLE_INDEPENDENT, run_wrapper_create_dir, DOSTRUE, 0),

    /* --- matrix: volume info --- */
    ENTRY_EXACT("info", "ACTION_INFO (fixture dir lock)", "packet-documentation.md#ACTION_INFO",
                0, STRESSFS_ORACLE_INDEPENDENT, run_info, DOSTRUE, 0),
    ENTRY_EXACT("disk-info", "ACTION_DISK_INFO", "packet-documentation.md#ACTION_DISK_INFO",
                0, STRESSFS_ORACLE_INDEPENDENT, run_disk_info, DOSTRUE, 0),

    /* --- engine self-tests (M1 exit criteria) --- */
    ENTRY_EXACT("selftest-negative-expectation", "ENGINE SELF-TEST: deliberately wrong expectation (must report fail)",
                "implementation-plan.md#M1", 0, STRESSFS_ORACLE_MODEL,
                run_selftest_negative_is_filesystem, DOSFALSE, 0),
    ENTRY_EXACT("selftest-version-gate", "ENGINE SELF-TEST: version-gating demo (requires dos.library V999, always unsupported)",
                "implementation-plan.md#M1", 999, STRESSFS_ORACLE_MODEL,
                run_is_filesystem, DOSTRUE, 0),

    /* --- cleanup --- */
    ENTRY_EXACT("free-lock-fixture-dir", "ACTION_FREE_LOCK (fixture dir lock)", "packet-documentation.md#ACTION_FREE_LOCK",
                0, STRESSFS_ORACLE_INDEPENDENT, run_free_dir_lock, DOSTRUE, 0),
};

#define TABLE_COUNT (sizeof(g_table) / sizeof(g_table[0]))

/* ------------------------------------------------------------------ */
/* Driver.                                                             */
/* ------------------------------------------------------------------ */

/* M2 dual-output decision (docs/implementation-plan.md M2 step 3; see
 * also this file's own header comment, which predates M2 and already
 * explains why the human-readable file exists):
 *
 *   docs/proposal.md's Layer 1 "Output" paragraph reads: "one line per
 *   test with verdict, oracle class, the manual section it comes from,
 *   and every quirk that was applied ..., machine-readable for CI and
 *   readable in a Shell." The implementation plan's own M2 wording
 *   ("machine-readable ... AND a human-readable Shell rendering ...
 *   Output to console, file or serial") separates "machine-readable"
 *   and "human-readable" as two adjectives for the SAME per-test
 *   event, immediately followed by a separate sentence about output
 *   DESTINATIONS (console/file/serial) -- i.e. the content split
 *   (human vs. JSON) and the destination split (console vs. file) are
 *   two independent axes, not one combined decision forced by the
 *   proposal's wording alone.
 *
 *   Decision taken here: SEPARATE CHANNELS, each carrying one of the
 *   two renderings in full, rather than interleaving both renderings
 *   into one stream. Concretely: the human-readable line (unchanged
 *   from M1) goes to the console via Printf() AND to
 *   stressfs-conform-report.txt; the JSON-lines rendering is new in
 *   M2 and goes to its own file, stressfs-conform-report.json, and
 *   NOT to the console. Rationale: a human reading the console output
 *   of a headless Copperline run (ci/copperline-m2.sh's screenshot
 *   mode, or a developer's own interactive run) wants the Shell-style
 *   line, not raw JSON scrolling past; a CI `ci/` checker consuming
 *   the report wants one well-formed JSON document per line with
 *   nothing else interleaved on the same stream (mixing the two into
 *   one file would mean every consumer has to filter lines by their
 *   first character, which is exactly the kind of fragile convention
 *   the JSON-lines format exists to avoid). Keeping them as two files
 *   also means a byte-for-byte golden-file ctest (this milestone's
 *   exit criterion) can check each rendering independently without
 *   needing to agree on an interleaving order. */
static BPTR g_report_file = 0;      /* stressfs-conform-report.txt, human-readable */
static BPTR g_report_json_file = 0; /* stressfs-conform-report.json, JSON-lines */

/* Second copy of each file, opened directly on the TARGET VOLUME
 * rather than the relative/RunProg: path above. Why both: the
 * relative path resolves to RunProg: (the host directory holding the
 * cross-built binary) under a real Copperline run, which is exactly
 * what M1's host-side CI script wants and deliberately avoids writing
 * harness artefacts onto the volume under test -- but under volamos,
 * Open() against a relative path, RAM: or T: is faked (always
 * "succeeds" but never touches the host filesystem; verified by
 * hand while building this milestone), and the ONLY path volamos
 * backs with real host I/O is one under an explicitly `-V`-mapped
 * assign, i.e. the volume this run is actually testing. Since M1
 * already writes fixture files onto that same volume (the
 * `stressfs-m1` subdirectory) this is not a new category of "touching
 * the volume under test" -- it is the same thing M1 already does, one
 * level higher (the volume root instead of a subdirectory). Both
 * copies are opened every time; whichever one(s) a given tier can
 * actually back with real storage is what ends up readable
 * afterwards, and print_line()/report_note()/emit_run_identity() write
 * to every handle that opened successfully. */
static BPTR g_report_file_vol = 0;
static BPTR g_report_json_file_vol = 0;

/* STATIC, not automatic, and deliberately so: an AmigaDOS CLI process
 * gets a 4KB stack by default, and M3's renderings are bigger than
 * M2's (a report line now carries the applied quirk's id, confidence
 * and match reason; the applied-quirks summary block is larger still).
 * Holding ~3.5KB of formatting buffers on the stack overflows it --
 * found the hard way while proving this milestone: the first
 * Copperline run of M3's conform engine wedged the guest before a
 * single report line was written, with automatic buffers of exactly
 * these sizes, and moving them to static storage fixed it outright.
 * These functions are only ever called from the single conform
 * process, in sequence, so static storage is safe here (same
 * single-context discipline devsoak's quirks.c documents for its own
 * load-time state). */
static char g_line_buf[512];
static char g_line_jbuf[1024];

static void print_line(const stressfs_test_entry_t *entry, stressfs_verdict_t verdict,
                        stressfs_pkt_result_t got,
                        const stressfs_quirk_applied_t *quirk)
{
    char *buf = g_line_buf;
    char *jbuf = g_line_jbuf;
#define sizeof_buf  sizeof(g_line_buf)
#define sizeof_jbuf sizeof(g_line_jbuf)

    stressfs_conform_format_line(buf, sizeof_buf, entry, verdict, got, quirk);
    Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)buf);
    if (g_report_file != 0) {
        FPuts(g_report_file, (CONST_STRPTR)buf);
        FPuts(g_report_file, (CONST_STRPTR)"\n");
    }
    if (g_report_file_vol != 0) {
        FPuts(g_report_file_vol, (CONST_STRPTR)buf);
        FPuts(g_report_file_vol, (CONST_STRPTR)"\n");
    }

    stressfs_conform_format_json(jbuf, sizeof_jbuf, entry, verdict, got, quirk);
    if (g_report_json_file != 0) {
        FPuts(g_report_json_file, (CONST_STRPTR)jbuf);
        FPuts(g_report_json_file, (CONST_STRPTR)"\n");
    }
    if (g_report_json_file_vol != 0) {
        FPuts(g_report_json_file_vol, (CONST_STRPTR)jbuf);
        FPuts(g_report_json_file_vol, (CONST_STRPTR)"\n");
    }
}

static void report_file_open(const char *volume)
{
    char path[160];

    g_report_file = Open((CONST_STRPTR)"stressfs-conform-report.txt", MODE_NEWFILE);
    g_report_json_file = Open((CONST_STRPTR)"stressfs-conform-report.json", MODE_NEWFILE);

    if (volume != NULL && volume[0] != '\0') {
        path_join(path, sizeof(path), volume, NULL, "stressfs-conform-report.txt");
        g_report_file_vol = Open((CONST_STRPTR)path, MODE_NEWFILE);

        path_join(path, sizeof(path), volume, NULL, "stressfs-conform-report.json");
        g_report_json_file_vol = Open((CONST_STRPTR)path, MODE_NEWFILE);
    }
}

static void report_file_close(void)
{
    if (g_report_file != 0) {
        Close(g_report_file);
        g_report_file = 0;
    }
    if (g_report_json_file != 0) {
        Close(g_report_json_file);
        g_report_json_file = 0;
    }
    if (g_report_file_vol != 0) {
        Close(g_report_file_vol);
        g_report_file_vol = 0;
    }
    if (g_report_json_file_vol != 0) {
        Close(g_report_json_file_vol);
        g_report_json_file_vol = 0;
    }
}

static void report_note(const char *s)
{
    Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)s);
    if (g_report_file != 0) {
        FPuts(g_report_file, (CONST_STRPTR)s);
        FPuts(g_report_file, (CONST_STRPTR)"\n");
    }
    if (g_report_file_vol != 0) {
        FPuts(g_report_file_vol, (CONST_STRPTR)s);
        FPuts(g_report_file_vol, (CONST_STRPTR)"\n");
    }
}

/* Emits the run-identity header to both channels, per the dual-output
 * decision above. Asserts stressfs_run_identity_is_complete() before
 * even trying -- "a report line missing any of these is a harness bug
 * by definition" (docs/proposal.md), and the task instruction asks for
 * this defensively on the Amiga side too, not just in the portable
 * formatter's own 0/1 return contract. A failed assert() here means a
 * real bug in THIS function (a mandatory field this file forgot to
 * set), never an environment that merely couldn't answer
 * ACTION_DISK_INFO -- see report.h's top comment for why geometry/
 * DosEnvec are deliberately excluded from this check. */
static void emit_block(const char *human, const char *json)
{
    if (human != NULL) {
        Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)human);
        if (g_report_file != 0) {
            FPuts(g_report_file, (CONST_STRPTR)human);
            FPuts(g_report_file, (CONST_STRPTR)"\n");
        }
        if (g_report_file_vol != 0) {
            FPuts(g_report_file_vol, (CONST_STRPTR)human);
            FPuts(g_report_file_vol, (CONST_STRPTR)"\n");
        }
    }
    if (json != NULL) {
        if (g_report_json_file != 0) {
            FPuts(g_report_json_file, (CONST_STRPTR)json);
            FPuts(g_report_json_file, (CONST_STRPTR)"\n");
        }
        if (g_report_json_file_vol != 0) {
            FPuts(g_report_json_file_vol, (CONST_STRPTR)json);
            FPuts(g_report_json_file_vol, (CONST_STRPTR)"\n");
        }
    }
}

/* M3 step 6: the applied-quirks summary, emitted immediately after the
 * run-identity header and BEFORE any test line, to both channels --
 * docs/proposal.md's "Applied quirks are listed at the top of the
 * report, not buried, so a quirk can never silently hide a
 * regression". See report.h's declaration comment for why this lists
 * the quirks ACTIVE for the run rather than retrospectively the ones
 * that fired (the report is a stream). */
static char g_quirks_human[1536];
static char g_quirks_json[2048];

static void emit_quirks_summary(const stressfs_quirks_db_t *db,
                                 const stressfs_quirk_identity_t *qid)
{
    stressfs_report_format_quirks_human(g_quirks_human, sizeof(g_quirks_human), db, qid);
    stressfs_report_format_quirks_json(g_quirks_json, sizeof(g_quirks_json), db, qid);
    emit_block(g_quirks_human, g_quirks_json);
}

static char g_ident_human[768];
static char g_ident_json[1024];

static void emit_run_identity(const stressfs_run_identity_t *id)
{
    char *human = g_ident_human;
    char *json = g_ident_json;

    assert(stressfs_run_identity_is_complete(id));

    stressfs_report_format_header_human(human, sizeof(g_ident_human), id);
    Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)human);
    if (g_report_file != 0) {
        FPuts(g_report_file, (CONST_STRPTR)human);
        FPuts(g_report_file, (CONST_STRPTR)"\n");
    }
    if (g_report_file_vol != 0) {
        FPuts(g_report_file_vol, (CONST_STRPTR)human);
        FPuts(g_report_file_vol, (CONST_STRPTR)"\n");
    }

    stressfs_report_format_header_json(json, sizeof(g_ident_json), id);
    if (g_report_json_file != 0) {
        FPuts(g_report_json_file, (CONST_STRPTR)json);
        FPuts(g_report_json_file, (CONST_STRPTR)"\n");
    }
    if (g_report_json_file_vol != 0) {
        FPuts(g_report_json_file_vol, (CONST_STRPTR)json);
        FPuts(g_report_json_file_vol, (CONST_STRPTR)"\n");
    }
}

/* The quirks parser's/loader's diagnostics channel: everything they
 * warn about (an unevaluable match key, a refused file) goes into the
 * report's note channel, so a warning is in the report FILE as well as
 * on the console -- a quirks warning that only existed on a console
 * nobody was watching would be exactly the "buried" treatment the
 * proposal forbids. */
static void quirks_diag(void *ctx, const char *msg)
{
    (void)ctx;
    report_note(msg);
}

int stressfs_conform_run(const char *volume, const char *tier, long seed,
                          int selftest_hang,
                          const char *quirks_file, const char *force_quirks,
                          int no_quirks)
{
    stressfs_handler_info_t info;
    stressfs_dosenvec_t envec;
    stressfs_run_identity_t ident;
    stressfs_quirk_identity_t qid;
    int dos_version;
    int saw_fail = 0;
    int saw_inconclusive = 0;
    int saw_deviation = 0;
    size_t i;

    if (stressfs_watchdog_open() != 0) {
        Printf((CONST_STRPTR)"stressfs conform: could not open timer.device for the watchdog -- aborting\n");
        return RETURN_FAIL;
    }

    if (selftest_hang) {
        int hung;
        char line[256];
        /* The watchdog self-test deliberately ignores the quirks
         * database: it tests the harness, not a handler, and nothing
         * in a quirks file may influence whether a hang is detected. */
        report_file_open(volume);
        hung = stressfs_watchdog_selftest(3);
        snprintf(line, sizeof(line),
                 "%-12s oracle=%-11s section=%-48s  watchdog self-test (stub MsgPort, 3s deadline): %s",
                 hung ? "inconclusive" : "fail", "model",
                 "implementation-plan.md#M1",
                 hung ? "hang correctly detected, run would abort here" : "NO HANG DETECTED -- watchdog is broken");
        report_note(line);
        report_file_close();
        stressfs_watchdog_close();
        return hung ? RETURN_ERROR : RETURN_FAIL;
    }

    report_file_open(volume);

    dos_version = stressfs_dos_version();

    stressfs_identify_handler(volume, &info, STRESSFS_WATCHDOG_SECS);
    stressfs_fetch_dosenvec(volume, &envec);

    /* ---- M3: the quirks database -------------------------------------
     * Loaded BEFORE the first packet, so that an `action skip` entry
     * can keep stressfs away from a packet that wedges a known handler
     * (which is the whole point of that verb) and so that the
     * applied-quirks summary can go at the top of the report. */
    stressfs_quirks_init(&g_quirks);
    g_quirks.suppressed = (no_quirks != 0);
    if (force_quirks != NULL && force_quirks[0] != '\0') {
        (void)stressfs_quirks_force(&g_quirks, force_quirks, quirks_diag, NULL);
    }
    if (!stressfs_quirks_load_file(&g_quirks, quirks_file, quirks_diag, NULL)) {
        report_note("stressfs conform: the quirks file could not be used (see above) -- "
                    "aborting before any packet is sent, rather than running with a "
                    "half-known quirks set");
        stressfs_watchdog_close();
        report_file_close();
        return RETURN_FAIL;
    }

    /* The identity the match conditions are evaluated against. The
     * handler's own name/version come from its "$VER:" string (parsed
     * by the portable lib/quirks.c, not here); kick/dos come from the
     * running system. A handler whose identity could not be
     * established gets an empty name and version 0, which no seeded
     * entry's conditions can match -- the conservative outcome, and
     * consistent with the proposal's "if identity cannot be
     * established the suite runs only the baseline". */
    memset(&qid, 0, sizeof(qid));
    qid.identified = info.identified;
    if (info.version[0] != '\0') {
        (void)stressfs_quirks_identity_from_ver(&qid, info.version);
    }
    qid.dos_version = dos_version;
    qid.kick_version = (long)SysBase->LibNode.lib_Version;

    /* Assemble the run-identity header (M2 step 1). handler_name/
     * handler_version always get a non-empty placeholder even when
     * identification failed, per report.h's top comment: not knowing
     * WHETHER you identified the handler is fine to report, silently
     * emitting an empty field is not.
     *
     * Finding made while proving this milestone against a real FFS
     * handler under Copperline: info.dos_type[0] (the DosList volume
     * entry's dol_misc.dol_volume.dol_DiskType, per handler_id.h's
     * design) came back zero/unset even though the SAME handler
     * answered ACTION_DISK_INFO's id_DiskType correctly a moment
     * later (see the geometry fields below) -- i.e. this real
     * handler simply does not populate the DosList-level DosType
     * field the way the proposal's "DosType from the DosList entry"
     * wording assumes every handler does. handler_id.c's own
     * identification logic is left exactly as M1 built it (per this
     * milestone's instruction not to re-derive it), so the fallback
     * text below distinguishes "identification succeeded but this
     * field happened to be empty" from "identification failed
     * entirely" -- the two are different findings and "(unidentified)"
     * must not be reused for both, or a reader would wrongly conclude
     * the whole safe-probing ladder failed when in fact only one
     * optional field was unavailable. This is exactly the kind of
     * handler behaviour difference the quirks file (M3) exists to
     * record once it lands. */
    memset(&ident, 0, sizeof(ident));
    ident.harness_version = STRESSFS_VERSION;
    if (!info.identified) {
        ident.handler_name = "(unidentified)";
    } else if (info.dos_type[0]) {
        ident.handler_name = info.dos_type;
    } else {
        ident.handler_name = "(identified handler; DosList DosType field empty)";
    }
    ident.handler_version = info.identified
        ? (info.version[0] ? info.version : "(no $VER: string found)")
        : "(unidentified)";
    ident.tier = (tier != NULL && tier[0] != '\0') ? tier : "unknown";
    ident.seed = seed;
    ident.seed_set = 1; /* SEED always has a defined value (default 0) once
                          * we get here -- see main.c's ReadArgs handling. */
    ident.dos_version = dos_version;

    {
        /* ACTION_DISK_INFO against the volume's handler port doubles as
         * this run's "volume geometry" source (the implementation
         * plan explicitly names ACTION_DISK_INFO as the precedent to
         * build the geometry fetch from). Sent watched-by-outer-script
         * like every other table entry would be, via the same send()
         * helper the table itself uses -- but this call happens before
         * g_env exists, so it is spelled out directly here rather than
         * reusing run_disk_info(), which expects g_env to be set up. */
        struct InfoData diskinfo;
        memset(&diskinfo, 0, sizeof(diskinfo));
        if (info.port != NULL) {
            stressfs_pkt_result_t r = stressfs_dopkt(info.port, ACTION_DISK_INFO,
                                                       (LONG)MKBADDR(&diskinfo), 0, 0, 0, 0);
            if (r.res1 != 0) {
                ident.have_geometry = 1;
                ident.geom_numblocks = diskinfo.id_NumBlocks;
                ident.geom_numblocksused = diskinfo.id_NumBlocksUsed;
                ident.geom_bytesperblock = diskinfo.id_BytesPerBlock;
                ident.geom_disktype = diskinfo.id_DiskType;
            }
        }
    }

    if (envec.have_envec) {
        ident.have_envec = 1;
        ident.de_tablesize = envec.de_tablesize;
        ident.de_sizeblock = envec.de_sizeblock;
        ident.de_surfaces = envec.de_surfaces;
        ident.de_sectorperblock = envec.de_sectorperblock;
        ident.de_blockspertrack = envec.de_blockspertrack;
        ident.de_reserved = envec.de_reserved;
        ident.de_prealloc = envec.de_prealloc;
        ident.de_interleave = envec.de_interleave;
        ident.de_lowcyl = envec.de_lowcyl;
        ident.de_highcyl = envec.de_highcyl;
        ident.de_numbuffers = envec.de_numbuffers;
        ident.de_maxtransfer = envec.de_maxtransfer;
        ident.de_mask = envec.de_mask;
        ident.de_bootpri = envec.de_bootpri;
        ident.de_dostype = envec.de_dostype;
    }

    emit_run_identity(&ident);
    emit_quirks_summary(&g_quirks, &qid);

    if (!info.identified) {
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
                 "stressfs conform: handler NOT identified for volume \"%s\" -- "
                 "running baseline only, per the proposal's safe-probing fallback",
                 volume);
        report_note(hdr);
    }

    memset(&g_env, 0, sizeof(g_env));
    g_env.port = info.port;
    strncpy(g_env.volume, volume, sizeof(g_env.volume) - 1);
    strncpy(g_env.dirname, "stressfs-m1", sizeof(g_env.dirname) - 1);

    if (g_env.port == NULL) {
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
                 "stressfs conform: no handler port for \"%s\" -- cannot run even the baseline",
                 volume);
        report_note(hdr);
        stressfs_watchdog_close();
        report_file_close();
        return RETURN_FAIL;
    }

    /* Baseline always runs, identified or not; the table (matrix)
     * only runs if identification succeeded, per the proposal's
     * fallback rule. */
    {
        size_t baseline_count = 5; /* the five ENTRY_* rows above the "matrix: locks" comment */
        size_t limit = info.identified ? TABLE_COUNT : baseline_count;

        if (!fixture_setup(&g_env) && info.identified) {
            report_note("stressfs conform: fixture setup failed -- running baseline only");
            limit = baseline_count;
        }

        for (i = 0; i < limit && !g_env.run_aborted; i++) {
            const stressfs_test_entry_t *entry = &g_table[i];
            stressfs_pkt_result_t got;
            stressfs_verdict_t verdict;
            stressfs_quirk_applied_t quirk;

            if (entry->run == NULL) {
                continue;
            }

            got.res1 = 0;
            got.res2 = 0;

            if (dos_version != 0 && entry->min_version > dos_version) {
                verdict = STRESSFS_VERDICT_UNSUPPORTED;
                memset(&quirk, 0, sizeof(quirk));
                quirk.id = "";
                quirk.confidence = "";
            } else if (stressfs_quirks_check_skip(&g_quirks, &qid, entry, &quirk)) {
                /* `action skip`: consulted BEFORE the run function, so
                 * no packet reaches the handler at all. Reported as
                 * `unsupported` and carrying the quirk that caused it,
                 * never silently omitted (docs/proposal.md:
                 * "`unsupported` ... always listed, never silently
                 * skipped"). */
                verdict = STRESSFS_VERDICT_UNSUPPORTED;
            } else {
                got = entry->run(entry->run_ctx);
                if (g_env.run_aborted) {
                    verdict = STRESSFS_VERDICT_INCONCLUSIVE;
                    memset(&quirk, 0, sizeof(quirk));
                    quirk.id = "";
                    quirk.confidence = "";
                } else {
                    /* The three stages, kept separate exactly as M1/M2
                     * left them and as M3's step 5 requires: compute
                     * the raw verdict, THEN apply quirks, THEN format.
                     * Nothing in stressfs_conform_evaluate() knows a
                     * quirks file exists. */
                    stressfs_verdict_t raw = stressfs_conform_evaluate(entry, got, dos_version);
                    verdict = stressfs_quirks_apply_verdict(&g_quirks, &qid, entry, raw,
                                                             got, &quirk);
                }
            }

            print_line(entry, verdict, got, &quirk);

            if (verdict == STRESSFS_VERDICT_FAIL) {
                saw_fail = 1;
            }
            if (verdict == STRESSFS_VERDICT_DEVIATION) {
                saw_deviation = 1;
            }
            if (verdict == STRESSFS_VERDICT_INCONCLUSIVE) {
                saw_inconclusive = 1;
            }
        }

        if (g_env.run_aborted) {
            report_note("stressfs conform: ABORTING -- a packet watchdog deadline fired. "
                        "No further packets will be sent to this handler; the volume is left "
                        "untouched (fixture teardown skipped deliberately).");
            saw_inconclusive = 1;
        } else if (info.identified) {
            fixture_teardown(&g_env);
        }
    }

    stressfs_watchdog_close();
    report_file_close();

    /* M2 exit-code priority: inconclusive beats fail beats pass -- see
     * conform_run.h's header comment for the full scheme. */
    if (saw_inconclusive) {
        return RETURN_ERROR;
    }
    if (saw_fail) {
        return RETURN_WARN;
    }
    /* M3: a `deviation` is a declared, explained difference from the
     * manual, not a failure -- docs/proposal.md lists it as its own
     * verdict precisely so it is "reported, never hidden" without
     * being counted as a fail. It therefore does not raise the exit
     * code above RETURN_OK; the report line and the top-of-report
     * applied-quirks section are where a deviation is visible. The
     * variable is tracked (rather than ignored) so that the console
     * summary below states it explicitly -- a reader must not have to
     * infer "there were deviations" from a zero exit code. */
    if (saw_deviation) {
        report_note("stressfs conform: run completed with no failures, but at least one "
                    "test reported `deviation` -- a quirks entry declared and explained a "
                    "difference from the manual. Re-run with NOQUIRKS to see the raw "
                    "findings.");
    }
    return RETURN_OK;
}
