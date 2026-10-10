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

   The text form is the tool's own authoring syntax (the game reads only the binary). The bytes it writes are
   MDEdit's layout as inferred from the bytes of the three distinct shipped scripts (gamemus.bin, menumus.bin and
   jox01's MJox01.bin; its GJox01.bin is gamemus.bin byte for byte), not witnessed in MDEdit itself (no MDEdit
   binary is at hand), so what the samples lack (a second function, a function of no parameter, the line table's
   places for constructs none of them holds) is extrapolated: a nop at the code's start (the bytecode's first byte, where the chunk's main entry and, with no
   MessageHandler function, its +0x40 handler pointer both point), the code padded with nops to four bytes, and
   the editor debug section MDEdit writes beside it (the source path, the sections, the globals Var00..Var15 and
   the user's, the functions and their parameters, and a line table: each statement's first instruction, an
   `else`'s jump, a section's closing `done` and a function's frame setup with the line of the text that wrote
   it). A shipped script decompiled and compiled again is its own bytes; the decompiler puts each statement on
   the line the table names (blank lines before it, or a `#line N` directive where the text runs past).

   Grammar (informal; matches what the decompiler emits):

       script_file := comment* 'script' IDENT bind_decl* global_decl*
                       declsection_decl* (section_block | handler_block)*
       bind_decl       := 'bind' IDENT STRING                  // aesthetic only
       global_decl     := 'global' IDENT IDENT                 // a user global: the next 4 bytes from 64
       declsection_decl:= 'declsection' IDENT                  // the section table's order
       section_block   := 'section' IDENT '{' stmt* '}' stmt*
       handler_block   := 'handler' IDENT '(' [IDENT {',' IDENT}] ')' '{' stmt* '}'
                          // a function: `enter N` (0x38, its N parameters banked at the frame base 0x20 + 4k),
                          // then its body, no `done`; one named MessageHandler is the chunk's +0x40 entry
       '#line' N           // the next line is line N (the line table's numbering)

       stmt := 'play' IDENT
             | 'enter' IDENT
             | 'goto' IDENT
             | 'return' | 'yield' | 'nop'
             | 'frame' NUMBER                                    // 0x38 outside a function's start
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

#include <formats/mus/mus.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace opennova::mus {

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
    KwHandler,
    KwFrame,
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
    int         cur_line = 1;   /* the line the current token is on (the line table's numbering) */
    int         phys_line = 1;  /* the text's own line, which a `#line` does not move: an error's line */

    void advance();
};

/* Skip whitespace and `// ...` line comments, and take a `#line N` directive: the line after it is line N
   (the line table's numbering, which the decompiler writes where its text runs past a table's line). Returns
   true if at EOF. */
static bool skip_ws(Lexer *L) {
    while (L->pos < L->len) {
        char c = L->src[L->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++L->pos; ++L->col;
        } else if (c == '\n') {
            ++L->pos; ++L->line; ++L->phys_line; L->col = 1;
        } else if (c == '#' && L->len - L->pos >= 5 && strncmp(L->src + L->pos, "#line", 5) == 0) {
            size_t q = L->pos + 5;
            while (q < L->len && (L->src[q] == ' ' || L->src[q] == '\t')) ++q;
            int n = 0;
            bool digits = false;
            while (q < L->len && L->src[q] >= '0' && L->src[q] <= '9') {
                if (n < 100000000) n = n * 10 + (L->src[q] - '0');
                digits = true;
                ++q;
            }
            while (L->pos < L->len && L->src[L->pos] != '\n') { ++L->pos; ++L->col; }
            if (L->pos < L->len) {
                ++L->pos;
                L->line = digits ? n : L->line + 1;
                ++L->phys_line;
                L->col = 1;
            }
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
        cur_line = line;
        return;
    }
    cur_line = line;
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
        else if (strcmp(cur_text, "handler")     == 0) cur_kind = Tok::KwHandler;
        else if (strcmp(cur_text, "frame")       == 0) cur_kind = Tok::KwFrame;
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
    bool     defined;           /* a `section NAME { ... }` block wrote it (an `enter` only names it) */
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

/* A number in a variable's name stops accumulating past this (no int overflow; any
   such number is past every one-byte operand, which the callers refuse). */
static constexpr int kNameNumberMax = 0xFFFF;

/* Var00..Var15 -> byte offsets 0..60. Returns -1 if not a Var-prefixed name. */
static int parse_var_offset(const char *name) {
    if (strncmp(name, "Var", 3) == 0 && name[3] != 0) {
        const char *p = name + 3;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            if (n <= kNameNumberMax) n = n * 10 + (*p - '0');
            ++p;
        }
        return n * 4;   /* MDEdit pre-defined slot (no operand reaches past a byte) */
    }
    return -1;
}

static int parse_g_offset(const char *name) {
    if (strncmp(name, "g_", 2) == 0 && name[2] != 0) {
        const char *p = name + 2;
        int n = 0;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            if (n <= kNameNumberMax) n = n * 10 + (*p - '0');
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
            if (n <= kNameNumberMax) n = n * 10 + (*p - '0');
            ++p;
        }
        return n;
    }
    return -1;
}

