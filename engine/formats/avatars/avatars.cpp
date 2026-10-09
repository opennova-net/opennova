/* Avatars.def parser + writer.
 *
 * Faithful structural port of the original Joint Operations loader
 * [orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0], with an authoring-superset
 * in-memory model (see avatars.h and docs/playerinfo/avatars-re.md). Lines and
 * tokens are the shared retail walk's (io::for_each_config_line_span: CR LF
 * only, the tokenizer's quotes, commas and comments) [orig: CAvatarDefs_Init
 * @ 0x57b180 -> File_ParseASCIIFile @ 0x53d810]. The writer creates output from scratch
 * (docs/adr/0003, policy docs/adr/0021): a parse->write->parse->write round-trip
 * is byte-identical on the second write and model-equal across the parse.
 */

#include <formats/avatars/avatars.h>
#include <base/io/crt_ftol.h>

#include <base/io/ascii_config.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>
#include <set>
#include <string>

namespace opennova::avatars {

/* ========================================================================= */
/* Lexer helpers (copied from engine/formats/def/def_scan.cpp)                     */
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

/* A token as the retail tokenizer hands it, one past the line's count
 * included (io::ConfigTokens::token: tokens 0..2 reset, 3.. what the walk's
 * earlier lines left). */
static const char *tok(const io::ConfigTokens &t, int index) {
    return t.token(index);
}

static int tok_ieq(const char *t, const char *lit) {
    for (; *t && *lit; ++t, ++lit)
        if (tolower((unsigned char)*t) != tolower((unsigned char)*lit)) return 0;
    return *t == '\0' && *lit == '\0';
}

static void tok_copy(const char *t, char *dst, size_t dst_size) {
    safe_copy(dst, dst_size, t, strlen(t));
}

/* atol [orig: j__atol @ 0x76ab1b] (io::retail_atol: 32 bits, saturating). */
static int tok_int(const char *t) {
    return io::retail_atol(t);
}

/* The byte the part fields store. */
static int tok_byte(const char *t) {
    return tok_int(t) & 0xff;
}

/* Lenient numeric id: skip a single leading character above '9' before atol.
 * The compare is on a signed byte, so a first byte of 0x80 or above is no
 * character above '9' and stays [orig: CAvatarDefs_ParseConfigLine, `cmp byte
 * ptr [eax], 39h; jle` @ 0x57a628 / @ 0x57a74e, the skip @ 0x57a62b /
 * @ 0x57a751] (D-PLAYERINFO-6). */
static int tok_lenient_id(const char *t) {
    if (static_cast<signed char>(t[0]) > '9') ++t;
    return tok_int(t);
}

/* Join tokens [first..count) into dst with single spaces (the nationality and
 * division trailing flags, e.g. "skipdemo"; the authoring model's superset, the
 * game reads none of them). */
static void join_flags(const io::ConfigTokens &t, int first, char *dst, size_t dst_size) {
    dst[0] = '\0';
    size_t off = 0;
    for (int i = first; i < t.count; ++i) {
        if (off && off + 1 < dst_size) dst[off++] = ' ';
        size_t cp = strlen(t.tokens[i]);
        if (off + cp >= dst_size) cp = (off < dst_size) ? dst_size - 1 - off : 0;
        memcpy(dst + off, t.tokens[i], cp);
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

/* The parse states [orig: CAvatarDefs_ParseConfigLine's switch on
 * dword_2697F10[depth]]: 0 the top level, 1/2/3 a head/body/arms part, 4 a
 * nationality, 5 a division, 7/8 a refused nationality/division (every line
 * ignored). */
enum ParseState { ST_TOP = 0, ST_HEAD = 1, ST_BODY = 2, ST_ARMS = 3, ST_NAT = 4, ST_DIV = 5,
                  ST_BAD_NAT = 7, ST_BAD_DIV = 8 };

/* Retail's walk, line by line [orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0].
 * The scope stack is dword_2697F10 with its depth dword_2697F0C: a scope's
 * state is state[depth], and state[depth + 1] the state the next `{` enters,
 * which a `define`, `nationality` or `division` line sets and nothing else
 * clears, so a line the switch does not take leaves the pending state as it was
 * (a `{` after `define legs X` still enters the previous define's state). A
 * `define` allocates its part at once (the part count dword_26A773C, the
 * current part dword_26A7740), `nationality` and `division` their slots
 * (g_AvatarDefsParseType / g_AvatarDefsParseSubtype), and the part, nationality
 * and division lines write the CURRENT one. A first token beginning `}` pops
 * (above depth 0) and one beginning `{` pushes, whatever follows the brace;
 * every other line first checks the part count, which at 512 ends the walk with
 * "ComboObj Parse Error" (D-PLAYERINFO-2). */
static int parse_buffer(const char *buf, size_t buf_len, AvatarsFile *out, textlayout::Notes *notes) {
    memset(out, 0, sizeof(*out));

    size_t parts_cap = 0, nats_cap = 0, diag_cap = 0;
    size_t combo_count = 0;

    enum { kMaxDepth = 16 };
    int state[kMaxDepth + 2] = {ST_TOP};
    int depth = 0;
    long part = -1;  /* dword_26A7740 */
    long nat = -1;   /* the nationality g_AvatarDefsParseType names */
    long div = -1;   /* the division g_AvatarDefsParseSubtype names, in nat */
    int error = 0;
    size_t line_no = 0;

    /* The file's layout (textlayout): each scope's record (0: none, its lines read for nothing), the record
       the next `{` at a depth enters, and the records whose `{` and `}` came (a `{` never enters a record
       again once its `}` closed it; a record never entered closes at the next opener of its depth). */
    textlayout::Noter noter(buf, buf_len, notes, textlayout::cut_ascii_walk);
    out->note = noter.root();
    uint64_t rec[kMaxDepth + 2] = {noter.root()};
    uint64_t pending_rec[kMaxDepth + 2] = {0};
    std::set<uint64_t> entered, closed;
    const auto open_record = [&](uint64_t parent) -> uint64_t {
        const uint64_t prior = pending_rec[depth + 1];
        if (prior && !entered.count(prior) && !closed.count(prior)) {
            noter.close(prior);
            closed.insert(prior);
        }
        const uint64_t note = parent ? noter.open(parent) : 0;
        pending_rec[depth + 1] = note;
        return note;
    };

    io::for_each_config_line_span(buf, buf_len, [&](const io::ConfigTokens &t,
                                                    const io::ConfigLineSpan &span) {
        ++line_no;
        noter.line(span.begin, span.end + 2);
        if (error) return;
        /* The walk's gate: no token, or a first token starting '/' [orig:
           File_ParseASCIIFile @0x53D915 / @0x53D91E]. */
        if (t.count == 0 || t.tokens[0][0] == '/') return;
        const char *first = t.tokens[0];
        size_t raw_len;
        const char *raw = trim_span(buf + span.begin, span.end - span.begin, &raw_len);

        /* [orig: the `}` test @ 0x57a40f, the `{` test @ 0x57a430] */
        if (first[0] == '}' && depth > 0) {
            if (rec[depth]) {
                noter.entry(rec[depth], "}");
                noter.close(rec[depth]);
                closed.insert(rec[depth]);
            }
            --depth;
            return;
        }
        if (first[0] == '{') {
            if (depth + 2 < kMaxDepth + 2) {
                state[depth + 2] = state[depth + 1];
                const uint64_t entering = pending_rec[depth + 1];
                rec[depth + 1] = entering && !closed.count(entering) ? entering : 0;
                pending_rec[depth + 2] = 0;
                if (rec[depth + 1]) {
                    noter.entry(rec[depth + 1], "{");
                    entered.insert(rec[depth + 1]);
                }
                ++depth;
            }
            return;
        }
        /* [orig: @ 0x57a456] D-PLAYERINFO-2 */
        if (out->parts_count >= 512) {
            error = 1;
            return;
        }

        switch (state[depth]) {
        case ST_TOP:
            if (tok_ieq(first, "define")) {
                /* [orig: CAvatarDefs_ParseConfigLine @ 0x57a49f] */
                int kind = -1;
                if (tok_ieq(tok(t, 1), "head")) kind = AVATAR_PART_HEAD;
                else if (tok_ieq(tok(t, 1), "body")) kind = AVATAR_PART_BODY;
                else if (tok_ieq(tok(t, 1), "arms")) kind = AVATAR_PART_ARMS;
                if (kind < 0) return; /* the pending state stays */
                state[depth + 1] = ST_HEAD + kind;
                AvatarPart fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.kind = kind;
                tok_copy(tok(t, 2), fresh.name, sizeof(fresh.name)); /* [orig @ 0x57a4ed] */
                fresh.note = open_record(rec[depth]);
                if (fresh.note) noter.entry(fresh.note, "define");
                DA_PUSH(out->parts, out->parts_count, parts_cap, fresh);
                part = (long)out->parts_count - 1;
            } else if (tok_ieq(first, "nationality")) {
                /* [orig @ 0x57a615] */
                const unsigned id = (unsigned)tok_lenient_id(tok(t, 1));
                if (id > 31) { /* error state 7 */
                    state[depth + 1] = ST_BAD_NAT;
                    open_record(0);
                    return;
                }
                if (has_nationality_slot(out, (int)id)) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING,
                              "duplicate_nationality", "duplicate nationality slot ignored");
                    state[depth + 1] = ST_BAD_NAT;
                    open_record(0);
                    return;
                }
                AvatarNationality fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.id = (int)id;
                tok_copy(tok(t, 1), fresh.raw_id, sizeof(fresh.raw_id));
                tok_copy(tok(t, 2), fresh.name_key, sizeof(fresh.name_key));
                join_flags(t, 3, fresh.flags, sizeof(fresh.flags));
                fresh.note = open_record(rec[depth]);
                if (fresh.note) noter.entry(fresh.note, "nationality");
                DA_PUSH(out->nationalities, out->nationalities_count, nats_cap, fresh);
                nat = (long)out->nationalities_count - 1;
                div = -1;
                state[depth + 1] = ST_NAT;
            }
            return;

        case ST_HEAD:
        case ST_BODY:
        case ST_ARMS: {
            /* [orig @ 0x57aaf8..] the part fields, written to the current part
               whatever its kind */
            AvatarPart *pp = &out->parts[part];
            /* A line of the part's own block is its entry; one that writes the current part from another
               block (a `{` re-entering its state) is read for nothing where it stands, as the layout goes. */
            const uint64_t own = rec[depth] && rec[depth] == pp->note ? pp->note : 0;
            const auto mark = [&](const char *key) {
                if (own) noter.entry(own, key);
            };
            if (tok_ieq(first, "name")) {
                tok_copy(tok(t, 1), pp->display_name, sizeof(pp->display_name));
                mark("name");
            } else if (tok_ieq(first, "graphic") || tok_ieq(first, "graphic_d")) {
                tok_copy(tok(t, 1), pp->graphic, sizeof(pp->graphic)); /* D-PLAYERINFO-3 */
                mark("graphic");
            } else if (tok_ieq(first, "graphic_j")) {
                tok_copy(tok(t, 1), pp->graphic_j, sizeof(pp->graphic_j));
                mark("graphic_j");
            } else if (tok_ieq(first, "graphic_s")) {
                tok_copy(tok(t, 1), pp->graphic_s, sizeof(pp->graphic_s));
                mark("graphic_s");
            } else if (tok_ieq(first, "camo")) {
                pp->camo[0] = tok_byte(tok(t, 1));
                pp->camo[1] = tok_byte(tok(t, 2));
                pp->camo[2] = tok_byte(tok(t, 3));
                mark("camo");
            } else if (tok_ieq(first, "voice")) {
                pp->voice = tok_byte(tok(t, 1));
                mark("voice");
            } else if (tok_ieq(first, "sex")) {
                pp->sex = tok_ieq(tok(t, 1), "F") ? AVATAR_SEX_FEMALE : AVATAR_SEX_MALE;
                mark("sex");
            } else {
                push_raw_line(&pp->raw_lines, &pp->raw_lines_count, raw, raw_len);
            }
            return;
        }

        case ST_NAT: {
            AvatarNationality *np = &out->nationalities[nat];
            if (tok_ieq(first, "alignment")) {
                /* [orig @ 0x57a6bc]: good or evil, any other word changes nothing */
                if (tok_ieq(tok(t, 1), "good")) {
                    np->alignment = AVATAR_ALIGN_GOOD;
                    np->has_alignment = 1;
                } else if (tok_ieq(tok(t, 1), "evil")) {
                    np->alignment = AVATAR_ALIGN_EVIL;
                    np->has_alignment = 1;
                }
                if (rec[depth] && rec[depth] == np->note && (tok_ieq(tok(t, 1), "good") || tok_ieq(tok(t, 1), "evil")))
                    noter.entry(np->note, "alignment");
            } else if (tok_ieq(first, "division")) {
                /* [orig @ 0x57a73b] */
                const unsigned id = (unsigned)tok_lenient_id(tok(t, 1));
                if (id > 15) { /* error state 8 */
                    state[depth + 1] = ST_BAD_DIV;
                    open_record(0);
                    return;
                }
                if (has_division_slot(np, (int)id)) {
                    push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING,
                              "duplicate_division", "duplicate division slot ignored");
                    state[depth + 1] = ST_BAD_DIV;
                    open_record(0);
                    return;
                }
                AvatarDivision fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.id = (int)id;
                tok_copy(tok(t, 1), fresh.raw_id, sizeof(fresh.raw_id));
                tok_copy(tok(t, 2), fresh.name_key, sizeof(fresh.name_key));
                join_flags(t, 3, fresh.flags, sizeof(fresh.flags));
                fresh.note = open_record(rec[depth] && rec[depth] == np->note ? np->note : 0);
                if (fresh.note) noter.entry(fresh.note, "division");
                size_t cap = np->divisions_count;
                np->divisions = (AvatarDivision *)realloc(np->divisions,
                                                          (cap + 1) * sizeof(*np->divisions));
                np->divisions[np->divisions_count++] = fresh;
                div = (long)np->divisions_count - 1;
                state[depth + 1] = ST_DIV;
            } else {
                push_raw_line(&np->raw_lines, &np->raw_lines_count, raw, raw_len);
            }
            return;
        }

        case ST_DIV: {
            if (div < 0) return; /* a division of an earlier nationality: no slot here */
            AvatarDivision *dp = &out->nationalities[nat].divisions[div];
            if (!tok_ieq(first, "combo")) {
                push_raw_line(&dp->raw_lines, &dp->raw_lines_count, raw, raw_len);
                return;
            }
            /* [orig @ 0x57a7ed]: the LAST part of each kind named by tokens 2..4
               (the search keeps the last match), the arms optional; the id a plain
               atol (@ 0x57a90d), the lenient skip being the nationality's and the
               division's alone */
            AvatarCombo c;
            memset(&c, 0, sizeof(c));
            tok_copy(tok(t, 1), c.raw_id, sizeof(c.raw_id));
            c.id = tok_int(tok(t, 1));
            tok_copy(tok(t, 2), c.head_name, sizeof(c.head_name));
            tok_copy(tok(t, 3), c.body_name, sizeof(c.body_name));
            char arms_name[64] = {0};
            tok_copy(tok(t, 4), arms_name, sizeof(arms_name));
            const AvatarPart *head = find_prior_part(out, AVATAR_PART_HEAD, c.head_name);
            const AvatarPart *body = find_prior_part(out, AVATAR_PART_BODY, c.body_name);
            if (!head) {
                push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "combo_missing_head",
                          "combo ignored because its head part is not defined yet");
                return;
            }
            if (!body) {
                push_diag(out, &diag_cap, line_no, AVATAR_DIAG_WARNING, "combo_missing_body",
                          "combo ignored because its body part is not defined yet");
                return;
            }
            /* The 129th combo finds the 128-row table full and retail writes
               through the null row (sub_579E10 @ 0x579e10); the parse fails. */
            if (combo_count >= 128) {
                error = 1;
                return;
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
            if (rec[depth] && rec[depth] == dp->note) {
                c.note = noter.open(dp->note);
                noter.entry(c.note, "combo");
                noter.close(c.note);
            }
            size_t cap = dp->combos_count;
            dp->combos = (AvatarCombo *)realloc(dp->combos, (cap + 1) * sizeof(*dp->combos));
            dp->combos[dp->combos_count++] = c;
            ++combo_count;
            return;
        }

        default:
            return; /* the refused states 7 and 8 take nothing */
        }
    });

