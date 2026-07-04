/* Avatars.def parser + writer.
 *
 * Faithful structural port of the original Joint Operations loader
 * [orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0], with an authoring-superset
 * in-memory model (see avatars.h and docs/playerinfo/avatars-re.md). The lexer
 * helpers are copied from libs/def/src/def.cpp per the libs/ convention (no
 * shared private header). The writer creates output from scratch
 * (docs/adr/0003, policy docs/adr/0021): a parse->write->parse->write round-trip
 * is byte-identical on the second write and model-equal across the parse.
 */

#include "avatars/avatars.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

/* ========================================================================= */
/* Lexer helpers (copied from libs/def/src/def.cpp)                          */
/* ========================================================================= */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static void safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len) {
    if (src_len >= dst_size) src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static const char *trim_span(const char *s, size_t len, size_t *out_len) {
    while (len > 0 && isspace((unsigned char)*s)) { ++s; --len; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) --len;
    *out_len = len;
    return s;
}

/* Line iterator: walks buf splitting on \n, stripping a trailing \r. */
typedef struct {
    const char *buf;
    size_t buf_len;
    size_t pos;
} LineIter;

static int next_line(LineIter *it, const char **out, size_t *out_len) {
    if (it->pos >= it->buf_len) return 0;
    const char *start = it->buf + it->pos;
    const char *nl = (const char *)memchr(start, '\n', it->buf_len - it->pos);
    size_t len;
    if (nl) {
        len = (size_t)(nl - start);
        it->pos += len + 1;
    } else {
        len = it->buf_len - it->pos;
        it->pos = it->buf_len;
    }
    if (len > 0 && start[len - 1] == '\r') --len;
    *out = start;
    *out_len = len;
    return 1;
}

#define DA_PUSH(arr, count, cap, elem) do {                                    \
    if ((count) >= (cap)) {                                                    \
        (cap) = (cap) ? (cap) * 2 : 8;                                         \
        (arr) = (decltype(arr))realloc((arr), (cap) * sizeof(*(arr)));         \
    }                                                                          \
    (arr)[(count)++] = (elem);                                                 \
} while (0)

/* Append a verbatim line to a block's raw_lines superset (grow-by-one; these
 * lines are rare — unrecognized keywords inside a block — so cost is moot). */
static void push_raw_line(char (**raw)[512], size_t *count, const char *line, size_t len) {
    char (*na)[512] = (char (*)[512])realloc(*raw, (*count + 1) * sizeof(**raw));
    if (!na) return;
    *raw = na;
    size_t cp = len < 511 ? len : 511;
    memcpy(na[*count], line, cp);
    na[*count][cp] = '\0';
    (*count)++;
}

/* ========================================================================= */
/* Avatars-specific helpers                                                  */
/* ========================================================================= */

/* Quote-aware tokenizer: a token starting with '"' runs to the next '"' and the
 * span excludes the quotes (so `name "Bare Arms"` yields the token `Bare Arms`).
 * Mirrors the original's File_ParseASCIIFile behavior of keeping quoted strings
 * whole. */
#define MAX_TOKENS 16
typedef struct { const char *s; size_t len; } Token;

static int tokenize(const char *s, size_t len, Token *tokens, int max_tok) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < max_tok) {
        while (i < len && isspace((unsigned char)s[i])) ++i;
        if (i >= len) break;
        if (s[i] == '"') {
            ++i; /* skip opening quote */
            size_t start = i;
            while (i < len && s[i] != '"') ++i;
            tokens[n].s = s + start;
            tokens[n].len = i - start;
            if (i < len) ++i; /* skip closing quote */
        } else {
            size_t start = i;
            while (i < len && !isspace((unsigned char)s[i])) ++i;
            tokens[n].s = s + start;
            tokens[n].len = i - start;
        }
        ++n;
    }
    return n;
}

static int tok_ieq(const Token *t, const char *lit) {
    size_t n = strlen(lit);
    if (t->len != n) return 0;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)t->s[i]) != tolower((unsigned char)lit[i])) return 0;
    return 1;
}

