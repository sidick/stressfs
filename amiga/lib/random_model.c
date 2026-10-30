/* amiga/lib/random_model.c -- see amiga/include/stressfs/random_model.h
 * for the design rationale.
 */

#include "stressfs/random_model.h"

#include <string.h>

void stressfs_model_init(stressfs_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->open_node = -1;
    m->pos_trusted = 1;
}

int stressfs_model_names_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }
}

int stressfs_model_has_room(const stressfs_model_t *m)
{
    return m->live_count < STRESSFS_MODEL_MAX_NODES;
}

int stressfs_model_find_child(const stressfs_model_t *m, int parent, const char *name)
{
    int i;
    for (i = 0; i < m->n_slots; i++) {
        const stressfs_model_node_t *n = &m->nodes[i];
        if (n->in_use && n->parent == parent && stressfs_model_names_equal(n->name, name)) {
            return i;
        }
    }
    return -1;
}

int stressfs_model_count_children(const stressfs_model_t *m, int parent)
{
    int i, c = 0;
    for (i = 0; i < m->n_slots; i++) {
        if (m->nodes[i].in_use && m->nodes[i].parent == parent) {
            c++;
        }
    }
    return c;
}

int stressfs_model_nth_child(const stressfs_model_t *m, int parent, int n)
{
    int i;
    for (i = 0; i < m->n_slots; i++) {
        if (m->nodes[i].in_use && m->nodes[i].parent == parent) {
            if (n == 0) {
                return i;
            }
            n--;
        }
    }
    return -1;
}

int stressfs_model_count(const stressfs_model_t *m, stressfs_node_type_t type, int any_type)
{
    int i, c = 0;
    for (i = 0; i < m->n_slots; i++) {
        if (!m->nodes[i].in_use) continue;
        if (!any_type && m->nodes[i].type != type) continue;
        c++;
    }
    return c;
}

int stressfs_model_nth(const stressfs_model_t *m, stressfs_node_type_t type, int any_type, int n)
{
    int i;
    for (i = 0; i < m->n_slots; i++) {
        if (!m->nodes[i].in_use) continue;
        if (!any_type && m->nodes[i].type != type) continue;
        if (n == 0) {
            return i;
        }
        n--;
    }
    return -1;
}

/* Allocates a free slot (reusing a deleted one before growing the
 * table) and marks it in_use; caller fills the rest. Returns -1 if the
 * table is genuinely full (stressfs_model_has_room() said otherwise --
 * the generator never calls this without checking first, but this
 * function stays safe either way). */
static int alloc_node(stressfs_model_t *m)
{
    int i;
    for (i = 0; i < m->n_slots; i++) {
        if (!m->nodes[i].in_use) {
            memset(&m->nodes[i], 0, sizeof(m->nodes[i]));
            m->nodes[i].in_use = 1;
            m->live_count++;
            return i;
        }
    }
    if (m->n_slots < STRESSFS_MODEL_MAX_NODES) {
        i = m->n_slots++;
        memset(&m->nodes[i], 0, sizeof(m->nodes[i]));
        m->nodes[i].in_use = 1;
        m->live_count++;
        return i;
    }
    return -1;
}

static void free_node(stressfs_model_t *m, int idx)
{
    if (idx >= 0 && idx < m->n_slots && m->nodes[idx].in_use) {
        m->nodes[idx].in_use = 0;
        m->live_count--;
    }
}

