/* MUS source-text -> bytecode compiler.

   The compiler is the inverse of `mus_decompile.cpp`: it lexes our MUS text
   (the same format the decompiler emits, line-for-line compatible with the
   pre-repo Python reference), parses the top-level declarations,
   and emits SCR0/MU01-shaped bytecode that the engine VM (witnessed at
   `Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20`) accepts.
   [orig: AudioVM_LoadScriptFile @ 0x672D20 (the compile path is the write-inverse); docs/audio/mus-sbf-re.md]

   Phase D scope: produce bytecode that round-trips through
   `decompile(compile(decompile(x))) == decompile(x)` against the
   `fixtures/mus/jo_gamemus.bin` golden.

   The compiler is original work: there is no engine-shipped reference. The
   inverse opcode table is derived by walking the decompiler's `kOps[256]`
   in reverse and cross-checking the engine's observed opcode dispatch.

   Grammar (informal; matches what the decompiler emits):

       script_file := comment* 'script' IDENT bind_decl* global_decl*
                       declsection_decl* section_block*
       bind_decl       := 'bind' IDENT STRING                  // aesthetic only
       global_decl     := 'global' IDENT IDENT                 // aesthetic only
       declsection_decl:= 'declsection' IDENT                  // aesthetic only
       section_block   := 'section' IDENT '{' stmt* '}' stmt*

       stmt := 'play' IDENT
             | 'enter' IDENT
             | 'goto' IDENT
             | 'return' | 'yield' | 'nop'
             | 'call' IDENT
             | 'if' '(' expr ')' '{' stmt* '}' [ 'else' '{' stmt* '}' ]
             | 'on' '(' expr ')' ('enter'|'play'|'goto') IDENT*  // tablexec
             | IDENT '++' | IDENT '--'                           // inc_g/dec_g
             | IDENT '=' expr                                    // pop_g/pop_l
             | expr                                              // expression statement (empty 0x0F)

       expr := NUMBER
             | IDENT                                              // VarN/g_n/l_n/Me
             | '(' expr OP expr ')'                               // binop
             | UNARYOP expr
             | IDENT '(' [ expr ] ')'                             // method call
             | IDENT '.' IDENT '(' [ expr ] ')'                   // qualified method
*/

#include "mus/mus.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* ---- Tokens ---- */

enum class Tok {
    Eof,
    Ident,
    Number,
    String,
    LBrace,
    RBrace,
    LParen,
    RParen,
    Comma,
    Equals,
    Dot,            /* '.' for qualified method names */
    PlusPlus,       /* '++' */
    MinusMinus,     /* '--' */
    Bang,           /* '!' */
    Tilde,          /* '~' */
    Plus,           /* '+' */
    Minus,          /* '-' */
    Star,           /* '*' */
    Slash,          /* '/' */
    Percent,        /* '%' */
    Amp,            /* '&' */
    AmpAmp,         /* '&&' */
    Pipe,           /* '|' */
    PipePipe,       /* '||' */
    Caret,          /* '^' */
    Lt,             /* '<' */
    Gt,             /* '>' */
    Le,             /* '<=' */
    Ge,             /* '>=' */
    LtLt,           /* '<<' */
    GtGt,           /* '>>' */
    EqEq,           /* '==' */
    BangEq,         /* '!=' */
    /* Keywords */
    KwScript,
    KwBind,
    KwSection,
    KwDeclsection,
    KwGlobal,
    KwIf,
    KwElse,
    KwReturn,
    KwYield,
    KwNop,
    KwGoto,
    KwEnter,
    KwPlay,
    KwOn,
    KwCall,
    KwDone,
    KwMe,
};

struct Lexer {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
    int         col;

    Tok         cur_kind;
    char        cur_text[256];
    int32_t     cur_int;

    void advance();
};