static int tok_is_comment(const Token *t) {
    return t->len >= 2 && t->s[0] == '/' && t->s[1] == '/';
}

static void tok_copy(const Token *t, char *dst, size_t dst_size) {
    safe_copy(dst, dst_size, t->s, t->len);
}

static int tok_int(const Token *t) {
    char buf[32];
    safe_copy(buf, sizeof(buf), t->s, t->len);
    return (int)strtol(buf, NULL, 10);
}

static int tok_byte(const Token *t) {
    return tok_int(t) & 0xff;
}

/* Lenient numeric id: skip a single leading non-digit char before atol.
 * [orig: CAvatarDefs_ParseConfigLine @ 0x57a62b / @ 0x57a751] (D-PLAYERINFO-6). */
static int tok_lenient_id(const Token *t) {
    const char *s = t->s;
    size_t len = t->len;
    if (len > 0 && (unsigned char)s[0] > '9') { ++s; --len; }
    char buf[32];
    safe_copy(buf, sizeof(buf), s, len);
    return (int)strtol(buf, NULL, 10);
}

/* Join tokens [first..n) into dst with single spaces (the nationality/division
 * trailing flags, e.g. "skipdemo"). */
static void join_flags(const Token *tokens, int n, int first, char *dst, size_t dst_size) {
    dst[0] = '\0';
    size_t off = 0;
    for (int i = first; i < n; ++i) {
        if (tok_is_comment(&tokens[i])) break; /* drop trailing comments */
        if (off && off + 1 < dst_size) dst[off++] = ' ';
        size_t cp = tokens[i].len;
        if (off + cp >= dst_size) cp = (off < dst_size) ? dst_size - 1 - off : 0;
        memcpy(dst + off, tokens[i].s, cp);
        off += cp;
    }
    if (off < dst_size) dst[off] = '\0'; else dst[dst_size - 1] = '\0';
}

static int cstr_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void push_diag(AvatarsFile *out, size_t *diag_cap, size_t line, int severity,
                      const char *code, const char *message) {
    AvatarDiagnostic d;
    memset(&d, 0, sizeof(d));
    d.line = line;
    d.severity = severity;
    snprintf(d.code, sizeof(d.code), "%s", code);
    snprintf(d.message, sizeof(d.message), "%s", message);
    DA_PUSH(out->diagnostics, out->diagnostics_count, *diag_cap, d);
}

static void part_to_snapshot(const AvatarPart *p, AvatarPartSnapshot *out) {
    memset(out, 0, sizeof(*out));
    if (!p) return;
    out->kind = p->kind;
    snprintf(out->name, sizeof(out->name), "%s", p->name);
    snprintf(out->display_name, sizeof(out->display_name), "%s", p->display_name);
    snprintf(out->graphic, sizeof(out->graphic), "%s", p->graphic);
    snprintf(out->graphic_j, sizeof(out->graphic_j), "%s", p->graphic_j);
    snprintf(out->graphic_s, sizeof(out->graphic_s), "%s", p->graphic_s);
    out->camo[0] = p->camo[0];
    out->camo[1] = p->camo[1];
    out->camo[2] = p->camo[2];
    out->voice = p->voice;
    out->sex = p->sex;
}

static const AvatarPart *find_prior_part(const AvatarsFile *out, int kind, const char *name) {
    for (size_t i = out->parts_count; i > 0; --i) {
        const AvatarPart *p = &out->parts[i - 1];
        if (p->kind == kind && cstr_ieq(p->name, name)) {
            return p;
        }
    }
    return NULL;
}

static int has_nationality_slot(const AvatarsFile *out, int id) {
    for (size_t i = 0; i < out->nationalities_count; ++i) {
        if (out->nationalities[i].id == id) return 1;
    }
    return 0;
}

static int has_division_slot(const AvatarNationality *nat, int id) {
    for (size_t i = 0; i < nat->divisions_count; ++i) {
        if (nat->divisions[i].id == id) return 1;
    }
    return 0;
}

