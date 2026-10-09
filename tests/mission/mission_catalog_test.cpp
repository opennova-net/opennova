// The front-end mission catalog [orig: MissionList_ScanAndBuildFromFiles
// @ 0x563170 + Mission_BuildMapListFromPFF @ 0x562910]: the loose walk, then
// the two archive pairs (<n>.pff with <n>L.pff, localres.pff with
// language.pff), each row titled from its own source's .bin (a loose .bin in
// the root for a loose row, the pair's text archive for an archived one; the
// header mission_name only when no table loaded), the briefing text, the
// loose-vs-archive flag behind the SP list's "*" prefix, the header's magic
// gate, the single-select game-mode extraction, and the stricmp-by-filename
// order with no dedupe.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/pff/pff.h>
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
	info.name = opennova::mission_catalog::kTextInfoSection;
	file.sections.push_back(info);
	if (include_title) {
		opennova::rtxt::Entry entry;
		entry.key = opennova::mission_catalog::kTextTitleKey;
		entry.text = title;
		entry.section_index = 0;
		file.entries.push_back(entry);
	}
	if (!briefing.empty()) {
		opennova::rtxt::Entry entry;
		entry.key = opennova::mission_catalog::kTextBriefingKey;
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

// PFF3 fixture writer (the resource_index_test recipe), every entry stamped as the
// writers stamp a new one (pff::PFF_NEW_ENTRY_TIMESTAMP, D-VFS-12).
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
		append_u32_le(bytes, opennova::pff::PFF_NEW_ENTRY_TIMESTAMP);
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
		const std::string &file, bool loose) {
	for (const opennova::mission_catalog::Row &row : rows) {
		if (row.file == file && row.loose == loose) return &row;
	}
	return nullptr;
}

int count_rows(const std::vector<opennova::mission_catalog::Row> &rows, const std::string &file) {
	int count = 0;
	for (const opennova::mission_catalog::Row &row : rows) count += row.file == file ? 1 : 0;
	return count;
}

} // namespace

