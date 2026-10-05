#pragma once

// The retail ASCII config-file reader the text formats share (.adm, .tsd, the
// .def families):
// File_ParseASCIIFile's line walk and the tokenizer it calls per line.
//
// Lines split STRICTLY on a CR LF pair; the line is terminated in place at the
// CR, and a tail line with no pair loses its final byte to that terminator
// [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5]. Each line is tokenized, and
// the per-format callback runs only for a line with at least one token whose
// first token does not start with '/' [orig: the count test @0x53D915, the
// '/' test @0x53D91E].
//
// The tokenizer skips leading spaces/tabs, reads the rest as a C string
// clamped to 1000 chars, and walks it: outside quotes "//" or ';' ends the
// line and space, comma or tab ends a token; '"' toggles quoting and ends a
// token either way, so a token is a quoted run's contents (spaces and commas
// included) and quoting is not recorded. The comment cut writes no terminator,
// so a token in progress there runs to the end of the line. Each line resets
// tokens 0..2 and leaves 3..29 pointing where earlier lines put them (the
// ConfigTokens::slot note). At most 30 tokens:
// the walk stops at the 30th without cutting it, so that one also runs to the
// end of the line (its separators, quotes and comment included).
// [orig: Terrain_TokenizeConfigLine @0x53CB60 — the skip @0x53CB90..0x53CB9E,
//  clamp @0x53CBBB, "//" @0x53CC16..0x53CC28, ';' @0x53CC2A..0x53CC31,
//  delimiters @0x53CC33..0x53CC4C, quote @0x53CC4E..0x53CC70, token start
//  @0x53CC72..0x53CC83, the 30 cap @0x53CC8C..0x53CC93]

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

namespace opennova::io {

inline constexpr int kConfigMaxTokens = 30;
inline constexpr size_t kConfigMaxLineChars = 1000;

struct ConfigTokens {
	int count = 0;
	const char *tokens[kConfigMaxTokens] = {};
	char buffer[kConfigMaxLineChars + 1] = {};
	// Where the walk started in the line (the leading spaces and tabs it
	// skipped) and where it stopped in `buffer`: the "//" or ';' that cut the
	// line, else the clamped length. The cut writes no terminator, so a token
	// in progress there runs on past it; `cut` bounds that token's value.
	size_t skip = 0;
	size_t cut = 0;

	// The slot a retail callback reads at `index`. Each line resets slots 0..2
	// to the empty string and leaves 3..29 where the last line that had them
	// put them, pointing into the reused line buffer, so a short line reads
	// whatever an earlier, longer one left at that offset (the current line's
	// bytes up to its terminator, the older bytes past it)
	// [orig: Terrain_TokenizeConfigLine @0x53CB71..0x53CB81; the static buffer
	// byte_24E5DF0 and slots dword_24E61E4]. A slot no line has set is NULL
	// (the zeroed static), and so is the one past the last: the dword after
	// slot 29 has no writer. The state lives as long as its ConfigTokens: a
	// reader that walks a file twice, as retail's ammo and power-up loads do,
	// passes one to both walks; retail's is the process's, across every file it
	// tokenized (D-ITEMDEF-8).
	const char *slot(int index) const {
		if (index < 0 || index >= kConfigMaxTokens) return nullptr;
		if (index < count) return tokens[index];
		if (index < 3) return "";
		return tokens[index];
	}

	// The slot as a string: "" where the slot is NULL.
	const char *token(int index) const {
		const char *s = slot(index);
		return s != nullptr ? s : "";
	}

