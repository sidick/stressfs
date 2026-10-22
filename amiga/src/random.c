/* amiga/src/random.c -- the M4 `stressfs random` engine: drives the
 * portable generator+model (amiga/lib/random_ops.c, random_model.c)
 * against a real volume, via plain dos.library wrapper calls (Open,
 * Read, Write, Seek, Close, DeleteFile, Rename, CreateDir, Lock,
 * Examine, ExNext, SetFileSize) -- not raw DoPkt(), per M1's
 * precedent for "wrapper twin" tests and because M4 explicitly scopes
 * out packet mode (docs/proposal.md's "the same generator can send
 * each operation as a raw packet" is phase 2's "packet mode" knob).
 *
 * Structure, mirroring amiga/src/conform.c's own layout:
 *
 *   1. stressfs_random_run() is the entry point. It opens the fixture
 *      directory (idempotent, same "stressfs-m4" scoped-subdirectory
 *      discipline every earlier milestone uses), checks the working
 *      set against AvailMem(), then runs the step loop.
 *   2. Each step: generate (bounded retry), compute the model's
 *      portable expectation from the PRE-apply model state, execute
 *      the SAME operation against the real volume, compare, print one
 *      report line, then apply the operation to the model. On
 *      divergence the loop stops immediately (an fsx-style tester's
 *      "the first divergence is the bug") -- the JSON report already
 *      on disk, carrying the run's seed in its header and the
 *      diverging step's own line, IS the operation log `stressfs
 *      replay` consumes (see stressfs/replay.h).
 *   3. Every `verify_every` steps, an additional full-tree compare
 *      (Examine+ExNext of the fixture root and every live
 *      subdirectory) catches anything a per-step check alone would
 *      miss (e.g. a stray leftover entry neither side's last op
 *      touched).
 *
 * Hang protection: same as M1/M3 -- plain dos.library calls, so the
 * outer script-level timeout (ci/copperline-m4.sh) is the watchdog,
 * not anything per-call here (see packet.h's header comment for why a
 * per-packet watchdog cannot wrap a wrapper call either).
 */

#include "stressfs/random_run.h"
#include "stressfs/random_ops.h"
#include "stressfs/random_model.h"
#include "stressfs/prng.h"
#include "stressfs/report.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <assert.h>
#include <string.h>
#include <stdio.h>

#ifndef RETURN_ERROR
#define RETURN_ERROR 10
#endif

/* M4 design decision: full-tree compares every 200 steps. The
 * proposal asks for "every N steps" without pinning N; 200 is chosen
 * as a round number that, at the >= 10000-op exit criterion, still
 * runs the (more expensive, O(live nodes)) full-tree compare 50+
 * times over a run -- frequent enough to catch a listing drift well
 * before the run ends, cheap enough (one Examine+ExNext walk of at
 * most two directory levels) not to dominate the run's time next to
 * 199 ordinary single-packet-ish steps. */
#define STRESSFS_RANDOM_DEFAULT_VERIFY_EVERY 200
#define STRESSFS_RANDOM_DEFAULT_OPS 1000
#define STRESSFS_RANDOM_MAX_GEN_RETRIES 8

/* Max bytes a single WRITE/READ step moves -- bounds the static
 * buffers below. Matches random_ops.c's own generator bound (up to
 * ~8192 bytes, biased small); kept here as its own constant rather
 * than shared so a change to one is a deliberate, visible edit to
 * both. */
#define STRESSFS_RANDOM_MAX_IO 8448

/* Shared cap for any in-memory directory-listing snapshot this file
 * takes (wipe_directory() at startup, compare_listing() per step/
 * full-tree-compare) -- generous relative to STRESSFS_MODEL_MAX_NODES
 * fan-out at any single directory level in practice. */
#define MAX_LISTING_NAMES 128

static char g_volume[32];
static char g_dirname[32] = "stressfs-m4";
static BPTR g_real_fh = 0;
static uint32_t g_seed;
static stressfs_model_t g_model;
static stressfs_prng_t g_prng;

static BPTR g_report_file = 0;
static BPTR g_report_json_file = 0;
static BPTR g_report_file_vol = 0;
static BPTR g_report_json_file_vol = 0;

