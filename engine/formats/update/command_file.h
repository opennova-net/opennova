#pragma once

// The updater's command file (JO's JOPTR.CMD): what a product ships and where each file goes.
// docs/update/updater-re.md §6.
// [orig: Cupdate2Dlg_ReadCommandFile @ 0x4076e0 (UPDATE.EXE); CmdFile_ReadCommandRow @ 0x404f60
//  (UPDATE.EXE)]

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// A row's kind, the UpdateCommandType the key table maps to. [orig: g_CmdKeyTable @ 0x4404c0
// (UPDATE.EXE)]
enum class CommandType : int32_t {
	Loose = 1, // UPD, RESEXE, REVEXE, WIZ: a file in the target directory
	Copy = 4, // COPY: an entry of the target .pff
	Remove = 5, // REMOVE: a name deleted from the target directory or .pff
	Target = 6, // TARGET: the directory or .pff the rows after it address
};

// The type the key table gives `key` (case-insensitive), nullopt for an unknown key, which the
// reader skips.
std::optional<CommandType> command_type_for_key(std::string_view key);

// One [COMMAND] row. `time` and `size` are required for Loose and Copy rows, which the reader
// otherwise skips; `crc` is optional. Both a Loose and a Copy row's crc is file_crc of the
// installed bytes (update_crc.h). Every number is a signed decimal on disk, since the client
// reads them with atol.
struct CommandRow {
	std::string key; // as written: UPD, RESEXE, REVEXE, WIZ, COPY, REMOVE, TARGET
	CommandType type = CommandType::Loose;
	std::string name; // the file, entry or target; no space, comma, CR or LF (command_name_refusal)
	int32_t time = 0; // a Unix time
	int32_t size = 0; // the installed file's size
	std::optional<int32_t> crc;
};

// The [HEADER] the dialog shows and runs, then the rows.
struct CommandFile {
	std::string exe; // EXE: launched at DONE when /RUN
	std::string
			product; // PRODUCT: the dialog's product line ("Your NovaLogic Product" when absent)
	std::string version; // VERSION
	std::vector<CommandRow> rows;
};

// The header's 128-byte fields. [orig: Cupdate2Dlg_ReadCommandFile @0x407728..0x407780
// (UPDATE.EXE)]
inline constexpr size_t kCommandHeaderValueBytes = 128;
// A row's name field. [orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE), the sprintf
// into UpdateCommand::name[260]]
inline constexpr size_t kCommandNameBytes = 260;

// Why UPDATE.EXE would not read a header value or a row back as it was written; None when it
// would. A [COMMAND] row is read a line of at most 255 characters at a time, its key taken with
// strtok up to the first '=' and its fields with strtok over " ,\r\n" [orig:
// CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE): IniFile_ReadLine into 0x100 bytes @0x404f95,
// strtok "=" @0x404faf, strtok " ,\r\n" @0x404ffd], so a '=' inside a name is kept (the second
// strtok does not split at it); a [HEADER] value as `%[^\n\r=]=%[^\n\r;]` over a line of at most
// 127 [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE)]. A line ends only at CR LF (update_ini.h).
enum class CommandRefusal {
	None,
	NameEmpty, // a row with no name: strtok finds none, and the reader skips the row
	NameSeparator, // a space or a comma in a name: the name ends there, the rest read as its fields
	NameLineBreak, // a CR or an LF in a name: the row's line ends there
	RowTooLong, // the row's line past the 255 characters the reader keeps of it
	RowKey, // a key the key table lacks, or one of another type than the row's [orig:
			// g_CmdKeyTable @ 0x4404c0 (UPDATE.EXE)]
	HeaderLineBreak, // a CR or an LF in a header value: its line ends there
	HeaderSemicolon, // a ';' in a header value: the value ends there
	HeaderTooLong, // a header line past the 127 characters the key lookup keeps of it
};

// The rule every row's name keeps, whatever its row: NameEmpty, NameSeparator, NameLineBreak or
// None. A name the release's package carries as a loose file's or an archive entry's row is one
// a build can check before any row is made (the editor's build gate, build.update_name).
CommandRefusal command_name_refusal(std::string_view name);
// The [HEADER] line `key=value`'s; None for an empty value, which the writer leaves out.
CommandRefusal command_header_refusal(std::string_view key, std::string_view value);
// The row's: its key, then its name, then its line as the writer would write it.
CommandRefusal command_row_refusal(const CommandRow &row);

// Why, in words to follow the refused text's own ("Audio Track.wav has a space, ..."), what
// UPDATE.EXE would read of `text` (the name or the value refused) and its witness cited; "" for
// None.
std::string command_refusal_words(CommandRefusal refusal, std::string_view text);

// The first header value or row of the file the command file cannot carry, in the writer's order.
struct CommandFileProblem {
	CommandRefusal refusal = CommandRefusal::None;
	bool header = false; // a [HEADER] value (`key` its key), else a [COMMAND] row
	std::string key; // EXE, PRODUCT, VERSION, or the row's key (COPY, UPD, ...)
	std::string text; // the header's value, or the row's name
	std::string target; // the TARGET a row follows ("." before any); "" for a header value
	bool ok() const { return refusal == CommandRefusal::None; }
};
CommandFileProblem command_file_problem(const CommandFile &file);
// The problem in a sentence's words, its subject first: `the COPY row "Audio Track.wav" (in
// RevX02.pff) has a space, ...`, `the PRODUCT value "A;B" has a ';', ...`; "" when it is ok().
std::string command_file_problem_words(const CommandFileProblem &problem);

// The file read as UPDATE.EXE reads it: nullopt without a [HEADER] (error 10); the rows run
// from the line after [COMMAND] to the end of the file, a later section header being just
// another unknown line.
std::optional<CommandFile> read_command_file(std::string_view text);

// The file written from the model in canonical form: [HEADER] with EXE, PRODUCT, VERSION when
// set, a blank line, then [COMMAND] and one `KEY=name[,time,size[,crc]]` line per row, CR LF
// throughout. Empty when command_file_problem finds a value or a row it cannot carry.
std::string write_command_file(const CommandFile &file);

} // namespace opennova::update
