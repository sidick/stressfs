/* amiga/lib/quirks.c -- the portable stressfs.quirks parser, match
 * evaluator and quirks-application stage (M3). See
 * amiga/include/stressfs/quirks.h for the whole design: the grammar,
 * the three action verbs and the verdict each produces, the confidence
 * tiers, and -- most importantly -- the MALFORMED vs. UNEVALUABLE
 * distinction that decides whether a defect refuses the whole file or
 * merely warns.
 *
 * No Amiga dependency, no allocation, no file I/O: the caller hands in
 * the file's text (amiga/src/quirks_file.c reads it with dos.library,
 * amiga/tests/quirks_test.c with stdio) and owns the db struct.
 */

#include "stressfs/quirks.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small string helpers. Hand-rolled for the same reason the rest of    */
/* lib/ hand-rolls its JSON: strcasecmp/strdup are not dependably      */
/* present on this cross toolchain, and the parser must behave         */
/* identically on the host and on m68k.                               */
/* ------------------------------------------------------------------ */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int ci_has(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *h;

    if (nlen == 0) {
        return 1;
    }
    for (h = hay; *h != '\0'; h++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (h[i] == '\0' || lower((unsigned char)h[i]) != lower((unsigned char)needle[i])) {
                break;
            }
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

/* Copies at most dstsz-1 bytes. Returns 0 if `src` did not fit --
 * callers treat that as a MALFORMED-file error rather than silently
 * truncating an id or a target into something that matches the wrong
 * thing. */
static int copy_checked(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        memcpy(dst, src, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return 0;
    }
    memcpy(dst, src, n + 1);
    return 1;
}

static int parse_long(const char *s, long *out)
{
    long v = 0;
    long sign = 1;
    int digits = 0;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (long)(*s - '0');
        s++;
        digits++;
    }
    if (digits == 0 || *s != '\0') {
        return 0;
    }
    *out = v * sign;
    return 1;
}

const char *stressfs_quirk_status_name(stressfs_quirk_status_t st)
{
    switch (st) {
    case STRESSFS_QUIRK_CONFIRMED: return "confirmed";
    case STRESSFS_QUIRK_REPORTED:  return "reported";
    case STRESSFS_QUIRK_SUSPECTED: return "suspected";
    default:                       return "?";
    }
}

const char *stressfs_quirk_verb_name(stressfs_quirk_verb_t v)
{
    switch (v) {
    case STRESSFS_QUIRK_SKIP:             return "skip";
    case STRESSFS_QUIRK_DOWNGRADE:        return "downgrade";
    case STRESSFS_QUIRK_EXPECT_DEVIATION: return "expect-deviation";
    default:                              return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

struct parse_state {
    stressfs_quirks_db_t *db;
    stressfs_quirk_diag_fn diag;
    void *diag_ctx;
    const char *path;
    long lineno;
    int fatal;
};

/* A malformed-grammar error: report it with file/line context and set
 * the fatal flag. The caller unwinds and discards the whole db. */
static void fatalf(struct parse_state *ps, const char *fmt, ...)
{
    char msg[256];
    char full[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(full, sizeof(full), "stressfs: quirks: %s:%ld: %s -- REFUSING THE WHOLE FILE "
                                 "(no quirk from it is applied)",
             (ps->path != NULL && ps->path[0] != '\0') ? ps->path : "stressfs.quirks",
             ps->lineno, msg);
    if (ps->diag != NULL) {
        ps->diag(ps->diag_ctx, full);
    }
    ps->fatal = 1;
}

static void warnf(struct parse_state *ps, const char *fmt, ...)
{
    char msg[256];
    char full[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(full, sizeof(full), "stressfs: quirks: %s:%ld: %s",
             (ps->path != NULL && ps->path[0] != '\0') ? ps->path : "stressfs.quirks",
             ps->lineno, msg);
    if (ps->diag != NULL) {
        ps->diag(ps->diag_ctx, full);
    }
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

void stressfs_quirks_init(stressfs_quirks_db_t *db)
{
    if (db == NULL) {
        return;
    }
    memset(db, 0, sizeof(*db));
}

static int parse_match_token(struct parse_state *ps, stressfs_quirk_entry_t *e,
                              const char *tok)
{
    char key[24];
    size_t ki = 0;
    const char *s = tok;
    unsigned char op;
    const char *val;
    stressfs_quirk_cond_t *c;

    while (*s != '\0' && *s != '=' && *s != '<' && *s != '>' && *s != '~') {
        if (ki + 1 >= sizeof(key)) {
            fatalf(ps, "match key in '%s' is too long", tok);
            return 0;
        }
        key[ki++] = *s;
        s++;
    }
    key[ki] = '\0';

    if (*s == '\0' || ki == 0) {
        /* No operator at all, or no key before it: this is not the
         * format. MALFORMED (case (a)), not a merely unevaluable key. */
        fatalf(ps, "malformed match token '%s' (expected key=value, key<value, "
                   "key<=value, key>value, key>=value or key~value)", tok);
        return 0;
    }

    if (*s == '~') {
        op = STRESSFS_QUIRK_OP_SUBSTR;
        s++;
    } else if (*s == '<') {
        s++;
        if (*s == '=') { op = STRESSFS_QUIRK_OP_LE; s++; } else { op = STRESSFS_QUIRK_OP_LT; }
    } else if (*s == '>') {
        s++;
        if (*s == '=') { op = STRESSFS_QUIRK_OP_GE; s++; } else { op = STRESSFS_QUIRK_OP_GT; }
    } else {
        op = STRESSFS_QUIRK_OP_EQ;
        s++;
    }
    val = s;

    if (*val == '\0') {
        fatalf(ps, "match token '%s' has no value", tok);
        return 0;
    }

    if (e->n_conds >= STRESSFS_QUIRK_MAX_CONDS) {
        fatalf(ps, "quirk '%s' has more than %d match conditions",
               e->id, STRESSFS_QUIRK_MAX_CONDS);
        return 0;
    }

    c = &e->conds[e->n_conds];
    memset(c, 0, sizeof(*c));
    c->op = op;
    if (!copy_checked(c->raw, sizeof(c->raw), tok)) {
        fatalf(ps, "match token '%s' is too long", tok);
        return 0;
    }

    if (ci_eq(key, "name")) {
        if (op != STRESSFS_QUIRK_OP_EQ) {
            fatalf(ps, "match key 'name' only supports '=' (got '%s')", tok);
            return 0;
        }
        c->key = STRESSFS_QUIRK_KEY_NAME;
        if (!copy_checked(c->str, sizeof(c->str), val)) {
            fatalf(ps, "match value in '%s' is too long", tok);
            return 0;
        }
    } else if (ci_eq(key, "id")) {
        if (op != STRESSFS_QUIRK_OP_SUBSTR) {
            fatalf(ps, "match key 'id' only supports '~' (got '%s')", tok);
            return 0;
        }
        c->key = STRESSFS_QUIRK_KEY_ID;
        if (!copy_checked(c->str, sizeof(c->str), val)) {
            fatalf(ps, "match value in '%s' is too long", tok);
            return 0;
        }
    } else if (ci_eq(key, "version") || ci_eq(key, "revision") ||
               ci_eq(key, "kick") || ci_eq(key, "dos")) {
        if (op == STRESSFS_QUIRK_OP_SUBSTR) {
            fatalf(ps, "match key '%s' is numeric and does not support '~'", key);
            return 0;
        }
        if (!parse_long(val, &c->num)) {
            fatalf(ps, "match token '%s' needs a decimal number", tok);
            return 0;
        }
        c->key = ci_eq(key, "version")  ? STRESSFS_QUIRK_KEY_VERSION
               : ci_eq(key, "revision") ? STRESSFS_QUIRK_KEY_REVISION
               : ci_eq(key, "kick")     ? STRESSFS_QUIRK_KEY_KICK
                                        : STRESSFS_QUIRK_KEY_DOS;
    } else {
        /* Case (b): well-formed, but this build cannot evaluate it.
         * Warn once per entry, mark the entry force-only, carry on. */
        c->key = STRESSFS_QUIRK_KEY_UNEVALUABLE;
        if (!e->unevaluable) {
            warnf(ps, "quirk '%s': match key '%s' cannot be evaluated by this build; "
                      "the entry is marked unmatchable and will only ever apply when "
                      "named explicitly with FORCEQUIRK=%s", e->id, key, e->id);
        }
        e->unevaluable = 1;
    }

    e->n_conds++;
    return 1;
}

static int parse_match_line(struct parse_state *ps, stressfs_quirk_entry_t *e,
                             const char *rest)
{
    const char *p = rest;
    int any = 0;

    while (*p != '\0') {
        char tok[STRESSFS_QUIRK_STR_LEN * 2];
        size_t n = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            if (n + 1 >= sizeof(tok)) {
                fatalf(ps, "match token is too long");
                return 0;
            }
            tok[n++] = *p++;
        }
        tok[n] = '\0';

        if (!parse_match_token(ps, e, tok)) {
            return 0;
        }
        any = 1;
    }

    if (!any) {
        fatalf(ps, "quirk '%s': empty 'match' line", e->id);
        return 0;
    }
    return 1;
}

/* "<res1>/<res2>", either half a decimal number or '*' for any. */
static int parse_res_pair(struct parse_state *ps, stressfs_quirk_action_t *a,
                           const char *tok)
{
    char left[24];
    const char *slash = strchr(tok, '/');
    size_t n;

    if (slash == NULL) {
        fatalf(ps, "expect-deviation needs a <res1>/<res2> pair (got '%s')", tok);
        return 0;
    }
    n = (size_t)(slash - tok);
    if (n == 0 || n >= sizeof(left)) {
        fatalf(ps, "malformed res pair '%s'", tok);
        return 0;
    }
    memcpy(left, tok, n);
    left[n] = '\0';

    if (strcmp(left, "*") == 0) {
        a->res1_any = 1;
    } else if (!parse_long(left, &a->res1)) {
        fatalf(ps, "malformed dp_Res1 in res pair '%s' (want a decimal number or '*')", tok);
        return 0;
    }

    slash++;
    if (strcmp(slash, "*") == 0) {
        a->res2_any = 1;
    } else if (!parse_long(slash, &a->res2)) {
        fatalf(ps, "malformed dp_Res2 in res pair '%s' (want a decimal number or '*')", tok);
        return 0;
    }
    return 1;
}

static int next_token(const char **pp, char *out, size_t outsz)
{
    const char *p = *pp;
    size_t n = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1 >= outsz) {
            break;
        }
        out[n++] = *p++;
    }
    /* skip anything that did not fit, so the caller's "extra token"
     * check still sees the rest of the line correctly */
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return 1;
}

static int parse_action_line(struct parse_state *ps, stressfs_quirk_entry_t *e,
                              const char *rest)
{
    const char *p = rest;
    char verb[32];
    char target[STRESSFS_QUIRK_TARGET_LEN + 8];
    char extra[64];
    stressfs_quirk_action_t *a;
    size_t tlen;

    if (!next_token(&p, verb, sizeof(verb))) {
        fatalf(ps, "quirk '%s': empty 'action' line", e->id);
        return 0;
    }

    if (e->n_actions >= STRESSFS_QUIRK_MAX_ACTIONS) {
        fatalf(ps, "quirk '%s' has more than %d actions", e->id, STRESSFS_QUIRK_MAX_ACTIONS);
        return 0;
    }
    a = &e->actions[e->n_actions];
    memset(a, 0, sizeof(*a));

    if (ci_eq(verb, "skip")) {
        a->verb = STRESSFS_QUIRK_SKIP;
    } else if (ci_eq(verb, "downgrade")) {
        a->verb = STRESSFS_QUIRK_DOWNGRADE;
    } else if (ci_eq(verb, "expect-deviation")) {
        a->verb = STRESSFS_QUIRK_EXPECT_DEVIATION;
    } else {
        fatalf(ps, "quirk '%s': unknown action verb '%s' (want skip, downgrade or "
                   "expect-deviation)", e->id, verb);
        return 0;
    }

    if (!next_token(&p, target, sizeof(target))) {
        fatalf(ps, "quirk '%s': action '%s' needs a test-id target", e->id, verb);
        return 0;
    }
    tlen = strlen(target);
    if (tlen > 0 && target[tlen - 1] == '*') {
        a->target_wild = 1;
        target[tlen - 1] = '\0';
        if (target[0] == '\0') {
            fatalf(ps, "quirk '%s': action '%s' target '*' would match every test",
                   e->id, verb);
            return 0;
        }
    }
    if (!copy_checked(a->target, sizeof(a->target), target)) {
        fatalf(ps, "quirk '%s': action target '%s' is too long", e->id, target);
        return 0;
    }

    if (a->verb == STRESSFS_QUIRK_EXPECT_DEVIATION) {
        char pair[48];
        if (!next_token(&p, pair, sizeof(pair))) {
            fatalf(ps, "quirk '%s': expect-deviation %s needs a <res1>/<res2> pair "
                       "(use 'downgrade' for the blanket form)", e->id, a->target);
            return 0;
        }
        if (!parse_res_pair(ps, a, pair)) {
            return 0;
        }
    }

    if (next_token(&p, extra, sizeof(extra))) {
        fatalf(ps, "quirk '%s': action '%s' has unexpected extra argument '%s'",
               e->id, verb, extra);
        return 0;
    }

    e->n_actions++;
    return 1;
}

static int finish_entry(struct parse_state *ps, stressfs_quirk_entry_t *e)
{
    if (e == NULL) {
        return 1;
    }
    if (e->n_conds == 0) {
        fatalf(ps, "quirk '%s' has no 'match' line", e->id);
        return 0;
    }
    if (e->n_actions == 0) {
        fatalf(ps, "quirk '%s' has no 'action' line", e->id);
        return 0;
    }
    if (e->status == 0xff) {
        fatalf(ps, "quirk '%s' has no 'status' line (want confirmed, reported or "
                   "suspected)", e->id);
        return 0;
    }
    return 1;
}

int stressfs_quirks_parse(stressfs_quirks_db_t *db, const char *path,
                          const char *text,
                          stressfs_quirk_diag_fn diag, void *diag_ctx)
{
    struct parse_state ps;
    stressfs_quirk_entry_t *cur = NULL;
    const char *p;
    int saved_forced_n;
    char saved_forced[STRESSFS_QUIRK_MAX_FORCED][STRESSFS_QUIRK_ID_LEN];
    int saved_suppressed;

    if (db == NULL || text == NULL) {
        return 0;
    }

    /* The forced-id list and the -K flag come from the command line,
     * not from the file, so they survive a refusal. */
    saved_forced_n = db->n_forced;
    memcpy(saved_forced, db->forced, sizeof(saved_forced));
    saved_suppressed = db->suppressed;

    stressfs_quirks_init(db);
    db->n_forced = saved_forced_n;
    memcpy(db->forced, saved_forced, sizeof(saved_forced));
    db->suppressed = saved_suppressed;
    if (path != NULL) {
        (void)copy_checked(db->path, sizeof(db->path), path);
    }

    memset(&ps, 0, sizeof(ps));
    ps.db = db;
    ps.diag = diag;
    ps.diag_ctx = diag_ctx;
    ps.path = path;
    ps.lineno = 0;

    p = text;
    while (*p != '\0' && !ps.fatal) {
        /* STATIC for the same reason amiga/src/conform.c's report
         * buffers are: a 4KB AmigaDOS CLI stack does not have room for
         * half a kilobyte of line buffer plus the diagnostic
         * formatter's own buffers underneath it. The parser runs once,
         * from the single conform process, before any other task
         * exists -- the same load-time, single-context discipline
         * devsoak's quirks.c documents -- so this is safe, and it is
         * why stressfs_quirks_parse() is documented as a load-time
         * call rather than a reentrant one. */
        static char line[512];
        size_t n = 0;
        const char *kwend;
        char kw[24];
        size_t kwlen;
        const char *rest;

        /* Pull one line out of the buffer without modifying it. */
        while (*p != '\0' && *p != '\n') {
            if (n + 1 < sizeof(line)) {
                line[n++] = *p;
            }
            p++;
        }
        if (*p == '\n') {
            p++;
        }
        line[n] = '\0';
        ps.lineno++;

        /* Trim trailing CR/space/tab (CRLF-tolerant). */
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }

        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            continue; /* continuation of a `because` block */
        }
        if (line[0] == '#') {
            continue;
        }

        kwend = line;
        while (*kwend != '\0' && *kwend != ' ' && *kwend != '\t') {
            kwend++;
        }
        kwlen = (size_t)(kwend - line);
        if (kwlen >= sizeof(kw)) {
            fatalf(&ps, "keyword is too long to be part of this format");
            break;
        }
        memcpy(kw, line, kwlen);
        kw[kwlen] = '\0';
        rest = kwend;
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }

        if (ci_eq(kw, "quirk")) {
            int i;

            if (!finish_entry(&ps, cur)) {
                break;
            }
            if (rest[0] == '\0') {
                fatalf(&ps, "'quirk' with no id");
                break;
            }
            if (db->n_entries >= STRESSFS_QUIRK_MAX_ENTRIES) {
                fatalf(&ps, "more than %d quirk entries (this build's limit)",
                       STRESSFS_QUIRK_MAX_ENTRIES);
                break;
            }
            for (i = 0; i < db->n_entries; i++) {
                if (strcmp(db->entries[i].id, rest) == 0) {
                    fatalf(&ps, "duplicate quirk id '%s'", rest);
                    break;
                }
            }
            if (ps.fatal) {
                break;
            }
            cur = &db->entries[db->n_entries];
            memset(cur, 0, sizeof(*cur));
            if (!copy_checked(cur->id, sizeof(cur->id), rest)) {
                fatalf(&ps, "quirk id '%s' is longer than %d characters",
                       rest, STRESSFS_QUIRK_ID_LEN - 1);
                break;
            }
            cur->status = 0xff; /* "no status line seen yet" -- finish_entry() rejects it */
            db->n_entries++;
        } else if (ci_eq(kw, "match")) {
            if (cur == NULL) {
                fatalf(&ps, "'match' before any 'quirk' line");
                break;
            }
            if (!parse_match_line(&ps, cur, rest)) {
                break;
            }
        } else if (ci_eq(kw, "action")) {
            if (cur == NULL) {
                fatalf(&ps, "'action' before any 'quirk' line");
                break;
            }
            if (!parse_action_line(&ps, cur, rest)) {
                break;
            }
        } else if (ci_eq(kw, "because")) {
            if (cur == NULL) {
                fatalf(&ps, "'because' before any 'quirk' line");
                break;
            }
            /* Free text, deliberately not stored (same as devsoak's
             * parser): docs/quirks.md is generated from the FILE by
             * ci/quirks-doc.sh, so the explanation never needs to be
             * held in the harness's memory on an Amiga. Continuation
             * lines are skipped by the leading-whitespace rule above. */
            if (rest[0] == '\0') {
                fatalf(&ps, "quirk '%s': empty 'because' line", cur->id);
                break;
            }
        } else if (ci_eq(kw, "status")) {
            if (cur == NULL) {
                fatalf(&ps, "'status' before any 'quirk' line");
                break;
            }
            if (ci_eq(rest, "confirmed")) {
                cur->status = STRESSFS_QUIRK_CONFIRMED;
            } else if (ci_eq(rest, "reported")) {
                cur->status = STRESSFS_QUIRK_REPORTED;
            } else if (ci_eq(rest, "suspected")) {
                cur->status = STRESSFS_QUIRK_SUSPECTED;
            } else {
                /* Deliberately NOT devsoak's "treat as suspected": an
                 * unrecognised confidence word means the file was
                 * written against a different vocabulary, and guessing
                 * the safe end of it is exactly the kind of quiet
                 * half-application M3's exit criterion forbids. */
                fatalf(&ps, "quirk '%s': unknown status '%s' (want confirmed, reported "
                            "or suspected)", cur->id, rest);
                break;
            }
        } else {
            fatalf(&ps, "unknown keyword '%s' (want quirk, match, because, action or "
                        "status)", kw);
            break;
        }
    }

    if (!ps.fatal) {
        (void)finish_entry(&ps, cur);
    }

    if (ps.fatal) {
        /* Never half-applied: throw away everything parsed so far. */
        char tmp_path[128];
        (void)copy_checked(tmp_path, sizeof(tmp_path), db->path);
        stressfs_quirks_init(db);
        db->n_forced = saved_forced_n;
        memcpy(db->forced, saved_forced, sizeof(saved_forced));
        db->suppressed = saved_suppressed;
        (void)copy_checked(db->path, sizeof(db->path), tmp_path);
        db->loaded = 0;
        return 0;
    }

    db->loaded = 1;
    return 1;
}