static void path_join(char *out, size_t outsz, const char *volume, const char *dir, const char *leaf)
{
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

/* Builds the on-disk path for a prospective child named `name` under
 * directory `parent_idx` (-1 = fixture root). `name` may be empty to
 * get the directory's own path. */
static void build_path_for(const char *volume, const char *dirname, const stressfs_model_t *model,
                            int parent_idx, const char *name, char *buf, size_t bufsz)
{
    char rel[2 * STRESSFS_RANDOM_NAME_LEN + 2];
    rel[0] = '\0';
    if (parent_idx >= 0) {
        strncat(rel, model->nodes[parent_idx].name, sizeof(rel) - 1);
        if (name != NULL && name[0] != '\0') {
            strncat(rel, "/", sizeof(rel) - strlen(rel) - 1);
        }
    }
    if (name != NULL && name[0] != '\0') {
        strncat(rel, name, sizeof(rel) - strlen(rel) - 1);
    }
    path_join(buf, bufsz, volume, dirname, rel);
}

static void build_path_for_node(const char *volume, const char *dirname, const stressfs_model_t *model,
                                 int node_idx, char *buf, size_t bufsz)
{
    if (node_idx < 0) {
        path_join(buf, bufsz, volume, dirname, "");
        return;
    }
    build_path_for(volume, dirname, model, model->nodes[node_idx].parent,
                    model->nodes[node_idx].name, buf, bufsz);
}

static void report_note(const char *s)
{
    Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)s);
    if (g_report_file != 0) { FPuts(g_report_file, (CONST_STRPTR)s); FPuts(g_report_file, (CONST_STRPTR)"\n"); }
    if (g_report_file_vol != 0) { FPuts(g_report_file_vol, (CONST_STRPTR)s); FPuts(g_report_file_vol, (CONST_STRPTR)"\n"); }
}

static void emit_block(const char *human, const char *json)
{
    if (human != NULL) {
        Printf((CONST_STRPTR)"%s\n", (CONST_STRPTR)human);
        if (g_report_file != 0) { FPuts(g_report_file, (CONST_STRPTR)human); FPuts(g_report_file, (CONST_STRPTR)"\n"); }
        if (g_report_file_vol != 0) { FPuts(g_report_file_vol, (CONST_STRPTR)human); FPuts(g_report_file_vol, (CONST_STRPTR)"\n"); }
    }
    if (json != NULL) {
        if (g_report_json_file != 0) { FPuts(g_report_json_file, (CONST_STRPTR)json); FPuts(g_report_json_file, (CONST_STRPTR)"\n"); }
        if (g_report_json_file_vol != 0) { FPuts(g_report_json_file_vol, (CONST_STRPTR)json); FPuts(g_report_json_file_vol, (CONST_STRPTR)"\n"); }
    }
}

static char g_line_buf[512];
static char g_line_jbuf[768];

static void print_step(const stressfs_random_step_t *s)
{
    stressfs_report_format_step_human(g_line_buf, sizeof(g_line_buf), s);
    stressfs_report_format_step_json(g_line_jbuf, sizeof(g_line_jbuf), s);
    emit_block(g_line_buf, g_line_jbuf);
}

static char g_ident_human[768];
static char g_ident_json[1024];

static void emit_run_identity(const stressfs_run_identity_t *id)
{
    assert(stressfs_run_identity_is_complete(id));
    stressfs_report_format_header_human(g_ident_human, sizeof(g_ident_human), id);
    stressfs_report_format_header_json(g_ident_json, sizeof(g_ident_json), id);
    emit_block(g_ident_human, g_ident_json);
}

static void report_file_open(const char *volume)
{
    char path[160];
    g_report_file = Open((CONST_STRPTR)"stressfs-random-report.txt", MODE_NEWFILE);
    g_report_json_file = Open((CONST_STRPTR)"stressfs-random-report.json", MODE_NEWFILE);
    if (volume != NULL && volume[0] != '\0') {
        path_join(path, sizeof(path), volume, NULL, "stressfs-random-report.txt");
        g_report_file_vol = Open((CONST_STRPTR)path, MODE_NEWFILE);
        path_join(path, sizeof(path), volume, NULL, "stressfs-random-report.json");
        g_report_json_file_vol = Open((CONST_STRPTR)path, MODE_NEWFILE);
    }
}

static void report_file_close(void)
{
    if (g_report_file != 0) { Close(g_report_file); g_report_file = 0; }
    if (g_report_json_file != 0) { Close(g_report_json_file); g_report_json_file = 0; }
    if (g_report_file_vol != 0) { Close(g_report_file_vol); g_report_file_vol = 0; }
    if (g_report_json_file_vol != 0) { Close(g_report_json_file_vol); g_report_json_file_vol = 0; }
}

