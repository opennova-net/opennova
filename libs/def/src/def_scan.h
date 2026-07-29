#pragma once

// Internal to libs/def — not part of def/def.h. Split out of def.cpp (quality
// campaign W3-3); the bodies are unchanged.
//
// The shared .def text scanner: line iteration, key matching, value tokenizing and
// the scalar conversions every family parser beside it uses.
//
// These were file-local statics when every parser lived in one file. They now cross
// a TU boundary, so they need linkage — but names this generic (read_file, tokenize,
// next_line) must not enter the global namespace: libs/avatars, libs/ase, libs/tdp
// and libs/resource_index each have their own static read_file or tokenize, and one
// of those turning external later would collide. Hence the namespace; the family
// TUs pull it in wholesale, so no call site changes.

#include "def/def.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace defscan {

// The scanner's own types and the caps/macros the family parsers use directly.
#define MAX_TOKENS 16
typedef struct { const char *s; size_t len; } Token;

/* Line iterator: walks through buf splitting on \n, stripping \r */
typedef struct {
    const char *buf;
    size_t buf_len;
    size_t pos;
} LineIter;

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

const char *trim_span(const char *s, size_t len, size_t *out_len);

void to_lower_buf(char *dst, const char *src, size_t len);

size_t extract_quoted(const char *line, size_t line_len, char *dst, size_t dst_size);

const char *consume_value_span(const char *line, size_t line_len, size_t key_len, size_t *out_len);

void consume_value_str(const char *line, size_t line_len, size_t key_len, char *dst, size_t dst_size);

int parse_int_n(const char *s, size_t len);

int signed_i16_value(int value);

float parse_float_n(const char *s, size_t len);

int tokenize(const char *s, size_t len, Token *tokens, int max_tok);

int death_piece_type_index(const char *name, size_t len);

int split_values(const char *s, size_t len, Token *tokens, int max_tok);

int lower_starts_with(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len);

int lower_match_key(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len);

int next_line(LineIter *it, const char **out, size_t *out_len);

const FlagEntry *lookup_flag(const char *name, size_t len);

int lookup_item_attrib(const char *name, size_t len);

int lookup_item_attrib2(const char *name, size_t len);

int parse_ints(const char *s, size_t len, int *out, int max_n);

int parse_floats(const char *s, size_t len, float *out, int max_n);

DefHudColor parse_hud_color(Token *vals, int n);

DefHudColor parse_hud_color_argb(Token *vals, int n);

void parse_pos_aligned(Token *vals, int n, int *out);

int parse_fixed16_digits_n(const char *s, size_t len);

} // namespace defscan
