/* amiga/lib/random_ops.c -- the M4 operation generator. See
 * amiga/include/stressfs/random_ops.h for scope and the determinism
 * contract.
 */

#include "stressfs/random_ops.h"
#include "stressfs/random_model.h"

#include <string.h>

const char *stressfs_op_kind_name(stressfs_op_kind_t k)
{
    switch (k) {
    case STRESSFS_OP_CREATE:  return "create";
    case STRESSFS_OP_OPEN:    return "open";
    case STRESSFS_OP_WRITE:   return "write";
    case STRESSFS_OP_READ:    return "read";
    case STRESSFS_OP_SEEK:    return "seek";
    case STRESSFS_OP_CLOSE:   return "close";
    case STRESSFS_OP_DELETE:  return "delete";
    case STRESSFS_OP_RENAME:  return "rename";
    case STRESSFS_OP_MKDIR:   return "mkdir";
    case STRESSFS_OP_EXNEXT:  return "exnext";
    case STRESSFS_OP_SETSIZE: return "setsize";
    default:                  return "?";
    }
}

static const char NAME_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static void gen_fresh_name(stressfs_prng_t *prng, char *out)
{
    unsigned len = 4 + stressfs_prng_below(prng, 9); /* 4..12 */
    unsigned i;
    for (i = 0; i < len; i++) {
        out[i] = NAME_CHARSET[stressfs_prng_below(prng, (uint32_t)(sizeof(NAME_CHARSET) - 1))];
    }
    out[len] = '\0';
}

/* A "shadow" of an existing name: identical fold-insensitive identity,
 * different exact bytes -- the proposal's "names that differ only in
 * case" knob, used here to exercise the fold-insensitive collision
 * paths in stressfs_model_apply()/stressfs_model_expect() rather than
 * leaving them untested by construction. */
static void gen_shadow_name(const char *src, char *out, size_t outsz)
{
    size_t i;
    strncpy(out, src, outsz - 1);
    out[outsz - 1] = '\0';
    for (i = 0; out[i] != '\0'; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 'a' && c <= 'z') { out[i] = (char)(c - 'a' + 'A'); break; }
        if (c >= 'A' && c <= 'Z') { out[i] = (char)(c - 'A' + 'a'); break; }
    }
}

/* Picks a parent directory for a new name: root (-1) or an existing
 * live directory, uniformly. */
static int pick_parent(stressfs_prng_t *prng, const stressfs_model_t *m)
{
    int n_dirs = stressfs_model_count(m, STRESSFS_NODE_DIR, 0);
    uint32_t choice = stressfs_prng_below(prng, (uint32_t)(n_dirs + 1));
    if ((int)choice == n_dirs) {
        return -1;
    }
    return stressfs_model_nth(m, STRESSFS_NODE_DIR, 0, (int)choice);
}

/* Fills `out` with either a fresh random name or, with roughly 1-in-6
 * odds and only when `parent` already has at least one child, a
 * case-shadow of one of that parent's existing children -- see
 * gen_shadow_name()'s comment. */
static void pick_name(stressfs_prng_t *prng, const stressfs_model_t *m, int parent, char *out)
{
    int n_siblings = stressfs_model_count_children(m, parent);
    if (n_siblings > 0 && stressfs_prng_below(prng, 6) == 0) {
        int sib = stressfs_model_nth_child(m, parent, (int)stressfs_prng_below(prng, (uint32_t)n_siblings));
        gen_shadow_name(m->nodes[sib].name, out, STRESSFS_RANDOM_NAME_LEN);
        return;
    }
    gen_fresh_name(prng, out);
}

/* Applicable op kinds given the model's CURRENT state, in a fixed
 * order so "pick the Nth applicable kind" is deterministic. Kept as a
 * small static table per branch rather than a dynamic bitmask: both
 * branches are short and the sets are fixed at compile time (only
 * their *availability*, computed below, is state-dependent). */
