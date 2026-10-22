/* amiga/lib/report.c -- see amiga/include/stressfs/report.h for the
 * design rationale (why geometry/DosEnvec are excluded from the
 * completeness check, the JSON escaping contract).
 */

#include "stressfs/report.h"
#include "stressfs/quirks.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int nonempty(const char *s)
{
    return s != NULL && s[0] != '\0';
}

int stressfs_run_identity_is_complete(const stressfs_run_identity_t *id)
{
    if (id == NULL) {
        return 0;
    }
    if (!nonempty(id->harness_version)) {
        return 0;
    }
    if (!nonempty(id->handler_name)) {
        return 0;
    }
    if (!nonempty(id->handler_version)) {
        return 0;
    }
    if (!nonempty(id->tier)) {
        return 0;
    }
    if (!id->seed_set) {
        return 0;
    }
    return 1;
}

void stressfs_json_escape_append(char *dst, size_t dstsz, const char *src)
{
    size_t len;
    size_t i;

    if (dst == NULL || dstsz == 0) {
        return;
    }
    len = strlen(dst);
    if (src == NULL) {
        return;
    }

    for (i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        char esc[7];
        size_t esclen;

        switch (c) {
        case '"':  esc[0] = '\\'; esc[1] = '"';  esclen = 2; break;
        case '\\': esc[0] = '\\'; esc[1] = '\\'; esclen = 2; break;
        case '\n': esc[0] = '\\'; esc[1] = 'n';  esclen = 2; break;
        case '\r': esc[0] = '\\'; esc[1] = 'r';  esclen = 2; break;
        case '\t': esc[0] = '\\'; esc[1] = 't';  esclen = 2; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                /* \u00XX form -- always exactly 6 bytes. */
                static const char hex[] = "0123456789abcdef";
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[(c >> 4) & 0xf];
                esc[5] = hex[c & 0xf];
                esclen = 6;
            } else {
                esc[0] = (char)c;
                esclen = 1;
            }
            break;
        }

        if (len + esclen + 1 > dstsz) {
            break; /* truncate: no room for this escape sequence plus NUL */
        }
        memcpy(dst + len, esc, esclen);
        len += esclen;
    }
    dst[len] = '\0';
}

/* Small helper: snprintf-append (like the escape helper above, but for
 * already-safe/formatted text) with the same "truncate, never
 * overflow" contract. Used throughout this file to build up a line
 * piece by piece without repeatedly recomputing strlen() at each call
 * site. */
static void append(char *dst, size_t dstsz, const char *fmt, ...)
{
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(dst + len, dstsz - len, fmt, ap);
        va_end(ap);
    }
}

int stressfs_report_format_header_json(char *buf, size_t bufsz,
                                        const stressfs_run_identity_t *id)
{
    if (buf == NULL || bufsz == 0) {
        return 0;
    }
    buf[0] = '\0';

    if (!stressfs_run_identity_is_complete(id)) {
        /* "a report line missing any of these is a harness bug by
         * definition" -- emit a still-valid JSON object saying so,
         * rather than a truncated/partial header a consumer might
         * mistake for the real thing. */
        append(buf, bufsz,
               "{\"type\":\"harness_bug\",\"reason\":"
               "\"run identity incomplete (missing harness_version, "
               "handler_name, handler_version, tier or seed)\"}");
        return 0;
    }

    append(buf, bufsz, "{\"type\":\"run_identity\",\"harness_version\":\"");
    stressfs_json_escape_append(buf, bufsz, id->harness_version);
    append(buf, bufsz, "\",\"handler_name\":\"");
    stressfs_json_escape_append(buf, bufsz, id->handler_name);
    append(buf, bufsz, "\",\"handler_version\":\"");
    stressfs_json_escape_append(buf, bufsz, id->handler_version);
    append(buf, bufsz, "\",\"tier\":\"");
    stressfs_json_escape_append(buf, bufsz, id->tier);
    append(buf, bufsz, "\",\"seed\":%ld,\"dos_version\":%ld,",
           id->seed, id->dos_version);

    if (id->have_geometry) {
        append(buf, bufsz,
               "\"geometry\":{\"available\":true,\"numblocks\":%ld,"
               "\"numblocksused\":%ld,\"bytesperblock\":%ld,\"disktype\":%ld},",
               id->geom_numblocks, id->geom_numblocksused,
               id->geom_bytesperblock, id->geom_disktype);
    } else {
        append(buf, bufsz, "\"geometry\":{\"available\":false},");
    }

    if (id->have_envec) {
        append(buf, bufsz,
               "\"dosenvec\":{\"available\":true,\"tablesize\":%lu,"
               "\"sizeblock\":%lu,\"surfaces\":%lu,\"sectorperblock\":%lu,"
               "\"blockspertrack\":%lu,\"reserved\":%lu,\"prealloc\":%lu,"
               "\"interleave\":%lu,\"lowcyl\":%lu,\"highcyl\":%lu,"
               "\"numbuffers\":%lu,\"maxtransfer\":%lu,\"mask\":%lu,"
               "\"bootpri\":%ld,\"dostype\":%lu}}",
               id->de_tablesize, id->de_sizeblock, id->de_surfaces,
               id->de_sectorperblock, id->de_blockspertrack, id->de_reserved,
               id->de_prealloc, id->de_interleave, id->de_lowcyl, id->de_highcyl,
               id->de_numbuffers, id->de_maxtransfer, id->de_mask,
               id->de_bootpri, id->de_dostype);
    } else {
        append(buf, bufsz, "\"dosenvec\":{\"available\":false}}");
    }

    return 1;
}

