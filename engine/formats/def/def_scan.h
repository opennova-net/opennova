#pragma once

// Internal to engine/formats/def — not part of def/def.h. Split out of def.cpp (quality
// campaign W3-3).
//
// The shared .def text scanner: the retail line walk and its tokens, key matching and
// the scalar conversions every family parser beside it uses.
//
// These were file-local statics when every parser lived in one file. They now cross
// a TU boundary, so they need linkage — but names this generic (read_file, tokenize,
// next_line) must not enter the global namespace: engine/formats/avatars and
// engine/base/resource_index each have their own static read_file or tokenize, and one
// of those turning external later would collide. Hence the namespace; the family
// TUs pull it in wholesale, so no call site changes.

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

#include <base/io/ascii_config.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace opennova::defscan {

// The scanner's own types and the caps/macros the family parsers use directly.
typedef struct { const char *s; size_t len; } Token; // a value token's span

typedef struct { const char *name; size_t name_len; int bit; int bit2; } FlagEntry;

/* Dynamic array helpers */
#define DA_PUSH(arr, count, cap, elem) do { \
    if ((count) >= (cap)) { \
        (cap) = (cap) ? (cap) * 2 : 8; \
        (arr) = (decltype(arr))realloc((arr), (cap) * sizeof(*(arr))); \
    } \
    (arr)[(count)++] = (elem); \
} while(0)

// Records a finding. A blocking code (def_issue_blocks) also counts against the
// record or file even when the caller collects no details, and the writers refuse
// what it counts; an ignored-input or reinterpreted-value finding is reported only.
// Diagnostics never carry replayable source text; `detail` names the one token concerned
// (the value the game reads, for a reinterpreted one), if any.
void authoring_issue(size_t &count, opennova::def::DefParseReport *report,
                     size_t line, const char *record, const char *key, size_t key_len,
                     opennova::def::DefIssueCode code = opennova::def::DefIssueCode::UnknownProperty,
                     const char *detail = nullptr);

// A line as the parser read it, for the authoring checks below: its tokens joined by
// single spaces, a token holding a delimiter or a comment mark quoted again, and a token
// the comment cut left running bounded at the cut. The checks so see the key and values
// the parser bound, whatever delimiters the text used (`DELAYEND,7`,
// `"ammoclass_max_carry" X 40`, `pos 1 2 3 0 0 0// hip`).
std::string line_as_read(const io::ConfigTokens &tokens);

// A block header as the parser read it (line_as_read): its keyword and its name are the
// line's first two tokens as the retail tokenizer cuts them, so a comma or a quote ends
// the keyword as a space does (`ACTION,SCOPEUP` and `ACTION"SCOPEUP"` read as `ACTION
// SCOPEUP`), and every family reads the name from token 1, quoted or not. A header the
// writer could not give back (no name, a token after it) is a malformed block; a name past
// the `cut` characters the reader copies reads as its first `cut` (a reinterpreted value).
void validate_header(const char *line, size_t length, size_t key_length, size_t cut,
                     size_t &issues, opennova::def::DefParseReport *report, size_t number, const char *record);
// The checks of one property line (line_as_read) against the kind's property table: input
// that cannot be saved as the record holds it.
void validate_property(opennova::def::DefRecordKind kind, const char *line, size_t length,
                       size_t &issues, opennova::def::DefParseReport *report,
                       size_t number, const char *record);
const FlagEntry *weapon_flag_at(size_t index);
const char *death_piece_keyword(size_t index);

char *read_file(const char *path, size_t *out_len);

void safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len);

void to_lower_buf(char *dst, const char *src, size_t len);

int parse_int_n(const char *s, size_t len);

int signed_i16_value(int value);

float parse_float_n(const char *s, size_t len);

int death_piece_type_index(const char *name, size_t len);

/* The table row for a piece name, or -1 when the name is not a row (row 0 is HULL). */
int death_piece_type_lookup(const char *name, size_t len);

// The value tokens a family parser reads off one line: every token the
// tokenizer keeps past the key, 29 of its 30. The tokenizer stops at its 30th
// token without cutting it, so the 29th value runs on to the line's end,
// separators, quotes and comment included, and nothing past it is a token of
// its own. [orig: Terrain_TokenizeConfigLine @0x53CB60, the compare
// @0x53CC8C and the exit @0x53CC93 that leaves the token unterminated]
inline constexpr int kMaxValueTokens = io::kConfigMaxTokens - 1;