static const stressfs_op_kind_t CLOSED_KINDS[] = {
    STRESSFS_OP_CREATE, STRESSFS_OP_OPEN, STRESSFS_OP_DELETE,
    STRESSFS_OP_RENAME, STRESSFS_OP_MKDIR, STRESSFS_OP_EXNEXT
};
#define N_CLOSED_KINDS (sizeof(CLOSED_KINDS) / sizeof(CLOSED_KINDS[0]))

static const stressfs_op_kind_t OPEN_KINDS[] = {
    STRESSFS_OP_WRITE, STRESSFS_OP_READ, STRESSFS_OP_SEEK,
    STRESSFS_OP_CLOSE, STRESSFS_OP_SETSIZE
};
#define N_OPEN_KINDS (sizeof(OPEN_KINDS) / sizeof(OPEN_KINDS[0]))

static int kind_available(const stressfs_model_t *m, stressfs_op_kind_t k)
{
    switch (k) {
    case STRESSFS_OP_CREATE: return stressfs_model_has_room(m);
    case STRESSFS_OP_MKDIR:  return stressfs_model_has_room(m);
    case STRESSFS_OP_OPEN:   return stressfs_model_count(m, STRESSFS_NODE_FILE, 0) > 0;
    case STRESSFS_OP_DELETE: return stressfs_model_count(m, STRESSFS_NODE_FILE, 0) > 0;
    case STRESSFS_OP_RENAME: return stressfs_model_count(m, STRESSFS_NODE_FILE, 0) > 0;
    case STRESSFS_OP_EXNEXT: return 1; /* root is always a valid, if empty, directory to walk */
    default: return 1; /* the OPEN_KINDS set is always available once something is open */
    }
}

