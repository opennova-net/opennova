#include <formats/mns/mns.h>
#include <base/io/strutil.h>

#include <formats/mns/mns_document.h>

// [orig: NapiXML_ExpandVariablesInText @0x63a000 — the %VAR% expansion over the table that
//  Menu_InitShellResources @0x552500 loads via NapiConfigMap_LoadIncludeFile @0x63b970 ->
//  NapiConfigMap_ParseKeyValueBuffer @0x639870; docs/mnu/menu-re.md]

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <tuple>
#include <vector>

namespace opennova::mns {

namespace {

// The CRT's isspace in the "C" locale [orig: isspace @ 0x76b964, the ctype table]: the
// space and \t..\r; no byte past 0x7f.
bool crt_isspace(char c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

// The skip every token boundary runs [orig: @ 0x639a49..0x639a7d, @ 0x639b64..0x639b9f,
// @ 0x639d27..0x639d6b]: isspace bytes, line ends included (the strchr(g_EmptyStr, c)
// test in the loop only ever matches the terminator).
void skip_space(const char *&cursor) {
	while (*cursor && crt_isspace(*cursor)) ++cursor;
}

} // namespace

std::string StyleSheet::get(const std::string &name) const {
	auto it = variables.find(strutil::to_upper(name));
	if (it != variables.end()) {
		return it->second;
	}
	return "";
}

bool StyleSheet::has(const std::string &name) const {
	return variables.find(strutil::to_upper(name)) != variables.end();
}

size_t variable_reference_at(const std::string &text, size_t at) {
	if (at >= text.size() || text[at] != '%') return 0;
	size_t end = at + 1;
	while (end < text.size() && std::strchr("% <>/\\", text[end]) == nullptr) ++end;
	if (end == at + 1 || end == text.size() || text[end] != '%') return 0;
	return end + 1 - at;
}

bool is_variable_reference(const std::string &value) {
	return !value.empty() && variable_reference_at(value, 0) == value.size();
}

std::string variable_name(const std::string &value) {
	return is_variable_reference(value) ? value.substr(1, value.size() - 2) : value;
}

bool holds_variable_reference(const std::string &text) {
	for (size_t at = text.find('%'); at != std::string::npos; at = text.find('%', at + 1))
		if (variable_reference_at(text, at) != 0) return true;
	return false;
}

std::vector<std::string> variables_named(const std::string &text) {
	std::vector<std::string> names;
	for (size_t at = text.find('%'); at != std::string::npos;) {
		const size_t length = variable_reference_at(text, at);
		if (length == 0) {
			at = text.find('%', at + 1);
			continue;
		}
		names.push_back(strutil::to_upper(text.substr(at + 1, length - 2)));
		at = text.find('%', at + length);
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

std::vector<std::string> changed_variables(const std::map<std::string, std::string> &before,
                                           const std::map<std::string, std::string> &after) {
	std::vector<std::string> out;
	auto was = before.begin();
	auto now = after.begin();
	while (was != before.end() || now != after.end()) {
		if (now == after.end() || (was != before.end() && was->first < now->first)) {
			out.push_back(was->first);
			++was;
		} else if (was == before.end() || now->first < was->first) {
			out.push_back(now->first);
			++now;
		} else {
			if (was->second != now->second) out.push_back(now->first);
			++was;
			++now;
		}
	}
	return out;
}

std::string StyleSheet::substitute(const std::string &text) const {
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const size_t length = variable_reference_at(text, i);
		if (length == 0) {
			result += text[i++];
			continue;
		}
		// The list's value for the name (a stricmp walk), or the reference copied whole
		// [orig: @ 0x63a382..0x63a3a1; the value @ 0x63a450..0x63a4d1; the copy @
		// 0x63a3a3..0x63a441].
		const auto it = variables.find(strutil::to_upper(text.substr(i + 1, length - 2)));
		result += it != variables.end() ? it->second : text.substr(i, length);
		i += length;
	}
	return result;
}

StyleSheet KeyValueList::sheet() const {
	StyleSheet out;
	for (const KeyValue &node : nodes) out.variables[strutil::to_upper(node.name)] = node.value;
	return out;
}

// A structural translation of the retail reader, its state machine byte for byte
// (docs/mnu/menu-re.md "The stylesheet reader").
ReadResult parse_key_value_buffer(const char *text, size_t size, KeyValueList &list) {
	// The loader hands the parse a NUL-terminated copy of the file [orig:
	// NapiConfigMap_LoadIncludeFile @ 0x63b970, the terminator @ 0x63b9c6], read as a C
	// string. The padding stands in for the bytes past the terminator a directive at the
	// very end steps over (cursor += 4, 5 or 6), which retail reads out of its heap block:
	// here they read as more terminators.
	std::string buffer(text, size);
	buffer.append(8, '\0');
	const char *const base = buffer.c_str();
	const char *cursor = base;
	ReadResult result;
	// A UTF-8 byte-order mark is skipped [orig: @ 0x639876..0x639899].
	if (static_cast<unsigned char>(cursor[0]) == 0xEF && static_cast<unsigned char>(cursor[1]) == 0xBB &&
	    static_cast<unsigned char>(cursor[2]) == 0xBF)
		cursor += 3;
	int if_depth = 0, saved_if_depth = 0, suppressed_depth = -1;
	bool has_continuation = false, is_suppressed = false, saved_suppressed = false;
	bool expect_key = false, expect_value = false, line_continuation = false, at_line_start = true;
	bool has_key = false, has_value = false;
	std::string parsed_key, parsed_value;
	// The node a continuation appends to: the list's head on entry, then each node this
	// parse creates, never one a later definition only updates [orig: current_node, set
	// @ 0x6398af, @ 0x639e00 and @ 0x639e73].
	size_t current = list.nodes.empty() ? SIZE_MAX : list.nodes.size() - 1;
	if (!*cursor) return result; // [orig: @ 0x6398a5]
	for (;;) {
		// The iteration's starting state, to recognize one that changes nothing (Hangs).
		const char *const iteration = cursor;
		const auto state = std::make_tuple(if_depth, suppressed_depth, has_continuation, is_suppressed, expect_key,
		                                   expect_value, line_continuation, at_line_start, has_key, has_value);
		enum class Next { Skip, Insert, End } next = Next::End;
		const char c = *cursor;
		if (c == '/' && cursor[1] == '/') {
			// A comment runs to the line's end [orig: @ 0x6398e8..0x639927].
			cursor += 2;
			while (*cursor && *cursor != '\r' && *cursor != '\n') ++cursor;
			next = Next::Skip;
		} else if (c == '\r' || c == '\n' || c == '\\') {
			// A line end, or a backslash at a token boundary, is stepped over [orig: @ 0x639938].
			++cursor;
			next = Next::Skip;
		} else if (c == '#') {
			// The directives, matched as prefixes [orig: @ 0x639944..0x639a38]. An #if inside
			// switched-off source only counts its depth.
			if (std::strncmp(cursor + 1, "if", 2) == 0) {
				++if_depth;
				cursor += 4; // '#', "if" and the byte after them [orig: @ 0x639964]
				saved_if_depth = if_depth;
				if (!is_suppressed) {
					// The argument's first byte decides, and is consumed [orig: @ 0x6399b3..0x6399d9].
					skip_space(cursor);
					if (*cursor == '1') {
						is_suppressed = false;
						suppressed_depth = -1;
						saved_suppressed = false;
					} else if (*cursor == '0') {
						is_suppressed = true;
						suppressed_depth = if_depth;
						saved_suppressed = true;
					}
					++cursor;
				}
			} else {
				bool known = true, toggle = false;
				if (std::strncmp(cursor + 1, "else", 4) == 0) {
					// An #else switches on or off at the depth its suppression started at
					// [orig: @ 0x6399f2..0x6399f9].
					cursor += 5;
					toggle = !is_suppressed || if_depth == suppressed_depth;
				} else if (std::strncmp(cursor + 1, "endif", 5) == 0) {
					// [orig: @ 0x639a13..0x639a1f]
					const int prev_depth = if_depth--;
					cursor += 6;
					toggle = prev_depth == suppressed_depth;
					saved_if_depth = if_depth;
				} else {
					known = false; // any other '#': the cursor stays on it [orig: @ 0x639a11]
				}
				if (known && toggle) {
					// [orig: @ 0x639a27..0x639a38]
					is_suppressed = !is_suppressed;
					saved_suppressed = is_suppressed;
					suppressed_depth = is_suppressed ? if_depth : -1;
				}
			}
			next = Next::Skip;
		} else if (at_line_start) {
			next = Next::Skip; // [orig: @ 0x639a47]
		} else if (is_suppressed) {
			// Switched-off source: seek the next '#' byte, a line start or not
			// [orig: @ 0x639aac..0x639ade].
			while (*cursor && *cursor != '#') ++cursor;
			next = Next::End;
		} else if (expect_key) {
			// A define's name: up to a space or one of \ / % < > # [orig: @ 0x639af4..0x639b19].
			const char *const key_start = cursor;
			while (*cursor && !crt_isspace(*cursor) && !std::strchr("\\/%<>#", *cursor)) ++cursor;
			const char *const key_end = cursor;
			const char delimiter = *cursor++;
			switch (delimiter) {
			case '\0':
			case '\n':
			case '\r':
			case '#':
			case '%':
			case '/':
			case '<':
			case '>':
				// The reader's one failure [orig: @ 0x639b3a -> 0x639f32]: it stops, and what it
				// read stays in the list.
				result.status = ReadStatus::Failed;
				result.offset = size_t(key_end - base);
				return result;
			default: break; // a space, a tab, VT, FF, or '\': the name is read
			}
			parsed_key.assign(key_start, key_end);
			has_key = true;
			expect_key = false;
			skip_space(cursor); // crosses line ends: the value may start on a later line
			expect_value = true;
			is_suppressed = saved_suppressed; // [orig: LABEL_96 @ 0x639da1]
			if_depth = saved_if_depth;
			next = Next::Insert;
		} else if (expect_value || has_continuation) {
			// A value segment: up to a line end, "//" or a lone '\'; a single '/' and a "\\"
			// pair are part of it, the pair kept doubled [orig: @ 0x639bba..0x639c1a, the pair
			// test @ 0x639c0e].
			const bool was_continuing = has_continuation;
			line_continuation = false;
			const char *const value_start = cursor;
			while (*cursor) {
				while (*cursor && !std::strchr("\r\n/\\", *cursor)) ++cursor;
				if (*cursor == '/' && cursor[1] != '/') ++cursor;
				else if (*cursor == '\\' && cursor[1] == '\\') cursor += 2;
				else break;
			}
			const char *value_end = cursor;
			const char stop = *cursor++;
			switch (stop) {
			case '\0':
				--cursor; // the buffer's end: the value keeps its trailing blanks [orig: @ 0x639c8b]
				break;
			case '\n':
			case '\r':
				// Trailing blanks trimmed, the cursor left past this one line-end byte
				// [orig: @ 0x639c3b..0x639c62].
				while (value_end > value_start && crt_isspace(value_end[-1])) --value_end;
				break;
			case '/':
				while (value_end > value_start && crt_isspace(value_end[-1])) --value_end;
				--cursor; // back on the comment [orig: @ 0x639c6b..0x639c8b]
				break;
			case '\\':
				// A lone backslash: the next text, on this line or a later one, joins the value
				// with nothing between [orig: @ 0x639d27..0x639d6b].
				line_continuation = true;
				skip_space(cursor);
				break;
			default: break;
			}
			if (was_continuing) {
				// Appended to the node this parse last created, not to the one the value's
				// name found [orig: @ 0x639c90..0x639d22].
				if (current < list.nodes.size()) list.nodes[current].value.append(value_start, value_end);
			} else {
				parsed_value.assign(value_start, value_end);
				has_value = true;
			}
			expect_value = false;
			is_suppressed = saved_suppressed;
			if_depth = saved_if_depth;
			next = Next::Insert;
		} else {
			next = Next::Insert;
		}
		if (next == Next::Skip) {
			// [orig: LABEL_40 @ 0x639a49..0x639aa0]
			skip_space(cursor);
			at_line_start = false;
			if (!is_suppressed && !expect_value && !line_continuation) expect_key = true;
		} else if (next == Next::Insert && has_key && has_value) {
			// A name already in the list takes the new value and keeps its spelling; a new
			// one is added and becomes the continuation target [orig: @ 0x639db9..0x639f19].
			const auto found = std::find_if(list.nodes.begin(), list.nodes.end(),
			                                [&](const KeyValue &node) { return strutil::iequals(node.name, parsed_key); });
			if (found != list.nodes.end()) {
				found->value = parsed_value;
			} else {
				list.nodes.push_back(KeyValue{parsed_key, parsed_value});
				current = list.nodes.size() - 1;
			}
			has_key = has_value = false;
			parsed_key.clear();
			parsed_value.clear();
			if_depth = saved_if_depth;
			is_suppressed = saved_suppressed;
		}
		// [orig: LABEL_118 @ 0x639f1d]
		if (!*cursor) return result;
		has_continuation = line_continuation;
		// Retail's loop has no other exit: an iteration that moved nothing and changed
		// nothing repeats forever (a value ending on a lone LF or CR, an unknown directive,
		// a value starting with '#'). The port stops there with what it read.
		if (cursor == iteration &&
		    state == std::make_tuple(if_depth, suppressed_depth, has_continuation, is_suppressed, expect_key,
		                             expect_value, line_continuation, at_line_start, has_key, has_value)) {
			result.status = ReadStatus::Hangs;
			result.offset = size_t(cursor - base);
			return result;
		}
	}
}

int line_at_offset(const char *text, size_t size, size_t offset) {
	int line = 1;
	for (size_t i = 0; i < size && i < offset; ++i) {
		if (text[i] == '\r') {
			if (i + 1 < size && text[i + 1] == '\n') {
				if (i + 1 >= offset) break;
				++i;
			}
			++line;
		} else if (text[i] == '\n') {
			++line;
		}
	}
	return line;
}

// The flat API is the game's own read, reported through the lossless Document's
// diagnostics (mns_document.cpp).
bool parse(const char *data, size_t size, StyleSheet &out, std::string &error) {
	const EvaluationResult result = Document::parse(data, size).evaluate();
	out = result.sheet;
	error.clear();
	if (!result.success) {
		for (const Diagnostic &diagnostic : result.diagnostics) {
			if (diagnostic.severity != Severity::Error) continue;
			error = "line " + std::to_string(diagnostic.line) + ": " +
					diagnostic.message;
			break;
		}
		if (error.empty()) error = "MNS evaluation failed";
	}
	return result.success;
}

// Canonical lossy dump of the flat map (sorted, tab-separated, CRLF: the reader stops
// responding on a value line ended by a lone LF [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]).
// Lossless serialization of an authored file is Document::serialize().
bool write(const StyleSheet &sheet, std::vector<uint8_t> &out, std::string &error) {
	(void)error;
	out.clear();

	std::ostringstream ss;

	// Sort keys for consistent output.
	std::vector<std::string> keys;
	keys.reserve(sheet.variables.size());
	for (const auto &kv : sheet.variables) {
		keys.push_back(kv.first);
	}
	std::sort(keys.begin(), keys.end());

	for (const auto &key : keys) {
		const auto &value = sheet.variables.at(key);
		ss << key << "\t" << value << "\r\n";
	}

	std::string s = ss.str();
	out.assign(s.begin(), s.end());
	return true;
}

}  // namespace opennova::mns
