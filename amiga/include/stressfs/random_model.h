/* stressfs/random_model.h -- M4 (docs/implementation-plan.md section 3's
 * M4 entry): the in-memory model for `stressfs random`, the proposal's
 * layer-2 "tree of names to byte arrays with the metadata the API
 * exposes" restricted to the portable expectations phase 1 asks for
 * (contents, sizes, existence, fold-insensitive name identity, basic
 * error codes). PORTABLE, same rule as conform.h/report.h/quirks.h: no
 * Amiga header, no allocation (one fixed-size struct the caller owns),
 * builds and unit-tests on the host.
 *
 * SCOPE DECISIONS not fully specified by the implementation plan:
 *
 *   - Tree depth: the model supports the fixture root plus exactly one
 *     level of subdirectories (STRESSFS_MODEL_MAX_DEPTH). This is
 *     enough to exercise every M4 operation ("rename within and across
 *     directories", "CreateDir", "directory walk via ExNext") without
 *     the model needing general recursive-path bookkeeping that
 *     nothing in the M4 op list actually requires; deeper nesting is
 *     phase 2 scope, not excluded by the proposal but not asked for
 *     either.
 *   - At most one open file handle at a time. The proposal's
 *     concurrency and record-lock modes are explicitly phase 2/3; a
 *     single-handle model keeps every generated operation sequence a
 *     straight line with no need to track which of several handles a
 *     WRITE/READ/SEEK/CLOSE applies to, which is the simplest model
 *     that still exercises every M4 operation.
 *   - DELETE only ever targets a file, never a directory (avoids the
 *     model having to track "is this directory empty", which nothing
 *     in M4's op list needs either -- directories are only ever
 *     removed by nothing at all in M4; they accumulate for the run's
 *     duration, which is fine at the node-table sizes this milestone
 *     uses).
 *   - Node identity is by STABLE INTEGER INDEX, not path string, so
 *     renaming or moving a node is a single field update and a
 *     directory's children need no bulk rename when their parent
 *     moves.
 *
 * File content is never stored (docs/proposal.md: "Content is
 * generated from the seed and offset rather than stored"): every node
 * has a `file_id` assigned once at creation and never reused, and
 * stressfs_model_content_byte() recomputes the expected byte at any
 * offset from (run seed, file_id, offset) via stressfs_prng_hash3() --
 * see prng.h's header comment on that function for why a direct hash,
 * not the sequential PRNG stream, is what makes random-access
 * read-back comparison free.
 */
#ifndef STRESSFS_RANDOM_MODEL_H
#define STRESSFS_RANDOM_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "stressfs/random_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Node table size. Bounds the model's working set (see
 * stressfs_model_node_bytes() below and amiga/src/random.c's
 * AvailMem() check): once every slot is live, the generator simply
 * stops offering CREATE/MKDIR (see random_ops.c), which caps growth
 * rather than failing the run -- a long `stressfs random OPS=100000`
 * run just recycles names once the table is full instead of being
 * refused outright, which is the graceful degradation the proposal's
 * "runs reduced profiles on small machines" asks for applied to node
 * count rather than only to the whole run being refused up front. */
#define STRESSFS_MODEL_MAX_NODES 1024
#define STRESSFS_MODEL_NAME_LEN  STRESSFS_RANDOM_NAME_LEN

/* Ported from AmiFSX (https://github.com/lainejones/AmiFSX, MIT, an
 * independent fsx-for-AmigaDOS port, unaffiliated with this project):
 * a SETSIZE-grown-but-never-WRITTEN byte's value is undefined by
 * AmigaDOS ("AmigaDOS doesn't promise that SetFileSize zero-fills, and
 * FFS doesn't. It hands back old disk blocks."), but once something
 * real IS on disk there, it should not change on its own -- so rather
 * than skip checking that region entirely (M4's original choice,
 * still the fallback once this cache is full), remember what the
 * FIRST read at an undefined offset returned and assert every later
 * read at that same offset returns the same byte. Catches real
 * corruption in that region without inventing a portable expectation
 * for its initial content, which AmigaDOS explicitly does not promise.
 *
 * A small, FIXED cache per node, not a growable one: this model
 * deliberately never stores a file's actual content (random_model.h's
 * top comment -- recomputed from seed+offset instead) specifically to
 * keep the working set bounded regardless of file size, and storing
 * every undefined byte ever read would reintroduce exactly the
 * unbounded-per-file memory this design avoids. 16 entries (chosen to
 * keep the per-node and total table cost small -- see
 * stressfs_model_node_bytes()'s use in the working-set estimate,
 * amiga/src/random.c) is enough to catch realistic repeated probes of
 * one gap region in a run of this scope; once exhausted for a node, a
 * read at a NEW undefined offset simply goes back to unchecked, the
 * same graceful degradation already used when the node table itself
 * fills up. */
#define STRESSFS_MODEL_UNDEF_CACHE_CAP 16

typedef struct {
    uint32_t offset;
    uint8_t value;
} stressfs_undef_cache_entry_t;

