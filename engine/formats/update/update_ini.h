#pragma once

// The updater's one INI reader, behind every text file UPDATE.EXE reads (the wizard, the command
// file, SERVER.LST). It is not the game's ConfigFile (formats/configfile); the dialect is
// docs/update/updater-re.md §2.
// [orig: IniFile_Load @ 0x403840 (UPDATE.EXE); IniFile_ReadLine @ 0x403990 (UPDATE.EXE);
//  IniFile_FindSection @ 0x403a60 (UPDATE.EXE); IniFile_GetValue @ 0x403b90 (UPDATE.EXE)]

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// The read buffer each caller hands IniFile_ReadLine: a key lookup's 128, a command row's 256,
// a server-list line's 260. A line keeps at most one less. [orig: IniFile_GetValue @ 0x403b90
// (UPDATE.EXE); CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE); Cupdate2Dlg_ReadServerList
// @ 0x408070 (UPDATE.EXE)]
inline constexpr size_t kIniKeyLineBytes = 128;
inline constexpr size_t kIniCommandLineBytes = 256;
inline constexpr size_t kIniServerLineBytes = 260;

class UpdateIni {
public:
	// The file's lines, split at CR LF only (a lone LF stays inside its line, where the retail
	// patterns stop reading), a last line without CR LF kept when it holds anything.
	// [orig: IniFile_Load @ 0x403840 (UPDATE.EXE)]
	explicit UpdateIni(std::string_view text);

	size_t line_count() const { return lines_.size(); }
	// Line `index` as a caller with a `buffer`-byte read buffer sees it.
	std::string line(size_t index, size_t buffer) const;

	// The index of the line after `[section]`, the first header whose upper-case run
	// (`[%[A-Z_0123456789]`) equals `section` upper-cased; nullopt when none does.
	// [orig: IniFile_FindSection @ 0x403a60 (UPDATE.EXE)]
	std::optional<size_t> find_section(std::string_view section) const;

	// `key`'s value in `section`, read as `%[^\n\r=]=%[^\n\r;]`: the key's trailing spaces
	// trimmed and compared case-sensitively with `key` upper-cased (so a file's keys are upper
	// case), the value's leading spaces skipped, `;` ending it, `max_bytes` - 1 characters kept.
	// The search stops at the next line that opens a section. nullopt when the section or the
	// key is absent. [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE)]
	std::optional<std::string> value(
			std::string_view section, std::string_view key, size_t max_bytes) const;

private:
	std::vector<std::string> lines_;
};

// ASCII upper case, as the CRT's _strupr does it under the C locale.
std::string ascii_upper(std::string_view text);

} // namespace opennova::update
