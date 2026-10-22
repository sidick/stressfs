/* stressfs/report.h -- M2 (docs/implementation-plan.md section 3's M2
 * entry): the run-identity header and the small JSON-escaping helper
 * shared by it and the per-test JSON formatter
 * (stressfs_conform_format_json(), declared in conform.h and
 * implemented alongside stressfs_conform_format_line() in
 * amiga/lib/conform.c).
 *
 * This file is PORTABLE, same rule as conform.h: it packs and formats
 * already-known fields, no Amiga dependency, builds and unit-tests on
 * the host. Fetching the real values (DosType, DosEnvec, ACTION_DISK_INFO)
 * is Amiga-only and lives in amiga/src/handler_id.c and amiga/src/conform.c;
 * this header only declares what a fully-populated
 * stressfs_run_identity_t looks like and how to render it.
 *
 * docs/proposal.md, "Reproducibility rules": "Every run is identified
 * by harness version, handler name and version, volume geometry and
 * DosEnvec, tier, and seed. A report line that lacks any of these is a
 * bug in the harness." The MANDATORY fields that
 * stressfs_run_identity_is_complete() checks are exactly: harness
 * version, handler name, handler version, tier, seed. Volume geometry
 * and DosEnvec are a separate case -- see the `have_geometry`/
 * `have_envec` flags below for why they are NOT part of the
 * completeness check even though the proposal lists them in the same
 * sentence:
 *
 *   Decision (not fully specified by the implementation plan): the
 *   mandatory-field assertion is about the harness FORGETTING to set a
 *   field it always controls (seed, tier, harness version are always
 *   known to the process that starts a run) vs. the harness being
 *   unable to FETCH a piece of real hardware/handler state that may
 *   legitimately be unavailable in a given tier (e.g. a handler whose
 *   DeviceNode carries no FileSysStartupMsg, or one answering
 *   ACTION_DISK_INFO with an error). Geometry and DosEnvec therefore
 *   always get a definite, explicit value in the rendered header --
 *   either the real numbers or an explicit "unavailable" marker (see
 *   format_header_json/human below) -- so a reader NEVER sees a field
 *   silently missing, but an environment that genuinely cannot answer
 *   ACTION_DISK_INFO does not make the whole run un-reportable. The
 *   handler_name/handler_version/tier/seed group has no such
 *   "legitimately unavailable" case -- not knowing the tier you're
 *   running under, or the seed you started with, really is a harness
 *   bug -- so those are what stressfs_run_identity_is_complete() checks,
 *   and amiga/src/conform.c additionally asserts this (C library
 *   assert(), belt-and-braces per the task's "ideally defensively in
 *   the Amiga-side caller too") before emitting a header.
 */
#ifndef STRESSFS_REPORT_H
#define STRESSFS_REPORT_H

#include <stddef.h>

/* quirks.h -> conform.h, and neither includes this file, so the
 * include graph stays acyclic and the summary formatters below can
 * take properly-typed arguments. */
#include "stressfs/quirks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* docs/implementation-plan.md M2 step 1: "harness version (pick a
 * scheme ... your call but document it)". Scheme chosen: semver-ish
 * "MAJOR.MINOR.PATCH-mN", where mN names the milestone that last
 * changed the report schema (M2 introduced the schema itself; M3
 * changed it by adding the per-line `test` id field, populating
 * `applied_quirks`, and adding the applied-quirks summary object to
 * the header block -- hence "m3" here) -- this makes "what does this report's shape look like"
 * answerable from the version string alone without consulting git
 * history. Bump the mN suffix whenever a future milestone changes the
 * JSON schema (new field, renamed field, etc.), bump MINOR for new
 * non-schema-breaking fields, PATCH for anything else. */
#define STRESSFS_VERSION "0.1.0-m3"

/* One run's identity, per docs/proposal.md's "Reproducibility rules".
 * All string fields are non-owning pointers; the caller (amiga/src/
 * conform.c) owns their storage for the lifetime of the format calls. */
typedef struct {
    const char *harness_version;  /* always STRESSFS_VERSION in practice */
    const char *handler_name;     /* e.g. "DOS\1" (DosType, printable form) or
                                    * "(unidentified)" if identification failed */
    const char *handler_version;  /* the handler's own "$VER:" string, or
                                    * "(no $VER: string found)" / "(unidentified)" */
    const char *tier;             /* "volamos" / "copperline" / "hardware" / "unknown" */

    long seed;
    int  seed_set;   /* nonzero iff `seed` was explicitly assigned -- seed 0
                       * is a perfectly valid seed, so a bare 0 cannot mean
                       * "not set" on its own. */

    long dos_version; /* running system's dos.library version, e.g. 47; 0 if unknown */

    /* Volume geometry, from ACTION_DISK_INFO's InfoData. Meaningful only
     * when have_geometry is nonzero -- see this header's top comment. */
    int  have_geometry;
    long geom_numblocks;
    long geom_numblocksused;
    long geom_bytesperblock;
    long geom_disktype;

    /* DosEnvec (dos/filehandler.h), fetched from the mounting device
     * node's FileSysStartupMsg. Meaningful only when have_envec is
     * nonzero. Field selection: every DosEnvec field except de_SecOrg
     * and de_Baud/de_Control (not used / not geometry-relevant per the
     * header's own comments) and de_BootBlocks (boot-specific, not
     * geometry). */
    int  have_envec;
    unsigned long de_tablesize;
    unsigned long de_sizeblock;
    unsigned long de_surfaces;
    unsigned long de_sectorperblock;
    unsigned long de_blockspertrack;
    unsigned long de_reserved;
    unsigned long de_prealloc;
    unsigned long de_interleave;
    unsigned long de_lowcyl;
    unsigned long de_highcyl;
    unsigned long de_numbuffers;
    unsigned long de_maxtransfer;
    unsigned long de_mask;
    long          de_bootpri;
    unsigned long de_dostype;
} stressfs_run_identity_t;

