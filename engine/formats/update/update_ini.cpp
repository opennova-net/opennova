#include "update_ini.h"

#include <base/io/strutil.h>

namespace opennova::update {

namespace {

// The text a scanf pattern sees of a line: its characters to the first CR or LF.
std::string_view scan_text(std::string_view line) {
	const size_t end = line.find_first_of("\r\n");
	return end == std::string_view::npos ? line : line.substr(0, end);
}

bool is_section_char(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// `[%[A-Z_0123456789]`: the run of upper-case name characters after an opening bracket.
// [orig: IniFile_FindSection @ 0x403a60 (UPDATE.EXE)]
std::optional<std::string> section_run(std::string_view line) {
	if (line.empty() || line[0] != '[')
		return std::nullopt;
	size_t end = 1;
	while (end < line.size() && is_section_char(line[end]))
		++end;
	if (end == 1)
		return std::nullopt;
	return std::string(line.substr(1, end - 1));
}

// `[%s`: an opening bracket followed by a non-space character, where a key search stops.
// [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE)]
bool opens_section(std::string_view line) {
	return line.size() >= 2 && line[0] == '[' && line[1] != ' ' && line[1] != '\t';
}

} // namespace

// [orig: IniFile_Load @ 0x403840 (UPDATE.EXE): each CR LF becomes a line's end]
UpdateIni::UpdateIni(std::string_view text) {
	size_t start = 0;
	while (start < text.size()) {
		const size_t crlf = text.find("\r\n", start);
		if (crlf == std::string_view::npos) {
			lines_.emplace_back(text.substr(start));
			break;
		}
		lines_.emplace_back(text.substr(start, crlf - start));
		start = crlf + 2;
	}
}

std::string UpdateIni::line(size_t index, size_t buffer) const {
	if (index >= lines_.size() || buffer == 0)
		return {};
	const std::string &text = lines_[index];
	return text.size() < buffer ? text : text.substr(0, buffer - 1);
}

std::optional<size_t> UpdateIni::find_section(std::string_view section) const {
	const std::string wanted = strutil::to_upper(section);
	for (size_t i = 0; i < lines_.size(); ++i) {
		const std::optional<std::string> run = section_run(scan_text(lines_[i]));
		if (run && *run == wanted)
			return i + 1;
	}
	return std::nullopt;
}

std::optional<std::string> UpdateIni::value(
		std::string_view section, std::string_view key, size_t max_bytes) const {
	const std::optional<size_t> first = find_section(section);
	if (!first)
		return std::nullopt;
	const std::string wanted = strutil::to_upper(key);
	for (size_t i = *first; i < lines_.size(); ++i) {
		const std::string text = line(i, kIniKeyLineBytes);
		const std::string_view scanned = scan_text(text);
		const size_t eq = scanned.find('=');
		if (eq != std::string_view::npos && eq > 0) {
			std::string_view name = scanned.substr(0, eq);
			while (!name.empty() && name.back() == ' ')
				name.remove_suffix(1);
			if (name == wanted) {
				std::string_view value = scanned.substr(eq + 1);
				const size_t semi = value.find(';');
				if (semi != std::string_view::npos)
					value = value.substr(0, semi);
				while (!value.empty() && value.front() == ' ')
					value.remove_prefix(1);
				if (max_bytes == 0)
					return std::string();
				return std::string(value.substr(0, max_bytes - 1));
			}
		}
		if (opens_section(scanned))
			return std::nullopt;
	}
	return std::nullopt;
}

} // namespace opennova::update