/* Skip whitespace and `// ...` line comments. Returns true if at EOF. */
static bool skip_ws(Lexer *L) {
    while (L->pos < L->len) {
        char c = L->src[L->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++L->pos; ++L->col;
        } else if (c == '\n') {
            ++L->pos; ++L->line; L->col = 1;
        } else if (c == '/' && L->pos + 1 < L->len && L->src[L->pos + 1] == '/') {
            while (L->pos < L->len && L->src[L->pos] != '\n') {
                ++L->pos; ++L->col;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

void Lexer::advance() {
    cur_text[0] = 0;
    cur_int = 0;
    if (skip_ws(this)) {
        cur_kind = Tok::Eof;
        return;
    }
    char c = src[pos];
    char c2 = (pos + 1 < len) ? src[pos + 1] : 0;

    /* Two-char punctuation first */
    if (c == '+' && c2 == '+') { cur_kind = Tok::PlusPlus;  pos += 2; col += 2; return; }
    if (c == '-' && c2 == '-') { cur_kind = Tok::MinusMinus; pos += 2; col += 2; return; }
    if (c == '&' && c2 == '&') { cur_kind = Tok::AmpAmp;   pos += 2; col += 2; return; }
    if (c == '|' && c2 == '|') { cur_kind = Tok::PipePipe; pos += 2; col += 2; return; }
    if (c == '<' && c2 == '<') { cur_kind = Tok::LtLt;     pos += 2; col += 2; return; }
    if (c == '>' && c2 == '>') { cur_kind = Tok::GtGt;     pos += 2; col += 2; return; }
    if (c == '<' && c2 == '=') { cur_kind = Tok::Le;       pos += 2; col += 2; return; }
    if (c == '>' && c2 == '=') { cur_kind = Tok::Ge;       pos += 2; col += 2; return; }
    if (c == '=' && c2 == '=') { cur_kind = Tok::EqEq;     pos += 2; col += 2; return; }
    if (c == '!' && c2 == '=') { cur_kind = Tok::BangEq;   pos += 2; col += 2; return; }

    /* Single-char punctuation */
    if (c == '{') { cur_kind = Tok::LBrace; ++pos; ++col; return; }
    if (c == '}') { cur_kind = Tok::RBrace; ++pos; ++col; return; }
    if (c == '(') { cur_kind = Tok::LParen; ++pos; ++col; return; }
    if (c == ')') { cur_kind = Tok::RParen; ++pos; ++col; return; }
    if (c == ',') { cur_kind = Tok::Comma;  ++pos; ++col; return; }
    if (c == '=') { cur_kind = Tok::Equals; ++pos; ++col; return; }
    if (c == '.') { cur_kind = Tok::Dot;    ++pos; ++col; return; }
    if (c == '!') { cur_kind = Tok::Bang;   ++pos; ++col; return; }
    if (c == '~') { cur_kind = Tok::Tilde;  ++pos; ++col; return; }
    if (c == '+') { cur_kind = Tok::Plus;   ++pos; ++col; return; }
    if (c == '*') { cur_kind = Tok::Star;   ++pos; ++col; return; }
    if (c == '/') { cur_kind = Tok::Slash;  ++pos; ++col; return; }
    if (c == '%') { cur_kind = Tok::Percent;++pos; ++col; return; }
    if (c == '&') { cur_kind = Tok::Amp;    ++pos; ++col; return; }
    if (c == '|') { cur_kind = Tok::Pipe;   ++pos; ++col; return; }
    if (c == '^') { cur_kind = Tok::Caret;  ++pos; ++col; return; }
    if (c == '<') { cur_kind = Tok::Lt;     ++pos; ++col; return; }
    if (c == '>') { cur_kind = Tok::Gt;     ++pos; ++col; return; }

    /* Number (decimal). Handle leading '-' in expression parser, not here. */
    if (is_digit(c)) {
        int32_t v = 0;
        while (pos < len && is_digit(src[pos])) {
            v = v * 10 + (src[pos] - '0');
            ++pos; ++col;
        }
        cur_kind = Tok::Number;
        cur_int = v;
        return;
    }

    /* Negative numbers (lexer handles only when '-' is followed directly by a
       digit and we're starting a token; an expression parser using '-' as
       unary minus must check the next token's kind/value). For simplicity,
       leave '-' as Minus and let the parser handle it. */
    if (c == '-') { cur_kind = Tok::Minus; ++pos; ++col; return; }

    /* String literal */
    if (c == '"') {
        ++pos; ++col;
        size_t k = 0;
        while (pos < len && src[pos] != '"') {
            if (k + 1 < sizeof(cur_text)) cur_text[k++] = src[pos];
            ++pos; ++col;
        }
        cur_text[k] = 0;
        if (pos < len) { ++pos; ++col; }
        cur_kind = Tok::String;
        return;
    }

    /* '@' is used by the decompiler's fallback label form '@HHHH'. We don't
       expect them in fixture text (sections cover all labels), but if we see
       one, lex it as an identifier '@HHHH' so the parser can treat it. */
    if (c == '@') {
        size_t k = 0;
        cur_text[k++] = c;
        ++pos; ++col;
        while (pos < len && is_ident_cont(src[pos])) {
            if (k + 1 < sizeof(cur_text)) cur_text[k++] = src[pos];
            ++pos; ++col;
        }
        cur_text[k] = 0;
        cur_kind = Tok::Ident;
        return;
    }

    /* Identifier / keyword */
    if (is_ident_start(c)) {
        size_t k = 0;
        while (pos < len && is_ident_cont(src[pos])) {
            if (k + 1 < sizeof(cur_text)) cur_text[k++] = src[pos];
            ++pos; ++col;
        }
        cur_text[k] = 0;
        if      (strcmp(cur_text, "script")      == 0) cur_kind = Tok::KwScript;
        else if (strcmp(cur_text, "bind")        == 0) cur_kind = Tok::KwBind;
        else if (strcmp(cur_text, "section")     == 0) cur_kind = Tok::KwSection;
        else if (strcmp(cur_text, "declsection") == 0) cur_kind = Tok::KwDeclsection;
        else if (strcmp(cur_text, "global")      == 0) cur_kind = Tok::KwGlobal;
        else if (strcmp(cur_text, "if")          == 0) cur_kind = Tok::KwIf;
        else if (strcmp(cur_text, "else")        == 0) cur_kind = Tok::KwElse;
        else if (strcmp(cur_text, "return")      == 0) cur_kind = Tok::KwReturn;
        else if (strcmp(cur_text, "yield")       == 0) cur_kind = Tok::KwYield;
        else if (strcmp(cur_text, "nop")         == 0) cur_kind = Tok::KwNop;
        else if (strcmp(cur_text, "goto")        == 0) cur_kind = Tok::KwGoto;
        else if (strcmp(cur_text, "enter")       == 0) cur_kind = Tok::KwEnter;
        else if (strcmp(cur_text, "play")        == 0) cur_kind = Tok::KwPlay;
        else if (strcmp(cur_text, "on")          == 0) cur_kind = Tok::KwOn;
        else if (strcmp(cur_text, "call")        == 0) cur_kind = Tok::KwCall;
        else if (strcmp(cur_text, "done")        == 0) cur_kind = Tok::KwDone;
        else if (strcmp(cur_text, "Me")          == 0) cur_kind = Tok::KwMe;
        else                                            cur_kind = Tok::Ident;
        return;
    }

    /* Unknown -> skip and try again. */
    ++pos; ++col;
    advance();
}

/* ---- Canonical intrinsic name table (MDEdit convention) ---- */

static const char *kIntrinsicNames[MUS_INTRINSIC_NAMES] = {
    "GEcho", "GGRnd", "GSV", "GSDV", "GFB",
    "FSet", "FClear", "FIsSet", "FIsClear",
    "TStart", "TStop",
};

/* Resolve "Me.Method", "Obj.Method", or "Method" from source text to an
   intrinsic index. The decompiler's split_method_name turns:
       GEcho -> "Echo"     (G stripped, no obj prefix)
       GSV   -> "SV"
       FSet  -> "F.Set"
       TStart-> "T.Start"
   so the inverse is: prepend 'G' if no obj, else obj+method joined.
   Returns -1 on miss. */
static int resolve_intrinsic_index(const char *obj, const char *method) {
    char combined[64];
    if (obj && obj[0]) {
        snprintf(combined, sizeof(combined), "%s%s", obj, method);
    } else {
        /* No prefix in source -> the original was 'G' + method. */
        snprintf(combined, sizeof(combined), "G%s", method);
    }
    for (int i = 0; i < MUS_INTRINSIC_NAMES; ++i) {
        if (strcmp(kIntrinsicNames[i], combined) == 0) return i;
    }
    /* Also try the name as-is (some methods have no G prefix in source: e.g.
       "Echo" decompiles from GEcho but "Random" might appear). */
    if (!obj || !obj[0]) {
        for (int i = 0; i < MUS_INTRINSIC_NAMES; ++i) {
            if (strcmp(kIntrinsicNames[i], method) == 0) return i;
        }
    }
    return -1;
}

/* ---- Section table accumulation ---- */

struct SectionDef {
    char     name[MUS_SECTION_NAME_SIZE];
    uint32_t code_offset;       /* bytecode-relative; populated when emitted */
    int      defined;           /* 1 once 'section X { ... }' has been seen */
};

/* ---- Bytecode emit buffer + label patching ---- */

struct LabelPatch {
    uint32_t patch_offset;       /* offset of 4-byte branch target within code */
    char     section_name[MUS_SECTION_NAME_SIZE]; /* destination section */
};

struct Emit {
    uint8_t     *bytes;
    size_t       cap;
    size_t       used;

    /* Pending branch patches resolved at finalize when we have all section
       offsets. */
    LabelPatch  *patches;
    size_t       patch_cap;
    size_t       patch_count;

    Emit() : bytes(NULL), cap(0), used(0),
             patches(NULL), patch_cap(0), patch_count(0) {}

    void byte(uint8_t b) {
        if (used >= cap) {
            size_t new_cap = cap ? cap * 2 : 256;
            bytes = (uint8_t *)realloc(bytes, new_cap);
            cap = new_cap;
        }
        bytes[used++] = b;
    }
    void u16le(uint16_t v) {
        byte((uint8_t)(v & 0xFF));
        byte((uint8_t)((v >> 8) & 0xFF));
    }
    void u32le(uint32_t v) {
        byte((uint8_t)(v & 0xFF));
        byte((uint8_t)((v >> 8) & 0xFF));
        byte((uint8_t)((v >> 16) & 0xFF));
        byte((uint8_t)((v >> 24) & 0xFF));
    }
    /* Reserve 4 bytes for a branch-target patch and return the offset. */
    uint32_t reserve_u32() {
        uint32_t off = (uint32_t)used;
        u32le(0xDEADBEEFu);
        return off;
    }
    void patch_u32(uint32_t at, uint32_t v) {
        if ((size_t)at + 4 > used) return;
        bytes[at + 0] = (uint8_t)(v & 0xFF);
        bytes[at + 1] = (uint8_t)((v >> 8) & 0xFF);
        bytes[at + 2] = (uint8_t)((v >> 16) & 0xFF);
        bytes[at + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
    void add_patch(uint32_t at, const char *section_name) {
        if (patch_count >= patch_cap) {
            size_t nc = patch_cap ? patch_cap * 2 : 16;
            patches = (LabelPatch *)realloc(patches, nc * sizeof(LabelPatch));
            patch_cap = nc;
        }
        LabelPatch &p = patches[patch_count++];
        p.patch_offset = at;
        memset(p.section_name, 0, sizeof(p.section_name));
        strncpy(p.section_name, section_name, sizeof(p.section_name) - 1);
    }
};

/* ---- Variable-name resolution ---- */

/* Var00..Var15 -> byte offsets 0..60. Returns -1 if not a Var-prefixed name. */
static int parse_var_offset(const char *name) {
    if (strncmp(name, "Var", 3) == 0 && name[3] != 0) {
        const char *p = name + 3;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            n = n * 10 + (*p - '0');
            ++p;
        }
        return n * 4;   /* MDEdit pre-defined slot */
    }
    return -1;
}

static int parse_g_offset(const char *name) {
    if (strncmp(name, "g_", 2) == 0 && name[2] != 0) {
        const char *p = name + 2;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            n = n * 10 + (*p - '0');
            ++p;
        }
        return n;
    }
    return -1;
}

static int parse_l_index(const char *name) {
    if (strncmp(name, "l_", 2) == 0 && name[2] != 0) {
        const char *p = name + 2;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            n = n * 10 + (*p - '0');
            ++p;
        }
        return n;
    }
    return -1;
}

/* Parse 'sound_<N>' -> N. Returns -1 on miss. */
static int parse_sound_index(const char *name) {
    if (strncmp(name, "sound_", 6) == 0 && name[6] != 0) {
        const char *p = name + 6;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            n = n * 10 + (*p - '0');
            ++p;
        }
        return n;
    }
    return -1;
}

/* ---- Compiler ---- */

struct Compiler {
    Lexer        lex;
    MusScript    out;
    Emit         emit;

    /* Section table accumulator */
    SectionDef  *sections;
    size_t       section_cap;
    size_t       section_count;

    /* Variable-name accumulator (named globals from `global INT name`) */
    MusVariable *variables;
    size_t       variable_cap;
    size_t       variable_count;

    /* Bind map. `bind sound_N "Name"` records BOTH `sound_N` and `Name` -> N so
       a later `play Name`, `play sound_N`, or `on (...) play Name` all resolve to
       the same sound index. This is MDEdit's real bind semantics (the binary
       carries no bind table, but the editor's names-aware decompile emits real
       SBF entry names at play sites). The decompiler emits all binds before the
       first section, so the map is fully populated before any play is parsed. */
    struct BindDef { char name[64]; int index; };
    BindDef     *binds;
    size_t       bind_cap;
    size_t       bind_count;

    Compiler() : sections(NULL), section_cap(0), section_count(0),
                 variables(NULL), variable_cap(0), variable_count(0),
                 binds(NULL), bind_cap(0), bind_count(0) {}

    /* Find/intern a section by name; returns its index in `sections`. */
    int section_find_or_create(const char *name) {
        for (size_t i = 0; i < section_count; ++i) {
            if (strcmp(sections[i].name, name) == 0) return (int)i;
        }
        if (section_count >= section_cap) {
            size_t nc = section_cap ? section_cap * 2 : 8;
            sections = (SectionDef *)realloc(sections, nc * sizeof(SectionDef));
            section_cap = nc;
        }
        SectionDef &s = sections[section_count++];
        memset(&s, 0, sizeof(s));
        strncpy(s.name, name, sizeof(s.name) - 1);
        s.code_offset = 0;
        s.defined = 0;
        return (int)(section_count - 1);
    }

    int variable_find_or_create(const char *name, uint32_t byte_offset) {
        for (size_t i = 0; i < variable_count; ++i) {
            if (strcmp(variables[i].name, name) == 0) return (int)i;
        }
        if (variable_count >= variable_cap) {
            size_t nc = variable_cap ? variable_cap * 2 : 4;
            variables = (MusVariable *)realloc(variables,
                                               nc * sizeof(MusVariable));
            variable_cap = nc;
        }
        MusVariable &v = variables[variable_count++];
        memset(&v, 0, sizeof(v));
        strncpy(v.name, name, MUS_INTRINSIC_NAME_SIZE - 1);
        v.byte_offset = byte_offset;
        return (int)(variable_count - 1);
    }

    /* Record a `bind <name> <index>` mapping (last write wins per name). */
    void bind_add(const char *name, int index) {
        if (!name || !name[0]) return;
        for (size_t i = 0; i < bind_count; ++i) {
            if (strcmp(binds[i].name, name) == 0) { binds[i].index = index; return; }
        }
        if (bind_count >= bind_cap) {
            size_t nc = bind_cap ? bind_cap * 2 : 8;
            binds = (BindDef *)realloc(binds, nc * sizeof(BindDef));
            bind_cap = nc;
        }
        BindDef &b = binds[bind_count++];
        memset(&b, 0, sizeof(b));
        strncpy(b.name, name, sizeof(b.name) - 1);
        b.index = index;
    }

    /* Resolve a `play` / `on (...) play` target to a sound index: a literal
       `sound_N` first, else a bound name (`bind sound_N "Name"`), else -1. */
    int resolve_play_target(const char *name) {
        int idx = parse_sound_index(name);
        if (idx >= 0) return idx;
        for (size_t i = 0; i < bind_count; ++i) {
            if (strcmp(binds[i].name, name) == 0) return binds[i].index;
        }
        return -1;
    }

    /* Resolve a global-variable reference name to a byte offset. */
    int resolve_global_offset(const char *name) {
        int v = parse_var_offset(name);
        if (v >= 0) return v;
        v = parse_g_offset(name);
        if (v >= 0) return v;
        /* Named global (declared via `global INT name`). Match against
           accumulated variables. */
        for (size_t i = 0; i < variable_count; ++i) {
            if (strcmp(variables[i].name, name) == 0) {
                return (int)variables[i].byte_offset;
            }
        }
        /* Auto-allocate at the next 4-byte slot >= 64 (user-globals area). */
        uint32_t off = 64;
        for (size_t i = 0; i < variable_count; ++i) {
            uint32_t end = variables[i].byte_offset + 4;
            if (end > off) off = end;
        }
        variable_find_or_create(name, off);
        return (int)off;
    }

    /* ---- Emit helpers ---- */

    /* push <int>: 0x01 (u8) when 0..255, else 0x02 (u32). The decompiler
       prints raw decimal regardless, so we can pick whichever fits. The
       jo_gamemus.bin original uses 0x01 for small values -- we follow it. */
    void emit_push_int(int32_t v) {
        if (v >= 0 && v <= 255) {
            emit.byte(0x01);
            emit.byte((uint8_t)v);
        } else {
            emit.byte(0x02);
            emit.u32le((uint32_t)v);
        }
    }

    void emit_push_g(int byte_offset) {
        emit.byte(0x03);
        emit.byte((uint8_t)byte_offset);
    }
    void emit_push_l(int idx) {
        emit.byte(0x04);
        emit.byte((uint8_t)idx);
    }
    void emit_pop_g(int byte_offset) {
        emit.byte(0x08);
        emit.byte((uint8_t)byte_offset);
    }
    void emit_pop_l(int idx) {
        emit.byte(0x09);
        emit.byte((uint8_t)idx);
    }
    void emit_inc_g(int byte_offset) {
        emit.byte(0x28);
        emit.byte((uint8_t)byte_offset);
    }
    void emit_dec_g(int byte_offset) {
        emit.byte(0x29);
        emit.byte((uint8_t)byte_offset);
    }
    void emit_inc_l(int idx) {
        emit.byte(0x2A);
        emit.byte((uint8_t)idx);
    }
    void emit_dec_l(int idx) {
        emit.byte(0x2B);
        emit.byte((uint8_t)idx);
    }

    /* ---- Recursive expression parsing ----

       Strategy: the decompiler emits binops always parenthesized as
       `(a OP b)`. That makes the grammar trivially left-to-right -- we
       expect `(`, recurse for `a`, read `OP`, recurse for `b`, expect `)`.
       Unary ops (`!`, `-`, `~`) prefix a single expression. Method calls
       are `IDENT '(' [expr] ')'` or `IDENT '.' IDENT '(' [expr] ')'`. */

    int parse_expr(const char **err);

    int parse_method_call(const char *first_ident, const char **err) {
        /* We've consumed `first_ident`. Optional `.METHOD` then `(arg?)`. */
        char obj[32]   = "";
        char method[64] = "";
        if (lex.cur_kind == Tok::Dot) {
            strncpy(obj, first_ident, sizeof(obj) - 1);
            lex.advance();
            if (lex.cur_kind != Tok::Ident && lex.cur_kind != Tok::KwReturn) {
                *err = "expected method name after '.'";
                return -1;
            }
            strncpy(method, lex.cur_text, sizeof(method) - 1);
            lex.advance();
        } else {
            strncpy(method, first_ident, sizeof(method) - 1);
        }
        if (lex.cur_kind != Tok::LParen) {
            *err = "expected '(' for method call";
            return -1;
        }
        lex.advance();
        if (lex.cur_kind != Tok::RParen) {
            int rc = parse_expr(err);
            if (rc != 0) return rc;
        }
        if (lex.cur_kind != Tok::RParen) {
            *err = "expected ')' to close method call";
            return -1;
        }
        lex.advance();
        int idx = resolve_intrinsic_index(obj[0] ? obj : NULL, method);
        if (idx < 0) {
            *err = "unknown intrinsic method";
            return -1;
        }
        emit.byte(0x40);
        emit.byte((uint8_t)idx);
        return 0;
    }

    /* Map a punctuation token to a binop opcode byte; returns 0 on miss. */
    static uint8_t binop_byte(Tok t) {
        switch (t) {
            case Tok::Plus:    return 0x10;
            case Tok::Minus:   return 0x11;
            case Tok::Star:    return 0x12;
            case Tok::Slash:   return 0x13;
            case Tok::Percent: return 0x14;
            case Tok::AmpAmp:  return 0x15;
            case Tok::PipePipe:return 0x16;
            case Tok::Amp:     return 0x17;
            case Tok::Pipe:    return 0x18;
            case Tok::Caret:   return 0x19;
            case Tok::LtLt:    return 0x1C;
            case Tok::GtGt:    return 0x1D;
            case Tok::EqEq:    return 0x20;
            case Tok::BangEq:  return 0x21;
            case Tok::Ge:      return 0x22;
            case Tok::Le:      return 0x23;
            case Tok::Gt:      return 0x24;
            case Tok::Lt:      return 0x25;
            default:           return 0;
        }
    }

    int parse_top_decl(const char **err);
    int parse_section_body(const char **err, bool inside_section);
    int parse_stmt(const char **err);
    int parse_top_level(const char **err);

    int finalize(const char **err);
    int parse_script(const char **err);
};

int Compiler::parse_expr(const char **err) {
    /* Unary prefix */
    if (lex.cur_kind == Tok::Bang) {
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        emit.byte(0x1E);   /* not */
        return 0;
    }
    if (lex.cur_kind == Tok::Tilde) {
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        emit.byte(0x1B);   /* inverse */
        return 0;
    }
    if (lex.cur_kind == Tok::Minus) {
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        emit.byte(0x1A);   /* neg */
        return 0;
    }

    /* Parenthesized binop: '(' expr OP expr ')' */
    if (lex.cur_kind == Tok::LParen) {
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        uint8_t op = binop_byte(lex.cur_kind);
        if (op == 0) {
            /* Could be a parenthesized-only sub-expression: '(' expr ')'.
               (The decompiler doesn't emit those, but accept them.) */
            if (lex.cur_kind == Tok::RParen) {
                lex.advance();
                return 0;
            }
            *err = "expected binary operator inside parens";
            return -1;
        }
        lex.advance();
        rc = parse_expr(err);
        if (rc != 0) return rc;
        if (lex.cur_kind != Tok::RParen) {
            *err = "expected ')'";
            return -1;
        }
        lex.advance();
        emit.byte(op);
        return 0;
    }

    /* Number literal */
    if (lex.cur_kind == Tok::Number) {
        emit_push_int(lex.cur_int);
        lex.advance();
        return 0;
    }

    /* Identifier: variable, Me, or method call */
    if (lex.cur_kind == Tok::KwMe) {
        lex.advance();
        if (lex.cur_kind == Tok::Dot) {
            return parse_method_call("Me", err);
        }
        emit.byte(0x0C);   /* push_me */
        return 0;
    }
    if (lex.cur_kind == Tok::Ident) {
        char name[256];
        strncpy(name, lex.cur_text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        lex.advance();
        /* Method call shape: IDENT '(' or IDENT '.' IDENT '(' */
        if (lex.cur_kind == Tok::LParen || lex.cur_kind == Tok::Dot) {
            return parse_method_call(name, err);
        }
        /* Variable reference: emit push_g / push_l. */
        int li = parse_l_index(name);
        if (li >= 0) {
            emit_push_l(li);
            return 0;
        }
        int gi = resolve_global_offset(name);
        if (gi >= 0) {
            emit_push_g(gi);
            return 0;
        }
        *err = "unrecognised identifier in expression";
        return -1;
    }

    *err = "unexpected token in expression";
    return -1;
}

int Compiler::parse_stmt(const char **err) {
    /* play <sound_N | bound-name | "bound name"> */
    if (lex.cur_kind == Tok::KwPlay) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident && lex.cur_kind != Tok::String) {
            *err = "expected sound name after 'play'";
            return -1;
        }
        int idx = resolve_play_target(lex.cur_text);
        if (idx < 0) {
            *err = "unknown play target (expected 'sound_N' or a bound name)";
            return -1;
        }
        /* The play opcode operand is a single byte; a larger index would silently
           wrap to a different sound. Reject it so the editor can never emit a
           valid-but-wrong play (the original engine reads only a u8 here too). */
        if (idx > 255) {
            *err = "play target index out of range (max 255)";
            return -1;
        }
        emit.byte(0x3E);
        emit.byte((uint8_t)idx);
        lex.advance();
        return 0;
    }

    /* enter SECTION */
    if (lex.cur_kind == Tok::KwEnter) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected section name after 'enter'";
            return -1;
        }
        int sidx = section_find_or_create(lex.cur_text);
        emit.byte(0x3B);             /* setstate -- decompiles same as 0x38 enter */
        emit.byte((uint8_t)sidx);
        lex.advance();
        return 0;
    }

    /* goto SECTION   (only top-level section names; we don't generate label
       targets inside sections in our text format) */
    if (lex.cur_kind == Tok::KwGoto) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected target after 'goto'";
            return -1;
        }
        char name[MUS_SECTION_NAME_SIZE];
        strncpy(name, lex.cur_text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        emit.byte(0x30);
        uint32_t patch = emit.reserve_u32();
        emit.add_patch(patch, name);
        lex.advance();
        return 0;
    }

    /* call SECTION */
    if (lex.cur_kind == Tok::KwCall) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected target after 'call'";
            return -1;
        }
        char name[MUS_SECTION_NAME_SIZE];
        strncpy(name, lex.cur_text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        emit.byte(0x34);             /* callv */
        uint32_t patch = emit.reserve_u32();
        emit.add_patch(patch, name);
        lex.advance();
        return 0;
    }

    /* return / yield / nop / done */
    if (lex.cur_kind == Tok::KwReturn) {
        lex.advance();
        emit.byte(0x39);
        return 0;
    }
    if (lex.cur_kind == Tok::KwYield) {
        lex.advance();
        emit.byte(0x3A);
        return 0;
    }
    if (lex.cur_kind == Tok::KwNop) {
        lex.advance();
        emit.byte(0x00);
        return 0;
    }
    if (lex.cur_kind == Tok::KwDone) {
        lex.advance();
        emit.byte(0x3F);
        return 0;
    }

    /* if (expr) { body } [else { body }] */
    if (lex.cur_kind == Tok::KwIf) {
        lex.advance();
        if (lex.cur_kind != Tok::LParen) {
            *err = "expected '(' after 'if'";
            return -1;
        }
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        if (lex.cur_kind != Tok::RParen) {
            *err = "expected ')' after if condition";
            return -1;
        }
        lex.advance();
        if (lex.cur_kind != Tok::LBrace) {
            *err = "expected '{' for if body";
            return -1;
        }
        lex.advance();
        /* Emit brfalse with placeholder. */
        emit.byte(0x31);
        uint32_t br_target_off = emit.reserve_u32();
        /* Body */
        while (lex.cur_kind != Tok::RBrace && lex.cur_kind != Tok::Eof) {
            int rc2 = parse_stmt(err);
            if (rc2 != 0) return rc2;
        }
        if (lex.cur_kind != Tok::RBrace) {
            *err = "expected '}' to close if body";
            return -1;
        }
        lex.advance();
        if (lex.cur_kind == Tok::KwElse) {
            lex.advance();
            if (lex.cur_kind != Tok::LBrace) {
                *err = "expected '{' after else";
                return -1;
            }
            lex.advance();
            /* End-of-if jump */
            emit.byte(0x30);
            uint32_t end_patch = emit.reserve_u32();
            /* Patch brfalse target = current pos */
            emit.patch_u32(br_target_off, (uint32_t)emit.used);
            while (lex.cur_kind != Tok::RBrace && lex.cur_kind != Tok::Eof) {
                int rc2 = parse_stmt(err);
                if (rc2 != 0) return rc2;
            }
            if (lex.cur_kind != Tok::RBrace) {
                *err = "expected '}' to close else body";
                return -1;
            }
            lex.advance();
            emit.patch_u32(end_patch, (uint32_t)emit.used);
        } else {
            emit.patch_u32(br_target_off, (uint32_t)emit.used);
        }
        return 0;
    }

    /* on (expr) ACTION TARGET TARGET ... -- tablexec
       Actions: enter (inner_op=0x3B), play (inner_op=0x3E), goto (inner_op=0x30) */
    if (lex.cur_kind == Tok::KwOn) {
        lex.advance();
        if (lex.cur_kind != Tok::LParen) {
            *err = "expected '(' after 'on'";
            return -1;
        }
        lex.advance();
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        if (lex.cur_kind != Tok::RParen) {
            *err = "expected ')' after on condition";
            return -1;
        }
        lex.advance();
        uint8_t inner_op = 0;
        int     entry_size = 2;
        if      (lex.cur_kind == Tok::KwEnter) inner_op = 0x3B;
        else if (lex.cur_kind == Tok::KwPlay)  inner_op = 0x3E;
        else if (lex.cur_kind == Tok::KwGoto)  { inner_op = 0x30; entry_size = 5; }
        else { *err = "expected enter/play/goto after 'on (...)'"; return -1; }
        lex.advance();
        /* Collect target identifiers. Names-aware decompiles can emit a play
           target as a quoted string when the SBF entry name isn't a bare
           identifier, so accept String here too (cur_text holds the unquoted
           bytes). enter/goto targets are always bare section idents. */
        char targets[64][MUS_SECTION_NAME_SIZE];
        int  ntargets = 0;
        while ((lex.cur_kind == Tok::Ident || lex.cur_kind == Tok::String)
               && ntargets < 64) {
            strncpy(targets[ntargets], lex.cur_text, MUS_SECTION_NAME_SIZE - 1);
            targets[ntargets][MUS_SECTION_NAME_SIZE - 1] = 0;
            ++ntargets;
            lex.advance();
        }
        /* Stopping at 64 must be a hard error, not a silent truncation: leftover
           target tokens would otherwise be misparsed as the next statement (a
           confusing downstream error) and the count byte can't represent them. */
        if (lex.cur_kind == Tok::Ident || lex.cur_kind == Tok::String) {
            *err = "too many targets in on(...) table (max 64)";
            return -1;
        }
        /* Emit tablexec opcode + 4 header bytes: count, inner_op, entry_size,
           skip_size. skip_size is the TOTAL encoded instruction length
           (1 opcode + 4 header + count*entry_size body); Jointops.exe's
           VmOp_TableExec @ 0x672BB0 reads it as `add esi, skip; dec esi` to
           land the next opcode, so an incorrect (or zero) value scrambles
           the dispatcher on out-of-range indices. Witnessed: jo_gamemus
           tablexec @ 0x00c8 has size=3, entry_stride=2 → skip_size = 0x0b. */
        uint32_t total_size = 5u + (uint32_t)ntargets * (uint32_t)entry_size;
        /* skip_size is a single byte the VM uses to step past the table; a wrapped
           value scrambles the dispatcher on out-of-range indices. Reachable with a
           goto-action table (entry_size 5) of >=51 targets. Error instead of wrap. */
        if (total_size > 255) {
            *err = "on(...) table too large to encode (reduce targets)";
            return -1;
        }
        emit.byte(0x35);
        emit.byte((uint8_t)ntargets);
        emit.byte(inner_op);
        emit.byte((uint8_t)entry_size);
        emit.byte((uint8_t)total_size);
        for (int t = 0; t < ntargets; ++t) {
            emit.byte(inner_op);
            if (entry_size == 2) {
                /* enter or play: entry[1] = section/sound idx (1 byte) */
                if (inner_op == 0x3B) {
                    int sidx = section_find_or_create(targets[t]);
                    if (sidx > 255) {
                        *err = "too many sections to index in on(...) table";
                        return -1;
                    }
                    emit.byte((uint8_t)sidx);
                } else {
                    /* play: target is sound_N or a bound name */
                    int sidx = resolve_play_target(targets[t]);
                    if (sidx < 0) {
                        *err = "unknown play target in on(...) play table";
                        return -1;
                    }
                    if (sidx > 255) {
                        *err = "play target index out of range in on(...) table (max 255)";
                        return -1;
                    }
                    emit.byte((uint8_t)sidx);
                }
            } else {
                /* goto: entry[1..4] = absolute target offset (4 LE bytes) */
                uint32_t patch = (uint32_t)emit.used;
                emit.u32le(0xDEADBEEFu);
                emit.add_patch(patch, targets[t]);
            }
        }
        return 0;
    }

    /* IDENT++ / IDENT-- / IDENT = expr / expression-statement starting with IDENT */
    if (lex.cur_kind == Tok::Ident) {
        char name[256];
        strncpy(name, lex.cur_text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        /* Peek ahead to disambiguate. */
        Lexer save = lex;
        lex.advance();
        if (lex.cur_kind == Tok::PlusPlus) {
            int li = parse_l_index(name);
            if (li >= 0) emit_inc_l(li);
            else {
                int g = resolve_global_offset(name);
                emit_inc_g(g);
            }
            lex.advance();
            return 0;
        }
        if (lex.cur_kind == Tok::MinusMinus) {
            int li = parse_l_index(name);
            if (li >= 0) emit_dec_l(li);
            else {
                int g = resolve_global_offset(name);
                emit_dec_g(g);
            }
            lex.advance();
            return 0;
        }
        if (lex.cur_kind == Tok::Equals) {
            lex.advance();
            int rc = parse_expr(err);
            if (rc != 0) return rc;
            int li = parse_l_index(name);
            if (li >= 0) {
                emit_pop_l(li);
            } else {
                int g = resolve_global_offset(name);
                emit_pop_g(g);
            }
            return 0;
        }
        /* Otherwise it's an expression statement: rewind and parse the whole
           expression, then emit `empty` (0x0F). */
        lex = save;
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        emit.byte(0x0F);
        return 0;
    }

    /* Number / parenthesised expression at statement start: expression statement. */
    if (lex.cur_kind == Tok::Number || lex.cur_kind == Tok::LParen
     || lex.cur_kind == Tok::KwMe   || lex.cur_kind == Tok::Bang
     || lex.cur_kind == Tok::Tilde  || lex.cur_kind == Tok::Minus) {
        int rc = parse_expr(err);
        if (rc != 0) return rc;
        emit.byte(0x0F);
        return 0;
    }

    *err = "unexpected token in statement";
    return -1;
}