	// Writes a NUL into the line buffer where `at` points, as a retail callback
	// that cuts a token in place does; the byte stays for later lines' stale
	// reads until a longer line overwrites it. A pointer outside the buffer
	// (the empty string of a reset slot) writes nothing.
	void terminate_at(const char *at) {
		const auto first = reinterpret_cast<std::uintptr_t>(buffer);
		const auto where = reinterpret_cast<std::uintptr_t>(at);
		if (where >= first && where < first + sizeof(buffer)) buffer[where - first] = 0;
	}
};

inline void tokenize_config_line(const char *line, ConfigTokens &out) {
	out.count = 0;
	out.tokens[0] = out.tokens[1] = out.tokens[2] = "";
	out.skip = 0;
	out.cut = 0;
	if (line == nullptr) return;
	while (*line == ' ' || *line == '\t') {
		++line;
		++out.skip;
	}
	size_t length = std::strlen(line);
	if (length == 0) return; // the buffer keeps the last line [orig: `if (lineLen)` @0x53CBB5]
	if (length > kConfigMaxLineChars) length = kConfigMaxLineChars;
	std::memcpy(out.buffer, line, length);
	out.buffer[length] = 0;
	out.cut = length;
	char *text = out.buffer;
	bool quoted = false;
	bool starts_token = true;
	for (size_t i = 0; i < length; ++i) {
		const char c = text[i];
		if (!quoted && ((c == '/' && text[i + 1] == '/') || c == ';')) {
			out.cut = i;
			break;
		}
		if (!quoted && (c == ' ' || c == ',' || c == '\t')) {
			text[i] = 0;
			starts_token = true;
		} else if (c == '"') {
			quoted = !quoted;
			text[i] = 0;
			starts_token = true;
		} else {
			if (starts_token) out.tokens[out.count++] = &text[i];
			starts_token = false;
		}
		if (out.count >= kConfigMaxTokens) break;
	}
}

// One line as the retail walk cuts it, located in the text it came from: the
// content's byte span (its CR LF excluded; a tail line with no pair loses its
// final byte), where a comment cuts it (else `end`), and each token's span.
// A token the comment cut, or the line's end, left unterminated runs on to
// `end`; `cut` is where its value really stops.
struct ConfigLineSpan {
	size_t begin = 0;
	size_t end = 0;
	size_t cut = 0;
	size_t token_begin[kConfigMaxTokens] = {};
	size_t token_end[kConfigMaxTokens] = {};
};

namespace detail {

// Runs a walk's callback and says whether it ends the walk. A callback that
// returns bool ends it on true, as a nonzero return from retail's ends
// File_ParseASCIIFile's loop [orig: @0x53D942]; one that returns nothing
// never does.
template <typename Apply, typename... Args>
bool walk_apply(Apply &apply, Args &&...args) {
	if constexpr (std::is_same_v<std::invoke_result_t<Apply &, Args...>, bool>) {
		return apply(std::forward<Args>(args)...);
	} else {
		apply(std::forward<Args>(args)...);
		return false;
	}
}

} // namespace detail

// Calls `apply(ConfigTokens &, const ConfigLineSpan &)` for EVERY line the
// retail walk cuts, the ones its callback never sees included (a line with no
// token, or whose first token starts with '/'), so a consumer that indexes
// lines counts them as the walk numbers them. The ONE CR LF split;
// for_each_config_line below is this walk with the callback gate applied.
// `tokens` carries the tokenizer's slots in and out of the walk. Returns the
// index of the line whose callback ended the walk, else the number of lines
// cut.
template <typename Apply>
size_t for_each_config_line_span(const char *text, size_t size, ConfigTokens &tokens, Apply &&apply) {
	if (text == nullptr) return 0;
	// The pair test reads the two bytes past the end before the tail leg.
	std::string data(text, size);
	data.push_back('\0');
	data.push_back('\0');
	char *const base = data.data();
	char *const end = base + size;
	ConfigLineSpan span;
	size_t index = 0;
	for (char *line = base; line < end; ++index) {
		char *cut = line;
		while (!(cut[0] == '\r' && cut[1] == '\n')) {
			if (cut >= end) {
				--cut;
				break;
			}
			++cut;
		}
		*cut = 0;
		tokenize_config_line(line, tokens);
		span.begin = static_cast<size_t>(line - base);
		span.end = static_cast<size_t>(cut - base);
		span.cut = span.begin + tokens.skip + tokens.cut;
		if (span.cut > span.end) span.cut = span.end;
		for (int i = 0; i < tokens.count; ++i) {
			span.token_begin[i] = span.begin + tokens.skip +
					static_cast<size_t>(tokens.tokens[i] - tokens.buffer);
			span.token_end[i] = span.token_begin[i] + std::strlen(tokens.tokens[i]);
		}
		line = cut + 2;
		if (detail::walk_apply(apply, tokens, static_cast<const ConfigLineSpan &>(span))) return index;
	}
	return index;
}

// The walk over a fresh tokenizer state.
template <typename Apply>
size_t for_each_config_line_span(const char *text, size_t size, Apply &&apply) {
	ConfigTokens tokens;
	return for_each_config_line_span(text, size, tokens, std::forward<Apply>(apply));
}

// Calls `apply(ConfigTokens &)` for every line the retail walk hands its
// callback; the return and `tokens` are for_each_config_line_span's.
template <typename Apply>
size_t for_each_config_line(const char *text, size_t size, ConfigTokens &tokens, Apply &&apply) {
	return for_each_config_line_span(text, size, tokens, [&](ConfigTokens &line, const ConfigLineSpan &) {
		if (line.count == 0 || line.tokens[0][0] == '/') return false;
		return detail::walk_apply(apply, line);
	});
}

template <typename Apply>
size_t for_each_config_line(const char *text, size_t size, Apply &&apply) {
	ConfigTokens tokens;
	return for_each_config_line(text, size, tokens, std::forward<Apply>(apply));
}

} // namespace opennova::io