/* ========================================================================= */
/* Parser                                                                    */
/* ========================================================================= */

enum Scope { SC_TOP, SC_PART, SC_NAT, SC_DIV, SC_SKIP };

static void free_part(AvatarPart *p) {
    free(p->raw_lines);
    p->raw_lines = NULL;
    p->raw_lines_count = 0;
}

static void free_division(AvatarDivision *d) {
    free(d->combos);
    free(d->raw_lines);
    d->combos = NULL;
    d->raw_lines = NULL;
}

static void free_nationality(AvatarNationality *n) {
    for (size_t i = 0; i < n->divisions_count; ++i) free_division(&n->divisions[i]);
    free(n->divisions);
    free(n->raw_lines);
    n->divisions = NULL;
    n->raw_lines = NULL;
}

static int parse_buffer(const char *buf, size_t buf_len, AvatarsFile *out) {
    memset(out, 0, sizeof(*out));

    size_t parts_cap = 0, nats_cap = 0, diag_cap = 0;
    size_t valid_combo_count = 0;

    /* Builder locals (ownership transfers into the file arrays on block close). */
    AvatarPart cur_part;
    AvatarNationality cur_nat;
    AvatarDivision cur_div;
    memset(&cur_part, 0, sizeof(cur_part));
    memset(&cur_nat, 0, sizeof(cur_nat));
    memset(&cur_div, 0, sizeof(cur_div));
    size_t cur_nat_div_cap = 0;
    size_t cur_div_combo_cap = 0;

    Scope stack[16];
    int depth = 0;
    stack[0] = SC_TOP;
    Scope pending = SC_TOP;

    int error = 0;
    LineIter it = { buf, buf_len, 0 };
    const char *line;
    size_t line_len;
    size_t line_no = 0;

    while (next_line(&it, &line, &line_len)) {
        ++line_no;
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue; /* blank */

        Token tok[MAX_TOKENS];
        int n = tokenize(trimmed, tlen, tok, MAX_TOKENS);
        if (n == 0) continue;
        if (tok_is_comment(&tok[0])) continue; /* full-line comment */

        Scope scope = stack[depth];

        /* Brace handling (first token only). */
        if (tok[0].len == 1 && tok[0].s[0] == '}') {
            if (depth > 0) {
                Scope closing = stack[depth];
                --depth;
                if (closing == SC_PART) {
                    DA_PUSH(out->parts, out->parts_count, parts_cap, cur_part);
                    memset(&cur_part, 0, sizeof(cur_part));
                } else if (closing == SC_DIV) {
                    DA_PUSH(cur_nat.divisions, cur_nat.divisions_count, cur_nat_div_cap, cur_div);
                    memset(&cur_div, 0, sizeof(cur_div));
                    cur_div_combo_cap = 0;
                } else if (closing == SC_NAT) {
                    DA_PUSH(out->nationalities, out->nationalities_count, nats_cap, cur_nat);
                    memset(&cur_nat, 0, sizeof(cur_nat));
                    cur_nat_div_cap = 0;
                }
            }
            continue;
        }
        if (tok[0].len == 1 && tok[0].s[0] == '{') {
            if (depth + 1 < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[++depth] = pending;
            }
            continue;
        }

        /* [orig: CAvatarDefs_ParseConfigLine @ 0x57a456] checks the global
         * 512-part guard before dispatching another meaningful top-level line. */
        if (scope == SC_TOP && out->parts_count >= 512) {
            error = 1;
            goto done;
        }

        switch (scope) {
        case SC_TOP:
            if (tok_ieq(&tok[0], "define") && n >= 3) {
                /* [orig: CAvatarDefs_ParseConfigLine @ 0x57a49f] */
                int kind = -1;
                if (tok_ieq(&tok[1], "head")) kind = AVATAR_PART_HEAD;
                else if (tok_ieq(&tok[1], "body")) kind = AVATAR_PART_BODY;
                else if (tok_ieq(&tok[1], "arms")) kind = AVATAR_PART_ARMS;
                if (kind < 0) { pending = SC_SKIP; break; }
                memset(&cur_part, 0, sizeof(cur_part));
                cur_part.kind = kind;
                tok_copy(&tok[2], cur_part.name, sizeof(cur_part.name)); /* [orig @ 0x57a4ed] */
                pending = SC_PART;
            } else if (tok_ieq(&tok[0], "nationality") && n >= 3) {
                /* [orig @ 0x57a615] */
                int id = tok_lenient_id(&tok[1]);
                if (id < 0 || id > 31) { pending = SC_SKIP; break; } /* error state 7 */
                if (has_nationality_slot(out, id)) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "duplicate_nationality",
                              "duplicate nationality slot ignored");
                    pending = SC_SKIP;
                    break;
                }
                memset(&cur_nat, 0, sizeof(cur_nat));
                cur_nat_div_cap = 0;
                cur_nat.id = id;
                tok_copy(&tok[1], cur_nat.raw_id, sizeof(cur_nat.raw_id));
                tok_copy(&tok[2], cur_nat.name_key, sizeof(cur_nat.name_key));
                join_flags(tok, n, 3, cur_nat.flags, sizeof(cur_nat.flags));
                pending = SC_NAT;
            } else {
                pending = SC_SKIP;
            }
            break;

        case SC_PART:
            /* [orig @ 0x57aaf8.. ] part fields */
            if (tok_ieq(&tok[0], "name") && n >= 2) {
                tok_copy(&tok[1], cur_part.display_name, sizeof(cur_part.display_name));
            } else if (tok_ieq(&tok[0], "graphic") && n >= 2) {
                tok_copy(&tok[1], cur_part.graphic, sizeof(cur_part.graphic));
            } else if (tok_ieq(&tok[0], "graphic_d") && n >= 2) {
                tok_copy(&tok[1], cur_part.graphic, sizeof(cur_part.graphic)); /* aliases graphic, D-PLAYERINFO-3 */
            } else if (tok_ieq(&tok[0], "graphic_j") && n >= 2) {
                tok_copy(&tok[1], cur_part.graphic_j, sizeof(cur_part.graphic_j));
            } else if (tok_ieq(&tok[0], "graphic_s") && n >= 2) {
                tok_copy(&tok[1], cur_part.graphic_s, sizeof(cur_part.graphic_s));
            } else if (tok_ieq(&tok[0], "camo") && n >= 4) {
                cur_part.camo[0] = tok_byte(&tok[1]);
                cur_part.camo[1] = tok_byte(&tok[2]);
                cur_part.camo[2] = tok_byte(&tok[3]);
            } else if (tok_ieq(&tok[0], "voice") && n >= 2) {
                cur_part.voice = tok_byte(&tok[1]);
            } else if (tok_ieq(&tok[0], "sex") && n >= 2) {
                cur_part.sex = tok_ieq(&tok[1], "f") ? AVATAR_SEX_FEMALE : AVATAR_SEX_MALE;
            } else {
                push_raw_line(&cur_part.raw_lines, &cur_part.raw_lines_count, trimmed, tlen);
            }
            break;

        case SC_NAT:
            if (tok_ieq(&tok[0], "alignment") && n >= 2) {
                /* [orig @ 0x57a6bc] */
                cur_nat.has_alignment = 1;
                cur_nat.alignment = tok_ieq(&tok[1], "evil") ? AVATAR_ALIGN_EVIL : AVATAR_ALIGN_GOOD;
            } else if (tok_ieq(&tok[0], "division") && n >= 3) {
                /* [orig @ 0x57a73b] */
                int id = tok_lenient_id(&tok[1]);
                if (id < 0 || id > 15) { pending = SC_SKIP; break; } /* error state 8 */
                if (has_division_slot(&cur_nat, id)) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "duplicate_division",
                              "duplicate division slot ignored");
                    pending = SC_SKIP;
                    break;
                }
                memset(&cur_div, 0, sizeof(cur_div));
                cur_div_combo_cap = 0;
                cur_div.id = id;
                tok_copy(&tok[1], cur_div.raw_id, sizeof(cur_div.raw_id));
                tok_copy(&tok[2], cur_div.name_key, sizeof(cur_div.name_key));
                join_flags(tok, n, 3, cur_div.flags, sizeof(cur_div.flags));
                pending = SC_DIV;
            } else {
                push_raw_line(&cur_nat.raw_lines, &cur_nat.raw_lines_count, trimmed, tlen);
            }
            break;

        case SC_DIV:
            if (tok_ieq(&tok[0], "combo") && n >= 4) {
                /* [orig @ 0x57a7ed] */
                AvatarCombo c;
                memset(&c, 0, sizeof(c));
                tok_copy(&tok[1], c.raw_id, sizeof(c.raw_id));
                c.id = tok_lenient_id(&tok[1]);
                tok_copy(&tok[2], c.head_name, sizeof(c.head_name));
                tok_copy(&tok[3], c.body_name, sizeof(c.body_name));
                char arms_name[64] = { 0 };
                if (n >= 5) tok_copy(&tok[4], arms_name, sizeof(arms_name));
                const AvatarPart *head = find_prior_part(out, AVATAR_PART_HEAD, c.head_name);
                const AvatarPart *body = find_prior_part(out, AVATAR_PART_BODY, c.body_name);
                if (!head) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "combo_missing_head",
                              "combo ignored because its head part is not defined yet");
                    break;
                }
                if (!body) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "combo_missing_body",
                              "combo ignored because its body part is not defined yet");
                    break;
                }
                if (valid_combo_count >= 128) {
                    error = 1;
                    goto done;
                }
                part_to_snapshot(head, &c.head);
                part_to_snapshot(body, &c.body);
                if (arms_name[0]) {
                    const AvatarPart *arms = find_prior_part(out, AVATAR_PART_ARMS, arms_name);
                    if (arms) {
                        snprintf(c.arms_name, sizeof(c.arms_name), "%s", arms_name);
                        part_to_snapshot(arms, &c.arms);
                        c.has_arms = 1;
                    } else {
                        push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "combo_missing_arms",
                                  "combo kept without arms because its arms part is not defined yet");
                    }
                }
                DA_PUSH(cur_div.combos, cur_div.combos_count, cur_div_combo_cap, c);
                ++valid_combo_count;
            } else {
                push_raw_line(&cur_div.raw_lines, &cur_div.raw_lines_count, trimmed, tlen);
            }
            break;

        case SC_SKIP:
        default:
            break;
        }
    }

