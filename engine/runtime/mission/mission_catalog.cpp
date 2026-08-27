#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include <formats/mission/bms.h>
#include <base/resource_index/resource_index.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::mission_catalog {

namespace {

// Retail's list order comparator [orig: Mission_CompareMapNames @ 0x5628e0 —
// stricmp over the two filenames].
bool file_less(const Row &a, const Row &b) {
	const char *pa = a.file.c_str();
	const char *pb = b.file.c_str();
	while (*pa && *pb) {
		const int ca = std::tolower(static_cast<unsigned char>(*pa));
		const int cb = std::tolower(static_cast<unsigned char>(*pb));
		if (ca != cb) return ca < cb;
		++pa;
		++pb;
	}
	return std::tolower(static_cast<unsigned char>(*pa)) <
			std::tolower(static_cast<unsigned char>(*pb));
}

bool ends_with_ci(const std::string &name, const char *suffix) {
	const size_t n = std::strlen(suffix);
	if (name.size() < n) return false;
	for (size_t i = 0; i < n; ++i) {
		if (std::tolower(static_cast<unsigned char>(name[name.size() - n + i])) !=
				std::tolower(static_cast<unsigned char>(suffix[i])))
			return false;
	}
	return true;
}

std::string bin_sibling_name(const std::string &file) {
	// [orig: Path_ReplaceOrAppendExtension(entry+527, "bin") @ 0x563170]
	const size_t dot = file.find_last_of('.');
	return (dot == std::string::npos ? file : file.substr(0, dot)) + ".bin";
}

std::string header_cstr(const char *field, size_t cap) {
	const size_t len = ::strnlen(field, cap);
	return std::string(field, len);
}

} // namespace

std::vector<Row> build(const ResourceIndex &index) {
	std::vector<Row> rows;
	for (const ResourceFileEntry &entry : index.resource_files("*")) {
		if (!ends_with_ci(entry.logical_name, ".bms")) continue;
		Row row;
		row.file = entry.logical_name;
		// Loose flag: retail's directory-scan pass stamps 1, the archive pass 0
		// [orig: entry+4380 — 1 @ 0x563170, 0 @ 0x562910]; the mount stack's
		// serving source carries the same fact.
		row.loose = entry.source_type == "file";

		// The 616-byte BMS header supplies the embedded name and the attrib
		// flags [orig: Mission_LoadBMSFromLooseFile/FromPFF fill headerBuf;
		// name = header+4, attribs = header+0x88].
		std::vector<uint8_t> bytes;
		bms::Header header{};
		bool header_ok = false;
		if (index.read_file(row.file, bytes) && bytes.size() >= bms::kHeaderSize) {
			std::memcpy(&header, bytes.data(), sizeof(header));
			header_ok = true;
		}

		// The single-select mode bit the code-word derivation consumes
		// [orig: AI_GetTaskTypeFromFlags(header+0x88) feeding the code-word
		// switch @ 0x5631f0.. / 0x562910..; the switch itself is
		// game_type::for_mission_mode at the net-linking consumer].
		row.game_mode = header_ok
				? bms::selected_game_mode(header.attrib_flags)
				: 0u;

		// Title + briefing: the sibling <mission>.bin text resource's [Info]
		// TITLE/BRIEFING entries. With a .bin present a missing TITLE leaves
		// the title EMPTY; only a missing .bin falls back to the header's
		// embedded mission_name [orig: the title arm @ 0x563170 — bin branch
		// vs strlen(headerBuf[1]) branch]. An unparseable .bin takes the
		// no-bin arm (our rtxt parse is deliberately strict — rtxt.h).
		std::vector<uint8_t> bin_bytes;
		rtxt::File bin;
		std::string error;
		if (index.read_file(bin_sibling_name(row.file), bin_bytes) &&
				rtxt::parse(bin_bytes.data(), bin_bytes.size(), bin, error)) {
			if (const rtxt::Entry *title = bin.find_in_section("Info", "TITLE"))
				row.title = title->text;
			if (const rtxt::Entry *briefing = bin.find_in_section("Info", "BRIEFING"))
				row.briefing = briefing->text;
		} else if (header_ok) {
			row.title = header_cstr(header.mission_name, sizeof(header.mission_name));
		}
		rows.push_back(std::move(row));
	}
	std::sort(rows.begin(), rows.end(), file_less);
	return rows;
}

std::string display_text(const Row &row) {
	// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9 ("*" for the loose
	// flag) + the +1044-empty fallback to entry+0 @ 0x56191e]
	std::string text = row.loose ? "*" : "";
	text += row.title.empty() ? row.file : row.title;
	return text;
}

} // namespace opennova::mission_catalog
