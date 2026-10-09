#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>

#include <base/gameprofile/game_type.h>
#include <base/io/log.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/mission/bms.h>
#include <base/resource_index/resource_index.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::mission_catalog {

namespace {

namespace fs = std::filesystem;

// Retail's list order comparator [orig: Mission_CompareMapNames @ 0x5628e0 —
// stricmp over the two filenames].
bool file_less(const Row &a, const Row &b) {
	return strutil::iless(a.file, b.file);
}

// [orig: Path_ReplaceOrAppendExtension @ 0x53c780, called with "bin" @ 0x56345b and
//  @ 0x562c1b — the extension after the FIRST '.' is replaced, ".bin" appended when
//  there is none]
std::string bin_sibling_name(const std::string &file) {
	const size_t dot = file.find('.');
	return (dot == std::string::npos ? file : file.substr(0, dot)) + ".bin";
}

// [orig: File_HasExtension @ 0x53c640 — stricmp from the FIRST '.' on, so "a.b.bms"
//  is not a ".bms"; a name with no '.' compares the empty tail]
bool has_extension(const std::string &name, const char *extension) {
	const size_t dot = name.find('.');
	return strutil::iequals(dot == std::string::npos ? std::string() : name.substr(dot),
			extension);
}

std::string header_cstr(const char *field, size_t cap) {
	const size_t len = ::strnlen(field, cap);
	return std::string(field, len);
}

// The 616-byte header the scan keeps: read only when the file opens 'B', 'M', 'S' and a
// version byte of at least 19 read SIGNED, else zeroed (the bytes past a short file stay
// zero, the read's memset).
// [orig: Mission_LoadBMSFromLooseFile @ 0x40d520 / Mission_LoadBMSFromPFF @ 0x40d420 —
//  memset(header, 0, 0x268), the magic and `(char)header[3] >= 19` test, the zeroing
//  on a mismatch]
bool read_header(const std::vector<uint8_t> &bytes, bms::Header &header) {
	header = bms::Header{};
	if (bytes.size() < 4 || bytes[0] != 'B' || bytes[1] != 'M' || bytes[2] != 'S' ||
			static_cast<int8_t>(bytes[3]) < static_cast<int8_t>(bms::kMinVersion))
		return false;
	std::memcpy(&header, bytes.data(), std::min(bytes.size(), sizeof(header)));
	return true;
}

// The install root's file names, lower case: what File_CheckExists can open by a bare
// name. Listed once per build (an extract's flat root holds thousands of files, and
// the loose leg asks once per mission); a listing the filesystem stops partway keeps
// the names it read and says so.
// [orig: File_CheckExists @ 0x75a5d0 — a raw `_lopen` of the bare name, so against the
//  working directory (the install's root) whatever the mount; case-insensitive, as the
//  filesystem retail ran on]
std::unordered_set<std::string> root_file_names(const std::string &root) {
	std::unordered_set<std::string> names;
	if (root.empty()) return names;
	std::error_code ec;
	fs::directory_iterator it(io::os_path(root), fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		std::error_code type_ec;
		if (it->is_regular_file(type_ec))
			names.insert(strutil::to_lower(io::utf8_path(it->path().filename())));
	}
	if (ec) {
		io::logf(io::LogLevel::kWarn, "mission catalog: listing %s stopped (%s); %zu names read",
				root.c_str(), ec.message().c_str(), names.size());
	}
	return names;
}

// One row from its header and its text table: the game-mode word, then the title and the
// briefing. With a table loaded a missing TITLE leaves the title EMPTY; without one the
// header's embedded mission_name stands in, and an empty name leaves it empty too.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 — AI_GetTaskTypeFromFlags(header
//  +0x88) feeding the code-word switch @ 0x563387..; the title arm @ 0x563489..0x56354f;
//  Mission_BuildMapListFromPFF @ 0x562910 the same @ 0x562b4b.. and @ 0x562c55..0x562d1f]
void fill_row(Row &row, const bms::Header &header, bool header_ok, const rtxt::File *text) {
	// The single-select mode bit the code-word derivation consumes (the switch itself is
	// game_type::for_mission_mode, game_type_of).
	row.game_mode = header_ok ? bms::selected_game_mode(header.attrib_flags) : 0u;
	if (text != nullptr) {
		if (const rtxt::Entry *title = text->find_in_section("Info", "TITLE"))
			row.title = title->text;
		if (const rtxt::Entry *briefing = text->find_in_section("Info", "BRIEFING"))
			row.briefing = briefing->text;
	} else {
		row.title = header_cstr(header.mission_name, sizeof(header.mission_name));
	}
}

// The text table by name through the front door, the session's lookup order
// [orig: TextResource_LoadFromArchive @ 0x75d0b0 -> File_LoadResource @ 0x75b540]. Our
// rtxt parse is deliberately strict (rtxt.h): an unparseable table counts as none.
bool load_text(const ResourceIndex &index, const std::string &name, rtxt::File &out) {
	std::vector<uint8_t> bytes;
	std::string error;
	return index.read_file(name, bytes, VfsLookupPolicy::SessionDefault) &&
			rtxt::parse(bytes.data(), bytes.size(), out, error);
}

// One archive pair: every `.bms` entry of the mission archive, its header read by name
// from the archives (the lowest slot holding it), titled only when the pair's own text
// archive holds its `.bin`, which then reads by name like any table. A missing mission
// archive lists nothing. `.npj`/`.npz` map projects are not listed (D-MNU-25: OpenNova
// cannot load one).
// [orig: Mission_BuildMapListFromPFF @ 0x562910 — the entry walk @ 0x56295f..0x562d51,
//  File_HasExtension(".bms"/".npj"/".npz") @ 0x5629a0/@ 0x5629bc/@ 0x5629d8,
//  Mission_LoadBMSFromPFF
//  @ 0x562aa7 (archive-only, @ 0x40d43c), the loose flag 0 @ 0x562b1b,
//  `textArchive && PFF_FileExists(bin, textArchive)` @ 0x562c2d before
//  TextResource_LoadFromArchive @ 0x562c47]
void walk_pair(const ResourceIndex &index, int mission_slot, int text_slot,
		std::vector<Row> &rows) {
	for (const VfsArchiveEntry &entry : index.archive_slot_entries(mission_slot)) {
		if (!has_extension(entry.name, ".bms")) continue;
		Row row;
		row.file = entry.name;
		row.loose = false;
		std::vector<uint8_t> bytes;
		bms::Header header{};
		const bool header_ok = index.read_file(row.file, bytes, VfsLookupPolicy::ForceArchiveOnly) &&
				read_header(bytes, header);
		const std::string bin = bin_sibling_name(row.file);
		rtxt::File text;
		const bool text_ok = index.archive_slot_has_file(text_slot, bin) && load_text(index, bin, text);
		fill_row(row, header, header_ok, text_ok ? &text : nullptr);
		rows.push_back(std::move(row));
	}
}

} // namespace