done:
    /* Free any half-built builders still owning memory (unclosed blocks). */
    free_part(&cur_part);
    free_division(&cur_div);
    free_nationality(&cur_nat);
    if (error) {
        avatars_free(out);
        return error;
    }
    return 0;
}

extern "C" int avatars_parse_memory(const void *data, size_t size, AvatarsFile *out) {
    if (!data || !out) return 1;
    return parse_buffer((const char *)data, size, out);
}

extern "C" int avatars_parse(const char *path, AvatarsFile *out) {
    if (!path || !out) return 1;
    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) { memset(out, 0, sizeof(*out)); return 1; }
    int rc = parse_buffer(buf, len, out);
    free(buf);
    return rc;
}

extern "C" void avatars_free(AvatarsFile *file) {
    if (!file) return;
    for (size_t i = 0; i < file->parts_count; ++i) free_part(&file->parts[i]);
    free(file->parts);
    for (size_t i = 0; i < file->nationalities_count; ++i) free_nationality(&file->nationalities[i]);
    free(file->nationalities);
    free(file->diagnostics);
    memset(file, 0, sizeof(*file));
}

/* ========================================================================= */
/* Writer (from scratch — docs/adr/0003, docs/adr/0021)                      */
/* ========================================================================= */

static const char *kKindKeyword[3] = { "head", "body", "arms" };