// Every family parser reads a line the way the retail walk hands it to its
// callback: cut into tokens by the shared ASCII tokenizer, the key the WHOLE
// first token compared without case, each value a token (a quoted run is one
// token and its quotes are not kept; `""` is no token at all; space, tab and
// comma separate; `//` or `;` outside quotes ends the line). A line with no
// token, or whose first token starts with '/', never reaches the callback.
// [orig: File_ParseASCIIFile @0x53D810 hands Terrain_TokenizeConfigLine
//  @0x53CB60's tokens to the callback (@0x53D908), skipping a line with no
//  token or whose first starts with '/' (@0x53D915 / @0x53D91E); the
//  tokenizer's cuts are io::tokenize_config_line's]
// The lines are the shared walk's (io::for_each_config_line_span): split at a
// CR LF pair and nowhere else, so an LF alone is a byte of the line and a file
// with no CR LF is one line; a last line with no pair loses its final byte
// [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5]. A line is read up to its
// first NUL, as the tokenizer's strlen reads it. `apply(tokens, line, line_len,
// line_index)` gets the line's bytes in `buf` (its CR LF excluded) for the
// parser's own use and its index counting every line the walk cuts, the
// skipped ones included. A callback that returns true ends the walk, as a
// nonzero return ends retail's [orig: @0x53D942]. `tokens` carries the
// tokenizer's slots across walks (io::ConfigTokens::slot). Returns the index of
// the line that ended the walk, else the number of lines cut.
template <typename Apply>
size_t for_each_def_line(const char *buf, size_t len, io::ConfigTokens &tokens, Apply &&apply) {
    size_t line_index = (size_t)-1;
    return io::for_each_config_line_span(buf, len, tokens, [&](io::ConfigTokens &line,
                                                               const io::ConfigLineSpan &span) {
        ++line_index;
        if (line.count == 0 || line.tokens[0][0] == '/') return false;
        const char *text = buf + span.begin;
        const size_t text_len = span.end - span.begin;
        return io::detail::walk_apply(apply, line, text, text_len, line_index);
    });
}

template <typename Apply>
size_t for_each_def_line(const char *buf, size_t len, Apply &&apply) {
    io::ConfigTokens tokens;
    return for_each_def_line(buf, len, tokens, std::forward<Apply>(apply));
}

// The walk above with `at(line)` told where each line it cuts begins, the lines
// its callback never sees included: what an authoring tool's layout recorder
// (def_notes.h's DefTextNoter) is told of the file, ahead of the callback.
template <typename At, typename Apply>
size_t for_each_def_line_noted(const char *buf, size_t len, io::ConfigTokens &tokens, At &&at, Apply &&apply) {
    size_t line_index = (size_t)-1;
    return io::for_each_config_line_span(buf, len, tokens, [&](io::ConfigTokens &line,
                                                               const io::ConfigLineSpan &span) {
        ++line_index;
        const char *text = buf + span.begin;
        at(text);
        if (line.count == 0 || line.tokens[0][0] == '/') return false;
        const size_t text_len = span.end - span.begin;
        return io::detail::walk_apply(apply, line, text, text_len, line_index);
    });
}

// The key compare every family parser makes [orig: _stricmp @0x76FDF6].
bool key_is(const char *key, const char *name);

// The value tokens (token 1 on) as spans, at most `max_tok` of them.
int value_tokens(const io::ConfigTokens &tokens, Token *out, int max_tok);

// Token `index` into `dst`, truncated to `dst_size - 1`; a slot past the
// line's count copies what io::ConfigTokens::token reads there.
void copy_token(char *dst, size_t dst_size, const io::ConfigTokens &tokens, int index);

const FlagEntry *lookup_flag(const char *name, size_t len);

int lookup_item_attrib(const char *name, size_t len);

int lookup_item_attrib2(const char *name, size_t len);

opennova::def::DefHudColor parse_hud_color(Token *vals, int n);

opennova::def::DefHudColor parse_hud_color_argb(Token *vals, int n);

void parse_pos_aligned(Token *vals, int n, int *out);

// The three-field positioned form (x, y, then the alignment word; no hidden
// dword) into out[0..2]: BREATHTIME's.
void parse_pos_align3(Token *vals, int n, int *out);

int parse_fixed16_digits_n(const char *s, size_t len);

// A hudpos number: every value HUD_ParseHudposToken reads is the CRT's atof of
// its token, converted by _ftol2_sse where the slot is an integer [orig:
// HUD_ParseHudposToken @0x59F370, e.g. HUDSPINMAPX1 @0x59F7E4..0x59F7EC].
double hud_double(const char *s, size_t len);
int hud_number(const char *s, size_t len);

}  // namespace opennova::defscan