void stressfs_model_apply(stressfs_model_t *m, const stressfs_op_t *op)
{
    switch (op->kind) {
    case STRESSFS_OP_CREATE: {
        /* Open(MODE_NEWFILE): create, or truncate-and-reuse an
         * existing same-(fold-insensitive)-name FILE; truncating an
         * existing DIRECTORY of that name is not legal and the
         * generator never produces that case (see random_ops.c) --
         * stressfs_model_expect() reports the failure for it without
         * stressfs_model_apply() needing to special-case it here,
         * since a failed CREATE must not mutate the model at all. */
        int existing = stressfs_model_find_child(m, op->parent_node, op->new_name);
        if (existing >= 0 && m->nodes[existing].type == STRESSFS_NODE_DIR) {
            /* Expected to fail; model state is unchanged. */
            return;
        }
        if (existing >= 0) {
            m->nodes[existing].size = 0;
            m->nodes[existing].written = 0;
            m->nodes[existing].undef_cache_count = 0; /* new file identity; stale garbage observations don't apply */
            m->open_node = existing;
        } else {
            int idx = alloc_node(m);
            if (idx < 0) {
                return; /* table full; expectation already covers this (see random_ops.c's room check) */
            }
            m->nodes[idx].type = STRESSFS_NODE_FILE;
            strncpy(m->nodes[idx].name, op->new_name, STRESSFS_MODEL_NAME_LEN - 1);
            m->nodes[idx].parent = op->parent_node;
            m->nodes[idx].file_id = m->next_file_id++;
            m->nodes[idx].size = 0;
            m->open_node = idx;
        }
        m->open_mode = STRESSFS_OPEN_WRITE;
        m->open_pos = 0;
        m->pos_trusted = 1;
        break;
    }

    case STRESSFS_OP_OPEN:
        m->open_node = op->target_node;
        m->open_mode = op->open_mode;
        m->open_pos = 0;
        m->pos_trusted = 1;
        break;

    case STRESSFS_OP_WRITE: {
        stressfs_model_node_t *n = &m->nodes[m->open_node];
        /* `written` only ever extends CONTIGUOUSLY from 0 -- it means
         * "bytes [0, written) are exactly known to have been WRITEn",
         * not merely "the highest offset any write has ever touched".
         * A write starting at op_pos > written opens a GAP (typically
         * a truncate-then-regrow-past-it, or a SEEK past the current
         * written end): found the hard way proving this milestone
         * against real FFS under Copperline -- an earlier version of
         * this code let ANY later write's end position extend
         * `written`, which then WRONGLY claimed bytes inside that gap
         * were defined content, producing a false divergence. The
         * safe fix is conservative, not exhaustive: a write across a
         * gap does NOT extend `written` at all, so the (truly
         * written) bytes past the gap simply go unchecked rather than
         * being falsely asserted -- consistent with M4's "portable
         * expectations only" scope, which would rather under-check
         * than invent a precise multi-range tracker nothing in the
         * plan asks for. */
        if (m->open_pos <= n->written) {
            uint32_t end = m->open_pos + op->length;
            if (end > n->written) {
                n->written = end;
            }
        }
        m->open_pos += op->length;
        if (m->open_pos > n->size) {
            n->size = m->open_pos;
        }
        break;
    }

    case STRESSFS_OP_READ: {
        const stressfs_model_node_t *n = &m->nodes[m->open_node];
        uint32_t remaining = (m->open_pos < n->size) ? (n->size - m->open_pos) : 0;
        uint32_t actual = (op->length < remaining) ? op->length : remaining;
        /* Live testing against two independent real FFS handlers
         * showed BOTH a full `size`-bounded read (garbage content,
         * already documented) and a 0-byte read occur for a read that
         * needs any byte at or past `written` -- genuinely not
         * portably predictable (see stressfs_model_expect()'s matching
         * comment). This guess (`actual`, the same as before) is kept
         * purely as a best-effort internal placeholder; the model
         * stops ASSERTING anything derived from `open_pos` for this
         * file the moment that happens (`pos_trusted` below), so which
         * guess is used here no longer matters for correctness -- only
         * for generating plausible-looking subsequent synthetic ops. */
        if (m->open_pos + actual > n->written) {
            m->pos_trusted = 0;
        }
        m->open_pos += actual;
        break;
    }

    case STRESSFS_OP_SEEK:
        /* Always restores trust: the generator re-expresses every SEEK
         * as an absolute OFFSET_BEGINNING target (random_ops.c), so
         * Seek() unconditionally lands the real file pointer at
         * `op->seek_offset` regardless of where it was before --
         * confirmed live, this is what actually re-syncs the model
         * with the real file after an untrusted-position READ. */
        m->open_pos = op->seek_offset < 0 ? 0 : (uint32_t)op->seek_offset;
        m->pos_trusted = 1;
        break;

    case STRESSFS_OP_CLOSE:
        m->open_node = -1;
        m->open_pos = 0;
        break;

    case STRESSFS_OP_DELETE:
        free_node(m, op->target_node);
        break;

    case STRESSFS_OP_RENAME: {
        int collision = stressfs_model_find_child(m, op->dest_parent_node, op->new_name);
        if (collision >= 0 && collision != op->target_node) {
            return; /* expected to fail; model unchanged */
        }
        m->nodes[op->target_node].parent = op->dest_parent_node;
        strncpy(m->nodes[op->target_node].name, op->new_name, STRESSFS_MODEL_NAME_LEN - 1);
        m->nodes[op->target_node].name[STRESSFS_MODEL_NAME_LEN - 1] = '\0';
        break;
    }

    case STRESSFS_OP_MKDIR: {
        int existing = stressfs_model_find_child(m, op->parent_node, op->new_name);
        int idx;
        if (existing >= 0) {
            return; /* expected to fail; model unchanged */
        }
        idx = alloc_node(m);
        if (idx < 0) {
            return;
        }
        m->nodes[idx].type = STRESSFS_NODE_DIR;
        strncpy(m->nodes[idx].name, op->new_name, STRESSFS_MODEL_NAME_LEN - 1);
        m->nodes[idx].parent = op->parent_node;
        break;
    }

    case STRESSFS_OP_EXNEXT:
        /* Read-only: no model mutation. */
        break;

    case STRESSFS_OP_SETSIZE: {
        stressfs_model_node_t *n = &m->nodes[m->open_node];
        n->size = op->setsize_new_size;
        if (m->open_pos > n->size) {
            m->open_pos = n->size;
        }
        /* Truncating below the written high-water mark discards that
         * written data too; growing past it does NOT retroactively
         * make the new region "written" (see
         * stressfs_model_expected_byte()'s header comment). */
        if (n->written > n->size) {
            n->written = n->size;
        }
        /* Either direction changes the undefined region's shape --
         * truncate removes some of it, grow adds new space whose
         * on-disk content this model has never observed -- so any
         * previously cached "first observed byte" no longer applies
         * (see STRESSFS_MODEL_UNDEF_CACHE_CAP's comment). */
        n->undef_cache_count = 0;
        break;
    }

    default:
        break;
    }
}