/* No play index past a word: the widest play reads 16 bits [orig: AudioVM_Op_PlayWait
   @ 0x672C90]. */
static constexpr int kSoundIndexMax = 0xFFFF;
static constexpr int kSoundIndexOutOfRange = -2;

/* Parse 'sound_<N>' -> N. Returns -1 on miss, kSoundIndexOutOfRange past
   kSoundIndexMax (it stops accumulating there, so no number wraps to a valid index). */
static int parse_sound_index(const char *name) {
    if (strncmp(name, "sound_", 6) == 0 && name[6] != 0) {
        const char *p = name + 6;
        int n = 0;
        bool past = false;
        while (*p) {
            if (*p < '0' || *p > '9') return -1;
            if (!past) {
                n = n * 10 + (*p - '0');
                past = n > kSoundIndexMax;
            }
            ++p;
        }
        return past ? kSoundIndexOutOfRange : n;
    }
    return -1;
}

/* ---- Compiler ---- */

/* How deep ifs may nest (the decompiler's bound, mus_decompile.cpp) and how deep an
   expression may: parse_stmt and parse_expr recurse, so a text past these would
   otherwise run the stack out. */
static constexpr int kMaxNesting = 64;
static constexpr int kMaxExpressionNesting = 256;

/* One level of a recursion counted while it runs. */
struct Depth {
    int &count;
    explicit Depth(int &c) : count(c) { ++count; }
    ~Depth() { --count; }
    Depth(const Depth &) = delete;
    Depth &operator=(const Depth &) = delete;
};

/* The most targets an `on (...)` table holds: its count is one byte and the
   compiler's own bound (the skip byte bounds it further). */
static constexpr int kMaxTableTargets = 64;

struct Compiler {
    Lexer        lex;
    MusScript    out;
    Emit         emit;

    int          if_nesting = 0;
    int          expr_nesting = 0;
    /* An `on` table's targets as parsed, kept here rather than in parse_stmt's
       frame, which every nested if repeats (an `on` statement nests nothing). */
    char         on_targets[kMaxTableTargets][MUS_SECTION_NAME_SIZE];
    int          on_play_index[kMaxTableTargets];

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

    /* MDEdit's debug tables the text makes (mus.h): the functions, their parameters, the line table. */
    MusFunction *functions = NULL;
    size_t       function_count = 0, function_cap = 0;
    MusLocal    *locals = NULL;
    size_t       local_count = 0, local_cap = 0;
    MusLine     *lines = NULL;
    size_t       line_count = 0, line_cap = 0;
    /* The function whose body is being compiled (-1 outside one): its parameters are its locals. */
    int          current_function = -1;
    size_t       current_locals_from = 0;

    Compiler() : sections(NULL), section_cap(0), section_count(0),
                 variables(NULL), variable_cap(0), variable_count(0),
                 binds(NULL), bind_cap(0), bind_count(0) {}

    /* The line table: an instruction's offset and the line that wrote it, once per offset (the table rises in
       both). */
    void mark_line(uint32_t offset, int line) {
        if (line_count > 0 && lines[line_count - 1].code_offset >= offset) return;
        if (line_count >= line_cap) {
            line_cap = line_cap ? line_cap * 2 : 64;
            lines = (MusLine *)realloc(lines, line_cap * sizeof(MusLine));
        }
        lines[line_count].code_offset = offset;
        lines[line_count].line = (uint32_t)(line > 0 ? line : 0);
        ++line_count;
    }

    /* Whether a function of the name is defined already. */
    bool function_named(const char *name) const {
        for (size_t i = 0; i < function_count; ++i)
            if (strcmp(functions[i].name, name) == 0) return true;
        return false;
    }

    /* A section's name is held in 32 bytes, its NUL among them (mus.h MusSection, the debug table's
       section names): a longer one is refused, never cut (two names agreeing in their first 31
       characters would be one section, and a cut `enter` target would name none). */
    static bool section_name_fits(const char *name, const char **err) {
        if (strlen(name) < (size_t)MUS_SECTION_NAME_SIZE) return true;
        *err = "a section's name past 31 characters, which the debug table cannot hold";
        return false;
    }

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
        if (idx >= 0 || idx == kSoundIndexOutOfRange) return idx;
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

