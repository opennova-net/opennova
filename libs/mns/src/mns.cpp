#include "mns/mns.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mns {

namespace {

std::string to_upper(const std::string &s) {
	std::string result = s;
	std::transform(result.begin(), result.end(), result.begin(),
				   [](unsigned char c) { return std::toupper(c); });
	return result;
}

// Skip whitespace (space and tab only, not newlines).
const char *skip_ws(const char *p, const char *end) {
	while (p < end && (*p == ' ' || *p == '\t')) {
		++p;
	}
	return p;
}

// Skip to end of line (or end of buffer).
const char *skip_to_eol(const char *p, const char *end) {
	while (p < end && *p != '\n' && *p != '\r') {
		++p;
	}
	return p;
}

// Skip past newline characters.
const char *skip_newline(const char *p, const char *end) {
	if (p < end && *p == '\r') ++p;
	if (p < end && *p == '\n') ++p;
	return p;
}

// Read a token (non-whitespace sequence).
const char *read_token(const char *p, const char *end, std::string &out) {
	out.clear();
	while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
		out += *p++;
	}
	return p;
}

// Read rest of line as value, handling line continuations.
const char *read_value(const char *p, const char *end, std::string &out) {
	out.clear();
	while (p < end) {
		// Check for line continuation.
		if (*p == '\\') {
			const char *next = p + 1;
			// Skip whitespace after backslash.
			while (next < end && (*next == ' ' || *next == '\t')) {
				++next;
			}
			// Check if followed by newline.
			if (next < end && (*next == '\n' || *next == '\r')) {
				// Line continuation - skip the backslash, whitespace, and newline.
				p = skip_newline(next, end);
				// Skip leading whitespace on next line.
				p = skip_ws(p, end);
				continue;
			}
		}

		// End of line.
		if (*p == '\n' || *p == '\r') {
			break;
		}

		out += *p++;
	}

	// Trim trailing whitespace.
	while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
		out.pop_back();
	}

	return p;
}

}  // namespace

std::string StyleSheet::get(const std::string &name) const {
	auto it = variables.find(to_upper(name));
	if (it != variables.end()) {
		return it->second;
	}
	return "";
}

bool StyleSheet::has(const std::string &name) const {
	return variables.find(to_upper(name)) != variables.end();
}

std::string StyleSheet::substitute(const std::string &text) const {
	std::string result;
	result.reserve(text.size());

	size_t i = 0;
	while (i < text.size()) {
		if (text[i] == '%') {
			// Look for closing %.
			size_t j = i + 1;
			while (j < text.size() && text[j] != '%' && text[j] != '\n' && text[j] != '\r') {
				++j;
			}
			if (j < text.size() && text[j] == '%' && j > i + 1) {
				// Found a variable reference.
				std::string var_name = text.substr(i + 1, j - i - 1);
				auto it = variables.find(to_upper(var_name));
				if (it != variables.end()) {
					result += it->second;
				} else {
					// Unknown variable - leave as-is.
					result += text.substr(i, j - i + 1);
				}
				i = j + 1;
			} else {
				// Not a valid variable reference.
				result += text[i++];
			}
		} else {
			result += text[i++];
		}
	}

	return result;
}

bool parse(const char *data, size_t size, StyleSheet &out, std::string &error) {
	out.variables.clear();

	if (size == 0) {
		return true;
	}

	const char *p = data;
	const char *end = data + size;

	// Skip UTF-8 BOM if present.
	if (size >= 3 && static_cast<uint8_t>(p[0]) == 0xEF &&
		static_cast<uint8_t>(p[1]) == 0xBB && static_cast<uint8_t>(p[2]) == 0xBF) {
		p += 3;
	}

	// Conditional compilation state.
	// When skip_depth > 0, we're inside a #if 0 block and should skip content.
	int skip_depth = 0;
	bool in_else = false;

	while (p < end) {
		// Skip leading whitespace.
		p = skip_ws(p, end);

		// Skip empty lines.
		if (p < end && (*p == '\n' || *p == '\r')) {
			p = skip_newline(p, end);
			continue;
		}

		// End of buffer.
		if (p >= end) {
			break;
		}

		// Check for // comment.
		if (p + 1 < end && p[0] == '/' && p[1] == '/') {
			p = skip_to_eol(p, end);
			p = skip_newline(p, end);
			continue;
		}

		// Check for preprocessor directives.
		if (*p == '#') {
			std::string directive;
			p = read_token(p + 1, end, directive);
			p = skip_ws(p, end);

			if (directive == "if") {
				std::string condition;
				p = read_token(p, end, condition);

				if (skip_depth > 0) {
					// Already skipping - just increase depth.
					++skip_depth;
				} else if (condition == "0") {
					// Start skipping.
					skip_depth = 1;
					in_else = false;
				}
				// #if 1 - continue normally.
			} else if (directive == "else") {
				if (skip_depth == 1 && !in_else) {
					// We were skipping due to #if 0, now stop.
					skip_depth = 0;
					in_else = true;
				} else if (skip_depth == 0 && in_else) {
					// We were in the true branch, now skip.
					skip_depth = 1;
				} else if (skip_depth == 0) {
					// We were in #if 1, now skip.
					skip_depth = 1;
					in_else = true;
				}
			} else if (directive == "endif") {
				if (skip_depth > 0) {
					--skip_depth;
				}
				in_else = false;
			}

			p = skip_to_eol(p, end);
			p = skip_newline(p, end);
			continue;
		}

		// Skip content if inside #if 0 block.
		if (skip_depth > 0) {
			p = skip_to_eol(p, end);
			p = skip_newline(p, end);
			continue;
		}

		// Read variable name.
		std::string name;
		p = read_token(p, end, name);

		if (name.empty()) {
			p = skip_to_eol(p, end);
			p = skip_newline(p, end);
			continue;
		}

		// Skip whitespace between name and value.
		p = skip_ws(p, end);

		// Read value (rest of line, with continuation support).
		std::string value;
		p = read_value(p, end, value);

		// Store with uppercase key.
		out.variables[to_upper(name)] = value;

		// Skip to next line.
		p = skip_newline(p, end);
	}

	return true;
}

bool parse_file(const std::string &path, StyleSheet &out, std::string &error) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		error = "Failed to open file: " + path;
		return false;
	}

	auto size = file.tellg();
	file.seekg(0, std::ios::beg);

	std::vector<char> buffer(size);
	if (!file.read(buffer.data(), size)) {
		error = "Failed to read file: " + path;
		return false;
	}

	return parse(buffer.data(), buffer.size(), out, error);
}

bool write(const StyleSheet &sheet, std::vector<uint8_t> &out, std::string &error) {
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
		ss << key << "\t" << value << "\n";
	}

	std::string s = ss.str();
	out.assign(s.begin(), s.end());
	return true;
}

}  // namespace mns
