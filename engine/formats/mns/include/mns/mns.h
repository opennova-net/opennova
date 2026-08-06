#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mns {

struct StyleSheet {
	std::unordered_map<std::string, std::string> variables;  // uppercase keys

	// Get a variable value by name (case-insensitive).
	// Returns empty string if not found.
	std::string get(const std::string &name) const;

	// Check if a variable exists (case-insensitive).
	bool has(const std::string &name) const;

	// Substitute all %VAR% patterns in text with their values.
	// Unknown variables are left as-is.
	std::string substitute(const std::string &text) const;
};

// Parse MNS stylesheet from memory buffer.
// Format features:
// - Skip UTF-8 BOM (EF BB BF)
// - Handle // comments (skip to end of line)
// - Handle #if 0, #if 1, #else, #endif conditionals
// - Handle \ line continuations
// - Parse NAME value pairs (whitespace separated)
// - Case-insensitive key storage (uppercase)
bool parse(const char *data, size_t size, StyleSheet &out, std::string &error);

// Parse MNS stylesheet from file.
bool parse_file(const std::string &path, StyleSheet &out, std::string &error);

// Write stylesheet to binary buffer.
bool write(const StyleSheet &sheet, std::vector<uint8_t> &out, std::string &error);

}  // namespace mns