    /* A global's byte offset as an operand: one byte [orig: AudioVM_Op_PushGlobal
       @ 0x6727B0 `movzx` of one byte], so an offset past 255 is refused rather than
       wrapped to another global. -1 with `err` set. */
    /* A local's index as an operand: one byte, as a global's [orig:
       AudioVM_Op_PushLocal @ 0x6727D0]. A parameter of the function being compiled is its frame offset; else
       `l_N`. -1 when `name` is no local; -2 with `err` set when it is one past 255. */
    int local_operand(const char *name, const char **err) {
        if (current_function >= 0) {
            const char *own = functions[current_function].name;
            const size_t own_len = strlen(own);
            for (size_t i = current_locals_from; i < local_count; ++i) {
                const char *full = locals[i].name;
                if (strncmp(full, own, own_len) == 0 && full[own_len] == ':' && full[own_len + 1] == ':' &&
                    strcmp(full + own_len + 2, name) == 0)
                    return (int)locals[i].frame_offset;
            }
        }
        const int l = parse_l_index(name);
        if (l > 255) {
            *err = "local variable out of range (indices 0..255)";
            return -2;
        }
        return l;
    }

    int global_operand(const char *name, const char **err) {
        const int g = resolve_global_offset(name);
        if (g < 0 || g > 255) {
            *err = "global variable out of range (byte offsets 0..255)";
            return -1;
        }
        return g;
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
    int parse_handler(const char **err);
    int parse_stmt(const char **err);
    int parse_stmt_at(const char **err);

    int finalize(const char **err);
    int parse_script(const char **err);
};

int Compiler::parse_expr(const char **err) {
    /* An expression recurses per operator and parenthesis: bounded as the ifs are. */
    Depth depth(expr_nesting);
    if (expr_nesting > kMaxExpressionNesting) {
        *err = "expression nested too deep";
        return -1;
    }
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
        int li = local_operand(name, err);
        if (li == -2) return -1;
        if (li >= 0) {
            emit_push_l(li);
            return 0;
        }
        int gi = global_operand(name, err);
        if (gi < 0) return -1;
        emit_push_g(gi);
        return 0;
    }

    *err = "unexpected token in expression";
    return -1;
}

/* A statement, its first instruction marked in the line table with the line of its first token. */
int Compiler::parse_stmt(const char **err) {
    mark_line((uint32_t)emit.used, lex.cur_line);
    return parse_stmt_at(err);
}