/* ------------------------------------------------------------------ */
/* Fixture setup/teardown.                                            */
/* ------------------------------------------------------------------ */

/* Deletes every entry currently under `dirpath` (one level: files
 * directly, sub-directories after first emptying them) -- best
 * effort, errors ignored (an entry that fails to delete here is not
 * this function's problem to diagnose; the caller's own packets will
 * surface a real problem soon enough if the volume itself is at
 * fault). */
/* Deletes entries ONE AT A TIME (re-locking and re-scanning from the
 * start after each deletion) rather than snapshotting the whole
 * listing into a stack array first. Found necessary the hard way
 * proving this milestone: a prior version collected up to
 * MAX_LISTING_NAMES entries into stack-local arrays (~3.5 KB) and
 * recursed into sub-directories while that frame was still live --
 * harmless-looking, but AmigaDOS gives a CLI process a famously small
 * default stack (often 4 KB), and a fixture left dirty by an earlier
 * aborted run (more leftover entries than usual) pushed this well
 * past it, SILENTLY corrupting unrelated stack state rather than
 * crashing outright -- observed as the generator producing a
 * DIFFERENT, non-reproducible operation at the same step number on
 * what should have been an identical seeded run, which is about as
 * confusing a symptom as a stack overflow can produce. This version's
 * only per-call locals are a FileInfoBlock and two short path buffers,
 * and recursion is bounded to depth 2 regardless of entry count
 * (random_model.h's MAX_DEPTH-1 decision: a directory's children are
 * never themselves directories with further children). */
