#include "mns/mns.h"

#include "mns/mns_document.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace mns {

namespace {

std::string to_upper(const std::string &s) {
	std::string result = s;
	std::transform(result.begin(), result.end(), result.begin(),
				   [](unsigned char c) { return std::toupper(c); });
	return result;
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

// The single tokenizer lives in the lossless Document (mns_document.cpp).
// The flat API routes through the retail evaluator and reports syntax errors,
// while still returning the useful partial sheet.
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

// Canonical lossy dump of the flat map (sorted, tab-separated). Lossless
// serialization of an authored file is Document::serialize().
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
		ss << key << "\t" << value << "\n";
	}

	std::string s = ss.str();
	out.assign(s.begin(), s.end());
	return true;
}

}  // namespace mns