int Compiler::parse_stmt_at(const char **err) {
    /* frame N: the frame setup outside a function's start [orig: AudioVM_Op_Enter @ 0x672C20]. */
    if (lex.cur_kind == Tok::KwFrame) {
        lex.advance();
        if (lex.cur_kind != Tok::Number || lex.cur_int > 255) {
            *err = "expected an argument count (1..255) after 'frame'";
            return -1;
        }
        /* `frame 0`: the game moves a word all the same (D-MUS-16), refused as a function of none is. */
        if (lex.cur_int == 0) {
            *err = "a frame setup of no argument, which the game does not run as written (D-MUS-16)";
            return -1;
        }
        emit.byte((uint8_t)MUS_OP_ENTER);
        emit.byte((uint8_t)lex.cur_int);
        lex.advance();
        return 0;
    }
    /* play <sound_N | bound-name | "bound name"> */
    if (lex.cur_kind == Tok::KwPlay) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident && lex.cur_kind != Tok::String) {
            *err = "expected sound name after 'play'";
            return -1;
        }
        int idx = resolve_play_target(lex.cur_text);
        /* The engine has two play opcodes that differ only in the index's width:
           0x3E reads a byte, 0x3D a 16-bit word, and both start the sound
           [orig: AudioVM_Op_Play @ 0x672CB0 `movzx eax, byte ptr [esi]`,
           AudioVM_Op_PlayWait @ 0x672C90 `movzx eax, word ptr [esi]`]. Retail's
           compiler writes the byte form up to 255 and the word form past it
           (jox01's MJox01.bin: 823 plays of 0..255, 61 wide plays of 256..316, no
           wide play below 256), so this does too; past a word is no index. */
        if (idx == kSoundIndexOutOfRange || idx > kSoundIndexMax) {
            *err = "play target index out of range (max 65535)";
            return -1;
        }
        if (idx < 0) {
            *err = "unknown play target (expected 'sound_N' or a bound name)";
            return -1;
        }
        if (idx > 255) {
            emit.byte((uint8_t)MUS_OP_PLAYW);
            emit.byte((uint8_t)(idx & 0xFF));
            emit.byte((uint8_t)(idx >> 8));
        } else {
            emit.byte((uint8_t)MUS_OP_PLAY);
            emit.byte((uint8_t)idx);
        }
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
        if (!section_name_fits(lex.cur_text, err)) return -1;
        int sidx = section_find_or_create(lex.cur_text);
        emit.byte((uint8_t)MUS_OP_SETSTATE);  /* decompiles same as 0x38 enter */
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
        if (!section_name_fits(lex.cur_text, err)) return -1;
        char name[MUS_SECTION_NAME_SIZE];
        strncpy(name, lex.cur_text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        emit.byte((uint8_t)MUS_OP_GOTO);
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
        if (!section_name_fits(lex.cur_text, err)) return -1;
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
        emit.byte((uint8_t)MUS_OP_DONE);
        return 0;
    }

    /* if (expr) { body } [else { body }] */
    if (lex.cur_kind == Tok::KwIf) {
        /* parse_stmt recurses per nested if: past kMaxNesting the text is refused
           (the decompiler's bound too, mus_decompile.cpp), so no text overflows the
           stack. */
        Depth depth(if_nesting);
        if (if_nesting > kMaxNesting) {
            *err = "if statements nested too deep (max 64)";
            return -1;
        }
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
            /* The jump past the else is the else's line. */
            mark_line((uint32_t)emit.used, lex.cur_line);
            lex.advance();
            if (lex.cur_kind != Tok::LBrace) {
                *err = "expected '{' after else";
                return -1;
            }
            lex.advance();
            /* End-of-if jump */
            emit.byte((uint8_t)MUS_OP_GOTO);
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
        if      (lex.cur_kind == Tok::KwEnter) inner_op = (uint8_t)MUS_OP_SETSTATE;
        else if (lex.cur_kind == Tok::KwPlay)  inner_op = (uint8_t)MUS_OP_PLAY;
        else if (lex.cur_kind == Tok::KwGoto)  { inner_op = (uint8_t)MUS_OP_GOTO; entry_size = 5; }
        else { *err = "expected enter/play/goto after 'on (...)'"; return -1; }
        lex.advance();
        /* Collect target identifiers. Names-aware decompiles can emit a play
           target as a quoted string when the SBF entry name isn't a bare
           identifier, so accept String here too (cur_text holds the unquoted
           bytes). enter/goto targets are always bare section idents. */
        char (&targets)[kMaxTableTargets][MUS_SECTION_NAME_SIZE] = on_targets;
        int (&play_index)[kMaxTableTargets] = on_play_index;
        int  ntargets = 0;
        while ((lex.cur_kind == Tok::Ident || lex.cur_kind == Tok::String)
               && ntargets < kMaxTableTargets) {
            if (inner_op != (uint8_t)MUS_OP_PLAY && !section_name_fits(lex.cur_text, err)) return -1;
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
        /* `null` is the entry whose opcode byte is 0, which the VM takes as "skip the
           table" [orig: AudioVM_Op_TableExec @ 0x672BE5..0x672BFA]; the decompiler
           prints such an entry as `null`, so it compiles back to zeros (a section
           named `null` cannot be a table's target). */
        auto is_null = [&](int t) { return strcmp(targets[t], "null") == 0; };
        /* A play table holding an index past a byte takes the word-wide play in every
           entry: the table executes its entry through the opcode table, so the entry
           [0x3D lo hi] reads its index as the plain statement does [orig:
           AudioVM_Op_TableExec @ 0x672BFB..0x672C03 dispatching the entry's opcode;
           AudioVM_Op_PlayWait @ 0x672C90]. The uniform word-wide table is OpenNova's
           own encoding: valid for the VM, but no retail program has a play table, so
           MDEdit's is unwitnessed (a mixed one it might write reads back the same,
           the decompiler reading each entry by its own opcode). */
        if (inner_op == MUS_OP_PLAY) {
            for (int t = 0; t < ntargets; ++t) {
                if (is_null(t)) { play_index[t] = 0; continue; }
                play_index[t] = resolve_play_target(targets[t]);
                if (play_index[t] == kSoundIndexOutOfRange || play_index[t] > kSoundIndexMax) {
                    *err = "play target index out of range in on(...) table (max 65535)";
                    return -1;
                }
                if (play_index[t] < 0) {
                    *err = "unknown play target in on(...) play table";
                    return -1;
                }
                if (play_index[t] > 255) { inner_op = (uint8_t)MUS_OP_PLAYW; entry_size = 3; }
            }
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
        emit.byte((uint8_t)MUS_OP_TABLEXEC);
        emit.byte((uint8_t)ntargets);
        emit.byte(inner_op);
        emit.byte((uint8_t)entry_size);
        emit.byte((uint8_t)total_size);
        for (int t = 0; t < ntargets; ++t) {
            if (is_null(t)) {
                for (int b = 0; b < entry_size; ++b) emit.byte(0);
                continue;
            }
            emit.byte(inner_op);
            if (inner_op == MUS_OP_PLAYW) {
                /* wide play: entry[1..2] = sound idx (16-bit LE) */
                emit.byte((uint8_t)(play_index[t] & 0xFF));
                emit.byte((uint8_t)(play_index[t] >> 8));
            } else if (entry_size == 2) {
                /* enter or play: entry[1] = section/sound idx (1 byte) */
                if (inner_op == MUS_OP_SETSTATE) {
                    int sidx = section_find_or_create(targets[t]);
                    if (sidx > 255) {
                        *err = "too many sections to index in on(...) table";
                        return -1;
                    }
                    emit.byte((uint8_t)sidx);
                } else {
                    /* play: target is sound_N or a bound name */
                    emit.byte((uint8_t)play_index[t]);
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
            int li = local_operand(name, err);
            if (li == -2) return -1;
            if (li >= 0) emit_inc_l(li);
            else {
                int g = global_operand(name, err);
                if (g < 0) return -1;
                emit_inc_g(g);
            }
            lex.advance();
            return 0;
        }
        if (lex.cur_kind == Tok::MinusMinus) {
            int li = local_operand(name, err);
            if (li == -2) return -1;
            if (li >= 0) emit_dec_l(li);
            else {
                int g = global_operand(name, err);
                if (g < 0) return -1;
                emit_dec_g(g);
            }
            lex.advance();
            return 0;
        }
        if (lex.cur_kind == Tok::Equals) {
            lex.advance();
            int rc = parse_expr(err);
            if (rc != 0) return rc;
            int li = local_operand(name, err);
            if (li == -2) return -1;
            if (li >= 0) {
                emit_pop_l(li);
            } else {
                int g = global_operand(name, err);
                if (g < 0) return -1;
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
            /* '}' closes a section: emit done (its line the brace's) and consume. */
            mark_line((uint32_t)emit.used, lex.cur_line);
            emit.byte((uint8_t)MUS_OP_DONE);
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

/* `handler NAME(p, ...) { body }`: a function. Its frame setup `enter N` (0x38) banks its N arguments at the
   frame base + 4k [orig: AudioVM_Op_Enter @ 0x672C20, the base instance[+0x3C]], each parameter a local of that
   frame offset (`NAME::p` in the debug table); its body follows, closed by nothing (MDEdit's MessageHandler ends
   on its table's last entry: gamemus.bin 0x09..0x1B). The frame setup is the header's line. */
int Compiler::parse_handler(const char **err) {
    const int header_line = lex.cur_line;
    lex.advance();
    if (lex.cur_kind != Tok::Ident) {
        *err = "expected a function name after 'handler'";
        return -1;
    }
    /* The debug table holds a name in 32 bytes, its NUL among them (mus.h MusFunction): a longer one is refused,
       never cut (a cut `NAME::param` would match no parameter, which then compiles as a global). */
    if (strlen(lex.cur_text) >= (size_t)MUS_SECTION_NAME_SIZE) {
        *err = "a function's name past 31 characters, which the debug table cannot hold";
        return -1;
    }
    if (function_named(lex.cur_text)) {
        *err = "a function of this name is defined already";
        return -1;
    }
    for (size_t i = 0; i < section_count; ++i) {
        if (strcmp(sections[i].name, lex.cur_text) == 0) {
            *err = "a section has this name: a section and a function may not share one";
            return -1;
        }
    }
    if (function_count >= function_cap) {
        function_cap = function_cap ? function_cap * 2 : 4;
        functions = (MusFunction *)realloc(functions, function_cap * sizeof(MusFunction));
    }
    MusFunction &fn = functions[function_count];
    memset(&fn, 0, sizeof(fn));
    strncpy(fn.name, lex.cur_text, sizeof(fn.name) - 1);
    fn.start = (uint32_t)emit.used;
    current_function = (int)function_count++;
    current_locals_from = local_count;
    lex.advance();
    if (lex.cur_kind != Tok::LParen) {
        *err = "expected '(' after the function's name";
        return -1;
    }
    lex.advance();
    int params = 0;
    while (lex.cur_kind == Tok::Ident) {
        if (local_count >= local_cap) {
            local_cap = local_cap ? local_cap * 2 : 4;
            locals = (MusLocal *)realloc(locals, local_cap * sizeof(MusLocal));
        }
        if (strlen(functions[current_function].name) + 2 + strlen(lex.cur_text) >= (size_t)MUS_SECTION_NAME_SIZE) {
            *err = "a parameter whose debug name (Function::name) runs past 31 characters, which the table cannot hold";
            return -1;
        }
        for (size_t i = current_locals_from; i < local_count; ++i) {
            const char *full = locals[i].name;
            const size_t own = strlen(functions[current_function].name);
            if (strcmp(full + own + 2, lex.cur_text) == 0) {
                *err = "a parameter of this name is in the list already";
                return -1;
            }
        }
        MusLocal &local = locals[local_count++];
        memset(&local, 0, sizeof(local));
        snprintf(local.name, sizeof(local.name), "%s::%s", functions[current_function].name, lex.cur_text);
        local.frame_offset = (uint32_t)(MUS_DEFAULT_LOCALS_BASE + 4 * params);
        ++params;
        lex.advance();
        if (lex.cur_kind == Tok::Comma) lex.advance();
    }
    if (lex.cur_kind != Tok::RParen || params > 255) {
        *err = "expected ')' to close the function's parameters";
        return -1;
    }
    /* A frame setup of no argument: the game's copy loop still moves one word, into the word below the frame
       base [orig: AudioVM_Op_Enter @ 0x672C3C..0x672C47], and MDEdit's form for a function of no parameter is
       unwitnessed: refused (D-MUS-16). */
    if (params == 0) {
        *err = "a function of no parameter, which the game's frame setup does not run as written (D-MUS-16)";
        return -1;
    }
    lex.advance();
    if (lex.cur_kind != Tok::LBrace) {
        *err = "expected '{' to open the function's body";
        return -1;
    }
    lex.advance();
    mark_line((uint32_t)emit.used, header_line);
    emit.byte((uint8_t)MUS_OP_ENTER);
    emit.byte((uint8_t)params);
    while (lex.cur_kind != Tok::RBrace && lex.cur_kind != Tok::Eof) {
        int rc = parse_stmt(err);
        if (rc != 0) return rc;
    }
    if (lex.cur_kind != Tok::RBrace) {
        *err = "expected '}' to close the function's body";
        return -1;
    }
    lex.advance();
    functions[current_function].end = (uint32_t)emit.used;
    current_function = -1;
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
        if (!section_name_fits(lex.cur_text, err)) return -1;
        section_find_or_create(lex.cur_text);
        lex.advance();
        return 0;
    }
    if (lex.cur_kind == Tok::KwHandler) return parse_handler(err);
    /* `section NAME { body }` */
    if (lex.cur_kind == Tok::KwSection) {
        lex.advance();
        if (lex.cur_kind != Tok::Ident) {
            *err = "expected section name";
            return -1;
        }
        if (function_named(lex.cur_text)) {
            *err = "a function has this name: a section and a function may not share one";
            return -1;
        }
        if (!section_name_fits(lex.cur_text, err)) return -1;
        int sidx = section_find_or_create(lex.cur_text);
        sections[sidx].code_offset = (uint32_t)emit.used;
        sections[sidx].defined = true;
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
    /* Every section a statement names is one a `section` block defines: one interned by an `enter` alone would
       sit at the code's start (its leading nop), and its decompile would not compile. */
    for (size_t s = 0; s < section_count; ++s) {
        if (!sections[s].defined) {
            *err = "a section an 'enter' or a 'declsection' names is defined nowhere";
            return -1;
        }
    }
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

    /* MDEdit pads the code with nops to four bytes (gamemus.bin 129 -> 132, menumus.bin 3122 -> 3124). */
    while (emit.used % 4 != 0) emit.byte(0x00);

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

    /* The globals as MDEdit lists them: its sixteen Var00..Var15 at 0..60, then the user's. */
    out.variables = (MusVariable *)calloc(16 + variable_count, sizeof(MusVariable));
    if (!out.variables) { *err = "OOM"; return -1; }
    out.variable_count = (uint32_t)(16 + variable_count);
    for (uint32_t k = 0; k < 16; ++k) {
        snprintf(out.variables[k].name, MUS_INTRINSIC_NAME_SIZE, "Var%02u", k);
        out.variables[k].byte_offset = 4 * k;
    }
    if (variable_count > 0) memcpy(out.variables + 16, variables, variable_count * sizeof(MusVariable));
    if (local_count > 0) {
        out.locals = (MusLocal *)calloc(local_count, sizeof(MusLocal));
        if (!out.locals) { *err = "OOM"; return -1; }
        memcpy(out.locals, locals, local_count * sizeof(MusLocal));
        out.local_count = (uint32_t)local_count;
    }
    if (function_count > 0) {
        out.functions = (MusFunction *)calloc(function_count, sizeof(MusFunction));
        if (!out.functions) { *err = "OOM"; return -1; }
        memcpy(out.functions, functions, function_count * sizeof(MusFunction));
        out.function_count = (uint32_t)function_count;
    }
    if (line_count > 0) {
        out.lines = (MusLine *)calloc(line_count, sizeof(MusLine));
        if (!out.lines) { *err = "OOM"; return -1; }
        memcpy(out.lines, lines, line_count * sizeof(MusLine));
        out.line_count = (uint32_t)line_count;
    }

    /* MDEdit pre-defines globals area = 64 bytes (Var00..Var15); user globals
       grow it. */
    uint32_t gsize = 64;
    for (size_t i = 0; i < variable_count; ++i) {
        uint32_t end = variables[i].byte_offset + 4;
        if (end > gsize) gsize = end;
    }
    out.globals_size = gsize;
    /* The locals area: the frame base and the most parameters a function banks (gamemus.bin 0x28: 0x20 and
       MessageHandler's two), none with no function (menumus.bin 0). */
    uint32_t lsize = 0;
    for (size_t i = 0; i < local_count; ++i) {
        const uint32_t end = locals[i].frame_offset + 4;
        if (end > lsize) lsize = end;
    }
    out.locals_size = lsize;
    out.locals_frame_offset = MUS_DEFAULT_LOCALS_BASE;   /* `enter` frame base; JO/MDEdit witness */
    out.entry_section_index = 0;
    /* The +0x40 MessageHandler entry: the function of that name, else the code's start, its leading nop
       (gamemus.bin 0x09, menumus.bin 0) [orig: sub_672E50 @ 0x672eba jumps there]. */
    out.message_handler_offset = 0;
    out.has_message_handler = 1;
    for (size_t i = 0; i < function_count; ++i)
        if (strcmp(functions[i].name, "MessageHandler") == 0) {
            out.message_handler_offset = functions[i].start;
            break;
        }

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

    /* MDEdit's leading nop: the code's start, the chunk's main entry. */
    emit.byte(0x00);
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

int mus_compile(const char *text, MusScript *out_script,
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
    free(c.functions);
    free(c.locals);
    free(c.lines);
    if (rc != 0) {
        if (err_msg)  *err_msg  = local_err ? local_err : "compile error";
        if (err_line) *err_line = c.lex.phys_line;
        if (err_col)  *err_col  = c.lex.col;
        mus_script_free(&c.out);
        return rc;
    }
    *out_script = c.out;
    return 0;
}

void mus_script_free(MusScript *s) {
    if (!s) return;
    free(s->code);
    free(s->sections);
    free(s->variables);
    free(s->locals);
    free(s->functions);
    free(s->lines);
    s->code = NULL;
    s->sections = NULL;
    s->variables = NULL;
    s->locals = NULL;
    s->functions = NULL;
    s->lines = NULL;
    s->code_size = 0;
    s->section_count = 0;
    s->variable_count = 0;
    s->local_count = 0;
    s->function_count = 0;
    s->line_count = 0;
}

void mus_free(void *p) { free(p); }

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

/* Zeros to the next multiple of 16 of the file offset (MDEdit's string section and the name resolve table). */
static void buf_align16(uint8_t **buf, size_t *cap, size_t *used) {
    buf_append_zero(buf, cap, used, (16 - *used % 16) % 16);
}

static void put32(uint8_t *at, uint32_t v) { memcpy(at, &v, 4); }

/* One 48-byte debug entry: a value at +0 (and +8), a 32-byte name at +0x10. */
static void debug_entry(uint8_t **buf, size_t *cap, size_t *used, uint32_t first, uint32_t third, const char *name) {
    uint8_t entry[48];
    memset(entry, 0, sizeof(entry));
    put32(entry + 0x00, first);
    put32(entry + 0x08, third);
    const size_t nlen = strnlen(name, 31);
    memcpy(entry + 0x10, name, nlen);
    buf_append(buf, cap, used, entry, sizeof(entry));
}

/* The SCR0 file in MDEdit's layout, as read from the shipped scripts' bytes (above): the 44-byte header and the chunk pointer
   table; each MU01 chunk's 0x48-byte header and 0x20 bytes of zeros, its section table (chunk-relative entry PCs)
   and its code; zeros to 16, then the editor debug section: the 256-byte source path, a 0x50-byte header of the
   five tables' entry size, offsets and counts, the sections' (bytecode-relative entries), the globals', the
   parameters' and the functions' 48-byte entries and the 8-byte lines; zeros to 16, the intrinsic names'
   resolve table (zeros the loader fills) and their length-prefixed strings. The runtime reads the header, the
   section table, the code and the names [orig: AudioVM_LoadScriptFile @ 0x672D20, AudioVM_FixupPointers @
   0x672470]; MDEdit's reader the rest. */
int mus_encode_file(const MusScript *const *scripts, uint32_t script_count,
                               uint8_t **out_buf, size_t *out_size) {
    if (!scripts || !out_buf || !out_size) return -1;

    uint8_t *buf = NULL;
    size_t   cap = 0, used = 0;

    buf_append_zero(&buf, &cap, &used, sizeof(MusFileHeader));
    size_t chunk_table_offset = used;
    buf_append_zero(&buf, &cap, &used, (size_t)script_count * 4);

    uint32_t *chunk_offs = (uint32_t *)calloc(script_count ? script_count : 1, sizeof(uint32_t));
    if (!chunk_offs) { free(buf); return -2; }

    for (uint32_t i = 0; i < script_count; ++i) {
        const MusScript *s = scripts[i];
        if (!s) { free(buf); free(chunk_offs); return -3; }

        chunk_offs[i] = (uint32_t)used;
        const size_t chunk_hdr_at = used;
        buf_append_zero(&buf, &cap, &used, sizeof(MusChunkHeader) + 0x20);

        const size_t sec_tab_at = used;
        buf_append_zero(&buf, &cap, &used, (size_t)s->section_count * 4);
        const size_t bc_at = used;
        const uint32_t bc_rel = (uint32_t)(bc_at - chunk_hdr_at);
        if (s->code && s->code_size > 0) buf_append(&buf, &cap, &used, s->code, s->code_size);
        for (uint32_t k = 0; k < s->section_count; ++k) put32(buf + sec_tab_at + 4 * k, bc_rel + s->sections[k].code_offset);
        const size_t debug_info_at = used;

        /* The editor debug section. */
        buf_align16(&buf, &cap, &used);
        const size_t str_at = used;
        buf_append_zero(&buf, &cap, &used, 256);
        size_t plen = strnlen(s->source_path, 255);
        memcpy(buf + str_at, s->source_path, plen);
        const size_t hdr_at = used;
        buf_append_zero(&buf, &cap, &used, 0x50);
        const auto rel = [&](size_t at) { return (uint32_t)(at - chunk_hdr_at); };
        const size_t sections_at = used;
        for (uint32_t k = 0; k < s->section_count; ++k)
            debug_entry(&buf, &cap, &used, s->sections[k].code_offset, 0, s->sections[k].name);
        const size_t vars_at = used;
        for (uint32_t k = 0; k < s->variable_count; ++k)
            debug_entry(&buf, &cap, &used, s->variables[k].byte_offset, 0, s->variables[k].name);
        const size_t locals_at = used;
        for (uint32_t k = 0; k < s->local_count; ++k)
            debug_entry(&buf, &cap, &used, s->locals[k].frame_offset, 0, s->locals[k].name);
        const size_t functions_at = used;
        for (uint32_t k = 0; k < s->function_count; ++k)
            debug_entry(&buf, &cap, &used, s->functions[k].start, s->functions[k].end, s->functions[k].name);
        const size_t lines_at = used;
        for (uint32_t k = 0; k < s->line_count; ++k) {
            uint8_t pair[8];
            put32(pair, s->lines[k].code_offset);
            put32(pair + 4, s->lines[k].line);
            buf_append(&buf, &cap, &used, pair, sizeof(pair));
        }
        uint8_t *h = buf + hdr_at;
        put32(h + 0x00, 48);
        put32(h + 0x04, rel(sections_at));
        put32(h + 0x08, s->section_count);
        put32(h + 0x0C, rel(vars_at));
        put32(h + 0x10, s->variable_count);
        put32(h + 0x14, rel(locals_at));
        put32(h + 0x18, s->local_count);
        put32(h + 0x1C, rel(functions_at));
        put32(h + 0x20, s->function_count);
        put32(h + 0x24, rel(lines_at));
        put32(h + 0x28, s->line_count);

        MusChunkHeader ch;
        memset(&ch, 0, sizeof(ch));
        ch.tag                  = MUS_CHUNK_TAG_MU01;
        ch.version              = 0x00000100;
        memcpy(ch.name, s->name, strnlen(s->name, MUS_NAME_SIZE));
        ch.globals_size         = s->globals_size ? s->globals_size : 64;
        ch.locals_size          = s->locals_size;
        ch.bytecode_offset      = bc_rel;
        ch.section_table_offset = rel(sec_tab_at);
        ch.section_count        = s->section_count;
        ch.entry_section_index  = s->entry_section_index;
        ch.debug_info_offset    = rel(debug_info_at);
        ch.debug_info_count     = 0;
        ch.string_section_offset = rel(str_at);
        /* +0x3C doubles as the `enter` frame base (instance[+0x3C]); 0x20 in every MDEdit file. */
        ch.string_section_size  = s->locals_frame_offset ? s->locals_frame_offset : MUS_DEFAULT_LOCALS_BASE;
        /* +0x40 the MessageHandler entry, +0x44 the code's start [orig: AudioVM_FixupPointers @ 0x672495 /
           @ 0x6724a1]. */
        ch.message_handler_offset = s->has_message_handler ? bc_rel + s->message_handler_offset : 0;
        ch.main_entry_offset    = bc_rel;
        memcpy(buf + chunk_hdr_at, &ch, sizeof(ch));
    }

    /* The intrinsic names' resolve table (11 slots the loader fills) and their strings, each its length (the
       byte, the name, its NUL) then the name. */
    buf_align16(&buf, &cap, &used);
    size_t resolve_at = used;
    buf_append_zero(&buf, &cap, &used, MUS_INTRINSIC_NAMES * 4);
    size_t strings_at = used;
    for (int i = 0; i < MUS_INTRINSIC_NAMES; ++i) {
        const char *name = kIntrinsicNames[i];
        size_t nlen = strlen(name);
        uint8_t L = (uint8_t)(nlen + 2);
        buf_append(&buf, &cap, &used, &L, 1);
        buf_append(&buf, &cap, &used, name, nlen);
        uint8_t z = 0;
        buf_append(&buf, &cap, &used, &z, 1);
    }

    MusFileHeader fh;
    memset(&fh, 0, sizeof(fh));
    fh.magic                     = MUS_MAGIC_SCR0;
    fh.version                   = 0x00000100;
    fh.chunk_count               = script_count;
    fh.chunk_table_offset        = (uint32_t)chunk_table_offset;
    fh.name_count                = MUS_INTRINSIC_NAMES;
    fh.name_strings_blob_offset  = (uint32_t)strings_at;
    fh.name_resolve_table_offset = (uint32_t)resolve_at;
    memcpy(buf, &fh, sizeof(fh));
    for (uint32_t i = 0; i < script_count; ++i) memcpy(buf + chunk_table_offset + i * 4, &chunk_offs[i], 4);
    free(chunk_offs);

    *out_buf = buf;
    *out_size = used;
    return 0;
}

} // namespace opennova::mus