    noter.finish();
    if (error) {
        avatars_free(out);
        return error;
    }
    return 0;
}

static void records_of(const AvatarsFile *file, textlayout::OutRecord &root);

int avatars_parse_memory(const void *data, size_t size, AvatarsFile *out) {
    if (!data || !out) return 1;
    return parse_buffer((const char *)data, size, out, nullptr);
}

int avatars_parse_memory(const void *data, size_t size, AvatarsFile *out, textlayout::Notes &notes) {
    if (!data || !out) return 1;
    const int rc = parse_buffer((const char *)data, size, out, &notes);
    if (rc != 0) return rc;
    /* Each record's lines modeled against what the writer puts down for it as read. */
    textlayout::OutRecord as_read;
    records_of(out, as_read);
    textlayout::model(notes, as_read, textlayout::cut_ascii_walk);
    return 0;
}

int avatars_parse(const char *path, AvatarsFile *out) {
    if (!path || !out) return 1;
    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) { memset(out, 0, sizeof(*out)); return 1; }
    int rc = parse_buffer(buf, len, out, nullptr);
    free(buf);
    return rc;
}

void avatars_free(AvatarsFile *file) {
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

/* Emit a value token, quoted when the tokenizer would cut it: a space, tab or
 * comma separates and `;` or `//` ends the line outside quotes. An empty value is
 * written `""`, which the tokenizer reads as no token at all, so it reads back
 * empty only as a line's last token (avatars_write refuses one ahead of a filled
 * field: fields_shift). */
static void emit_value(std::string &s, const char *v) {
    int needs_quote = (v[0] == '\0');
    for (const char *p = v; *p; ++p) {
        if (isspace((unsigned char)*p) || *p == ',' || *p == ';' || (p[0] == '/' && p[1] == '/')) {
            needs_quote = 1;
            break;
        }
    }
    if (needs_quote) { s += '"'; s += v; s += '"'; }
    else s += v;
}

/* Whether a line's fields hold an empty one ahead of a filled one: the `""`
 * the writer spells it with is no token to the game's tokenizer, so every later
 * field would read back one token early [orig: Terrain_TokenizeConfigLine
 * @0x53CB60, the quote arm @0x53CC4E..0x53CC70]. */
static bool fields_shift(std::initializer_list<const char *> fields) {
    bool empty_seen = false;
    for (const char *field : fields) {
        if (field[0] == '\0') empty_seen = true;
        else if (empty_seen) return true;
    }
    return false;
}

/* The raw lines a block holds, each a line of the writer's form alone (an entry of no key, textlayout): over a
 * file's layout they are its lines the walk reads nothing of, where they stand. */
static void raw_lines_of(textlayout::OutRecord &record, char (*raw)[512], size_t count, const char *indent) {
    for (size_t i = 0; i < count; ++i) record.lines.push_back({"", std::string(indent) + raw[i]});
}

static std::string value_of(const char *v) {
    std::string out;
    emit_value(out, v);
    return out;
}

/* The records the writer puts down (textlayout): the file's header comment, each part (its `define` line, its
 * braces, its keys, a blank line after it), then each nationality (its line, its braces, its alignment, each
 * division after a blank line, its combos one line each). The blank lines and the header are its form's
 * separators. */
static void records_of(const AvatarsFile *file, textlayout::OutRecord &root) {
    root = textlayout::OutRecord();
    root.note = file->note;
    root.lines.push_back({"", "// Avatars.def - generated by OpenNova engine/formats/avatars (do not hand-edit formatting)"});
    root.lines.push_back({"", ""});
    char num[32];
    for (size_t i = 0; i < file->parts_count; ++i) {
        const AvatarPart *p = &file->parts[i];
        int k = (p->kind >= 0 && p->kind <= 2) ? p->kind : 0;
        textlayout::OutRecord part;
        part.note = p->note;
        part.kind = "part";
        part.lines.push_back({"define", std::string("define ") + kKindKeyword[k] + " " + value_of(p->name)});
        part.lines.push_back({"{", "{"});
        if (p->display_name[0]) part.lines.push_back({"name", "\tname\t\t" + value_of(p->display_name)});
        if (p->graphic[0]) part.lines.push_back({"graphic", "\tgraphic\t\t" + value_of(p->graphic)});
        if (p->graphic_j[0]) part.lines.push_back({"graphic_j", "\tgraphic_j\t" + value_of(p->graphic_j)});
        if (p->graphic_s[0]) part.lines.push_back({"graphic_s", "\tgraphic_s\t" + value_of(p->graphic_s)});
        snprintf(num, sizeof(num), "%d %d %d", p->camo[0], p->camo[1], p->camo[2]);
        part.lines.push_back({"camo", std::string("\tcamo\t\t") + num});
        snprintf(num, sizeof(num), "%d", p->voice);
        part.lines.push_back({"voice", std::string("\tvoice\t\t") + num});
        part.lines.push_back({"sex", std::string("\tsex\t\t") + ((p->sex == AVATAR_SEX_FEMALE) ? "f" : "m")});
        raw_lines_of(part, p->raw_lines, p->raw_lines_count, "\t");
        part.lines.push_back({"}", "}"});
        part.lines.push_back({"", ""});
        root.lines.push_back({"", "", int(root.children.size())});
        root.children.push_back(std::move(part));
    }
    for (size_t i = 0; i < file->nationalities_count; ++i) {
        const AvatarNationality *nat = &file->nationalities[i];
        textlayout::OutRecord record;
        record.note = nat->note;
        record.kind = "nationality";
        std::string line = "nationality " + value_of(nat->raw_id) + " " + value_of(nat->name_key);
        if (nat->flags[0]) line += std::string(" ") + nat->flags;
        record.lines.push_back({"nationality", line});
        record.lines.push_back({"{", "{"});
        if (nat->has_alignment)
            record.lines.push_back({"alignment", std::string("\talignment\t") + ((nat->alignment == AVATAR_ALIGN_EVIL) ? "evil" : "good")});
        raw_lines_of(record, nat->raw_lines, nat->raw_lines_count, "\t");
        for (size_t j = 0; j < nat->divisions_count; ++j) {
            const AvatarDivision *d = &nat->divisions[j];
            textlayout::OutRecord division;
            division.note = d->note;
            division.kind = "division";
            division.lines.push_back({"", ""});
            std::string head = "\tdivision " + value_of(d->raw_id) + " " + value_of(d->name_key);
            if (d->flags[0]) head += std::string(" ") + d->flags;
            division.lines.push_back({"division", head});
            division.lines.push_back({"{", "\t{"});
            for (size_t c = 0; c < d->combos_count; ++c) {
                const AvatarCombo *cm = &d->combos[c];
                textlayout::OutRecord combo;
                combo.note = cm->note;
                combo.kind = "combo";
                std::string text = "\t\tcombo " + value_of(cm->raw_id) + " " + value_of(cm->head_name) + " " +
                                   value_of(cm->body_name);
                if (cm->arms_name[0]) text += " " + value_of(cm->arms_name);
                combo.lines.push_back({"combo", text});
                division.lines.push_back({"", "", int(division.children.size())});
                division.children.push_back(std::move(combo));
            }
            raw_lines_of(division, d->raw_lines, d->raw_lines_count, "\t\t");
            division.lines.push_back({"}", "\t}"});
            record.lines.push_back({"", "", int(record.children.size())});
            record.children.push_back(std::move(division));
        }
        record.lines.push_back({"}", "}"});
        record.lines.push_back({"", ""});
        root.lines.push_back({"", "", int(root.children.size())});
        root.children.push_back(std::move(record));
    }
}

static int fields_refused(const AvatarsFile *file) {
    for (size_t i = 0; i < file->nationalities_count; ++i) {
        const AvatarNationality *nat = &file->nationalities[i];
        if (fields_shift({nat->raw_id, nat->name_key, nat->flags})) return 2;
        for (size_t j = 0; j < nat->divisions_count; ++j) {
            const AvatarDivision *d = &nat->divisions[j];
            if (fields_shift({d->raw_id, d->name_key, d->flags})) return 2;
            for (size_t c = 0; c < d->combos_count; ++c) {
                const AvatarCombo *cm = &d->combos[c];
                if (fields_shift({cm->raw_id, cm->head_name, cm->body_name, cm->arms_name}))
                    return 2;
            }
        }
    }
    return 0;
}

static int hand_out(const std::string &text, char **out_data, size_t *out_size) {
    char *buf = (char *)malloc(text.size() + 1);
    if (!buf) return 1;
    memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    *out_data = buf;
    *out_size = text.size();
    return 0;
}

int avatars_write(const AvatarsFile *file, char **out_data, size_t *out_size) {
    return avatars_write(file, nullptr, out_data, out_size, nullptr);
}

int avatars_write(const AvatarsFile *file, const textlayout::Notes *notes, char **out_data, size_t *out_size,
                  bool *rewritten) {
    if (rewritten) *rewritten = false;
    if (!file || !out_data || !out_size) return 1;
    if (const int refused = fields_refused(file)) return refused;
    textlayout::OutRecord root;
    records_of(file, root);
    /* Every line ends in CR LF, the one break the game's walk splits at
       [orig: File_ParseASCIIFile @ 0x53d810]. */
    const std::string eol = notes ? textlayout::file_eol(*notes, "\r\n") : std::string("\r\n");
    std::string text = textlayout::compose(notes, root, textlayout::cut_ascii_walk, eol);
    if (notes) {
        AvatarsFile again;
        const int rc = avatars_parse_memory(text.data(), text.size(), &again);
        const bool same = rc == 0 && avatars_equal(again, *file);
        if (rc == 0) avatars_free(&again);
        if (!same) {
            text = textlayout::compose(nullptr, root, textlayout::cut_ascii_walk, "\r\n");
            if (rewritten) *rewritten = true;
        }
    }
    return hand_out(text, out_data, out_size);
}

static bool same_raw(char (*a)[512], size_t an, char (*b)[512], size_t bn) {
    if (an != bn) return false;
    for (size_t i = 0; i < an; ++i)
        if (strcmp(a[i], b[i]) != 0) return false;
    return true;
}

bool avatars_equal(const AvatarsFile &a, const AvatarsFile &b) {
    if (a.parts_count != b.parts_count || a.nationalities_count != b.nationalities_count) return false;
    for (size_t i = 0; i < a.parts_count; ++i) {
        const AvatarPart &x = a.parts[i], &y = b.parts[i];
        if (x.kind != y.kind || strcmp(x.name, y.name) || strcmp(x.display_name, y.display_name) ||
            strcmp(x.graphic, y.graphic) || strcmp(x.graphic_j, y.graphic_j) || strcmp(x.graphic_s, y.graphic_s) ||
            x.camo[0] != y.camo[0] || x.camo[1] != y.camo[1] || x.camo[2] != y.camo[2] || x.voice != y.voice ||
            x.sex != y.sex || !same_raw(x.raw_lines, x.raw_lines_count, y.raw_lines, y.raw_lines_count))
            return false;
    }
    for (size_t i = 0; i < a.nationalities_count; ++i) {
        const AvatarNationality &x = a.nationalities[i], &y = b.nationalities[i];
        if (strcmp(x.raw_id, y.raw_id) || x.id != y.id || strcmp(x.name_key, y.name_key) || strcmp(x.flags, y.flags) ||
            x.alignment != y.alignment || x.has_alignment != y.has_alignment ||
            !same_raw(x.raw_lines, x.raw_lines_count, y.raw_lines, y.raw_lines_count) ||
            x.divisions_count != y.divisions_count)
            return false;
        for (size_t j = 0; j < x.divisions_count; ++j) {
            const AvatarDivision &d = x.divisions[j], &e = y.divisions[j];
            if (strcmp(d.raw_id, e.raw_id) || d.id != e.id || strcmp(d.name_key, e.name_key) || strcmp(d.flags, e.flags) ||
                !same_raw(d.raw_lines, d.raw_lines_count, e.raw_lines, e.raw_lines_count) ||
                d.combos_count != e.combos_count)
                return false;
            for (size_t c = 0; c < d.combos_count; ++c) {
                const AvatarCombo &m = d.combos[c], &n = e.combos[c];
                if (strcmp(m.raw_id, n.raw_id) || m.id != n.id || strcmp(m.head_name, n.head_name) ||
                    strcmp(m.body_name, n.body_name) || strcmp(m.arms_name, n.arms_name) || m.has_arms != n.has_arms)
                    return false;
            }
        }
    }
    return true;
}

void avatars_free_buffer(char *data) {
    free(data);
}

} // namespace opennova::avatars
