#pragma once

// The updater's command file (JO's JOPTR.CMD): what a product ships and where each file goes.
// docs/update/updater-re.md §6.
// [orig: Cupdate2Dlg_ReadCommandFile @ 0x4076e0 (UPDATE.EXE); CmdFile_ReadCommandRow @ 0x404f60
//  (UPDATE.EXE)]
//
// STAGED, NOT WIRED: consumed by apps/update (the client, ADR 0061 phase 3) and
// apps/novaworld_server/update (the service, phases 2 and 4); until those land only tests/update
// include it.

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
	std::string name; // the file, entry or target; no separator, comma or space
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

// The file read as UPDATE.EXE reads it: nullopt without a [HEADER] (error 10); the rows run
// from the line after [COMMAND] to the end of the file, a later section header being just
// another unknown line.
std::optional<CommandFile> read_command_file(std::string_view text);

// The file written from the model in canonical form: [HEADER] with EXE, PRODUCT, VERSION when
// set, a blank line, then [COMMAND] and one `KEY=name[,time,size[,crc]]` line per row, CR LF
// throughout. Empty when a row or value cannot be carried (a separator in a name, an unknown
// key, a line past the read buffers, a CR, LF or ';' in a header value).
std::string write_command_file(const CommandFile &file);

} // namespace opennova::update
