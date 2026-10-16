#pragma once

#include <cstddef>

#include <base/io/strutil.h>

namespace opennova::editor {

// The session's compiled-in tables of rows, one row per value of an enum (the request kinds, the request
// fields, the queries, the view sections, the concerns, the view events, the operation kinds, the command
// line's verbs): the checks each table's static_asserts make, and its lookup by token. (S26 Cy13, Cy15,
// Cy16: each table wrote its own.)

// Two C strings equal, at compile time (base/io/strutil.h's, which the session's tables and the
// command line's reach as editor::same_text).
using strutil::same_text;

// The rows follow their enum's order: row i's `member` is the enum's value i.
template <class Row, size_t N, class Enum>
constexpr bool rows_follow(const Row (&rows)[N], Enum Row::*member) {
	for (size_t i = 0; i < N; ++i)
		if (static_cast<size_t>(rows[i].*member) != i) return false;
	return true;
}

// Each row's token its own.
template <class Row, size_t N>
constexpr bool tokens_unique(const Row (&rows)[N], const char *Row::*token) {
	for (size_t i = 0; i < N; ++i)
		for (size_t j = 0; j < i; ++j)
			if (same_text(rows[i].*token, rows[j].*token)) return false;
	return true;
}

// The value of the row whose token is `text` (a std::string or a std::string_view) into `out`; false for a
// token no row has (`out` as it was).
template <class Row, size_t N, class Enum, class Text>
bool row_from_token(const Row (&rows)[N], Enum Row::*member, const char *Row::*token, const Text &text, Enum &out) {
	for (const Row &row : rows) {
		if (text == row.*token) {
			out = row.*member;
			return true;
		}
	}
	return false;
}

} // namespace opennova::editor