int Compiler::parse_section_body(const char **err, bool inside_section) {
    while (lex.cur_kind != Tok::Eof) {
        if (lex.cur_kind == Tok::RBrace && inside_section) {
            /* '}' closes a section: emit done and consume. */
            emit.byte(0x3F);
            lex.advance();
            return 0;
        }
        int rc = parse_stmt(err);
        if (rc != 0) return rc;
    }
    if (inside_section) {
        *err = "unexpected EOF inside section body";
        return -1;
    }
    return 0;
}

int Compiler::parse_top_decl(const char **err) {
    /* `bind sound_N "Name"` -- record the name->index mapping so a later
       `play Name` (the names-aware decompile form) resolves to sound index N.
       Emits no bytecode (the runtime carries no bind table); the identifier
       `sound_N` itself also maps to N so the names-less form keeps working. */
    if (lex.cur_kind == Tok::KwBind) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected bind name";
            return -1;
        }
        char bind_id[64];
        strncpy(bind_id, lex.cur_text, sizeof(bind_id) - 1);
        bind_id[sizeof(bind_id) - 1] = 0;
        lex.advance();
        char display[64];
        display[0] = 0;
        if (lex.cur_kind == Tok::String) {
            strncpy(display, lex.cur_text, sizeof(display) - 1);
            display[sizeof(display) - 1] = 0;
            lex.advance();
        }
        int idx = parse_sound_index(bind_id);
        if (idx >= 0) {
            bind_add(bind_id, idx);              /* sound_N -> N */
            if (display[0]) bind_add(display, idx);  /* "Name" -> N */
        }
        return 0;
    }
    /* `global INT name` -- aesthetic; remember name -> auto-allocated offset. */
    if (lex.cur_kind == Tok::KwGlobal) {
        lex.advance();
        if (lex.cur_kind == Tok::Ident) lex.advance();   /* type token */
        if (lex.cur_kind == Tok::Ident) {
            char name[MUS_INTRINSIC_NAME_SIZE];
            strncpy(name, lex.cur_text, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            uint32_t off = 64;
            for (size_t i = 0; i < variable_count; ++i) {
                uint32_t end = variables[i].byte_offset + 4;
                if (end > off) off = end;
            }
            variable_find_or_create(name, off);
            lex.advance();
        }
        return 0;
    }
    /* `declsection NAME` -- aesthetic; intern name so first-use ordering is
       stable. */
    if (lex.cur_kind == Tok::KwDeclsection) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected section name after 'declsection'";
            return -1;
        }
        section_find_or_create(lex.cur_text);
        lex.advance();
        return 0;
    }
    /* `section NAME { body }` */
    if (lex.cur_kind == Tok::KwSection) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected section name";
            return -1;
        }
        int sidx = section_find_or_create(lex.cur_text);
        sections[sidx].code_offset = (uint32_t)emit.used;
        sections[sidx].defined = 1;
        lex.advance();
        if (lex.cur_kind != Tok::LBrace) {
            *err = "expected '{' after section name";
            return -1;
        }
        lex.advance();
        return parse_section_body(err, /*inside_section=*/true);
    }

    /* Statement appearing at top level (after a section closed with `}`):
       belongs to whatever section last had an entry-PC label. We just emit
       it into the current bytecode stream. */
    return parse_stmt(err);
}

