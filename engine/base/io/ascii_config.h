#pragma once

// The retail ASCII config-file reader the text formats share (.adm, .tsd):
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
// so a token in progress there runs to the end of the line. At most 30 tokens.
// [orig: Terrain_TokenizeConfigLine @0x53CB60 — the skip @0x53CB90..0x53CB9E,
//  clamp @0x53CBBB, "//" @0x53CC16..0x53CC28, ';' @0x53CC2A..0x53CC31,
//  delimiters @0x53CC33..0x53CC4C, quote @0x53CC4E..0x53CC70, token start
//  @0x53CC72..0x53CC83, the 30 cap @0x53CC8C]

#include <cstddef>
#include <cstring>
#include <string>

namespace opennova::io {

inline constexpr int kConfigMaxTokens = 30;
inline constexpr size_t kConfigMaxLineChars = 1000;

struct ConfigTokens {
	int count = 0;
	const char *tokens[kConfigMaxTokens] = {};
	char buffer[kConfigMaxLineChars + 1] = {};

	// A token past the count reads as the empty string.
	const char *token(int index) const {
		return index >= 0 && index < count ? tokens[index] : "";
	}
};

inline void tokenize_config_line(const char *line, ConfigTokens &out) {
	out.count = 0;
	if (line == nullptr) return;
	while (*line == ' ' || *line == '\t') ++line;
	size_t length = std::strlen(line);
	if (length > kConfigMaxLineChars) length = kConfigMaxLineChars;
	std::memcpy(out.buffer, line, length);
	out.buffer[length] = 0;
	char *text = out.buffer;
	bool quoted = false;
	bool starts_token = true;
	for (size_t i = 0; i < length; ++i) {
		const char c = text[i];
		if (!quoted && ((c == '/' && text[i + 1] == '/') || c == ';')) break;
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

// Calls `apply(const ConfigTokens &)` for every line the retail walk hands
// its callback.
template <typename Apply>
void for_each_config_line(const char *text, size_t size, Apply &&apply) {
	if (text == nullptr) return;
	// The pair test reads the two bytes past the end before the tail leg.
	std::string data(text, size);
	data.push_back('\0');
	data.push_back('\0');
	char *const end = data.data() + size;
	ConfigTokens tokens;
	for (char *line = data.data(); line < end;) {
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
		line = cut + 2;
		if (tokens.count == 0 || tokens.tokens[0][0] == '/') continue;
		apply(static_cast<const ConfigTokens &>(tokens));
	}
}

} // namespace opennova::io