stressfs_model_expect_t stressfs_model_expect(const stressfs_model_t *m, const stressfs_op_t *op)
{
    stressfs_model_expect_t e;
    e.res1 = 1; e.res1_any = 0;
    e.res2 = 0; e.res2_any = 1; /* res2 (IoErr) is undefined on success for most ops; _any by default */

    switch (op->kind) {
    case STRESSFS_OP_CREATE: {
        int existing = stressfs_model_find_child(m, op->parent_node, op->new_name);
        if (existing >= 0 && m->nodes[existing].type == STRESSFS_NODE_DIR) {
            e.res1 = 0; e.res2_any = 1; /* fails; exact error code is handler-specific (phase 2) */
        }
        break;
    }
    case STRESSFS_OP_OPEN:
        break; /* generator only ever targets a live file; always succeeds */
    case STRESSFS_OP_WRITE:
        e.res1 = (long)op->length; e.res1_any = 0;
        break;
    case STRESSFS_OP_READ: {
        const stressfs_model_node_t *n = &m->nodes[m->open_node];
        uint32_t remaining = (m->open_pos < n->size) ? (n->size - m->open_pos) : 0;
        uint32_t actual = (op->length < remaining) ? op->length : remaining;
        /* FOUND WHILE PROVING M5 FOR REAL (not anticipated by the plan's
         * text, and not what an earlier version of this function
         * assumed): a read that needs any byte at or past `written` --
         * i.e. inside a SETSIZE-grown-but-never-WRITTEN region, exactly
         * the same region stressfs_model_byte_is_defined() already
         * refuses to predict CONTENT for -- was observed to return
         * byte COUNT 0 against two independent real FFS handlers
         * (V40 and V47) at a position well inside `size`, not the
         * size-bounded count this function used to assert. The
         * apparent real behaviour: SetFileSize() updates the file's
         * declared size but allocates no real data blocks for the
         * grown region, and ACTION_READ's own EOF is bounded by the
         * actual block chain (== `written`), not the declared size --
         * so the COUNT a real Read() returns there is exactly as
         * handler-specific as the CONTENT already was, not merely the
         * content. Treating it as portable was the bug M5's
         * fixed-seed runs surfaced (identical step, identical wrong
         * shape, on two unrelated handlers -- the signature of a
         * harness assumption, not a handler quirk). The fix mirrors
         * stressfs_model_byte_is_defined()'s own boundary: once the
         * read would need to return anything from offset `written`
         * onward, the exact count is no longer portably predictable,
         * so this leaves it `_any` instead of asserting the
         * size-bounded figure. A read entirely within [pos, written)
         * stays fully predictable, unchanged from before. */
        /* `!m->pos_trusted` (set by a PRIOR op on this file, not this
         * read's own range) also forces `_any`: once open_pos itself
         * may not match the real file pointer, `remaining`/`actual`
         * above are computed from a possibly-wrong starting point and
         * can't be asserted even when they happen to land short of
         * `written`. */
        if (!m->pos_trusted || m->open_pos + actual > n->written) {
            e.res1_any = 1;
        } else {
            e.res1 = (long)actual; e.res1_any = 0;
        }
        break;
    }
    case STRESSFS_OP_SEEK:
        /* Seek() returns the OLD position -- not portably known once
         * `pos_trusted` is false, per this struct's header comment;
         * the NEW position this seek lands on is always fine (it's an
         * absolute target, see stressfs_model_apply()'s SEEK case),
         * only the reported OLD one is in question here. */
        e.res1 = (long)m->open_pos;
        e.res1_any = !m->pos_trusted;
        break;
    case STRESSFS_OP_CLOSE:
        break;
    case STRESSFS_OP_DELETE:
        break;
    case STRESSFS_OP_RENAME: {
        int collision = stressfs_model_find_child(m, op->dest_parent_node, op->new_name);
        if (collision >= 0 && collision != op->target_node) {
            e.res1 = 0; e.res2_any = 1;
        }
        break;
    }
    case STRESSFS_OP_MKDIR: {
        int existing = stressfs_model_find_child(m, op->parent_node, op->new_name);
        if (existing >= 0) {
            e.res1 = 0; e.res2_any = 1;
        }
        break;
    }
    case STRESSFS_OP_EXNEXT:
        break; /* listing comparison, not a {res1,res2} check -- see amiga/src/random.c */
    case STRESSFS_OP_SETSIZE:
        e.res1 = (long)op->setsize_new_size; e.res1_any = 0; /* SetFileSize() returns the new size */
        break;
    default:
        break;
    }

    return e;
}