int Compiler::finalize(const char **err) {
    /* Resolve all label patches. */
    for (size_t i = 0; i < emit.patch_count; ++i) {
        const LabelPatch &p = emit.patches[i];
        int found = -1;
        for (size_t s = 0; s < section_count; ++s) {
            if (strcmp(sections[s].name, p.section_name) == 0) {
                found = (int)s;
                break;
            }
        }
        if (found < 0) {
            *err = "unresolved branch target";
            return -1;
        }
        emit.patch_u32(p.patch_offset, sections[found].code_offset);
    }

    /* Allocate output buffers. */
    out.code_size = (uint32_t)emit.used;
    if (out.code_size > 0) {
        out.code = (uint8_t *)malloc(out.code_size);
        if (!out.code) { *err = "OOM"; return -1; }
        memcpy(out.code, emit.bytes, out.code_size);
    }

    if (section_count > 0) {
        out.sections = (MusSection *)calloc(section_count, sizeof(MusSection));
        if (!out.sections) { *err = "OOM"; return -1; }
        out.section_count = (uint32_t)section_count;
        for (size_t i = 0; i < section_count; ++i) {
            strncpy(out.sections[i].name, sections[i].name,
                    MUS_SECTION_NAME_SIZE - 1);
            out.sections[i].code_offset = sections[i].code_offset;
        }
    }

    if (variable_count > 0) {
        out.variables = (MusVariable *)calloc(variable_count, sizeof(MusVariable));
        if (!out.variables) { *err = "OOM"; return -1; }
        out.variable_count = (uint32_t)variable_count;
        memcpy(out.variables, variables, variable_count * sizeof(MusVariable));
    }

    /* MDEdit pre-defines globals area = 64 bytes (Var00..Var15); user globals
       grow it. */
    uint32_t gsize = 64;
    for (size_t i = 0; i < variable_count; ++i) {
        uint32_t end = variables[i].byte_offset + 4;
        if (end > gsize) gsize = end;
    }
    out.globals_size = gsize;
    out.locals_size = 0x28;       /* match the JO fixture default */
    out.locals_frame_offset = 0x20;   /* `enter` frame base; JO/MDEdit witness */
    out.entry_section_index = 0;

    /* Populate intrinsic names with the canonical 11. */
    for (int i = 0; i < MUS_INTRINSIC_NAMES; ++i) {
        memset(out.intrinsic_names[i], 0, MUS_INTRINSIC_NAME_SIZE);
        strncpy(out.intrinsic_names[i], kIntrinsicNames[i],
                MUS_INTRINSIC_NAME_SIZE - 1);
    }
    out.intrinsic_count = MUS_INTRINSIC_NAMES;

    return 0;
}

