#include "server_list.h"

#include "update_ini.h"

namespace opennova::update {

namespace {

bool is_separator(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

// [orig: Cupdate2Dlg_ReadServerList @ 0x408070 (UPDATE.EXE): IniFile_ReadLine over the file, the
// first strtok token of each line]
std::vector<std::string> read_server_list(std::string_view text) {
	const UpdateIni ini(text);
	std::vector<std::string> hosts;
	for (size_t i = 0; i < ini.line_count(); ++i) {
		const std::string line = ini.line(i, kIniServerLineBytes);
		size_t start = 0;
		while (start < line.size() && is_separator(line[start]))
			++start;
		size_t end = start;
		while (end < line.size() && !is_separator(line[end]))
			++end;
		if (end > start)
			hosts.push_back(line.substr(start, end - start));
	}
	return hosts;
}

std::string write_server_list(const std::vector<std::string> &hosts) {
	std::string out;
	for (const std::string &host : hosts) {
		if (host.empty() || host.size() >= kIniServerLineBytes)
			return {};
		for (char c : host)
			if (is_separator(c))
				return {};
		out.append(host).append("\r\n");
	}
	return out;
}

} // namespace opennova::update