uint8_t stressfs_model_content_byte(uint32_t seed, uint32_t file_id, uint32_t offset)
{
    uint32_t word_index = offset >> 2;
    uint32_t word = stressfs_prng_hash3(seed, file_id, word_index);
    int shift = (int)((offset & 3u) * 8u);
    return (uint8_t)((word >> shift) & 0xFFu);
}

int stressfs_model_byte_is_defined(const stressfs_model_t *m, int node_idx, uint32_t offset)
{
    return offset < m->nodes[node_idx].written;
}

int stressfs_model_undef_cache_lookup(const stressfs_model_t *m, int node_idx,
                                       uint32_t offset, uint8_t *out_byte)
{
    const stressfs_model_node_t *n = &m->nodes[node_idx];
    int i;
    for (i = 0; i < n->undef_cache_count; i++) {
        if (n->undef_cache[i].offset == offset) {
            *out_byte = n->undef_cache[i].value;
            return 1;
        }
    }
    return 0;
}

void stressfs_model_undef_cache_record(stressfs_model_t *m, int node_idx,
                                        uint32_t offset, uint8_t value)
{
    stressfs_model_node_t *n = &m->nodes[node_idx];
    int i;
    if (n->undef_cache_count >= STRESSFS_MODEL_UNDEF_CACHE_CAP) {
        return; /* full; graceful degradation, see the cap's own comment */
    }
    for (i = 0; i < n->undef_cache_count; i++) {
        if (n->undef_cache[i].offset == offset) {
            return; /* first observation wins, never overwritten */
        }
    }
    n->undef_cache[n->undef_cache_count].offset = offset;
    n->undef_cache[n->undef_cache_count].value = value;
    n->undef_cache_count++;
}

size_t stressfs_model_node_bytes(void)
{
    return sizeof(stressfs_model_node_t);
}
