#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "resource_index/resource_index.h"

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path &path, const char *text) {
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << text;
}

void append_u32_le(std::vector<uint8_t> &bytes, uint32_t value) {
	bytes.push_back(static_cast<uint8_t>(value & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
	bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

struct PffFixtureEntry {
	std::string name;
	std::string bytes;
};

void write_pff(const fs::path &path, const std::vector<PffFixtureEntry> &entries) {
	fs::create_directories(path.parent_path());

	const uint32_t header_size = 20;
	const uint32_t entry_size = 36;
	const uint32_t table_offset = header_size;
	const uint32_t payload_offset = header_size + static_cast<uint32_t>(entries.size()) * entry_size;
	uint32_t next_payload_offset = payload_offset;

	std::vector<uint8_t> bytes;
	append_u32_le(bytes, header_size);
	append_u32_le(bytes, 0x33464650u);
	append_u32_le(bytes, static_cast<uint32_t>(entries.size()));
	append_u32_le(bytes, entry_size);
	append_u32_le(bytes, table_offset);

	for (const PffFixtureEntry &entry : entries) {
		append_u32_le(bytes, 0);
		append_u32_le(bytes, next_payload_offset);
		append_u32_le(bytes, static_cast<uint32_t>(entry.bytes.size()));
		append_u32_le(bytes, 0);
		for (size_t i = 0; i < 16; ++i) {
			bytes.push_back(i < entry.name.size() ? static_cast<uint8_t>(entry.name[i]) : 0);
		}
		append_u32_le(bytes, 0);
		next_payload_offset += static_cast<uint32_t>(entry.bytes.size());
	}

	for (const PffFixtureEntry &entry : entries) {
		bytes.insert(bytes.end(), entry.bytes.begin(), entry.bytes.end());
	}

	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool has_relative_path(const std::vector<opennova::ResourceFileEntry> &entries,
                       const std::string &relative_path) {
	return std::any_of(entries.begin(), entries.end(), [&](const opennova::ResourceFileEntry &entry) {
		return entry.relative_path == relative_path;
	});
}

const opennova::ResourceFileEntry *find_relative_path(const std::vector<opennova::ResourceFileEntry> &entries,
                                                      const std::string &relative_path) {
	for (const opennova::ResourceFileEntry &entry : entries) {
		if (entry.relative_path == relative_path) {
			return &entry;
		}
	}
	return nullptr;
}

std::string as_string(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

} // namespace

int main() {
	const fs::path root = fs::temp_directory_path() / "opennova_resource_index_test";
	fs::remove_all(root);
	fs::create_directories(root);

	write_file(root / "Alpha.TRN", "trn");
	write_file(root / "Bravo.env", "env");
	write_file(root / "object.3dp", "3dp");
	write_file(root / "preview.3di", "3di");
	write_file(root / "scene.ASE", "ase");
	write_file(root / "vehicle.glb", "glb");
	write_file(root / "Menus.BIN", "RTXTstrings");
	write_file(root / "jo_gamemus.sbf", "SBF0");
	write_file(root / "jo_gamemus.bin", "SCR0music");
	write_file(root / "Scratch.bin", "DATA");
	write_file(root / "first.bms", "bms");
	write_file(root / "finale.kda", "kda");
	write_file(root / "Serpen24.fnt", "fnt");
	write_file(root / "main.mnu", "mnu");
	write_file(root / "menu_style.mns", "DEF_TEXT_FG FFFFFFFF");
	write_file(root / "missions" / "first.bms", "bms");
	write_file(root / "models" / "tree.glb", "glb");
	write_file(root / "models" / "tree.3di", "3di");
	write_file(root / "models" / "source.ase", "ase");
	write_file(root / "credits" / "finale.kda", "kda");
	write_file(root / "fonts" / "Serpen24.fnt", "fnt");
	write_file(root / "strings" / "menu.bin", "bin");
	write_file(root / "briefing.MIS", "// mission metafile\r\n");

	opennova::ResourceIndex index;
	TEST_EXPECT(!index.scan((root / "missing").string()));
	TEST_EXPECT(!index.last_error().empty());

	TEST_EXPECT(index.scan(root.string()));
	const std::vector<opennova::ResourceFileEntry> all_files = index.resource_files("*");
	TEST_EXPECT(all_files.size() == 14);
	TEST_EXPECT(has_relative_path(all_files, "Alpha.TRN"));
	TEST_EXPECT(has_relative_path(all_files, "Menus.BIN"));
	TEST_EXPECT(has_relative_path(all_files, "main.mnu"));
	TEST_EXPECT(has_relative_path(all_files, "menu_style.mns"));
	TEST_EXPECT(has_relative_path(all_files, "jo_gamemus.sbf"));
	TEST_EXPECT(has_relative_path(all_files, "jo_gamemus.bin"));
	TEST_EXPECT(has_relative_path(all_files, "first.bms"));
	TEST_EXPECT(has_relative_path(all_files, "briefing.MIS"));
	TEST_EXPECT(has_relative_path(all_files, "finale.kda"));
	TEST_EXPECT(has_relative_path(all_files, "Serpen24.fnt"));
	TEST_EXPECT(!has_relative_path(all_files, "missions/first.bms"));
	TEST_EXPECT(!has_relative_path(all_files, "models/tree.3di"));
	TEST_EXPECT(!has_relative_path(all_files, "models/source.ase"));
	TEST_EXPECT(!has_relative_path(all_files, "credits/finale.kda"));
	TEST_EXPECT(!has_relative_path(all_files, "fonts/Serpen24.fnt"));
	TEST_EXPECT(!has_relative_path(all_files, "Scratch.bin"));
	TEST_EXPECT(!has_relative_path(all_files, "vehicle.glb"));
	TEST_EXPECT(!has_relative_path(all_files, "models/tree.glb"));

	// Loose entries carry on-disk size + modified time (the editor's resource
	// browser shows these as columns). Sizes match the fixture byte counts.
	const opennova::ResourceFileEntry *alpha = find_relative_path(all_files, "Alpha.TRN");
	TEST_EXPECT(alpha != nullptr);
	TEST_EXPECT(alpha->source_type == "file");
	TEST_EXPECT(alpha->size_bytes == 3);  // "trn"
	TEST_EXPECT(alpha->modified_time > 0);
	const opennova::ResourceFileEntry *menus = find_relative_path(all_files, "Menus.BIN");
	TEST_EXPECT(menus != nullptr);
	TEST_EXPECT(menus->size_bytes == 11);  // "RTXTstrings"

	TEST_EXPECT(index.resource_files("terrain").size() == 1);
	TEST_EXPECT(index.resource_files("environment").size() == 1);
	TEST_EXPECT(index.resource_files("mission").size() == 2);
	TEST_EXPECT(index.resource_files("object_project").size() == 1);
	TEST_EXPECT(index.resource_files("object_model").size() == 1);
	TEST_EXPECT(index.resource_files("object_scene").size() == 1);
	TEST_EXPECT(index.resource_files("strings").size() == 1);
	TEST_EXPECT(index.resource_files("menu").size() == 1);
	TEST_EXPECT(index.resource_files("mnu").size() == 1);  // alias normalizes to "menu"
	TEST_EXPECT(index.resource_files("menu_style").size() == 1);
	TEST_EXPECT(index.resource_files("mns").size() == 1);  // alias normalizes to "menu_style"
	TEST_EXPECT(index.resource_files("menu_style")[0].display_name == "menu_style");
	TEST_EXPECT(index.resource_files("glb").empty());
	TEST_EXPECT(index.resource_files("bms").size() == 2);
	TEST_EXPECT(index.resource_files("mis").size() == 2);
	TEST_EXPECT(index.resource_files("trn").size() == 1);
	TEST_EXPECT(index.resource_files("env").size() == 1);
	TEST_EXPECT(index.resource_files("3dp").size() == 1);
	TEST_EXPECT(index.resource_files("3di").size() == 1);
	TEST_EXPECT(index.resource_files("ase").size() == 1);
	TEST_EXPECT(index.resource_files("object").size() == 3);
	TEST_EXPECT(index.resource_files("object_model").size() == 1);
	TEST_EXPECT(index.resource_files("object_model")[0].display_name == "preview");
	TEST_EXPECT(index.resource_files("credits").size() == 1);
	TEST_EXPECT(index.resource_files("kda").size() == 1);
	TEST_EXPECT(index.resource_files("credits")[0].display_name == "finale");
	TEST_EXPECT(index.resource_files("font").size() == 1);
	TEST_EXPECT(index.resource_files("fnt").size() == 1);
	TEST_EXPECT(index.resource_files("font")[0].display_name == "Serpen24");
	TEST_EXPECT(index.resource_files("strings").size() == 1);
	TEST_EXPECT(index.resource_files("bin").size() == 1);
	TEST_EXPECT(index.resource_files("rtxt").size() == 1);
	TEST_EXPECT(index.resource_files("strings")[0].display_name == "Menus");
	TEST_EXPECT(index.resource_files("sbf").size() == 1);
	TEST_EXPECT(index.resource_files("music_script").size() == 1);
	TEST_EXPECT(index.resource_files("mus").size() == 1);
	TEST_EXPECT(index.resource_files("music").size() == 2);

	write_file(root / "bad.pff", "not a pff");
	write_pff(root / "aa_base.pff", {
		{"Alpha.TRN", "archived trn shadow"},
		{"Archive.env", "env from aa"},
		{"MenusP.BIN", "RTXTarchived strings"},
	});
	write_pff(root / "zz_patch.pff", {
		{"Archive.env", "env from zz"},
		{"Patch.3DI", "archived model"},
	});
	TEST_EXPECT(index.scan(root.string()));
	const std::vector<opennova::ResourceFileEntry> mounted_files = index.resource_files("*");

	std::vector<uint8_t> bytes;
	TEST_EXPECT(index.read_file("alpha.trn", bytes));
	TEST_EXPECT(as_string(bytes) == "trn");
	TEST_EXPECT(index.read_file("Archive.env", bytes));
	TEST_EXPECT(as_string(bytes) == "env from aa");
	TEST_EXPECT(index.read_file("Patch.3di", bytes));
	TEST_EXPECT(as_string(bytes) == "archived model");
	TEST_EXPECT(!index.read_file("missing.trn", bytes));

	TEST_EXPECT(mounted_files.size() == 17);
	TEST_EXPECT(has_relative_path(mounted_files, "Archive.env"));
	TEST_EXPECT(has_relative_path(mounted_files, "MenusP.BIN"));
	TEST_EXPECT(has_relative_path(mounted_files, "Patch.3DI"));
	TEST_EXPECT(index.resource_files("environment").size() == 2);
	TEST_EXPECT(index.resource_files("strings").size() == 2);
	TEST_EXPECT(index.resource_files("object_model").size() == 2);

	const opennova::ResourceFileEntry *loose = find_relative_path(mounted_files, "Alpha.TRN");
	TEST_EXPECT(loose != nullptr);
	TEST_EXPECT(loose->source_type == "file");
	TEST_EXPECT(!loose->path.empty());
	TEST_EXPECT(loose->archive_path.empty());
	TEST_EXPECT(loose->logical_name == "Alpha.TRN");

	const opennova::ResourceFileEntry *archived = find_relative_path(mounted_files, "Archive.env");
	TEST_EXPECT(archived != nullptr);
	TEST_EXPECT(archived->source_type == "pff");
	TEST_EXPECT(archived->path.empty());
	TEST_EXPECT(fs::path(archived->archive_path).filename().string() == "aa_base.pff");
	TEST_EXPECT(archived->logical_name == "Archive.env");

	// --- expansion override: scan(root, expansion) mounts loose expansion + L.pff + main.pff
	// ahead of the base archives (matches the engine). ---
	const fs::path game = fs::temp_directory_path() / "opennova_resource_index_exp";
	fs::remove_all(game);
	fs::create_directories(game / "expansion" / "jox01");
	write_pff(game / "resource.pff", {{"shared.env", "base env"}, {"baseonly.trn", "base trn"}});
	write_pff(game / "expansion" / "jox01" / "jox01.pff",
	          {{"shared.env", "main env"}, {"exponly.3di", "exp model"}});
	write_pff(game / "expansion" / "jox01" / "jox01L.pff", {{"shared.env", "local env"}});
	write_file(game / "expansion" / "jox01" / "shared.env", "loose env");

	opennova::ResourceIndex exp_index;
	TEST_EXPECT(exp_index.scan(game.string(), "jox01"));
	std::vector<uint8_t> eb;
	TEST_EXPECT(exp_index.read_file("shared.env", eb));
	TEST_EXPECT(as_string(eb) == "loose env");   // loose expansion file wins over all archives
	TEST_EXPECT(exp_index.read_file("exponly.3di", eb));
	TEST_EXPECT(as_string(eb) == "exp model");   // expansion archive content
	TEST_EXPECT(exp_index.read_file("baseonly.trn", eb));
	TEST_EXPECT(as_string(eb) == "base trn");    // base archive still reachable
	exp_index.clear();

	// --- mount modes: LooseOnly (editor) ignores archives; Packed (shipping runtime)
	// ignores loose overrides. The default above is PackedWithLooseOverride (runtime /d). ---
	{
		std::vector<uint8_t> mb;
		opennova::ResourceIndex loose_idx;
		TEST_EXPECT(loose_idx.scan(game.string(), "jox01", opennova::VfsMountMode::LooseOnly));
		TEST_EXPECT(loose_idx.read_file("shared.env", mb));
		TEST_EXPECT(as_string(mb) == "loose env");          // loose expansion file
		TEST_EXPECT(!loose_idx.read_file("exponly.3di", mb)); // archive-only entry invisible
		loose_idx.clear();

		opennova::ResourceIndex packed_idx;
		TEST_EXPECT(packed_idx.scan(game.string(), "jox01", opennova::VfsMountMode::Packed));
		TEST_EXPECT(packed_idx.read_file("shared.env", mb));
		TEST_EXPECT(as_string(mb) == "local env");          // L.pff wins; loose ignored
		TEST_EXPECT(packed_idx.read_file("exponly.3di", mb)); // archive entry present
		packed_idx.clear();
	}
	fs::remove_all(game);

	// Release mounted .pff handles before deleting the directory: the VFS keeps archives
	// open for the session (engine-faithful, fast reads), and Windows blocks deletion of
	// open files.
	index.clear();
	fs::remove_all(root);
	return 0;
}
