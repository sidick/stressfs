/* amiga/lib/report.c -- see amiga/include/stressfs/report.h for the
 * design rationale (why geometry/DosEnvec are excluded from the
 * completeness check, the JSON escaping contract).
 */

#include "stressfs/report.h"

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
