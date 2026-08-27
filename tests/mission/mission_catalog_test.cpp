// The front-end mission catalog [orig: MissionList_ScanAndBuildFromFiles
// @ 0x563170 + Mission_BuildMapListFromPFF @ 0x562910]: titles from the
// sibling .bin's [Info] TITLE (header mission_name only when NO .bin), the
// briefing text, the loose-vs-archive flag behind the SP list's "*" prefix,
// the single-select game-mode extraction, and the stricmp-by-filename order.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include <formats/mission/bms.h>
#include <runtime/mission/mission_catalog.h>
#include <base/resource_index/resource_index.h>
#include <formats/rtxt/rtxt.h>

namespace {

namespace fs = std::filesystem;

void append_u32_le(std::vector<uint8_t> &bytes, uint32_t value) {
	bytes.push_back(static_cast<uint8_t>(value & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

std::vector<uint8_t> bms_blob(const std::string &mission_name, uint32_t attribs) {
	std::vector<uint8_t> bytes(opennova::bms::kHeaderSize, 0);
	bytes[0] = 'B';
	bytes[1] = 'M';
	bytes[2] = 'S';
	bytes[3] = opennova::bms::kMinVersion;
	std::memcpy(bytes.data() + 4, mission_name.c_str(),
			std::min<size_t>(mission_name.size(), 31));
	std::memcpy(bytes.data() + 136, &attribs, sizeof(attribs));
	return bytes;
}

std::vector<uint8_t> info_bin(const std::string &title, const std::string &briefing,
		bool include_title = true) {
	opennova::rtxt::File file;
	opennova::rtxt::Section info;
	info.name = "Info";
	file.sections.push_back(info);
	if (include_title) {
		opennova::rtxt::Entry entry;
		entry.key = "TITLE";
		entry.text = title;
		entry.section_index = 0;
		file.entries.push_back(entry);
	}
	if (!briefing.empty()) {
		opennova::rtxt::Entry entry;
		entry.key = "BRIEFING";
		entry.text = briefing;
		entry.section_index = 0;
		file.entries.push_back(entry);
	}
	file.normalize_grouping();
	std::vector<uint8_t> out;
	std::string error;
	if (!opennova::rtxt::write(file, out, error)) {
		std::fprintf(stderr, "rtxt write failed: %s\n", error.c_str());
		std::exit(1);
	}
	return out;
}

void write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes) {
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
}

struct PffFixtureEntry {
	std::string name;
	std::vector<uint8_t> bytes;
};

// PFF3 fixture writer (the resource_index_test recipe).
void write_pff(const fs::path &path, const std::vector<PffFixtureEntry> &entries) {
	fs::create_directories(path.parent_path());
	const uint32_t header_size = 20;
	const uint32_t entry_size = 36;
	const uint32_t payload_offset =
			header_size + static_cast<uint32_t>(entries.size()) * entry_size;
	uint32_t next_payload_offset = payload_offset;
	std::vector<uint8_t> bytes;
	append_u32_le(bytes, header_size);
	append_u32_le(bytes, 0x33464650u);
	append_u32_le(bytes, static_cast<uint32_t>(entries.size()));
	append_u32_le(bytes, entry_size);
	append_u32_le(bytes, header_size);
	for (const PffFixtureEntry &entry : entries) {
		append_u32_le(bytes, 0);
		append_u32_le(bytes, next_payload_offset);
		append_u32_le(bytes, static_cast<uint32_t>(entry.bytes.size()));
		append_u32_le(bytes, 0);
		for (size_t i = 0; i < 16; ++i) {
			bytes.push_back(i < entry.name.size()
					? static_cast<uint8_t>(entry.name[i])
					: 0);
		}
		append_u32_le(bytes, 0);
		next_payload_offset += static_cast<uint32_t>(entry.bytes.size());
	}
	for (const PffFixtureEntry &entry : entries) {
		bytes.insert(bytes.end(), entry.bytes.begin(), entry.bytes.end());
	}
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
}

const opennova::mission_catalog::Row *find_row(
		const std::vector<opennova::mission_catalog::Row> &rows,
		const std::string &file) {
	for (const opennova::mission_catalog::Row &row : rows) {
		if (row.file == file) return &row;
	}
	return nullptr;
}

} // namespace

int main() {
	using opennova::bms::AttribFlags;
	namespace catalog = opennova::mission_catalog;

	const fs::path root = fs::temp_directory_path() / "opennova_mission_catalog_test";
	fs::remove_all(root);
	fs::create_directories(root);

	// Loose missions: one fully .bin-titled, one header-name fallback, one
	// .bin without a TITLE key (title stays EMPTY — the header name only
	// stands in when NO .bin exists at all).
	write_bytes(root / "zulu.bms",
			bms_blob("Zulu Header Name",
					static_cast<uint32_t>(AttribFlags::Coop)));
	write_bytes(root / "zulu.bin", info_bin("Zulu Strike", "Take the island."));
	write_bytes(root / "alpha.bms",
			bms_blob("Alpha Header Name",
					static_cast<uint32_t>(AttribFlags::TeamDeathmatch)));
	write_bytes(root / "mike.bms", bms_blob("Mike Header Name", 0));
	write_bytes(root / "mike.bin",
			info_bin("", "Briefing without a title.", /*include_title=*/false));

	// Archived mission + its archived .bin sibling: not loose, titled.
	write_pff(root / "resource.pff", {
			{"pack.bms", bms_blob("Pack Header Name", 0)},
			{"pack.bin", info_bin("Packed Op", "From the archive.")},
	});

	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(root.string()));

	const std::vector<catalog::Row> rows = catalog::build(index);
	TEST_EXPECT(rows.size() == 4u);

	// Retail order: case-insensitive by FILENAME
	// [orig: Mission_CompareMapNames @ 0x5628e0].
	TEST_EXPECT(rows[0].file == "alpha.bms");
	TEST_EXPECT(rows[1].file == "mike.bms");
	TEST_EXPECT(rows[2].file == "pack.bms");
	TEST_EXPECT(rows[3].file == "zulu.bms");

	const catalog::Row *zulu = find_row(rows, "zulu.bms");
	TEST_EXPECT(zulu != nullptr);
	TEST_EXPECT(zulu->title == "Zulu Strike");
	TEST_EXPECT(zulu->briefing == "Take the island.");
	TEST_EXPECT(zulu->loose);
	TEST_EXPECT(zulu->game_mode == static_cast<uint32_t>(AttribFlags::Coop));
	// Loose + titled: the "*" marker leads the title
	// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9].
	TEST_EXPECT(catalog::display_text(*zulu) == "*Zulu Strike");

	// No .bin: the header's embedded mission_name stands in.
	const catalog::Row *alpha = find_row(rows, "alpha.bms");
	TEST_EXPECT(alpha != nullptr);
	TEST_EXPECT(alpha->title == "Alpha Header Name");
	TEST_EXPECT(alpha->briefing.empty());
	TEST_EXPECT(alpha->game_mode ==
			static_cast<uint32_t>(AttribFlags::TeamDeathmatch));

	// .bin present but TITLE-less: the title stays empty and the row text
	// falls back to the FILENAME [orig: the +1044-empty arm @ 0x56191e].
	const catalog::Row *mike = find_row(rows, "mike.bms");
	TEST_EXPECT(mike != nullptr);
	TEST_EXPECT(mike->title.empty());
	TEST_EXPECT(mike->briefing == "Briefing without a title.");
	TEST_EXPECT(mike->game_mode == 0u);
	TEST_EXPECT(catalog::display_text(*mike) == "*mike.bms");

	// Archive entries carry no "*" and resolve their archived .bin.
	const catalog::Row *pack = find_row(rows, "pack.bms");
	TEST_EXPECT(pack != nullptr);
	TEST_EXPECT(!pack->loose);
	TEST_EXPECT(pack->title == "Packed Op");
	TEST_EXPECT(pack->briefing == "From the archive.");
	TEST_EXPECT(catalog::display_text(*pack) == "Packed Op");

	// Release the mounted archive handle before deleting the fixture tree
	// (an open resource.pff makes remove_all throw).
	index.clear();
	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	return 0;
}
