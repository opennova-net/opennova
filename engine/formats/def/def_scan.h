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

#define DA_PUSH_RAW(raw_lines, raw_count, raw_cap, line, line_len) do { \
    if ((raw_count) >= (raw_cap)) { \
        (raw_cap) = (raw_cap) ? (raw_cap) * 2 : 8; \
        (raw_lines) = (decltype(raw_lines))realloc((raw_lines), (raw_cap) * sizeof(*(raw_lines))); \
    } \
    size_t _cplen = (line_len) < 511 ? (line_len) : 511; \
    memcpy((raw_lines)[(raw_count)], (line), _cplen); \
    (raw_lines)[(raw_count)][_cplen] = '\0'; \
    (raw_count)++; \
} while(0)

char *read_file(const char *path, size_t *out_len);

void safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len);

void to_lower_buf(char *dst, const char *src, size_t len);

int parse_int_n(const char *s, size_t len);

int signed_i16_value(int value);

float parse_float_n(const char *s, size_t len);

int death_piece_type_index(const char *name, size_t len);

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
// parsers' raw_lines and its index counting every line the walk cuts, the
// skipped ones included.
template <typename Apply>
void for_each_def_line(const char *buf, size_t len, Apply &&apply) {
    size_t line_index = (size_t)-1;
    io::for_each_config_line_span(buf, len, [&](const io::ConfigTokens &tokens,
                                                 const io::ConfigLineSpan &span) {
        ++line_index;
        if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
        apply(tokens, buf + span.begin, span.end - span.begin, line_index);
    });
}

// The key compare every family parser makes [orig: _stricmp @0x76FDF6].
bool key_is(const char *key, const char *name);

// The value tokens (token 1 on) as spans, at most `max_tok` of them.
int value_tokens(const io::ConfigTokens &tokens, Token *out, int max_tok);

// Token `index` into `dst`, truncated to `dst_size - 1`; a token past the
// line's count copies "".
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

}  // namespace opennova::defscan