int main() {
	using opennova::bms::AttribFlags;
	namespace catalog = opennova::mission_catalog;

	const fs::path root = fs::temp_directory_path() / test_paths_unique("opennova_mission_catalog_test");
	fs::remove_all(root);
	fs::create_directories(root);

	// Loose missions: one fully .bin-titled, one header-name fallback, one
	// .bin without a TITLE key (title stays EMPTY — the header name only
	// stands in when NO table loaded), and one whose .bin exists only in an
	// archive: a loose row's table is read only when its .bin exists LOOSE in
	// the root [orig: File_CheckExists @ 0x563461], so it takes the header name.
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
	write_bytes(root / "kilo.bms", bms_blob("Kilo Header Name", 0));
	// The root's names compare case-insensitively, as File_CheckExists's open did on
	// the filesystem retail ran on: an upper-case .bin titles its mission.
	write_bytes(root / "oscar.bms", bms_blob("Oscar Header Name", 0));
	write_bytes(root / "OSCAR.BIN", info_bin("Oscar Op", "Upper-case table."));

	std::vector<uint8_t> bad_magic = bms_blob("Bad Header Name", 0);
	bad_magic[3] = static_cast<uint8_t>(0x80); // a version byte read signed: below 19
	// The base pair: localres.pff's missions, titled from language.pff.
	write_pff(root / "localres.pff", {
			{"pack.bms", bms_blob("Pack Header Name", 0)},
			{"lonely.bms", bms_blob("Lonely Header Name", 0)},
			{"lonely.bin", info_bin("Lonely Op", "Wrong archive.")}, // not its pair's text archive
			{"bad.bms", bad_magic},
			{"asp_g7.npz", {1, 2, 3}},     // a map project: not listed (D-MNU-25)
			{"a.b.bms", bms_blob("Two Dots", 0)}, // File_HasExtension reads from the FIRST '.'
	});
	write_pff(root / "language.pff", {
			{"pack.bin", info_bin("Packed Op", "From the archive.")},
			{"kilo.bin", info_bin("Kilo Archived", "")},
	});
	// resource.pff is never walked.
	write_pff(root / "resource.pff", {
			{"res.bms", bms_blob("Res Header Name", 0)},
			{"res.bin", info_bin("Res Op", "")},
	});
	// The expansion pair: jox01.pff's missions, titled from jox01L.pff; a .bms in
	// jox01L.pff is never listed; pack.bms shares a base mission's name.
	const fs::path exp = root / "expansion" / "jox01";
	write_pff(exp / "jox01.pff", {
			{"exp.bms", bms_blob("Exp Header Name", static_cast<uint32_t>(AttribFlags::Coop))},
			{"pack.bms", bms_blob("Exp Pack Header", 0)},
	});
	write_pff(exp / "jox01L.pff", {
			{"exp.bin", info_bin("Expansion Op", "From the expansion.")},
			{"inl.bms", bms_blob("In L Header", 0)},
	});

	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(root.string(), "jox01", opennova::VfsMountMode::PackedWithLooseOverride,
			opennova::VfsArchiveDiscovery::RetailTable));
	TEST_EXPECT(index.mounted_expansion() == "jox01");

	const std::vector<catalog::Row> rows = catalog::build(index);
	// alpha, kilo, mike, oscar, zulu (loose); exp, pack (expansion pair); bad, lonely,
	// pack (base pair).
	TEST_EXPECT(rows.size() == 10u);

	// Retail order: case-insensitive by FILENAME, no dedupe
	// [orig: Mission_CompareMapNames @ 0x5628e0; the qsort @ 0x5635f0].
	const char *order[] = {"alpha.bms", "bad.bms", "exp.bms", "kilo.bms", "lonely.bms",
		"mike.bms", "oscar.bms", "pack.bms", "pack.bms", "zulu.bms"};
	for (size_t i = 0; i < rows.size() && i < 10; ++i) TEST_EXPECT(rows[i].file == order[i]);

	// The loose leg lists the root once for every mission's .bin; each finds its own.
	const catalog::Row *oscar = find_row(rows, "oscar.bms", true);
	TEST_EXPECT(oscar != nullptr && oscar->title == "Oscar Op" &&
			oscar->briefing == "Upper-case table.");

	const catalog::Row *zulu = find_row(rows, "zulu.bms", true);
	TEST_EXPECT(zulu != nullptr);
	TEST_EXPECT(zulu->title == "Zulu Strike");
	TEST_EXPECT(zulu->briefing == "Take the island.");
	TEST_EXPECT(zulu->game_mode == static_cast<uint32_t>(AttribFlags::Coop));
	// Loose + titled: the "*" marker leads the title
	// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9].
	TEST_EXPECT(catalog::display_text(*zulu) == "*Zulu Strike");

	// No table: the header's embedded mission_name stands in.
	const catalog::Row *alpha = find_row(rows, "alpha.bms", true);
	TEST_EXPECT(alpha != nullptr);
	TEST_EXPECT(alpha->title == "Alpha Header Name");
	TEST_EXPECT(alpha->briefing.empty());
	TEST_EXPECT(alpha->game_mode ==
			static_cast<uint32_t>(AttribFlags::TeamDeathmatch));

	// A table without TITLE: the title stays empty and the row text falls back to
	// the FILENAME [orig: the +1044-empty arm @ 0x56191e].
	const catalog::Row *mike = find_row(rows, "mike.bms", true);
	TEST_EXPECT(mike != nullptr);
	TEST_EXPECT(mike->title.empty());
	TEST_EXPECT(mike->briefing == "Briefing without a title.");
	TEST_EXPECT(mike->game_mode == 0u);
	TEST_EXPECT(catalog::display_text(*mike) == "*mike.bms");

	// A loose mission whose .bin is archived only: no loose .bin, no table.
	const catalog::Row *kilo = find_row(rows, "kilo.bms", true);
	TEST_EXPECT(kilo != nullptr);
	TEST_EXPECT(kilo->title == "Kilo Header Name");

	// The base pair: archived rows carry no "*" and take their .bin from language.pff.
	const catalog::Row *base_pack = nullptr;
	const catalog::Row *exp_pack = nullptr;
	for (const catalog::Row &row : rows) {
		if (row.file != "pack.bms") continue;
		TEST_EXPECT(!row.loose);
		(row.title == "Packed Op" ? base_pack : exp_pack) = &row;
	}
	TEST_EXPECT(base_pack != nullptr && exp_pack != nullptr);
	TEST_EXPECT(base_pack != nullptr && base_pack->briefing == "From the archive.");
	TEST_EXPECT(base_pack != nullptr && catalog::display_text(*base_pack) == "Packed Op");
	// The same name in the expansion pair lists again; its pair's text archive has no
	// pack.bin, so it takes the header, which both rows read from the lowest slot
	// holding the name: the expansion's [orig: Mission_LoadBMSFromPFF @ 0x40d420].
	TEST_EXPECT(exp_pack != nullptr && exp_pack->title == "Exp Pack Header" &&
			exp_pack->briefing.empty());
	TEST_EXPECT(count_rows(rows, "pack.bms") == 2);

	// A .bin outside its pair's text archive does not title the row.
	const catalog::Row *lonely = find_row(rows, "lonely.bms", false);
	TEST_EXPECT(lonely != nullptr && lonely->title == "Lonely Header Name" &&
			lonely->briefing.empty());

	// The expansion pair titles from <n>L.pff.
	const catalog::Row *exp_row = find_row(rows, "exp.bms", false);
	TEST_EXPECT(exp_row != nullptr && exp_row->title == "Expansion Op" &&
			exp_row->briefing == "From the expansion." &&
			exp_row->game_mode == static_cast<uint32_t>(AttribFlags::Coop));

	// A header that fails the magic gate reads as zero: no name, no mode.
	const catalog::Row *bad = find_row(rows, "bad.bms", false);
	TEST_EXPECT(bad != nullptr && bad->title.empty() && bad->game_mode == 0u);
	TEST_EXPECT(bad != nullptr && catalog::display_text(*bad) == "bad.bms");

	// Never listed: resource.pff's missions, a .bms in <n>L.pff, a map project, a
	// name whose first '.' is not the extension's.
	TEST_EXPECT(count_rows(rows, "res.bms") == 0);
	TEST_EXPECT(count_rows(rows, "inl.bms") == 0);
	TEST_EXPECT(count_rows(rows, "asp_g7.npz") == 0);
	TEST_EXPECT(count_rows(rows, "a.b.bms") == 0);

	// A loose-only mount has no archive slots: its loose files alone.
	opennova::ResourceIndex loose_index;
	TEST_EXPECT(loose_index.scan(root.string(), "", opennova::VfsMountMode::LooseOnly));
	const std::vector<catalog::Row> loose_rows = catalog::build(loose_index);
	TEST_EXPECT(loose_rows.size() == 5u);
	for (const catalog::Row &row : loose_rows) TEST_EXPECT(row.loose);

	// The loose leg over an embedder's own reads (loose_rows): each .bms among the names
	// in their order, its table where the caller's read_text answers, a failed read a
	// zeroed header; then the catalog's order (sort_rows) and each row's session code word.
	{
		const std::vector<uint8_t> bravo = bms_blob("Bravo Header Name",
				static_cast<uint32_t>(AttribFlags::Coop));
		const std::vector<uint8_t> bravo_bin = info_bin("Bravo Op", "Read beside it.");
		const std::vector<std::string> names = { "Bravo.bms", "notes.txt", "Able.BMS", "gone.bms" };
		std::vector<std::string> asked;
		std::vector<catalog::Row> own = catalog::loose_rows(names,
				[&](const std::string &name, std::vector<uint8_t> &bytes) {
					if (name == "Bravo.bms") bytes = bravo;
					else if (name == "Able.BMS") bytes = bms_blob("Able Header Name", 0);
					else return false;
					return true;
				},
				[&](const std::string &bin, opennova::rtxt::File &text) {
					asked.push_back(bin);
					std::string error;
					return bin == "Bravo.bin" && opennova::rtxt::parse(bravo_bin.data(), bravo_bin.size(), text, error);
				});
		TEST_EXPECT(own.size() == 3u);
		TEST_EXPECT((asked == std::vector<std::string>{ "Bravo.bin", "Able.bin", "gone.bin" }));
		TEST_EXPECT(own[0].file == "Bravo.bms" && own[0].loose && own[0].title == "Bravo Op" &&
				own[0].briefing == "Read beside it." &&
				own[0].game_mode == static_cast<uint32_t>(AttribFlags::Coop));
		TEST_EXPECT(own[1].file == "Able.BMS" && own[1].title == "Able Header Name");
		TEST_EXPECT(own[2].file == "gone.bms" && own[2].title.empty() && own[2].game_mode == 0u);
		catalog::sort_rows(own);
		TEST_EXPECT(own[0].file == "Able.BMS" && own[1].file == "Bravo.bms" && own[2].file == "gone.bms");
		TEST_EXPECT(catalog::game_type_of(own[1]) ==
				opennova::game_type::for_mission_mode(static_cast<uint32_t>(AttribFlags::Coop)));
		TEST_EXPECT(catalog::game_type_of(own[0]) == opennova::game_type::for_mission_mode(0));
	}

	// What the archive walk lists, from the FIRST '.' as File_HasExtension compares
	// [orig: Mission_BuildMapListFromPFF @ 0x5629a0 / @ 0x5629bc / @ 0x5629d8]; the
	// table's section and keys [orig: @ 0x563489..0x56354f].
	TEST_EXPECT(catalog::lists_as_mission("00TRa.bms") && catalog::lists_as_mission("ASP_G7.NPZ") &&
			catalog::lists_as_mission("Map.npj"));
	TEST_EXPECT(!catalog::lists_as_mission("op.v2.bms") && !catalog::lists_as_mission("bms") &&
			!catalog::lists_as_mission("00TRa.bin") && !catalog::lists_as_mission("x.bms.bak"));
	TEST_EXPECT(std::string(catalog::kTextInfoSection) == "Info" &&
			std::string(catalog::kTextTitleKey) == "TITLE" &&
			std::string(catalog::kTextBriefingKey) == "BRIEFING");

	// Release the mounted archive handles before deleting the fixture tree
	// (an open .pff makes remove_all throw).
	index.clear();
	loose_index.clear();
	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	return 0;
}
