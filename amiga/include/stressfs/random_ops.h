/* stressfs/random_ops.h -- M4: the seeded operation generator for
 * `stressfs random`, restricted to the proposal's layer-2 phase-1
 * scope (docs/implementation-plan.md's M4 entry):
 *
 *   create, open in each mode, write, read, seek, close, delete,
 *   rename within and across directories, CreateDir, directory walk
 *   via ExNext, SetFileSize grow and truncate.
 *
 * Deliberately NOT built here (phase 2 per the plan's own "restricted
 * to phase 1 scope" wording): hard/soft links, protection bits,
 * comments, dates, record locks, Inhibit, Flush, invalid operations
 * and packet mode. No stubs for these -- they simply are not in
 * stressfs_op_kind_t.
 *
 * PORTABLE, same rule as the rest of amiga/lib: this header and
 * random_ops.c touch no Amiga type, only stressfs/prng.h and
 * stressfs/random_model.h (also portable). The mapping from a
 * generated op to the actual Open()/Write()/... dos.library calls that
 * apply it to a real volume is Amiga-only and lives in
 * amiga/src/random.c.
 */
#ifndef STRESSFS_RANDOM_OPS_H
#define STRESSFS_RANDOM_OPS_H

#include <stdint.h>

#include "stressfs/prng.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STRESSFS_OP_CREATE = 0, /* Open(MODE_NEWFILE): create-or-truncate, becomes the open file */
    STRESSFS_OP_OPEN,       /* Open(MODE_OLDFILE or MODE_READWRITE) on an existing file */
    STRESSFS_OP_WRITE,      /* Write() at the open file's current position */
    STRESSFS_OP_READ,       /* Read() from the open file's current position */
    STRESSFS_OP_SEEK,       /* Seek() the open file */
    STRESSFS_OP_CLOSE,      /* Close() the open file */
    STRESSFS_OP_DELETE,     /* DeleteFile() an existing, unopened file */
    STRESSFS_OP_RENAME,     /* Rename()/move an existing file or directory */
    STRESSFS_OP_MKDIR,      /* CreateDir() under the fixture root */
    STRESSFS_OP_EXNEXT,     /* Examine()+ExNext() walk of one directory, compared to the model's listing */
    STRESSFS_OP_SETSIZE,    /* SetFileSize() on the open file: grow or truncate */
    STRESSFS_OP_KIND_COUNT
} stressfs_op_kind_t;

typedef enum {
    STRESSFS_OPEN_READ = 0,   /* MODE_OLDFILE */
    STRESSFS_OPEN_WRITE,      /* MODE_NEWFILE -- CREATE always uses this; OPEN never generates it (see random_ops.c) */
    STRESSFS_OPEN_UPDATE      /* MODE_READWRITE */
} stressfs_open_mode_t;

#define STRESSFS_RANDOM_NAME_LEN 24

/* One generated operation. Node references are indices into the
 * model's node table (random_model.h); -1 means "not applicable to
 * this op kind". */
typedef struct {
    stressfs_op_kind_t kind;

    int target_node;       /* OPEN/DELETE/RENAME(source)/EXNEXT(directory): the node acted on */
    int dest_parent_node;   /* RENAME only: destination directory, -1 = fixture root */
    int parent_node;        /* CREATE/MKDIR only: directory the new name is created in, -1 = root */

    char new_name[STRESSFS_RANDOM_NAME_LEN]; /* CREATE/MKDIR/RENAME: the name used */

    stressfs_open_mode_t open_mode; /* OPEN only */

    uint32_t length;        /* WRITE/READ: byte count requested */

    int seek_mode;           /* SEEK only: 0=OFFSET_BEGINNING,1=OFFSET_CURRENT,2=OFFSET_END
                               * (plain ints, not dos/dos.h's symbols, so this header stays
                               * Amiga-free; amiga/src/random.c maps them). */
    int32_t seek_offset;

    int setsize_grow;        /* SETSIZE only: nonzero = grow, zero = truncate (reporting only) */
    uint32_t setsize_new_size; /* SETSIZE only: the absolute target size */
} stressfs_op_t;

struct stressfs_model_t; /* see stressfs/random_model.h; forward-declared here only */

/* Synthesizes the NEXT operation from the generator's running PRNG
 * state and the model's CURRENT state. `prng` is advanced in place by
 * every draw this function makes; `model` is read-only here (the
 * caller applies the op via stressfs_model_apply() afterwards). Given
 * the same seed and the same sequence of model states (which is
 * itself fully determined by the operations this function previously
 * produced), the stream this function produces is bit-for-bit
 * reproducible -- M4's first exit criterion.
 *
 * Returns 1 and fills `out` with a well-formed operation (every
 * referenced node exists and is of the right type), or 0 if the
 * model's current state offers no legal operation of the kind drawn
 * (e.g. WRITE drawn while nothing is open should not happen given the
 * state-dependent kind selection below, but the return value exists so
 * a caller's bounded retry loop -- see stressfs_random_step() in
 * amiga/src/random.c -- has a well-defined "try again" signal instead
 * of ever producing a malformed op). */
int stressfs_random_generate(stressfs_prng_t *prng, const struct stressfs_model_t *model,
                              stressfs_op_t *out);

const char *stressfs_op_kind_name(stressfs_op_kind_t k);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_RANDOM_OPS_H */
