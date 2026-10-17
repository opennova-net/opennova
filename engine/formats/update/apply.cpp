#include "apply.h"

#include "nlz.h"
#include "update_crc.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/pff/pff.h>

#include <chrono>
#include <map>
#include <system_error>

namespace opennova::update {

namespace fs = std::filesystem;

namespace {

bool is_archive_name(std::string_view name) {
	return strutil::ends_with_icase(name, ".pff");
}

// The section's entry named `name`, case-insensitively (the blobs land in one flat update
// directory, where the stock install opens them by the row's name).
const SfsEntry *find_entry(const SfsSection &section, std::string_view name) {
	for (const SfsEntry &entry : section.entries)
		if (strutil::iequals(entry.name, name))
			return &entry;
	return nullptr;
}

// A row's installed bytes: its blob inflated, checked against the row's size and crc.
bool row_content(const SfsSection &section, const CommandRow &row, std::vector<uint8_t> &out,
		std::string &error) {
	const SfsEntry *entry = find_entry(section, row.name);
	if (entry == nullptr) {
		error = "the package has no file for the row " + row.name;
		return false;
	}
	if (nlz_decode(entry->data.data(), entry->data.size(), out) == NlzDecode::Corrupt) {
		error = "the file for the row " + row.name + " does not inflate";
		return false;
	}
	if (out.size() != static_cast<size_t>(row.size) || row.size < 0) {
		error = "the file for the row " + row.name + " is " + std::to_string(out.size()) +
				" bytes where its row says " + std::to_string(row.size);
		return false;
	}
	if (row.crc && *row.crc != 0 && *row.crc != crc_as_signed(file_crc(out.data(), out.size()))) {
		error = "the file for the row " + row.name + " does not match its row's crc";
		return false;
	}
	return true;
}

// `seconds` since the Unix epoch as the file clock's time (the inverse of
// io::file_modified_unix_seconds's offset trick).
fs::file_time_type file_time_from_unix(int64_t seconds) {
	const auto system_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(seconds));
	return fs::file_time_type::clock::now() +
			std::chrono::duration_cast<fs::file_time_type::duration>(
					system_time - std::chrono::system_clock::now());
}

// The edits queued for one archive target, written when the target changes or the rows end.
struct ArchiveEdits {
	std::vector<pff::PffEditEntry> adds;
	std::vector<std::string> removes;
};

// The archive's queued adds and removes written in one pass (pff_apply_edits), where the
// stock updater spawns pack.exe per job [orig: Pack_Tick @ 0x402a70 (UPDATE.EXE)] (D-UPD-1).
bool flush_archive(const fs::path &dir, const std::string &archive, ArchiveEdits &edits,
		ApplyReport &report, std::string &error) {
	if (edits.adds.empty() && edits.removes.empty())
		return true;
	const std::string path = io::utf8_path(dir / io::os_path(archive));
	bool rewritten = false;
	const int rc = pff::pff_apply_edits(path.c_str(), edits.adds, edits.removes, &rewritten);
	if (rc != pff::PFF_WRITE_OK) {
		error = "the archive " + archive + " could not be written (" + std::to_string(rc) + ")";
		return false;
	}
	if (rewritten)
		report.archives_rewritten.push_back(archive);
	edits = ArchiveEdits{};
	return true;
}

} // namespace

bool is_safe_row_name(std::string_view name) {
	if (name.empty() || name.size() >= kCommandNameBytes)
		return false;
	if (name == "." || name == "..")
		return false;
	for (const char c : name)
		if (c == '/' || c == '\\' || c == ':' || c == '\0')
			return false;
	return true;
}

bool apply_section(const SfsSection &section, std::string_view command_file,
		const fs::path &dir, ApplyReport &report, std::string &error) {
	report = ApplyReport{};
	const SfsEntry *cmd_entry = find_entry(section, command_file);
	if (cmd_entry == nullptr) {
		error = "the package has no command file " + std::string(command_file);
		return false;
	}
	const std::optional<CommandFile> cmd = read_command_file(
			std::string(cmd_entry->data.begin(), cmd_entry->data.end()));
	if (!cmd) {
		error = "the command file " + std::string(command_file) + " does not read";
		return false;
	}

	// Every row checked before any is applied.
	std::map<size_t, std::vector<uint8_t>> contents; // by row index
	for (size_t i = 0; i < cmd->rows.size(); ++i) {
		const CommandRow &row = cmd->rows[i];
		if (!is_safe_row_name(row.name)) {
			error = "the row \"" + row.name + "\" does not name a file of the tree";
			return false;
		}
		if (row.type == CommandType::Loose || row.type == CommandType::Copy) {
			std::vector<uint8_t> content;
			if (!row_content(section, row, content, error))
				return false;
			contents[i] = std::move(content);
		}
	}

	std::error_code ec;
	fs::create_directories(dir, ec);
	std::string target = "."; // the root until a TARGET row
	ArchiveEdits edits;
	for (size_t i = 0; i < cmd->rows.size(); ++i) {
		const CommandRow &row = cmd->rows[i];
		const bool archive_target = is_archive_name(target);
		switch (row.type) {
			case CommandType::Target:
				if (archive_target && !flush_archive(dir, target, edits, report, error))
					return false;
				target = row.name;
				break;
			case CommandType::Loose: {
				// [orig: Cupdate2Dlg_InstallLooseRows @ 0x406860 (UPDATE.EXE)]: the row's file
				// written under its name, stamped with the row's time.
				if (archive_target) {
					error = "the loose row " + row.name + " follows the archive target " + target;
					return false;
				}
				const fs::path parent = target == "." ? dir : dir / io::os_path(target);
				fs::create_directories(parent, ec);
				const fs::path path = parent / io::os_path(row.name);
				const std::vector<uint8_t> &content = contents[i];
				if (!io::write_file_atomic(io::utf8_path(path), content.data(), content.size(), error))
					return false;
				fs::last_write_time(path, file_time_from_unix(row.time), ec);
				report.files_written.push_back(
						target == "." ? row.name : target + "/" + row.name);
				break;
			}
			case CommandType::Copy:
				if (!archive_target) {
					error = "the COPY row " + row.name + " has no archive target";
					return false;
				}
				edits.adds.push_back({ row.name, std::move(contents[i]),
						static_cast<uint32_t>(row.time) });
				break;
			case CommandType::Remove:
				if (archive_target) {
					edits.removes.push_back(row.name);
				} else {
					const fs::path parent = target == "." ? dir : dir / io::os_path(target);
					const fs::path path = parent / io::os_path(row.name);
					if (fs::exists(path, ec)) {
						if (!fs::remove(path, ec) || ec) {
							error = "the file " + row.name + " could not be deleted";
							return false;
						}
						report.files_deleted.push_back(
								target == "." ? row.name : target + "/" + row.name);
					}
				}
				break;
		}
		++report.rows_applied;
	}
	if (is_archive_name(target) && !flush_archive(dir, target, edits, report, error))
		return false;
	return true;
}

} // namespace opennova::update