/* Emit a value token, quoting if it contains whitespace or is empty. */
static void emit_value(std::string &s, const char *v) {
    int needs_quote = (v[0] == '\0');
    for (const char *p = v; *p; ++p) {
        if (isspace((unsigned char)*p)) { needs_quote = 1; break; }
    }
    if (needs_quote) { s += '"'; s += v; s += '"'; }
    else s += v;
}

static void emit_raw_lines(std::string &s, char (*raw)[512], size_t count, const char *indent) {
    for (size_t i = 0; i < count; ++i) {
        s += indent;
        s += raw[i];
        s += '\n';
    }
}

extern "C" int avatars_write(const AvatarsFile *file, char **out_data, size_t *out_size) {
    if (!file || !out_data || !out_size) return 1;
    std::string s;
    s.reserve(8192);
    s += "// Avatars.def - generated by OpenNova libs/avatars (do not hand-edit formatting)\n\n";

    char num[32];

    for (size_t i = 0; i < file->parts_count; ++i) {
        const AvatarPart *p = &file->parts[i];
        int k = (p->kind >= 0 && p->kind <= 2) ? p->kind : 0;
        s += "define "; s += kKindKeyword[k]; s += ' ';
        emit_value(s, p->name); s += "\n{\n";
        if (p->display_name[0]) { s += "\tname\t\t"; emit_value(s, p->display_name); s += '\n'; }
        if (p->graphic[0])      { s += "\tgraphic\t\t"; emit_value(s, p->graphic); s += '\n'; }
        if (p->graphic_j[0])    { s += "\tgraphic_j\t"; emit_value(s, p->graphic_j); s += '\n'; }
        if (p->graphic_s[0])    { s += "\tgraphic_s\t"; emit_value(s, p->graphic_s); s += '\n'; }
        snprintf(num, sizeof(num), "%d %d %d", p->camo[0], p->camo[1], p->camo[2]);
        s += "\tcamo\t\t"; s += num; s += '\n';
        snprintf(num, sizeof(num), "%d", p->voice);
        s += "\tvoice\t\t"; s += num; s += '\n';
        s += "\tsex\t\t"; s += (p->sex == AVATAR_SEX_FEMALE) ? "f" : "m"; s += '\n';
        emit_raw_lines(s, p->raw_lines, p->raw_lines_count, "\t");
        s += "}\n\n";
    }

    for (size_t i = 0; i < file->nationalities_count; ++i) {
        const AvatarNationality *nat = &file->nationalities[i];
        s += "nationality "; emit_value(s, nat->raw_id); s += ' ';
        emit_value(s, nat->name_key);
        if (nat->flags[0]) { s += ' '; s += nat->flags; }
        s += "\n{\n";
        if (nat->has_alignment) {
            s += "\talignment\t";
            s += (nat->alignment == AVATAR_ALIGN_EVIL) ? "evil" : "good";
            s += '\n';
        }
        emit_raw_lines(s, nat->raw_lines, nat->raw_lines_count, "\t");
        for (size_t j = 0; j < nat->divisions_count; ++j) {
            const AvatarDivision *d = &nat->divisions[j];
            s += "\n\tdivision "; emit_value(s, d->raw_id); s += ' ';
            emit_value(s, d->name_key);
            if (d->flags[0]) { s += ' '; s += d->flags; }
            s += "\n\t{\n";
            for (size_t c = 0; c < d->combos_count; ++c) {
                const AvatarCombo *cm = &d->combos[c];
                s += "\t\tcombo "; emit_value(s, cm->raw_id); s += ' ';
                emit_value(s, cm->head_name); s += ' ';
                emit_value(s, cm->body_name);
                if (cm->arms_name[0]) { s += ' '; emit_value(s, cm->arms_name); }
                s += '\n';
            }
            emit_raw_lines(s, d->raw_lines, d->raw_lines_count, "\t\t");
            s += "\t}\n";
        }
        s += "}\n\n";
    }

    char *buf = (char *)malloc(s.size() + 1);
    if (!buf) return 1;
    memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out_data = buf;
    *out_size = s.size();
    return 0;
}

extern "C" void avatars_free_buffer(char *data) {
    free(data);
}