int stressfs_random_generate(stressfs_prng_t *prng, const stressfs_model_t *model,
                              stressfs_op_t *out)
{
    stressfs_op_kind_t avail[N_CLOSED_KINDS]; /* big enough for either branch */
    int n_avail = 0;
    size_t i;
    stressfs_op_kind_t kind;

    memset(out, 0, sizeof(*out));
    out->target_node = -1;
    out->dest_parent_node = -1;
    out->parent_node = -1;

    if (model->open_node < 0) {
        for (i = 0; i < N_CLOSED_KINDS; i++) {
            if (kind_available(model, CLOSED_KINDS[i])) {
                avail[n_avail++] = CLOSED_KINDS[i];
            }
        }
    } else {
        for (i = 0; i < N_OPEN_KINDS; i++) {
            avail[n_avail++] = OPEN_KINDS[i];
        }
    }

    if (n_avail == 0) {
        return 0; /* model offers nothing legal right now (should not happen; see header) */
    }

    kind = avail[stressfs_prng_below(prng, (uint32_t)n_avail)];
    out->kind = kind;

    switch (kind) {
    case STRESSFS_OP_CREATE:
        out->parent_node = pick_parent(prng, model);
        pick_name(prng, model, out->parent_node, out->new_name);
        break;

    case STRESSFS_OP_OPEN: {
        int n_files = stressfs_model_count(model, STRESSFS_NODE_FILE, 0);
        out->target_node = stressfs_model_nth(model, STRESSFS_NODE_FILE, 0,
                                               (int)stressfs_prng_below(prng, (uint32_t)n_files));
        /* OPEN never draws STRESSFS_OPEN_WRITE -- CREATE already covers
         * the MODE_NEWFILE case (including on an existing name), so
         * OPEN only ever means "read it back" or "update it in
         * place", which is what "open in each mode" needs once CREATE
         * supplies the third. */
        out->open_mode = (stressfs_prng_below(prng, 2) == 0) ? STRESSFS_OPEN_READ : STRESSFS_OPEN_UPDATE;
        break;
    }

    case STRESSFS_OP_WRITE:
        /* Biased small-to-moderate: inline-ish sizes are the common
         * case for a model this size, with an occasional larger write
         * (up to ~8 KB) to push past a single block on most formats --
         * the proposal's "file-size distribution biased to inline...
         * sizes" knob, implemented minimally rather than with a full
         * configurable distribution (M4 does not expose a knobs CLI
         * surface; see the final report). */
        out->length = (stressfs_prng_below(prng, 10) == 0)
            ? (64 + stressfs_prng_below(prng, 8192))
            : (1 + stressfs_prng_below(prng, 256));
        break;

    case STRESSFS_OP_READ:
        out->length = 1 + stressfs_prng_below(prng, 512);
        break;

    case STRESSFS_OP_SEEK: {
        const stressfs_model_node_t *n = &model->nodes[model->open_node];
        out->seek_mode = (int)stressfs_prng_below(prng, 3);
        switch (out->seek_mode) {
        case 0: /* OFFSET_BEGINNING */
            out->seek_offset = (int32_t)stressfs_prng_below(prng, n->size + 1);
            break;
        case 1: /* OFFSET_CURRENT */
            if (model->open_pos > 0 && stressfs_prng_below(prng, 2) == 0) {
                out->seek_offset = -(int32_t)stressfs_prng_below(prng, model->open_pos + 1);
            } else {
                out->seek_offset = (int32_t)stressfs_prng_below(
                    prng, (n->size >= model->open_pos ? (n->size - model->open_pos) : 0) + 1);
            }
            break;
        default: /* OFFSET_END */
            out->seek_offset = -(int32_t)stressfs_prng_below(prng, n->size + 1);
            break;
        }
        /* Pin down the resulting absolute position so
         * stressfs_model_apply() (which only understands an absolute
         * target) and the real Seek() call agree exactly, without
         * either of them re-deriving the arithmetic above. */
        {
            int32_t base = (out->seek_mode == 0) ? 0
                          : (out->seek_mode == 1) ? (int32_t)model->open_pos
                          : (int32_t)n->size;
            int32_t abspos = base + out->seek_offset;
            if (abspos < 0) abspos = 0;
            if ((uint32_t)abspos > n->size) abspos = (int32_t)n->size;
            out->seek_offset = abspos;
            out->seek_mode = 0; /* re-expressed as an absolute OFFSET_BEGINNING target */
        }
        break;
    }

    case STRESSFS_OP_CLOSE:
        break;

    case STRESSFS_OP_DELETE: {
        int n_files = stressfs_model_count(model, STRESSFS_NODE_FILE, 0);
        out->target_node = stressfs_model_nth(model, STRESSFS_NODE_FILE, 0,
                                               (int)stressfs_prng_below(prng, (uint32_t)n_files));
        break;
    }

    case STRESSFS_OP_RENAME: {
        int n_files = stressfs_model_count(model, STRESSFS_NODE_FILE, 0);
        out->target_node = stressfs_model_nth(model, STRESSFS_NODE_FILE, 0,
                                               (int)stressfs_prng_below(prng, (uint32_t)n_files));
        out->dest_parent_node = pick_parent(prng, model);
        pick_name(prng, model, out->dest_parent_node, out->new_name);
        break;
    }

    case STRESSFS_OP_MKDIR:
        /* M4 scope: directories only ever live directly under the
         * fixture root (random_model.h's MAX_DEPTH-1 decision). */
        out->parent_node = -1;
        pick_name(prng, model, -1, out->new_name);
        break;

    case STRESSFS_OP_EXNEXT: {
        int n_dirs = stressfs_model_count(model, STRESSFS_NODE_DIR, 0);
        uint32_t choice = stressfs_prng_below(prng, (uint32_t)(n_dirs + 1));
        out->target_node = ((int)choice == n_dirs) ? -1
            : stressfs_model_nth(model, STRESSFS_NODE_DIR, 0, (int)choice);
        break;
    }

    case STRESSFS_OP_SETSIZE: {
        const stressfs_model_node_t *n = &model->nodes[model->open_node];
        out->setsize_grow = (int)stressfs_prng_below(prng, 2);
        if (out->setsize_grow) {
            out->setsize_new_size = n->size + 1 + stressfs_prng_below(prng, 4096);
        } else {
            out->setsize_new_size = stressfs_prng_below(prng, n->size + 1);
        }
        break;
    }

    default:
        return 0;
    }

    return 1;
}