std::vector<Row> build(const ResourceIndex &index) {
	std::vector<Row> rows;
	// The loose leg: the mount's loose `.bms` files, flagged loose. Its table is read
	// only when the `.bin` exists loose in the install's root, then by name through the
	// front door. [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 — FindFirstFile
	//  "*.bms" @ 0x563222, Mission_LoadBMSFromLooseFile @ 0x5632e7, the loose flag 1
	//  @ 0x563353, File_CheckExists(bin) @ 0x563461 before TextResource_LoadFromArchive
	//  @ 0x56347b]
	std::optional<std::unordered_set<std::string>> root_names; // listed on first need
	std::vector<std::string> loose;
	for (const ResourceFileEntry &entry : index.resource_files("*"))
		if (entry.source_type == "file") loose.push_back(entry.logical_name);
	rows = loose_rows(loose,
			[&](const std::string &name, std::vector<uint8_t> &bytes) {
				return index.read_file(name, bytes, VfsLookupPolicy::ForceLooseFirst);
			},
			[&](const std::string &bin, rtxt::File &text) {
				if (!root_names) root_names = root_file_names(index.root_dir());
				return root_names->count(strutil::to_lower(bin)) != 0 && load_text(index, bin, text);
			});
	// Then the two archive pairs: the expansion's <n>.pff with <n>L.pff, then
	// localres.pff with language.pff. Slot 4 (resource.pff) is never walked, a `.bms` in
	// <n>L.pff never listed, and a name both pairs carry lists once per pair.
	// [orig: MissionList_ScanAndBuildFromFiles @ 0x5635a5..0x5635d8 —
	//  Mission_BuildMapListFromPFF(slot 1, slot 0), then (slot 3, slot 2)]
	walk_pair(index, kArchiveSlotExpansion, kArchiveSlotExpansionText, rows);
	walk_pair(index, kArchiveSlotLocalres, kArchiveSlotLanguage, rows);
	// [orig: qsort(list, count, 0x11E8, Mission_CompareMapNames) @ 0x5635f0, no dedupe]
	// The CRT's qsort is a platform primitive the port does not reproduce, and it is not
	// stable: two rows with the same name (a loose copy of an archived mission, or one
	// name in both pairs) come out in its order in retail, in walk order here.
	sort_rows(rows);
	return rows;
}

Row loose_row(const std::string &file, const std::vector<uint8_t> &bms, const rtxt::File *text) {
	Row row;
	row.file = file;
	row.loose = true;
	bms::Header header{};
	const bool header_ok = read_header(bms, header);
	fill_row(row, header, header_ok, text);
	return row;
}

std::vector<Row> loose_rows(const std::vector<std::string> &names, const ReadBms &read_bms,
		const ReadText &read_text) {
	std::vector<Row> rows;
	for (const std::string &name : names) {
		if (!strutil::ends_with_icase(name, ".bms")) continue;
		std::vector<uint8_t> bytes;
		if (!read_bms || !read_bms(name, bytes)) bytes.clear();
		rtxt::File text;
		const bool text_ok = read_text && read_text(bin_sibling_name(name), text);
		rows.push_back(loose_row(name, bytes, text_ok ? &text : nullptr));
	}
	return rows;
}

void sort_rows(std::vector<Row> &rows) {
	std::stable_sort(rows.begin(), rows.end(), file_less);
}

uint32_t game_type_of(const Row &row) {
	return game_type::for_mission_mode(row.game_mode);
}

std::string text_table_name(const std::string &file) {
	return bin_sibling_name(file);
}

std::string display_text(const Row &row) {
	// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9 ("*" for the loose
	// flag) + the +1044-empty fallback to entry+0 @ 0x56191e]
	std::string text = row.loose ? "*" : "";
	text += row.title.empty() ? row.file : row.title;
	return text;
}

} // namespace opennova::mission_catalog