int stressfs_report_format_header_human(char *buf, size_t bufsz,
                                         const stressfs_run_identity_t *id)
{
    if (buf == NULL || bufsz == 0) {
        return 0;
    }
    buf[0] = '\0';

    if (!stressfs_run_identity_is_complete(id)) {
        append(buf, bufsz,
               "HARNESS BUG: run identity incomplete (missing harness_version, "
               "handler_name, handler_version, tier or seed) -- report is not trustworthy");
        return 0;
    }

    append(buf, bufsz,
           "stressfs run identity: harness=%s handler=\"%s\" (%s) tier=%s "
           "seed=%ld dos_version=%ld",
           id->harness_version, id->handler_name, id->handler_version,
           id->tier, id->seed, id->dos_version);

    if (id->have_geometry) {
        append(buf, bufsz,
               "\nstressfs run identity: geometry numblocks=%ld numblocksused=%ld "
               "bytesperblock=%ld disktype=0x%08lx",
               id->geom_numblocks, id->geom_numblocksused,
               id->geom_bytesperblock, (unsigned long)id->geom_disktype);
    } else {
        append(buf, bufsz, "\nstressfs run identity: geometry unavailable");
    }

    if (id->have_envec) {
        append(buf, bufsz,
               "\nstressfs run identity: dosenvec surfaces=%lu sectorperblock=%lu "
               "blockspertrack=%lu reserved=%lu prealloc=%lu lowcyl=%lu highcyl=%lu "
               "numbuffers=%lu maxtransfer=%lu dostype=0x%08lx bootpri=%ld",
               id->de_surfaces, id->de_sectorperblock, id->de_blockspertrack,
               id->de_reserved, id->de_prealloc, id->de_lowcyl, id->de_highcyl,
               id->de_numbuffers, id->de_maxtransfer, id->de_dostype, id->de_bootpri);
    } else {
        append(buf, bufsz, "\nstressfs run identity: dosenvec unavailable");
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* M3: the applied-quirks summary at the top of the report. See        */
/* report.h's declaration comment for the "active for this run" vs.   */
/* "actually fired" decision, and quirks.h for what makes an entry    */
/* active.                                                            */
/* ------------------------------------------------------------------ */

/* Shared by both renderings so they can never disagree about WHICH
 * entries are listed -- only about how they are spelled. */
static int quirks_state(const stressfs_quirks_db_t *db, const char **why)
{
    if (db == NULL) {
        *why = "no quirks database";
        return 0;
    }
    if (db->suppressed) {
        *why = "suppressed by NOQUIRKS (-K): the file is not consulted at all, so a "
               "quirk-covered deviation below shows up as the raw finding it is";
        return 0;
    }
    if (!db->loaded) {
        *why = "no quirks file was loaded (not found, or REFUSED as malformed -- see the "
               "notes above)";
        return 0;
    }
    *why = "";
    return 1;
}

void stressfs_report_format_quirks_human(char *buf, size_t bufsz,
                                          const stressfs_quirks_db_t *db,
                                          const stressfs_quirk_identity_t *ident)
{
    const char *why;
    int i;
    int n_active = 0;

    if (buf == NULL || bufsz == 0) {
        return;
    }
    buf[0] = '\0';

    if (!quirks_state(db, &why)) {
        append(buf, bufsz, "stressfs applied quirks: none -- %s", why);
        return;
    }

    for (i = 0; i < db->n_entries; i++) {
        char reason[STRESSFS_QUIRK_REASON_LEN];
        int j;

        if (!stressfs_quirks_entry_active(db, i, ident, reason, sizeof(reason))) {
            continue;
        }
        if (n_active == 0) {
            append(buf, bufsz, "stressfs applied quirks: from %s (%d entries in file)",
                   db->path[0] != '\0' ? db->path : "stressfs.quirks", db->n_entries);
        }
        n_active++;
        append(buf, bufsz, "\nstressfs applied quirks:   %s [%s] %s",
               db->entries[i].id,
               stressfs_quirk_status_name((stressfs_quirk_status_t)db->entries[i].status),
               reason);
        for (j = 0; j < db->entries[i].n_actions; j++) {
            const stressfs_quirk_action_t *a = &db->entries[i].actions[j];
            append(buf, bufsz, "\nstressfs applied quirks:     action %s %s%s",
                   stressfs_quirk_verb_name((stressfs_quirk_verb_t)a->verb),
                   a->target, a->target_wild ? "*" : "");
            if (a->verb == STRESSFS_QUIRK_EXPECT_DEVIATION) {
                if (a->res1_any) {
                    append(buf, bufsz, " res1=*");
                } else {
                    append(buf, bufsz, " res1=%ld", a->res1);
                }
                if (a->res2_any) {
                    append(buf, bufsz, " res2=*");
                } else {
                    append(buf, bufsz, " res2=%ld", a->res2);
                }
            }
        }
    }

    if (n_active == 0) {
        append(buf, bufsz,
               "stressfs applied quirks: none active -- %d entries in %s, none both matched "
               "this handler and cleared the confidence gate (reported/suspected entries "
               "need FORCEQUIRK=ID)",
               db->n_entries, db->path[0] != '\0' ? db->path : "stressfs.quirks");
    }
}

void stressfs_report_format_quirks_json(char *buf, size_t bufsz,
                                         const stressfs_quirks_db_t *db,
                                         const stressfs_quirk_identity_t *ident)
{
    const char *why;
    int i;
    int n_active = 0;
    int loaded;

    if (buf == NULL || bufsz == 0) {
        return;
    }
    buf[0] = '\0';

    loaded = quirks_state(db, &why);

    append(buf, bufsz, "{\"type\":\"applied_quirks\",\"file\":\"");
    stressfs_json_escape_append(buf, bufsz,
                                 (db != NULL && db->path[0] != '\0') ? db->path : "");
    append(buf, bufsz, "\",\"loaded\":%s,\"suppressed\":%s,\"entries_in_file\":%d,\"reason\":\"",
           loaded ? "true" : "false",
           (db != NULL && db->suppressed) ? "true" : "false",
           (db != NULL) ? db->n_entries : 0);
    stressfs_json_escape_append(buf, bufsz, why);
    append(buf, bufsz, "\",\"quirks\":[");

    if (loaded) {
        for (i = 0; i < db->n_entries; i++) {
            char reason[STRESSFS_QUIRK_REASON_LEN];
            int j;

            if (!stressfs_quirks_entry_active(db, i, ident, reason, sizeof(reason))) {
                continue;
            }
            append(buf, bufsz, "%s{\"id\":\"", (n_active == 0) ? "" : ",");
            n_active++;
            stressfs_json_escape_append(buf, bufsz, db->entries[i].id);
            append(buf, bufsz, "\",\"confidence\":\"%s\",\"match_reason\":\"",
                   stressfs_quirk_status_name((stressfs_quirk_status_t)db->entries[i].status));
            stressfs_json_escape_append(buf, bufsz, reason);
            append(buf, bufsz, "\",\"actions\":[");
            for (j = 0; j < db->entries[i].n_actions; j++) {
                const stressfs_quirk_action_t *a = &db->entries[i].actions[j];
                append(buf, bufsz, "%s{\"verb\":\"%s\",\"target\":\"",
                       (j == 0) ? "" : ",",
                       stressfs_quirk_verb_name((stressfs_quirk_verb_t)a->verb));
                stressfs_json_escape_append(buf, bufsz, a->target);
                if (a->target_wild) {
                    stressfs_json_escape_append(buf, bufsz, "*");
                }
                append(buf, bufsz, "\"");
                if (a->verb == STRESSFS_QUIRK_EXPECT_DEVIATION) {
                    if (a->res1_any) {
                        append(buf, bufsz, ",\"res1\":null");
                    } else {
                        append(buf, bufsz, ",\"res1\":%ld", a->res1);
                    }
                    if (a->res2_any) {
                        append(buf, bufsz, ",\"res2\":null");
                    } else {
                        append(buf, bufsz, ",\"res2\":%ld", a->res2);
                    }
                }
                append(buf, bufsz, "}");
            }
            append(buf, bufsz, "]}");
        }
    }

    append(buf, bufsz, "],\"active_count\":%d}", n_active);
}

/* ------------------------------------------------------------------ */
/* M4: the per-step report line for `stressfs random`.                 */
/* ------------------------------------------------------------------ */

void stressfs_report_format_step_human(char *buf, size_t bufsz,
                                        const stressfs_random_step_t *s)
{
    if (buf == NULL || bufsz == 0) {
        return;
    }
    buf[0] = '\0';
    if (s == NULL) {
        append(buf, bufsz, "inconclusive  step=? (NULL step)");
        return;
    }

    append(buf, bufsz, "%-12s step=%-6ld op=%-8s real=(%ld,%ld) expect=(",
           s->diverged ? "fail" : "pass", s->step,
           s->op_kind != NULL ? s->op_kind : "?",
           s->real_res1, s->real_res2);
    if (s->expect_res1_any) {
        append(buf, bufsz, "*,");
    } else {
        append(buf, bufsz, "%ld,", s->expect_res1);
    }
    if (s->expect_res2_any) {
        append(buf, bufsz, "*)");
    } else {
        append(buf, bufsz, "%ld)", s->expect_res2);
    }
    append(buf, bufsz, "  %s", s->detail != NULL ? s->detail : "");
    if (s->diverged) {
        append(buf, bufsz, "  [DIVERGED: %s]",
               s->diverge_reason != NULL ? s->diverge_reason : "?");
    }
}

void stressfs_report_format_step_json(char *buf, size_t bufsz,
                                       const stressfs_random_step_t *s)
{
    if (buf == NULL || bufsz == 0) {
        return;
    }
    buf[0] = '\0';
    if (s == NULL) {
        append(buf, bufsz, "{\"type\":\"harness_bug\",\"reason\":\"NULL step\"}");
        return;
    }

    append(buf, bufsz,
           "{\"type\":\"random_step\",\"verdict\":\"%s\",\"oracle\":\"model\",\"step\":%ld,"
           "\"op\":\"",
           s->diverged ? "fail" : "pass", s->step);
    stressfs_json_escape_append(buf, bufsz, s->op_kind != NULL ? s->op_kind : "?");
    append(buf, bufsz, "\",\"detail\":\"");
    stressfs_json_escape_append(buf, bufsz, s->detail != NULL ? s->detail : "");
    append(buf, bufsz, "\",\"real_res1\":%ld,\"real_res2\":%ld,", s->real_res1, s->real_res2);

    if (s->expect_res1_any) {
        append(buf, bufsz, "\"expect_res1\":null,");
    } else {
        append(buf, bufsz, "\"expect_res1\":%ld,", s->expect_res1);
    }
    if (s->expect_res2_any) {
        append(buf, bufsz, "\"expect_res2\":null,");
    } else {
        append(buf, bufsz, "\"expect_res2\":%ld,", s->expect_res2);
    }

    append(buf, bufsz, "\"diverged\":%s,\"diverge_reason\":\"",
           s->diverged ? "true" : "false");
    stressfs_json_escape_append(buf, bufsz, s->diverge_reason != NULL ? s->diverge_reason : "");
    append(buf, bufsz, "\"}");
}