typedef enum {
    STRESSFS_NODE_FILE = 0,
    STRESSFS_NODE_DIR
} stressfs_node_type_t;

typedef struct {
    int in_use;
    stressfs_node_type_t type;
    char name[STRESSFS_MODEL_NAME_LEN]; /* case-preserving, as created */
    int parent;           /* -1 = fixture root; otherwise a DIR node's index */
    uint32_t file_id;     /* FILE only: stable content-hash key */
    uint32_t size;         /* FILE only: current logical size in bytes */
    uint32_t written;      /* FILE only: the highest offset ever actually WRITEn to
                             * (see stressfs_model_expected_byte()'s header comment --
                             * `size` can exceed this after a SETSIZE grow, and the
                             * gap is zero-filled, not PRNG content). */
    /* FILE only: AmiFSX's "adopt first read, assert consistency" cache
     * for the undefined (>= `written`) region -- see the macro's own
     * comment above. Reset (count = 0) whenever the undefined region's
     * shape changes: a fresh CREATE, or any SETSIZE (grow or truncate
     * both change what is/isn't in that region, and a truncate then
     * regrow past an old boundary is exactly the gap scenario the
     * `written`-tracking WRITE case already treats conservatively). */
    stressfs_undef_cache_entry_t undef_cache[STRESSFS_MODEL_UNDEF_CACHE_CAP];
    int undef_cache_count;
} stressfs_model_node_t;

typedef struct stressfs_model_t {
    stressfs_model_node_t nodes[STRESSFS_MODEL_MAX_NODES];
    int n_slots;            /* 1 + highest index ever allocated */
    int live_count;         /* nodes currently in_use, for the "table full" check */
    uint32_t next_file_id;  /* monotonic; never reused even when a node is deleted */

    int open_node;          /* -1 = nothing open */
    stressfs_open_mode_t open_mode;
    uint32_t open_pos;
    /* FOUND WHILE FIXING THE M5-FLAGGED RANDOM-MODEL BUG (see
     * stressfs_model_expect()'s READ case): once a READ needs any byte
     * at or past `written` (the SETSIZE-grown-but-never-WRITTEN zone),
     * the real byte count it returns is genuinely not portable -- live
     * testing against two independent real FFS handlers under
     * Copperline showed BOTH outcomes occur (sometimes the full
     * requested count with garbage content, sometimes 0), not a single
     * predictable one. Once that happens, this model's own `open_pos`
     * is no longer known to match the real file pointer -- confirmed
     * live: a Seek() immediately after a 0-byte read into that zone
     * returned the file's SIZE as its "old position", not the pointer
     * value this model had been tracking. `pos_trusted` records
     * whether `open_pos` can still be asserted against the real side
     * for WRITE/READ/SEEK; it clears the moment a READ crosses into
     * that undefined zone and is restored only by a fresh CREATE/OPEN
     * (position newly and definitely 0) or a SEEK (whose target is
     * always re-expressed as an absolute OFFSET_BEGINNING value by the
     * generator, so it unconditionally lands the real file there
     * regardless of where it was before -- see random_ops.c's SEEK
     * case). SETSIZE/CLOSE/DELETE/RENAME/MKDIR/EXNEXT are unaffected:
     * none of their portable expectations depend on `open_pos`, and
     * every live run so far confirms SetFileSize()'s own return value
     * stays reliable even once position tracking has drifted. */
    int pos_trusted;
} stressfs_model_t;

void stressfs_model_init(stressfs_model_t *m);

/* Fold-insensitive name identity (AmigaDOS: case-preserving storage,
 * case-insensitive matching) -- ASCII toupper only, which is all of
 * stressfs's own generated names ever use. */
int stressfs_model_names_equal(const char *a, const char *b);

/* True iff the table has room for at least one more node (see the
 * node-table-size comment above). */
int stressfs_model_has_room(const stressfs_model_t *m);

/* Looks up a live child of `parent` (-1 = fixture root) named `name`,
 * fold-insensitively. Returns its node index, or -1. */
int stressfs_model_find_child(const stressfs_model_t *m, int parent, const char *name);

/* Counts/enumerates live children of `parent` (-1 = root), in
 * table-slot order -- used both by the generator (picking a node to
 * act on) and by the EXNEXT op's real-side listing comparison
 * (amiga/src/random.c), which needs the exact expected name set. */
int stressfs_model_count_children(const stressfs_model_t *m, int parent);
int stressfs_model_nth_child(const stressfs_model_t *m, int parent, int n);

/* Counts/enumerates live nodes of a given type anywhere in the tree
 * (used by the generator to pick e.g. "any live file" for OPEN/DELETE,
 * or "any live directory" for a rename destination or an EXNEXT
 * target); `any_type` nonzero ignores `type` and matches every live
 * node. */
int stressfs_model_count(const stressfs_model_t *m, stressfs_node_type_t type, int any_type);
int stressfs_model_nth(const stressfs_model_t *m, stressfs_node_type_t type, int any_type, int n);

