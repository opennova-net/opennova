#pragma once

// A packed update's section applied to a product's tree: what UPDATE.EXE's PACK installs once
// the rows are fetched (docs/update/updater-re.md §7, §8), run over a directory in one pass with
// no precheck, no download and no second pass. The hosted fleet's data trees are built with it
// (a published release applied to a data set's baseline, ADR 0064) and our own update.exe will
// install with it (ADR 0061 phase 3). Service policy and tooling over the witnessed formats; the
// row semantics are the record's:
//
//   Loose   (UPD, RESEXE, REVEXE, WIZ) the row's blob, inflated (nlz.h), written to
//           <target>/<name> and stamped with the row's time (its mtime), which the precheck
//           compares. [orig: Cupdate2Dlg_InstallLooseRows @ 0x406860 (UPDATE.EXE)]
//   TARGET  the archive (a .pff at the tree's root) or the directory the rows after it
//           address; "." is the root, the target before any TARGET row.
//           [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE), the TARGET case]
//   COPY    an entry of the target archive, stored under the row's name, stamped with the
//           row's time and the file CRC of its bytes (pff_apply_edits, ADR 0061 d8); the
//           archive is written once, when the target changes or the rows end. D-UPD-1: in
//           process, never pack.exe. [orig: Pack_Tick @ 0x402a70 (UPDATE.EXE)]
//   REMOVE  the name taken out of the target archive, or the file deleted under a directory
//           target; a name that is not there is nothing to do.
//
// Before a row is applied its blob must be in the section, inflate to the row's size and, when
// the row carries a crc, match it (update_crc.h file_crc), and every name must be a bare file
// name (no separator, no "." or ".."; D-UPD-3's hardening): one bad row applies nothing.

#include "command_file.h"
#include "sfs.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// What apply_section did, as tree-relative generic paths ("a.txt", "expansion/x/version.txt"
// never: a section addresses one product root).
struct ApplyReport {
	std::vector<std::string> files_written; // loose rows
	std::vector<std::string> archives_rewritten; // the .pff targets whose entries changed
	std::vector<std::string> files_deleted; // REMOVE rows under a directory target
	size_t rows_applied = 0;
};

// The section's command file (its entry named `command_file`, case-insensitive) read, every
// row checked against the section's blobs, then every row applied in order to the tree at
// `dir`. False with `error` and nothing written when the command file is missing or does not
// read, a row's blob is missing, short, corrupt or of the wrong crc, or a name is unsafe; false
// with `error` after a write failed (the tree is then partly updated, as a stock install cut
// short would be).
bool apply_section(const SfsSection &section, std::string_view command_file,
		const std::filesystem::path &dir, ApplyReport &report, std::string &error);

// Whether `name` may name a file or entry of the tree: not empty, no '/', '\\' or ':', not "."
// or "..", at most kCommandNameBytes - 1 bytes.
bool is_safe_row_name(std::string_view name);

} // namespace opennova::update
