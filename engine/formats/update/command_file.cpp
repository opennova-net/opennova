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

// The row's line as the writer writes it, without its CR LF.
std::string row_line(const CommandRow &row) {
	std::string line = row.key + "=" + row.name;
	if (row.type == CommandType::Loose || row.type == CommandType::Copy) {
		line += "," + std::to_string(row.time) + "," + std::to_string(row.size);
		if (row.crc)
			line += "," + std::to_string(*row.crc);
	}
	return line;
}

// What the reader's strtok over " ,\r\n" takes for the name: the text's first token.
std::string_view first_token(std::string_view text) {
	const std::vector<std::string_view> all = tokens(text);
	return all.empty() ? std::string_view() : all.front();
}

std::string quoted(std::string_view text) {
	return "\"" + std::string(text) + "\"";
}

} // namespace

CommandRefusal command_name_refusal(std::string_view name) {
	if (name.empty())
		return CommandRefusal::NameEmpty;
	for (char c : name) {
		if (c == ' ' || c == ',')
			return CommandRefusal::NameSeparator;
		if (c == '\r' || c == '\n')
			return CommandRefusal::NameLineBreak;
	}
	return CommandRefusal::None;
}

CommandRefusal command_header_refusal(std::string_view key, std::string_view value) {
	if (value.empty())
		return CommandRefusal::None;
	for (char c : value) {
		if (c == '\r' || c == '\n')
			return CommandRefusal::HeaderLineBreak;
		if (c == ';')
			return CommandRefusal::HeaderSemicolon;
	}
	if (key.size() + 1 + value.size() >= kIniKeyLineBytes)
		return CommandRefusal::HeaderTooLong;
	return CommandRefusal::None;
}

CommandRefusal command_row_refusal(const CommandRow &row) {
	const std::optional<CommandType> type = command_type_for_key(row.key);
	if (!type || *type != row.type)
		return CommandRefusal::RowKey;
	if (const CommandRefusal name = command_name_refusal(row.name); name != CommandRefusal::None)
		return name;
	if (row_line(row).size() >= kIniCommandLineBytes)
		return CommandRefusal::RowTooLong;
	return CommandRefusal::None;
}

std::string command_refusal_words(CommandRefusal refusal, std::string_view text) {
	switch (refusal) {
	case CommandRefusal::None:
		return {};
	case CommandRefusal::NameEmpty:
		return "has no name: UPDATE.EXE's command-row reader finds none and skips the row "
			   "[orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE)]";
	case CommandRefusal::NameSeparator: {
		const size_t at = text.find_first_of(" ,");
		const std::string_view read = first_token(text);
		return std::string(at != std::string_view::npos && text[at] == ',' ? "has a comma"
																			: "has a space") +
				": UPDATE.EXE splits a command row's fields at spaces and commas, so it reads the "
				"name as " +
				(read.empty() ? std::string("nothing") : quoted(read)) +
				" and the rest as the row's other fields [orig: CmdFile_ReadCommandRow @ 0x404f60 "
				"(UPDATE.EXE), strtok \" ,\\r\\n\" @0x404ffd]";
	}
	case CommandRefusal::NameLineBreak:
		return "has a line break (a CR or an LF), where UPDATE.EXE's command-row reader ends the "
			   "name [orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE), strtok \" ,\\r\\n\" "
			   "@0x404ffd]";
	case CommandRefusal::RowTooLong:
		return "makes a row past the 255 characters UPDATE.EXE's command-row reader keeps of a "
			   "line, so it reads the row cut short [orig: CmdFile_ReadCommandRow @ 0x404f60 "
			   "(UPDATE.EXE), IniFile_ReadLine into 0x100 bytes @0x404f95]";
	case CommandRefusal::RowKey:
		return "has a key UPDATE.EXE's key table does not give its kind (UPD, RESEXE, REVEXE and "
			   "WIZ a loose file, COPY, REMOVE, TARGET), so the reader skips the row or reads it as "
			   "another [orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE); g_CmdKeyTable @ "
			   "0x4404c0 (UPDATE.EXE)]";
	case CommandRefusal::HeaderLineBreak:
		return "has a line break (a CR or an LF), where UPDATE.EXE's header reader ends the value "
			   "[orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE)]";
	case CommandRefusal::HeaderSemicolon:
		return "has a ';', where UPDATE.EXE's header reader ends the value, so it reads " +
				quoted(text.substr(0, text.find(';'))) +
				" [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE), %[^\\n\\r=]=%[^\\n\\r;]]";
	case CommandRefusal::HeaderTooLong:
		return "makes a line past the 127 characters UPDATE.EXE's header reader keeps of it, so it "
			   "reads the value cut short [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE); "
			   "Cupdate2Dlg_ReadCommandFile @ 0x4076e0 (UPDATE.EXE)]";
	}
	return {};
}

CommandFileProblem command_file_problem(const CommandFile &file) {
	CommandFileProblem problem;
	const struct {
		const char *key;
		const std::string &value;
	} header[] = { { "EXE", file.exe }, { "PRODUCT", file.product }, { "VERSION", file.version } };
	for (const auto &row : header) {
		problem.refusal = command_header_refusal(row.key, row.value);
		if (!problem.ok()) {
			problem.header = true;
			problem.key = row.key;
			problem.text = row.value;
			return problem;
		}
	}
	std::string target = ".";
	for (const CommandRow &row : file.rows) {
		problem.refusal = command_row_refusal(row);
		if (!problem.ok()) {
			problem.key = row.key;
			problem.text = row.name;
			problem.target = target;
			return problem;
		}
		if (row.type == CommandType::Target)
			target = row.name;
	}
	return problem;
}

std::string command_file_problem_words(const CommandFileProblem &problem) {
	if (problem.ok())
		return {};
	if (problem.header)
		return "the " + problem.key + " value " + quoted(problem.text) + " " +
				command_refusal_words(problem.refusal, problem.text);
	return "the " + problem.key + " row " + quoted(problem.text) +
			(problem.target.empty() || problem.target == "." ? std::string()
															 : " (in " + problem.target + ")") +
			" " + command_refusal_words(problem.refusal, problem.text);
}

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
	if (!command_file_problem(file).ok())
		return {};
	std::string out = "[HEADER]\r\n";
	const struct {
		std::string_view key;
		const std::string &value;
	} header[] = { { "EXE", file.exe }, { "PRODUCT", file.product }, { "VERSION", file.version } };
	for (const auto &row : header)
		if (!row.value.empty())
			out.append(row.key).append("=").append(row.value).append("\r\n");
	out.append("\r\n[COMMAND]\r\n");
	for (const CommandRow &row : file.rows)
		out.append(row_line(row)).append("\r\n");
	return out;
}

} // namespace opennova::update