/* Checks the mandatory group only (harness_version, handler_name,
 * handler_version, tier all non-NULL and non-empty; seed_set nonzero).
 * See this header's top comment for why geometry/envec are excluded. */
int stressfs_run_identity_is_complete(const stressfs_run_identity_t *id);

/* Renders the header as one JSON object (no trailing newline), the
 * first line of a report's JSON-lines stream. Returns 1 on success, 0
 * if `id` fails stressfs_run_identity_is_complete() -- in which case
 * `buf` is left holding a single diagnostic JSON object of type
 * "harness_bug" instead of a real header, so a consumer parsing the
 * stream still sees valid JSON and a human still sees why nothing else
 * should be trusted; the caller must additionally treat a 0 return as
 * fatal (see conform.c). */
int stressfs_report_format_header_json(char *buf, size_t bufsz,
                                        const stressfs_run_identity_t *id);

/* Renders the header as a short human-readable block (lines separated
 * by '\n', no trailing newline after the last one). Same 0/1 contract
 * as the JSON formatter above. */
int stressfs_report_format_header_human(char *buf, size_t bufsz,
                                         const stressfs_run_identity_t *id);

/* M3 (docs/implementation-plan.md M3 step 6): the applied-quirks
 * summary that sits at the TOP of the report, immediately after the
 * run-identity header and before any test line --
 * docs/proposal.md's "Applied quirks are listed at the top of the
 * report, not buried, so a quirk can never silently hide a
 * regression".
 *
 * Decision (the plan's wording "listing every quirk that applied
 * anywhere in the run" vs. the proposal's "at the top of the report"):
 * the report is written as a STREAM -- a conform run prints each line
 * as it happens, so that a crash mid-run still leaves everything up to
 * that point on disk/serial (docs/proposal.md's "logs each operation
 * before performing it ... so a crash leaves the last operation on
 * record"). A top-of-report section therefore cannot be a retrospective
 * list of entries that actually fired; it is the list of entries that
 * are ACTIVE for this run -- matched, confidence-eligible and not
 * suppressed -- which is computable before the first packet is sent and
 * is the same thing devsoak's own quirks_report() prints up front. That
 * is also the stronger statement for a reader: it names every quirk
 * that COULD have changed a verdict in what follows, not merely the
 * ones that did, so no quirk can hide in a line nobody greps. Which
 * ones actually fired is recoverable exactly, per line, from the
 * `applied_quirks` field.
 *
 * Both renderings also state explicitly when the file was suppressed
 * (-K/NOQUIRKS), when no file was found, and when the file was REFUSED
 * as malformed -- a reader must never have to infer "no quirks applied"
 * from the absence of a section.
 *
 * Pass NULL for `db` to render the "no quirks database at all" case.
 * Neither formatter ever fails: there is no mandatory-field notion
 * here (an empty quirks database is a perfectly valid, and the most
 * common, state), so unlike the run-identity header these return
 * nothing. */
void stressfs_report_format_quirks_human(char *buf, size_t bufsz,
                                          const stressfs_quirks_db_t *db,
                                          const stressfs_quirk_identity_t *ident);
void stressfs_report_format_quirks_json(char *buf, size_t bufsz,
                                         const stressfs_quirks_db_t *db,
                                         const stressfs_quirk_identity_t *ident);

/* Appends `src`, JSON-string-escaped (backslash, double-quote and
 * control characters per RFC 8259; stressfs never needs to emit
 * non-ASCII here so no UTF-8/\u-surrogate handling is implemented),
 * into `dst` starting at the current NUL terminator -- NOT including
 * the surrounding quotes, which callers add themselves so a field can
 * be omitted/null without this function needing to know that. `dst`
 * must already be NUL-terminated on entry; always leaves it
 * NUL-terminated on return, truncating rather than overflowing. Shared
 * by this file's own formatters and stressfs_conform_format_json()
 * (amiga/lib/conform.c) -- hand-rolled per the task's instruction (no
 * JSON library cross-compiles to m68k-amigaos bare metal), and
 * deliberately minimal: the field set both formatters emit is narrow
 * and mostly numeric/enum-controlled strings, so "simple and correct"
 * beats "general-purpose". */
void stressfs_json_escape_append(char *dst, size_t dstsz, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_REPORT_H */