/* Applies a GENERATED op to the model. Never fails: stressfs_random_generate()
 * (random_ops.h) only ever produces operations the model can apply
 * (every referenced node exists, every name collision it creates is
 * one the expectation function below already accounts for). Mutates
 * node table, open_node/open_mode/open_pos as appropriate for `op`. */
void stressfs_model_apply(stressfs_model_t *m, const stressfs_op_t *op);

/* The portable, handler-independent expectation for `op`, computed
 * from the model's state BEFORE stressfs_model_apply() runs for it --
 * exactly "result code and IoErr() against the model's expectation"
 * (docs/proposal.md, layer 2's "checks after each step"). `res1_any`/
 * `res2_any` mean "the real side's value cannot be pinned down exactly
 * by the portable model" (there is currently exactly one such case --
 * WRITE/READ counts of zero-length requests on some handlers -- kept
 * here rather than special-cased at every call site). Content, size
 * and listing checks are separate (stressfs_model_content_byte(),
 * stressfs_model_count_children()) because they need the real side's
 * own data to compare against, not just a {res1,res2} pair. */
typedef struct {
    long res1;
    int  res1_any;
    long res2;
    int  res2_any;
} stressfs_model_expect_t;

stressfs_model_expect_t stressfs_model_expect(const stressfs_model_t *m, const stressfs_op_t *op);

/* The raw content hash: the byte a WRITE at `offset` within the file
 * identified by `file_id` is generated to contain, from (run `seed`,
 * `file_id`, `offset`), never stored -- see this header's top comment.
 * NOT the same as "the expected byte at this offset of this file" in
 * general -- see stressfs_model_expected_byte() below, which is what
 * a READ-back comparison should actually call. */
uint8_t stressfs_model_content_byte(uint32_t seed, uint32_t file_id, uint32_t offset);

/* True iff `offset` within `node_idx` has a PORTABLE content
 * expectation at all -- i.e. offset < the node's `written` high-water
 * mark (some WRITE actually put seed-derived content there). Bytes at
 * or past `written` but still below `size` came from a SETSIZE GROW,
 * and what a handler leaves there is NOT specified by the AmigaDOS
 * manual -- it is observably handler-specific.
 *
 * FOUND WHILE PROVING M4 FOR REAL (not anticipated by the plan's
 * text, and not what an earlier version of this header assumed): a
 * run against real FFS under Copperline grew a file from 63 to 2508
 * bytes via SETSIZE and read back a NON-ZERO byte at offset 63 --
 * i.e. FFS does NOT zero-fill grown space, it exposes whatever was
 * already on the disk blocks it allocated. Treating "zero" as the
 * portable expectation there (an earlier version of this code did)
 * produced a FALSE divergence against a real, unmodified handler --
 * exactly the harness-credibility failure the proposal's verdict
 * system exists to prevent. The fix is to have NO portable content
 * expectation for that region at all: it is squarely a
 * handler-specific expectation (ExAll ordering, date granularity and
 * the like are the proposal's own examples of the category), which
 * M4 explicitly defers to phase 2's quirks-driven expectations (see
 * this header's top comment) rather than guessing one here. Callers
 * (amiga/src/random.c's READ comparison) must check this before
 * calling stressfs_model_content_byte() for a read-back byte, and
 * simply skip the content check (while still checking the returned
 * BYTE COUNT, which is portable) when it returns false -- or, now,
 * consult the AmiFSX-style cache below first; see
 * STRESSFS_MODEL_UNDEF_CACHE_CAP's comment above. */
int stressfs_model_byte_is_defined(const stressfs_model_t *m, int node_idx, uint32_t offset);

/* Looks up a previously-recorded value for an undefined-zone `offset`
 * in `node_idx`'s cache (see STRESSFS_MODEL_UNDEF_CACHE_CAP's
 * comment). Returns 1 and sets `*out_byte` if found, 0 if this offset
 * has never been recorded (or was evicted by the cache being full --
 * callers then have nothing to compare against for this read, same
 * as the original skip-entirely behaviour). */
int stressfs_model_undef_cache_lookup(const stressfs_model_t *m, int node_idx,
                                       uint32_t offset, uint8_t *out_byte);

/* Records `value` as the first-observed byte at `offset` in
 * `node_idx`'s undefined-zone cache, for stressfs_model_undef_cache_lookup()
 * to check future reads against. A no-op if the cache is already full
 * for this node (graceful degradation, not an error) or if `offset`
 * is already recorded (first observation wins, never overwritten). */
void stressfs_model_undef_cache_record(stressfs_model_t *m, int node_idx,
                                        uint32_t offset, uint8_t value);

/* sizeof(stressfs_model_node_t), exposed so the working-set estimate
 * (amiga/src/random.c) and any host test that wants to check the
 * arithmetic agree on one number rather than two independently
 * maintained constants. */
size_t stressfs_model_node_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_RANDOM_MODEL_H */