static void wipe_directory(const char *dirpath)
{
    for (;;) {
        BPTR lock;
        struct FileInfoBlock fib;
        char name[STRESSFS_MODEL_NAME_LEN];
        int is_dir = 0;
        int found = 0;

        lock = Lock((CONST_STRPTR)dirpath, SHARED_LOCK);
        if (lock == 0) {
            return;
        }
        if (Examine(lock, &fib) && ExNext(lock, &fib)) {
            strncpy(name, (const char *)fib.fib_FileName, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            is_dir = (fib.fib_DirEntryType > 0);
            found = 1;
        }
        UnLock(lock);

        if (!found) {
            return; /* directory is empty */
        }

        {
            char child[200];
            /* NOT path_join(): that helper assumes its first argument
             * ends in ':' (a volume), but `dirpath` here is already a
             * full path that may or may not -- insert '/' explicitly. */
            child[0] = '\0';
            strncat(child, dirpath, sizeof(child) - 1);
            strncat(child, "/", sizeof(child) - strlen(child) - 1);
            strncat(child, name, sizeof(child) - strlen(child) - 1);
            if (is_dir) {
                wipe_directory(child);
            }
            DeleteFile((CONST_STRPTR)child);
        }
    }
}

static int fixture_setup(void)
{
    char path[160];
    BPTR lock;
    LONG create_err;

    path_join(path, sizeof(path), g_volume, NULL, g_dirname);
    lock = CreateDir((CONST_STRPTR)path);
    /* Captured IMMEDIATELY, before any other dos.library call this
     * process makes -- IoErr() is reset by the next one (found the
     * hard way while proving this milestone: an earlier version of
     * this function read IoErr() again after a diagnostic
     * report_note()/Printf() had already run, by which point it had
     * silently become 0, so the ERROR_OBJECT_EXISTS branch below was
     * never taken on a second run against the same persistent
     * Copperline volume). */
    create_err = (lock == 0) ? IoErr() : 0;

    if (lock == 0) {
        int tried_lock = (create_err == ERROR_OBJECT_EXISTS);
        LONG lock_err = 0;
        if (tried_lock) {
            lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
            lock_err = (lock == 0) ? IoErr() : 0;
        }
        if (lock == 0) {
            char hdr[320];
            if (tried_lock) {
                snprintf(hdr, sizeof(hdr),
                         "stressfs random: CreateDir(\"%s\") found ERROR_OBJECT_EXISTS, but the "
                         "fallback Lock() also failed (IoErr=%ld)", path, (long)lock_err);
            } else {
                snprintf(hdr, sizeof(hdr),
                         "stressfs random: CreateDir(\"%s\") failed (IoErr=%ld), not "
                         "ERROR_OBJECT_EXISTS so no fallback was attempted", path, (long)create_err);
            }
            report_note(hdr);
            return 0;
        }
        /* The fixture directory survived from a PRIOR run that
         * diverged (fixture_teardown() is deliberately skipped on
         * divergence -- see this file's own header comment -- so the
         * directory, and everything the model from that earlier run
         * put in it, is still there). The in-memory model for THIS
         * run always starts empty, so any real leftover entry would
         * desync it on step 1 -- found the hard way running this
         * milestone's own Copperline exit-criterion check twice in a
         * row with the same seed: the very first MKDIR collided with
         * last run's leftover directory of the same (deterministic)
         * name. Wiping here, once, up front, is the fix, and is a
         * test-fixture hygiene concern, not a finding about the
         * handler under test. */
        wipe_directory(path);
    }
    UnLock(lock);
    return 1;
}

/* Best-effort: deletes every entry the model still believes is live,
 * deepest first (files, then the directories that held them), so a
 * clean run never leaves "stressfs-m4" behind on the volume. Not
 * trusted after a divergence -- the caller skips this entirely then,
 * same discipline as M1's fixture_teardown(). */
static void fixture_teardown(void)
{
    int i;
    char path[200];

    for (i = 0; i < g_model.n_slots; i++) {
        if (g_model.nodes[i].in_use && g_model.nodes[i].type == STRESSFS_NODE_FILE) {
            build_path_for_node(g_volume, g_dirname, &g_model, i, path, sizeof(path));
            DeleteFile((CONST_STRPTR)path);
        }
    }
    for (i = 0; i < g_model.n_slots; i++) {
        if (g_model.nodes[i].in_use && g_model.nodes[i].type == STRESSFS_NODE_DIR) {
            build_path_for_node(g_volume, g_dirname, &g_model, i, path, sizeof(path));
            DeleteFile((CONST_STRPTR)path);
        }
    }
    path_join(path, sizeof(path), g_volume, NULL, g_dirname);
    DeleteFile((CONST_STRPTR)path);
}

/* ------------------------------------------------------------------ */
/* Working-set estimate (M4 step 4).                                  */
/* ------------------------------------------------------------------ */

/* The model's own node table is the dominant, and only non-constant,
 * cost: everything else (the prng state, the step-report buffers
 * above) is a handful of fixed-size static objects already counted in
 * the binary's own BSS, not something the run itself grows. So the
 * "working set" the proposal asks the tool to report is simply
 * sizeof(stressfs_model_t) (dominated by STRESSFS_MODEL_MAX_NODES *
 * stressfs_model_node_bytes()) plus a fixed ~16 KB margin for the
 * report line buffers and dos.library's own per-call overhead
 * (FileHandle/FileLock objects it allocates internally, which this
 * code does not control directly).
 *
 * At STRESSFS_MODEL_MAX_NODES nodes (random_model.h), this comes to a
 * few tens of KB -- comfortably inside the proposal's "4 MB comfortable
 * target" even on a stock A1200's stock 2 MB chip RAM, so the refusal
 * below is a real, if rarely-tripped, safety net rather than
 * decoration: it is what makes running stressfs on a genuinely
 * memory-starved configuration (a bare A500 with no Fast RAM, several
 * other tools already resident) fail loudly with a clear number
 * instead of AllocMem() silently failing deep inside some later
 * operation's dos.library call. */
static unsigned long working_set_bytes(void)
{
    return (unsigned long)sizeof(stressfs_model_t) + 16UL * 1024UL;
}

/* Requires at least double the estimated working set to actually be
 * free -- a safety margin, not a tight fit, since dos.library and the
 * handler under test both need headroom of their own that this
 * estimate cannot see. Returns 1 if the run may proceed. */
static int working_set_check(unsigned long *out_need, unsigned long *out_avail)
{
    unsigned long need = working_set_bytes();
    unsigned long avail = (unsigned long)AvailMem(MEMF_ANY);
    *out_need = need;
    *out_avail = avail;
    return avail >= need * 2UL;
}

/* ------------------------------------------------------------------ */
/* Directory listing comparison (EXNEXT op and the full-tree compare). */
/* ------------------------------------------------------------------ */

/* Walks the real directory at `dirpath` and checks that its entry set
 * is EXACTLY the model's live children of `dirnode` (-1 = root) --
 * same names, byte-for-byte (AmigaDOS is case-preserving, so a correct
 * handler returns exactly what was created), set equality regardless
 * of order. Fills `reason` on mismatch. */
/* STATIC, not automatic -- same reasoning as wipe_directory()'s header
 * comment and the identical policy in amiga/src/conform.c (its own
 * "STATIC, not automatic" comment on g_line_buf/g_line_jbuf): this
 * ~3 KB table on a 4 KB-stack AmigaDOS CLI process is a real overflow
 * risk, not a style nit. compare_listing() is never called
 * re-entrantly (no caller invokes it from within another call to
 * itself), so one shared instance is safe. */
static char g_real_names[MAX_LISTING_NAMES][STRESSFS_MODEL_NAME_LEN];

static int compare_listing(const char *dirpath, const stressfs_model_t *model, int dirnode,
                            char *reason, size_t reasonsz)
{
    BPTR lock;
    struct FileInfoBlock fib;
    char (*real_names)[STRESSFS_MODEL_NAME_LEN] = g_real_names;
    int n_real = 0;
    int expected = stressfs_model_count_children(model, dirnode);
    int i, j;

    lock = Lock((CONST_STRPTR)dirpath, SHARED_LOCK);
    if (lock == 0) {
        snprintf(reason, reasonsz, "could not lock \"%s\" for listing (IoErr=%ld)",
                 dirpath, (long)IoErr());
        return 0;
    }
    if (!Examine(lock, &fib)) {
        snprintf(reason, reasonsz, "Examine(\"%s\") failed (IoErr=%ld)", dirpath, (long)IoErr());
        UnLock(lock);
        return 0;
    }
    while (n_real < MAX_LISTING_NAMES && ExNext(lock, &fib)) {
        strncpy(real_names[n_real], (const char *)fib.fib_FileName, STRESSFS_MODEL_NAME_LEN - 1);
        real_names[n_real][STRESSFS_MODEL_NAME_LEN - 1] = '\0';
        n_real++;
    }
    UnLock(lock);

    if (n_real != expected) {
        snprintf(reason, reasonsz,
                 "\"%s\" has %d real entries, model expects %d",
                 dirpath, n_real, expected);
        return 0;
    }

    for (i = 0; i < expected; i++) {
        int node = stressfs_model_nth_child(model, dirnode, i);
        int found = 0;
        for (j = 0; j < n_real; j++) {
            if (strcmp(model->nodes[node].name, real_names[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            snprintf(reason, reasonsz,
                     "\"%s\": model entry \"%s\" not found in the real listing",
                     dirpath, model->nodes[node].name);
            return 0;
        }
    }

    return 1;
}

static int full_tree_compare(char *reason, size_t reasonsz)
{
    char path[200];
    int i;

    path_join(path, sizeof(path), g_volume, NULL, g_dirname);
    if (!compare_listing(path, &g_model, -1, reason, reasonsz)) {
        return 0;
    }
    for (i = 0; i < g_model.n_slots; i++) {
        if (g_model.nodes[i].in_use && g_model.nodes[i].type == STRESSFS_NODE_DIR) {
            build_path_for_node(g_volume, g_dirname, &g_model, i, path, sizeof(path));
            if (!compare_listing(path, &g_model, i, reason, reasonsz)) {
                return 0;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Executing one generated op against the real volume.                 */
/* ------------------------------------------------------------------ */

static UBYTE g_io_buf[STRESSFS_RANDOM_MAX_IO];

/* Fills `real` with {res1,res2} and, for EXNEXT, performs the listing
 * comparison itself (folding the result into `real`/`diverged`
 * directly, since that check has no single {res1,res2} shape).
 * `diverged`/`reason` are OUTPUT-ONLY content/listing divergence
 * signals beyond the plain {res1,res2} compare the caller already
 * does against stressfs_model_expect(); initialised to "no extra
 * divergence" on entry. */
static void execute_op(const stressfs_op_t *op, long *res1, long *res2,
                        int *diverged, char *reason, size_t reasonsz)
{
    char path[200], path2[200];

    *diverged = 0;
    reason[0] = '\0';

    switch (op->kind) {
    case STRESSFS_OP_CREATE: {
        BPTR fh;
        build_path_for(g_volume, g_dirname, &g_model, op->parent_node, op->new_name, path, sizeof(path));
        fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
        *res1 = (fh != 0) ? 1 : 0;
        *res2 = (fh != 0) ? 0 : IoErr();
        g_real_fh = fh;
        break;
    }
    case STRESSFS_OP_OPEN: {
        BPTR fh;
        LONG mode = (op->open_mode == STRESSFS_OPEN_READ) ? MODE_OLDFILE : MODE_READWRITE;
        build_path_for_node(g_volume, g_dirname, &g_model, op->target_node, path, sizeof(path));
        fh = Open((CONST_STRPTR)path, mode);
        *res1 = (fh != 0) ? 1 : 0;
        *res2 = (fh != 0) ? 0 : IoErr();
        g_real_fh = fh;
        break;
    }
    case STRESSFS_OP_WRITE: {
        LONG n;
        uint32_t file_id = g_model.nodes[g_model.open_node].file_id;
        uint32_t base = g_model.open_pos;
        uint32_t i;
        uint32_t len = op->length;
        if (len > sizeof(g_io_buf)) len = sizeof(g_io_buf);
        for (i = 0; i < len; i++) {
            g_io_buf[i] = stressfs_model_content_byte(g_seed, file_id, base + i);
        }
        n = Write(g_real_fh, g_io_buf, (LONG)len);
        *res1 = n;
        *res2 = (n < 0) ? IoErr() : 0;
        break;
    }
    case STRESSFS_OP_READ: {
        LONG n;
        uint32_t base = g_model.open_pos;
        uint32_t len = op->length;
        if (len > sizeof(g_io_buf)) len = sizeof(g_io_buf);
        n = Read(g_real_fh, g_io_buf, (LONG)len);
        *res1 = n;
        *res2 = (n < 0) ? IoErr() : 0;
        if (n > 0) {
            LONG i;
            uint32_t file_id = g_model.nodes[g_model.open_node].file_id;
            for (i = 0; i < n; i++) {
                UBYTE want;
                if (!stressfs_model_byte_is_defined(&g_model, g_model.open_node, base + (uint32_t)i)) {
                    continue; /* grown-but-never-written region: handler-specific, not checked at M4 */
                }
                want = stressfs_model_content_byte(g_seed, file_id, base + (uint32_t)i);
                if (g_io_buf[i] != want) {
                    *diverged = 1;
                    snprintf(reason, reasonsz,
                             "content mismatch at file offset %lu: got 0x%02x want 0x%02x",
                             (unsigned long)(base + (uint32_t)i), g_io_buf[i], want);
                    break;
                }
            }
        }
        break;
    }
    case STRESSFS_OP_SEEK: {
        LONG old = Seek(g_real_fh, op->seek_offset, OFFSET_BEGINNING);
        *res1 = old;
        *res2 = (old < 0) ? IoErr() : 0;
        break;
    }
    case STRESSFS_OP_CLOSE: {
        BOOL ok = Close(g_real_fh);
        g_real_fh = 0;
        *res1 = ok ? 1 : 0;
        *res2 = 0;
        break;
    }
    case STRESSFS_OP_DELETE: {
        BOOL ok;
        build_path_for_node(g_volume, g_dirname, &g_model, op->target_node, path, sizeof(path));
        ok = DeleteFile((CONST_STRPTR)path);
        *res1 = ok ? 1 : 0;
        *res2 = ok ? 0 : IoErr();
        break;
    }
    case STRESSFS_OP_RENAME: {
        BOOL ok;
        build_path_for_node(g_volume, g_dirname, &g_model, op->target_node, path, sizeof(path));
        build_path_for(g_volume, g_dirname, &g_model, op->dest_parent_node, op->new_name, path2, sizeof(path2));
        ok = Rename((CONST_STRPTR)path, (CONST_STRPTR)path2);
        *res1 = ok ? 1 : 0;
        *res2 = ok ? 0 : IoErr();
        break;
    }
    case STRESSFS_OP_MKDIR: {
        BPTR lock;
        build_path_for(g_volume, g_dirname, &g_model, op->parent_node, op->new_name, path, sizeof(path));
        lock = CreateDir((CONST_STRPTR)path);
        *res1 = (lock != 0) ? 1 : 0;
        *res2 = (lock != 0) ? 0 : IoErr();
        if (lock != 0) {
            UnLock(lock);
        }
        break;
    }
    case STRESSFS_OP_EXNEXT: {
        char lreason[160];
        build_path_for_node(g_volume, g_dirname, &g_model, op->target_node, path, sizeof(path));
        if (compare_listing(path, &g_model, op->target_node, lreason, sizeof(lreason))) {
            *res1 = 1; *res2 = 0;
        } else {
            *res1 = 0; *res2 = 0;
            *diverged = 1;
            strncpy(reason, lreason, reasonsz - 1);
            reason[reasonsz - 1] = '\0';
        }
        break;
    }
    case STRESSFS_OP_SETSIZE: {
        LONG newsize = SetFileSize(g_real_fh, (LONG)op->setsize_new_size, OFFSET_BEGINNING);
        *res1 = newsize;
        *res2 = (newsize < 0) ? IoErr() : 0;
        break;
    }
    default:
        *res1 = 0; *res2 = 0;
        break;
    }
}

static const char *describe_op(const stressfs_op_t *op)
{
    static char buf[160];
    switch (op->kind) {
    case STRESSFS_OP_CREATE:
        snprintf(buf, sizeof(buf), "create parent=%d name=%s", op->parent_node, op->new_name);
        break;
    case STRESSFS_OP_OPEN:
        snprintf(buf, sizeof(buf), "open target=%d mode=%d", op->target_node, (int)op->open_mode);
        break;
    case STRESSFS_OP_WRITE:
        snprintf(buf, sizeof(buf), "write len=%lu at pos=%lu", (unsigned long)op->length,
                 (unsigned long)g_model.open_pos);
        break;
    case STRESSFS_OP_READ:
        snprintf(buf, sizeof(buf), "read len=%lu at pos=%lu", (unsigned long)op->length,
                 (unsigned long)g_model.open_pos);
        break;
    case STRESSFS_OP_SEEK:
        snprintf(buf, sizeof(buf), "seek to=%ld", (long)op->seek_offset);
        break;
    case STRESSFS_OP_CLOSE:
        snprintf(buf, sizeof(buf), "close");
        break;
    case STRESSFS_OP_DELETE:
        snprintf(buf, sizeof(buf), "delete target=%d", op->target_node);
        break;
    case STRESSFS_OP_RENAME:
        snprintf(buf, sizeof(buf), "rename target=%d dest_parent=%d new_name=%s",
                 op->target_node, op->dest_parent_node, op->new_name);
        break;
    case STRESSFS_OP_MKDIR:
        snprintf(buf, sizeof(buf), "mkdir parent=%d name=%s", op->parent_node, op->new_name);
        break;
    case STRESSFS_OP_EXNEXT:
        snprintf(buf, sizeof(buf), "exnext dir=%d", op->target_node);
        break;
    case STRESSFS_OP_SETSIZE:
        snprintf(buf, sizeof(buf), "setsize %s to=%lu", op->setsize_grow ? "grow" : "truncate",
                 (unsigned long)op->setsize_new_size);
        break;
    default:
        buf[0] = '\0';
        break;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Driver.                                                             */
/* ------------------------------------------------------------------ */

int stressfs_random_run(const char *volume, const char *tier, long seed,
                         long ops, long verify_every, int break_model,
                         long stop_after)
{
    stressfs_run_identity_t ident;
    unsigned long need, avail;
    long step;
    int diverged_overall = 0;

    g_seed = (uint32_t)seed;
    strncpy(g_volume, volume, sizeof(g_volume) - 1);
    if (ops <= 0) ops = STRESSFS_RANDOM_DEFAULT_OPS;
    if (verify_every <= 0) verify_every = STRESSFS_RANDOM_DEFAULT_VERIFY_EVERY;

    report_file_open(volume);

    memset(&ident, 0, sizeof(ident));
    ident.harness_version = STRESSFS_VERSION;
    ident.handler_name = "(model-only; see run below for operation kinds exercised)";
    ident.handler_version = "(not identified by `random`; see `conform` for handler identity)";
    ident.tier = (tier != NULL && tier[0] != '\0') ? tier : "unknown";
    ident.seed = seed;
    ident.seed_set = 1;
    ident.dos_version = 0;
    emit_run_identity(&ident);

    if (!working_set_check(&need, &avail)) {
        char hdr[200];
        snprintf(hdr, sizeof(hdr),
                 "stressfs random: REFUSING TO RUN -- estimated working set %lu bytes needs "
                 "at least %lu bytes free (2x margin), AvailMem(MEMF_ANY) reports only %lu",
                 need, need * 2UL, avail);
        report_note(hdr);
        report_file_close();
        return RETURN_FAIL;
    }
    {
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
                 "stressfs random: working set estimate %lu bytes (AvailMem reports %lu free)",
                 need, avail);
        report_note(hdr);
    }

    if (!fixture_setup()) {
        report_file_close();
        return RETURN_FAIL;
    }

    stressfs_model_init(&g_model);
    stressfs_prng_seed(&g_prng, g_seed);

    if (break_model) {
        /* The M4 self-test (mirrors M1's SELFTESTHANG): one scripted
         * create+write against the REAL volume, with a DELIBERATELY
         * wrong model expectation for the write, to prove the
         * comparison logic detects it end to end without needing a
         * real misbehaving handler. */
        stressfs_op_t op;
        long res1, res2;
        int extra_diverged;
        char reason[160];
        stressfs_random_step_t s;

        memset(&op, 0, sizeof(op));
        op.kind = STRESSFS_OP_CREATE;
        op.parent_node = -1;
        strcpy(op.new_name, "selftest.txt");
        execute_op(&op, &res1, &res2, &extra_diverged, reason, sizeof(reason));
        stressfs_model_apply(&g_model, &op);

        memset(&op, 0, sizeof(op));
        op.kind = STRESSFS_OP_WRITE;
        op.length = 32;
        execute_op(&op, &res1, &res2, &extra_diverged, reason, sizeof(reason));

        memset(&s, 0, sizeof(s));
        s.step = 1;
        s.op_kind = stressfs_op_kind_name(op.kind);
        s.detail = describe_op(&op);
        s.real_res1 = res1;
        s.real_res2 = res2;
        s.expect_res1 = res1 + 1; /* DELIBERATELY WRONG: the real write actually returned `res1` bytes */
        s.expect_res2 = 0;
        s.diverged = 1;
        s.diverge_reason = "SELFTEST: model expectation deliberately corrupted (expect_res1 = real_res1 + 1)";
        print_step(&s);

        stressfs_model_apply(&g_model, &op);
        fixture_teardown();
        report_file_close();
        report_note("stressfs random: self-test complete -- the deliberately wrong expectation "
                    "above was correctly reported as a divergence");
        return RETURN_ERROR;
    }

    for (step = 1; step <= ops; step++) {
        stressfs_op_t op;
        int tries, ok = 0;
        stressfs_model_expect_t expect;
        long res1 = 0, res2 = 0;
        int extra_diverged = 0;
        char reason[160];
        stressfs_random_step_t s;
        int step_diverged;

        for (tries = 0; tries < STRESSFS_RANDOM_MAX_GEN_RETRIES && !ok; tries++) {
            ok = stressfs_random_generate(&g_prng, &g_model, &op);
        }
        if (!ok) {
            report_note("stressfs random: generator found nothing legal to do -- stopping early");
            break;
        }

        expect = stressfs_model_expect(&g_model, &op);
        reason[0] = '\0';
        execute_op(&op, &res1, &res2, &extra_diverged, reason, sizeof(reason));

        step_diverged = extra_diverged ||
            (!expect.res1_any && res1 != expect.res1) ||
            (!expect.res2_any && res2 != expect.res2);

        if (step_diverged && !extra_diverged) {
            snprintf(reason, sizeof(reason), "res1/res2 mismatch (model vs. real)");
        }

        memset(&s, 0, sizeof(s));
        s.step = step;
        s.op_kind = stressfs_op_kind_name(op.kind);
        s.detail = describe_op(&op);
        s.real_res1 = res1;
        s.real_res2 = res2;
        s.expect_res1 = expect.res1;
        s.expect_res1_any = expect.res1_any;
        s.expect_res2 = expect.res2;
        s.expect_res2_any = expect.res2_any;
        s.diverged = step_diverged;
        s.diverge_reason = reason;
        print_step(&s);

        stressfs_model_apply(&g_model, &op);

        if (step_diverged) {
            diverged_overall = 1;
            report_note("stressfs random: DIVERGENCE -- stopping the run here. The report file "
                        "written so far (seed + this step) is what `stressfs replay` re-runs.");
            break;
        }

        if (stop_after > 0 && step >= stop_after) {
            report_note("stressfs random: stopping after the requested step count (replay mode)");
            break;
        }

        if (verify_every > 0 && (step % verify_every) == 0) {
            char treason[200];
            if (!full_tree_compare(treason, sizeof(treason))) {
                stressfs_random_step_t ts;
                memset(&ts, 0, sizeof(ts));
                ts.step = step;
                ts.op_kind = "full-tree-compare";
                ts.detail = "periodic full-tree compare";
                ts.diverged = 1;
                ts.diverge_reason = treason;
                ts.expect_res1_any = 1;
                ts.expect_res2_any = 1;
                print_step(&ts);
                diverged_overall = 1;
                report_note("stressfs random: DIVERGENCE (full-tree compare) -- stopping the run here.");
                break;
            }
        }
    }

    if (!diverged_overall) {
        fixture_teardown();
    } else {
        report_note("stressfs random: fixture left in place (not torn down) after a divergence, "
                     "for inspection -- same discipline as the conform engine's abort path.");
    }

    report_file_close();

    if (diverged_overall) {
        return RETURN_WARN; /* "the run completed [generated a report]; the handler disagreed" */
    }
    return RETURN_OK;
}
