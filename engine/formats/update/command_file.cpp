#include "command_file.h"

#include "update_ini.h"

#include <base/io/strutil.h>

namespace opennova::update {

namespace {

const struct {
	std::string_view key;
	CommandType type;
} kKeyTable[] = {
	{ "UPD", CommandType::Loose },
	{ "RESEXE", CommandType::Loose },
	{ "REVEXE", CommandType::Loose },
	{ "WIZ", CommandType::Loose },
	{ "COPY", CommandType::Copy },
	{ "REMOVE", CommandType::Remove },
	{ "TARGET", CommandType::Target },
};

bool is_token_separator(char c) {
	return c == ' ' || c == ',' || c == '\r' || c == '\n';
}

// strtok over " ,\r\n": the tokens of `text` in order.
std::vector<std::string_view> tokens(std::string_view text) {
	std::vector<std::string_view> out;
	size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && is_token_separator(text[i]))
			++i;
		if (i >= text.size())
			break;
		size_t end = i;
		while (end < text.size() && !is_token_separator(text[end]))
			++end;
		out.push_back(text.substr(i, end - i));
		i = end;
	}
	return out;
}

// atol: leading spaces, an optional sign, digits; 0 when none.
int32_t atol_like(std::string_view text) {
	size_t i = 0;
	while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
		++i;
	bool negative = false;
	if (i < text.size() && (text[i] == '-' || text[i] == '+'))
		negative = text[i++] == '-';
	int64_t value = 0;
	for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
		value = value * 10 + (text[i] - '0');
		if (value > 0x80000000ll)
			value = 0x80000000ll; // atol saturates
	}
	if (negative)
		value = -value;
	if (value > 0x7FFFFFFFll)
		value = 0x7FFFFFFFll;
	return static_cast<int32_t>(value);
}

// One [COMMAND] line, nullopt for a line the reader skips: no key the table knows before the
// first '=' (strtok skips leading '='), no name, or a Loose / Copy row without its time and
// size. [orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE)]
std::optional<CommandRow> read_row(std::string_view line) {
	size_t start = 0;
	while (start < line.size() && line[start] == '=')
		++start;
	if (start >= line.size())
		return std::nullopt;
	const size_t eq = line.find('=', start);
	const std::string_view key =
			line.substr(start, eq == std::string_view::npos ? line.npos : eq - start);
	const std::optional<CommandType> type = command_type_for_key(key);
	if (!type || eq == std::string_view::npos)
		return std::nullopt;
	const std::vector<std::string_view> values = tokens(line.substr(eq + 1));
	if (values.empty())
		return std::nullopt;
	CommandRow row;
	row.key = std::string(key);
	row.type = *type;
	row.name = std::string(values[0].substr(0, kCommandNameBytes - 1));
	if (*type == CommandType::Loose || *type == CommandType::Copy) {
		if (values.size() < 3)
			return std::nullopt;
		row.time = atol_like(values[1]);
		row.size = atol_like(values[2]);
		if (values.size() >= 4)
			row.crc = atol_like(values[3]);
	}
	return row;
}

bool header_value_carries(const std::string &value) {
	return value.size() < kCommandHeaderValueBytes &&
			value.find_first_of("\r\n;") == std::string::npos;
}

bool name_carries(const std::string &name) {
	if (name.empty() || name.size() >= kCommandNameBytes)
		return false;
	for (char c : name)
		if (is_token_separator(c) || c == '=')
			return false;
	return true;
}

} // namespace

std::optional<CommandType> command_type_for_key(std::string_view key) {
	for (const auto &entry : kKeyTable)
		if (strutil::iequals(entry.key, key))
			return entry.type;
	return std::nullopt;
}

std::optional<CommandFile> read_command_file(std::string_view text) {
	const UpdateIni ini(text);
	if (!ini.find_section("HEADER"))
		return std::nullopt;
	CommandFile file;
	file.exe = ini.value("HEADER", "EXE", kCommandHeaderValueBytes).value_or(std::string());
	file.product = ini.value("HEADER", "PRODUCT", kCommandHeaderValueBytes).value_or(std::string());
	file.version = ini.value("HEADER", "VERSION", kCommandHeaderValueBytes).value_or(std::string());
	if (const std::optional<size_t> first = ini.find_section("COMMAND")) {
		for (size_t i = *first; i < ini.line_count(); ++i) {
			const std::string line = ini.line(i, kIniCommandLineBytes);
			if (std::optional<CommandRow> row = read_row(line))
				file.rows.push_back(std::move(*row));
		}
	}
	return file;
}

std::string write_command_file(const CommandFile &file) {
	std::string out = "[HEADER]\r\n";
	const struct {
		std::string_view key;
		const std::string &value;
	} header[] = { { "EXE", file.exe }, { "PRODUCT", file.product }, { "VERSION", file.version } };
	for (const auto &row : header) {
		if (row.value.empty())
			continue;
		if (!header_value_carries(row.value) ||
				row.key.size() + 1 + row.value.size() >= kIniKeyLineBytes)
			return {};
		out.append(row.key).append("=").append(row.value).append("\r\n");
	}
	out.append("\r\n[COMMAND]\r\n");
	for (const CommandRow &row : file.rows) {
		const std::optional<CommandType> type = command_type_for_key(row.key);
		if (!type || *type != row.type || !name_carries(row.name))
			return {};
		std::string line = row.key + "=" + row.name;
		if (row.type == CommandType::Loose || row.type == CommandType::Copy) {
			line += "," + std::to_string(row.time) + "," + std::to_string(row.size);
			if (row.crc)
				line += "," + std::to_string(*row.crc);
		}
		if (line.size() >= kIniCommandLineBytes)
			return {};
		out.append(line).append("\r\n");
	}
	return out;
}

} // namespace opennova::update