int Compiler::parse_script(const char **err) {
    lex.advance();
    if (lex.cur_kind != Tok::KwScript) {
        *err = "expected 'script' at top of file";
        return -1;
    }
    lex.advance();
    if (lex.cur_kind != Tok::Ident) {
        *err = "expected script name";
        return -1;
    }
    strncpy(out.name, lex.cur_text, MUS_NAME_SIZE - 1);
    out.name[MUS_NAME_SIZE - 1] = 0;
    lex.advance();

    while (lex.cur_kind != Tok::Eof) {
        int rc = parse_top_decl(err);
        if (rc != 0) return rc;
    }
    return finalize(err);
}

}  /* anonymous namespace */

/* Pre-pass: scan comment lines for "// Original source: <path>" so the
   decompiler's source-path comment round-trips through compile. The path
   isn't part of any encoded structure -- it lives in the editor debug
   section in the original binary and the compiler stashes it directly into
   MusScript.source_path so a downstream re-decompile can reproduce the line. */
static void capture_source_path(const char *text, char *out_path, size_t cap) {
    out_path[0] = 0;
    const char *needle = "// Original source: ";
    size_t nlen = strlen(needle);
    const char *p = text;
    while (*p) {
        if (strncmp(p, needle, nlen) == 0) {
            const char *e = p + nlen;
            const char *line_end = e;
            while (*line_end && *line_end != '\n' && *line_end != '\r') ++line_end;
            size_t copy = (size_t)(line_end - e);
            if (copy >= cap) copy = cap - 1;
            memcpy(out_path, e, copy);
            out_path[copy] = 0;
            return;
        }
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
}

extern "C" int mus_compile(const char *text, MusScript *out_script,
                           int *err_line, int *err_col, const char **err_msg) {
    if (!text || !out_script) return -1;
    Compiler c;
    c.lex.src  = text;
    c.lex.len  = strlen(text);
    c.lex.pos  = 0;
    c.lex.line = 1;
    c.lex.col  = 1;
    memset(&c.out, 0, sizeof(c.out));
    capture_source_path(text, c.out.source_path, MUS_SOURCE_PATH_SIZE);

    const char *local_err = NULL;
    int rc = c.parse_script(&local_err);
    free(c.emit.bytes);
    free(c.emit.patches);
    free(c.sections);
    free(c.variables);
    free(c.binds);
    if (rc != 0) {
        if (err_msg)  *err_msg  = local_err ? local_err : "compile error";
        if (err_line) *err_line = c.lex.line;
        if (err_col)  *err_col  = c.lex.col;
        mus_script_free(&c.out);
        return rc;
    }
    *out_script = c.out;
    return 0;
}

extern "C" void mus_script_free(MusScript *s) {
    if (!s) return;
    free(s->code);
    free(s->sections);
    free(s->variables);
    s->code = NULL;
    s->sections = NULL;
    s->variables = NULL;
    s->code_size = 0;
    s->section_count = 0;
    s->variable_count = 0;
}

extern "C" void mus_free(void *p) { free(p); }

/* Append `n` bytes from `src` to `*buf`, growing as needed. */
static void buf_append(uint8_t **buf, size_t *cap, size_t *used,
                       const void *src, size_t n) {
    if (*used + n > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *used + n) nc *= 2;
        *buf = (uint8_t *)realloc(*buf, nc);
        *cap = nc;
    }
    if (n) memcpy(*buf + *used, src, n);
    *used += n;
}
static void buf_append_zero(uint8_t **buf, size_t *cap, size_t *used, size_t n) {
    if (*used + n > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *used + n) nc *= 2;
        *buf = (uint8_t *)realloc(*buf, nc);
        *cap = nc;
    }
    if (n) memset(*buf + *used, 0, n);
    *used += n;
}