int stressfs_quirks_force(stressfs_quirks_db_t *db, const char *ids,
                          stressfs_quirk_diag_fn diag, void *diag_ctx)
{
    const char *p;

    if (db == NULL || ids == NULL) {
        return 0;
    }

    p = ids;
    while (*p != '\0') {
        char one[STRESSFS_QUIRK_ID_LEN];
        size_t n = 0;

        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t') {
            if (n + 1 >= sizeof(one)) {
                if (diag != NULL) {
                    diag(diag_ctx, "stressfs: quirks: FORCEQUIRK id is too long, ignored");
                }
                return 0;
            }
            one[n++] = *p++;
        }
        one[n] = '\0';

        if (db->n_forced >= STRESSFS_QUIRK_MAX_FORCED) {
            if (diag != NULL) {
                diag(diag_ctx, "stressfs: quirks: too many FORCEQUIRK ids, ignored");
            }
            return 0;
        }
        memcpy(db->forced[db->n_forced], one, n + 1);
        db->n_forced++;
    }
    return 1;
}

int stressfs_quirks_identity_from_ver(stressfs_quirk_identity_t *ident,
                                      const char *verstring)
{
    const char *p;
    size_t n;

    if (ident == NULL) {
        return 0;
    }
    ident->name[0] = '\0';
    ident->idstring[0] = '\0';
    ident->version = 0;
    ident->revision = 0;

    if (verstring == NULL || verstring[0] == '\0') {
        return 0;
    }
    (void)copy_checked(ident->idstring, sizeof(ident->idstring), verstring);

    p = verstring;
    if (strncmp(p, "$VER:", 5) == 0) {
        p += 5;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    /* The name is the first whitespace-delimited token. Real handlers
     * spell this "fs" (AmigaOS 3.2's FastFileSystem), "FastFileSystem",
     * "PFS3", ... -- whatever its own $VER: string says, which is the
     * only name the harness can actually observe, so it is the name the
     * quirks file must use. */
    n = 0;
    while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t') {
        n++;
    }
    if (n == 0 || n >= sizeof(ident->name)) {
        return 0;
    }
    memcpy(ident->name, p, n);
    ident->name[n] = '\0';
    p += n;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    /* "<version>.<revision>" */
    {
        long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0) {
            return 0;
        }
        ident->version = v;
        if (*p == '.') {
            long r = 0;
            p++;
            digits = 0;
            while (*p >= '0' && *p <= '9') {
                r = r * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) {
                ident->revision = r;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Match evaluation                                                    */
/* ------------------------------------------------------------------ */

static int cmp_num(long a, unsigned char op, long b)
{
    switch (op) {
    case STRESSFS_QUIRK_OP_EQ: return a == b;
    case STRESSFS_QUIRK_OP_LT: return a <  b;
    case STRESSFS_QUIRK_OP_LE: return a <= b;
    case STRESSFS_QUIRK_OP_GT: return a >  b;
    case STRESSFS_QUIRK_OP_GE: return a >= b;
    default:                   return 0;
    }
}

static int eval_cond(const stressfs_quirk_cond_t *c,
                      const stressfs_quirk_identity_t *ident)
{
    switch (c->key) {
    case STRESSFS_QUIRK_KEY_NAME:
        return ci_eq(ident->name, c->str);
    case STRESSFS_QUIRK_KEY_ID:
        return ci_has(ident->idstring, c->str);
    case STRESSFS_QUIRK_KEY_VERSION:
        return cmp_num(ident->version, c->op, c->num);
    case STRESSFS_QUIRK_KEY_REVISION:
        return cmp_num(ident->revision, c->op, c->num);
    case STRESSFS_QUIRK_KEY_KICK:
        return cmp_num(ident->kick_version, c->op, c->num);
    case STRESSFS_QUIRK_KEY_DOS:
        return cmp_num(ident->dos_version, c->op, c->num);
    default:
        /* An unevaluable key never holds on its own; the entry-level
         * `unevaluable` flag plus FORCEQUIRK is what lets such an entry
         * apply at all (see quirks.h). */
        return 0;
    }
}

static int is_forced(const stressfs_quirks_db_t *db, const char *id)
{
    int i;
    for (i = 0; i < db->n_forced; i++) {
        if (strcmp(db->forced[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

int stressfs_quirks_entry_active(const stressfs_quirks_db_t *db, int idx,
                                 const stressfs_quirk_identity_t *ident,
                                 char *reason, size_t reasonsz)
{
    const stressfs_quirk_entry_t *e;
    int forced;
    int i;

    if (reason != NULL && reasonsz > 0) {
        reason[0] = '\0';
    }
    if (db == NULL || ident == NULL || db->suppressed || !db->loaded) {
        return 0;
    }
    if (idx < 0 || idx >= db->n_entries) {
        return 0;
    }
    e = &db->entries[idx];
    forced = is_forced(db, e->id);

    /* Confidence gate. */
    if (e->status != STRESSFS_QUIRK_CONFIRMED && !forced) {
        return 0;
    }

    /* Match gate: every evaluable condition must hold. An unevaluable
     * key makes the entry force-only (and FORCEQUIRK then waives just
     * that key, not the evaluable ones). */
    for (i = 0; i < e->n_conds; i++) {
        if (e->conds[i].key == STRESSFS_QUIRK_KEY_UNEVALUABLE) {
            if (!forced) {
                return 0;
            }
            continue;
        }
        if (!eval_cond(&e->conds[i], ident)) {
            return 0;
        }
    }

    if (reason != NULL && reasonsz > 0) {
        size_t len;
        snprintf(reason, reasonsz, "%s; ",
                 forced ? (e->status == STRESSFS_QUIRK_CONFIRMED
                               ? "auto (confirmed) and forced by id"
                               : "forced by id (FORCEQUIRK)")
                        : "auto (confirmed)");
        for (i = 0; i < e->n_conds; i++) {
            len = strlen(reason);
            if (len + 1 >= reasonsz) {
                break;
            }
            snprintf(reason + len, reasonsz - len, "%s%s%s",
                     (i == 0) ? "" : " AND ",
                     e->conds[i].raw,
                     (e->conds[i].key == STRESSFS_QUIRK_KEY_UNEVALUABLE)
                         ? " (unevaluable, waived by FORCEQUIRK)" : "");
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Action targeting + the application stage                            */
/* ------------------------------------------------------------------ */

static int target_matches(const stressfs_quirk_action_t *a, const char *test_id)
{
    if (test_id == NULL || test_id[0] == '\0') {
        return 0;
    }
    if (a->target_wild) {
        size_t n = strlen(a->target);
        return strncmp(test_id, a->target, n) == 0;
    }
    return strcmp(test_id, a->target) == 0;
}

static void fill_applied(stressfs_quirk_applied_t *out,
                          const stressfs_quirk_entry_t *e,
                          const stressfs_quirk_action_t *a,
                          const char *reason)
{
    out->applied = 1;
    out->id = e->id;
    out->confidence = stressfs_quirk_status_name((stressfs_quirk_status_t)e->status);
    out->verb = a->verb;
    if (reason != NULL) {
        size_t n = strlen(reason);
        if (n >= sizeof(out->match_reason)) {
            n = sizeof(out->match_reason) - 1;
        }
        memcpy(out->match_reason, reason, n);
        out->match_reason[n] = '\0';
    }
}

static void clear_applied(stressfs_quirk_applied_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->id = "";
    out->confidence = "";
}

int stressfs_quirks_check_skip(const stressfs_quirks_db_t *db,
                               const stressfs_quirk_identity_t *ident,
                               const stressfs_test_entry_t *entry,
                               stressfs_quirk_applied_t *out)
{
    int i, j;

    clear_applied(out);
    if (db == NULL || ident == NULL || entry == NULL || db->suppressed || !db->loaded) {
        return 0;
    }

    for (i = 0; i < db->n_entries; i++) {
        char reason[STRESSFS_QUIRK_REASON_LEN];
        const stressfs_quirk_entry_t *e = &db->entries[i];

        if (!stressfs_quirks_entry_active(db, i, ident, reason, sizeof(reason))) {
            continue;
        }
        for (j = 0; j < e->n_actions; j++) {
            const stressfs_quirk_action_t *a = &e->actions[j];
            if (a->verb != STRESSFS_QUIRK_SKIP) {
                continue;
            }
            if (!target_matches(a, entry->id)) {
                continue;
            }
            if (out != NULL) {
                fill_applied(out, e, a, reason);
            }
            return 1;
        }
    }
    return 0;
}

stressfs_verdict_t stressfs_quirks_apply_verdict(const stressfs_quirks_db_t *db,
                                                 const stressfs_quirk_identity_t *ident,
                                                 const stressfs_test_entry_t *entry,
                                                 stressfs_verdict_t raw,
                                                 stressfs_pkt_result_t got,
                                                 stressfs_quirk_applied_t *out)
{
    int i, j;

    clear_applied(out);
    if (db == NULL || ident == NULL || entry == NULL || db->suppressed || !db->loaded) {
        return raw;
    }

    /* Only a raw `fail` is ever softened. `pass` means the handler did
     * the documented thing (a quirk for it simply does not fire, and
     * the report shows a clean pass -- never "deviation" for correct
     * behaviour); `unsupported` is already the gentlest verdict; and
     * `inconclusive` is deliberately untouchable, because a hang means
     * the handler may be damaged and no entry in a text file is
     * allowed to mute that. */
    if (raw != STRESSFS_VERDICT_FAIL) {
        return raw;
    }

    for (i = 0; i < db->n_entries; i++) {
        char reason[STRESSFS_QUIRK_REASON_LEN];
        const stressfs_quirk_entry_t *e = &db->entries[i];

        if (!stressfs_quirks_entry_active(db, i, ident, reason, sizeof(reason))) {
            continue;
        }
        for (j = 0; j < e->n_actions; j++) {
            const stressfs_quirk_action_t *a = &e->actions[j];

            if (!target_matches(a, entry->id)) {
                continue;
            }
            switch (a->verb) {
            case STRESSFS_QUIRK_DOWNGRADE:
                if (out != NULL) {
                    fill_applied(out, e, a, reason);
                }
                return STRESSFS_VERDICT_DEVIATION;

            case STRESSFS_QUIRK_EXPECT_DEVIATION:
                /* The whole point of this verb: the declared result
                 * must be the observed one, or the quirk does not
                 * cover this failure and the raw `fail` stands. */
                if (!a->res1_any && got.res1 != a->res1) {
                    continue;
                }
                if (!a->res2_any && got.res2 != a->res2) {
                    continue;
                }
                if (out != NULL) {
                    fill_applied(out, e, a, reason);
                }
                return STRESSFS_VERDICT_DEVIATION;

            case STRESSFS_QUIRK_SKIP:
            default:
                /* `skip` is a pre-execution decision
                 * (stressfs_quirks_check_skip). If the driver ran the
                 * test anyway, the honest thing is to report what
                 * actually happened rather than retroactively pretend
                 * it was skipped. */
                continue;
            }
        }
    }

    return raw;
}