extern "C" int mus_encode_file(const MusScript *const *scripts, uint32_t script_count,
                               uint8_t **out_buf, size_t *out_size) {
    if (!scripts || !out_buf || !out_size) return -1;

    uint8_t *buf = NULL;
    size_t   cap = 0, used = 0;

    /* Reserve the file header (44 bytes) and chunk pointer table. */
    buf_append_zero(&buf, &cap, &used, sizeof(MusFileHeader));
    size_t chunk_table_offset = used;
    buf_append_zero(&buf, &cap, &used, (size_t)script_count * 4);

    /* Per-chunk offsets we'll backpatch later. */
    uint32_t *chunk_offs = (uint32_t *)calloc(script_count, sizeof(uint32_t));
    if (!chunk_offs) { free(buf); return -2; }

    for (uint32_t i = 0; i < script_count; ++i) {
        const MusScript *s = scripts[i];
        if (!s) { free(buf); free(chunk_offs); return -3; }

        chunk_offs[i] = (uint32_t)used;

        /* Chunk header: write zeros, fill in fields after we know offsets. */
        size_t chunk_hdr_at = used;
        buf_append_zero(&buf, &cap, &used, sizeof(MusChunkHeader));

        /* Section table immediately after chunk header. Stored as
           chunk-relative offsets; we'll fill them in after we know
           bytecode_offset (so we can shift bytecode-relative -> chunk-relative). */
        size_t   sec_tab_off_in_chunk = used - chunk_hdr_at;
        uint32_t *sec_tab_slots = NULL;
        if (s->section_count > 0) {
            sec_tab_slots = (uint32_t *)calloc(s->section_count, sizeof(uint32_t));
            if (!sec_tab_slots) { free(buf); free(chunk_offs); return -4; }
        }
        size_t sec_tab_at = used;
        buf_append_zero(&buf, &cap, &used,
                        (size_t)s->section_count * sizeof(uint32_t));

        /* Bytecode follows. */
        size_t bc_at = used;
        size_t bc_off_in_chunk = bc_at - chunk_hdr_at;
        if (s->code && s->code_size > 0) {
            buf_append(&buf, &cap, &used, s->code, s->code_size);
        }

        /* Now we know the bytecode chunk-relative offset; populate the
           section-table slots (they store chunk-relative entry-PC offsets
           = bytecode_offset + bytecode-relative section.code_offset). */
        for (uint32_t k = 0; k < s->section_count; ++k) {
            uint32_t chunk_rel = (uint32_t)bc_off_in_chunk + s->sections[k].code_offset;
            uint32_t bytes[1] = { chunk_rel };
            memcpy(buf + sec_tab_at + k * 4, bytes, 4);
        }
        free(sec_tab_slots);

        /* Optional editor debug section: source path + section name table +
           variable name table. Emit it whenever author-facing names exist;
           otherwise a freshly compiled script would reload with synthetic
           Section_N labels even though the source named its sections. */
        size_t str_section_at = 0;
        size_t debug_info_at  = 0;
        bool   emit_debug     = (s->source_path[0] != 0)
                             || (s->section_count > 0)
                             || (s->variable_count > 0);
        if (emit_debug) {
            /* debug_info_offset is editor-only and the runtime never reads
               it. The original engine relocates it but we set it to point
               just before the string section (matches BHD layout). */
            debug_info_at = used;
            /* The original keeps a small debug-info area of zeros; we emit
               nothing here (debug_info_count=0). */
            str_section_at = used;
            const uint32_t SRC_PATH_BYTES = 256;
            buf_append_zero(&buf, &cap, &used, SRC_PATH_BYTES);
            /* Copy source path into the slot (the parser tolerates leading
               zeros, but we just write the path at offset 0). */
            size_t plen = strlen(s->source_path);
            if (plen >= SRC_PATH_BYTES) plen = SRC_PATH_BYTES - 1;
            memcpy(buf + str_section_at, s->source_path, plen);

            /* Header (0x50 bytes): entry_size at +0, section_count at +8,
               var_count at +0x10. */
            uint8_t hdr[0x50];
            memset(hdr, 0, sizeof(hdr));
            uint32_t entry_size = 48;
            uint32_t sc = s->section_count;
            uint32_t vc = s->variable_count;
            memcpy(hdr + 0x00, &entry_size, 4);
            memcpy(hdr + 0x08, &sc, 4);
            memcpy(hdr + 0x10, &vc, 4);
            buf_append(&buf, &cap, &used, hdr, sizeof(hdr));
            /* Section entries: 4-byte code_offset (chunk-relative) at +0,
               32-byte name at +0x10. */
            for (uint32_t k = 0; k < s->section_count; ++k) {
                uint8_t entry[48];
                memset(entry, 0, sizeof(entry));
                uint32_t off = (uint32_t)bc_off_in_chunk
                             + s->sections[k].code_offset;
                memcpy(entry + 0x00, &off, 4);
                size_t nlen = strnlen(s->sections[k].name, 32);
                memcpy(entry + 0x10, s->sections[k].name, nlen);
                buf_append(&buf, &cap, &used, entry, sizeof(entry));
            }
            /* Variable entries. */
            for (uint32_t k = 0; k < s->variable_count; ++k) {
                uint8_t entry[48];
                memset(entry, 0, sizeof(entry));
                memcpy(entry + 0x00, &s->variables[k].byte_offset, 4);
                size_t nlen = strnlen(s->variables[k].name,
                                      MUS_INTRINSIC_NAME_SIZE);
                if (nlen > 31) nlen = 31;
                memcpy(entry + 0x10, s->variables[k].name, nlen);
                buf_append(&buf, &cap, &used, entry, sizeof(entry));
            }
        }

        /* Now backpatch the chunk header. */
        MusChunkHeader ch;
        memset(&ch, 0, sizeof(ch));
        ch.tag                  = MUS_CHUNK_TAG_MU01;
        ch.version              = 0x00000100;
        size_t nlen = strnlen(s->name, MUS_NAME_SIZE);
        memcpy(ch.name, s->name, nlen);
        ch.globals_size         = s->globals_size ? s->globals_size : 64;
        ch.locals_size          = s->locals_size;
        ch.bytecode_offset      = (uint32_t)bc_off_in_chunk;
        ch.section_table_offset = (uint32_t)sec_tab_off_in_chunk;
        ch.section_count        = s->section_count;
        ch.entry_section_index  = s->entry_section_index;
        if (emit_debug) {
            ch.debug_info_offset    = (uint32_t)(debug_info_at - chunk_hdr_at);
            ch.debug_info_count     = 0;
            ch.string_section_offset= (uint32_t)(str_section_at - chunk_hdr_at);
            /* +0x3C doubles as the `enter` frame base (instance[+0x3C]); preserve
               a parsed value, default 0x20 (the JO/MDEdit witness). */
            ch.string_section_size  = s->locals_frame_offset ? s->locals_frame_offset : 0x20;
        } else {
            /* No debug section: still set debug_info_offset to the end of
               the bytecode region so the parser's smallest_after() heuristic
               can bound the bytecode. The parser only walks debug data when
               debug_info_count > 0, which we keep at zero. */
            ch.debug_info_offset    = (uint32_t)(used - chunk_hdr_at);
            ch.debug_info_count     = 0;
        }
        memcpy(buf + chunk_hdr_at, &ch, sizeof(ch));
    }

    /* File-level intrinsic-name resolve table + strings blob. The table is
       11 uint32 zero-filled slots populated at load by the engine; we just
       reserve the space. The strings blob holds the canonical 11 names as
       Pascal-style length-prefixed strings (length includes itself). */
    size_t resolve_at = used;
    buf_append_zero(&buf, &cap, &used, MUS_INTRINSIC_NAMES * 4);
    size_t strings_at = used;
    for (int i = 0; i < MUS_INTRINSIC_NAMES; ++i) {
        const char *name = kIntrinsicNames[i];
        size_t nlen = strlen(name);
        uint8_t L = (uint8_t)(nlen + 2);   /* 1-byte length + name + NUL */
        if (L < 2) L = 2;
        buf_append(&buf, &cap, &used, &L, 1);
        buf_append(&buf, &cap, &used, name, nlen);
        uint8_t z = 0;
        buf_append(&buf, &cap, &used, &z, 1);
    }

    /* Patch the file header. */
    MusFileHeader h;
    memset(&h, 0, sizeof(h));
    h.magic                     = MUS_MAGIC_SCR0;
    h.version                   = 0x00000100;
    h.chunk_count               = script_count;
    h.chunk_table_offset        = (uint32_t)chunk_table_offset;
    h.name_count                = MUS_INTRINSIC_NAMES;
    h.name_strings_blob_offset  = (uint32_t)strings_at;
    h.name_resolve_table_offset = (uint32_t)resolve_at;
    memcpy(buf, &h, sizeof(h));

    /* Patch the chunk pointer table. */
    for (uint32_t i = 0; i < script_count; ++i) {
        memcpy(buf + chunk_table_offset + i * 4, &chunk_offs[i], 4);
    }
    free(chunk_offs);

    *out_buf = buf;
    *out_size = used;
    return 0;
}
