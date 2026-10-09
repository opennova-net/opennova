// The typed kinds and their loader facts (base/resource_index/file_kind.h): the rule that no runtime
// format arrives untyped or unplaced, made checkable (ADR 0046 S13 D5). Every rule the runtime's
// classifier types a file by (resource_kind_rules: a whole name, an extension, a `.bin`'s magic) and
// every row of the required-resources manifest (base/gameprofile: a literal name, each name a
// pattern stands for) maps to a kind that is not Unknown and has a place (an archive or loose), but
// an archive; a catalog kind lands on the row that names its catalog token, and no row names a token
// the runtime does not give. The table's own shape (one row per kind, in order; a catalog token, a
// file name or an extension named once) is its static_asserts'; here, what they cannot say: no name
// gives an import source or a material chunk, Unknown names nothing and is read from nowhere, and
// each name a row lists types to the row. A slot is pinned where the game reads a file apart from
// the archives: game.cfg, assets.cd, CC.BIN and filter.txt loose, fgn2.bin packed. And the table
// agrees with master's other copies of its facts: the shader loader's stored form
// (vfs_loader_takes_stored), the boot table's names (kBootArchiveTable), the mission list's walk
// (runtime/mission/mission_catalog) and the names the game reads from an expansion's folder
// (vfs_read_from_expansion_folder).
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <base/resource_index/resource_kind.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>

#include "common/test_expect.h"

using namespace opennova;

namespace {

// A kind a runtime format may land on: one the game knows, which it reads from somewhere.
bool typed_and_placed(FileKind kind) {
	return kind != FileKind::Unknown && file_kind_facts(kind).archive_slot != ArchiveSlot::None;
}

// The file names a manifest row stands for: its literal, or for a pattern each of its
// alternatives ("*.npj/*.npz") by the file name after its folders, a placeholder or a wildcard
// standing for a letter ("expansion\<n>\<n>.pff" is x.pff).
std::vector<std::string> names_of(const gameprofile::RequiredResource &row) {
	if (!(row.flags & gameprofile::RES_F_PATTERN)) return {row.name};
	std::vector<std::string> out;
	const std::string text = row.name;
	size_t start = 0;
	for (;;) {
		const size_t slash = text.find('/', start);
		const size_t length = slash == std::string::npos ? std::string::npos : slash - start;
		std::string part = text.substr(start, length);
		const size_t folder = part.find_last_of('\\');
		if (folder != std::string::npos) part = part.substr(folder + 1);
		std::string name;
		for (size_t i = 0; i < part.size(); ++i) {
			if (part[i] == '*') {
				name += 'x';
			} else if (part[i] == '<') {
				const size_t close = part.find('>', i);
				name += 'x';
				i = close == std::string::npos ? part.size() : close;
			} else {
				name += part[i];
			}
		}
		out.push_back(name);
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	return out;
}

// The file a rule types: its whole name, or a name of its extension (x.trn), a `.bin` with the
// magic its content starts with.
std::string probe_of(const ResourceKindRule &rule) {
	return *rule.name ? std::string(rule.name) : std::string("x") + rule.extension;
}

// What the typed classifier makes of a rule's file.
FileKind kind_of(const ResourceKindRule &rule) {
	if (!*rule.magic) return file_kind_for_file(probe_of(rule), nullptr);
	std::vector<uint8_t> bytes(rule.magic, rule.magic + std::strlen(rule.magic));
	bytes.resize(16, 0);
	return file_kind_for_file(probe_of(rule), &bytes);
}

} // namespace

// Every rule of the runtime's classifier lands on the row naming its catalog token, typed and
// placed; every row naming a catalog token is one a rule gives.
static int test_runtime_formats() {
	std::set<std::string> given;
	size_t rules = 0;
	for (const ResourceKindRule &rule : resource_kind_rules()) {
		const FileKind kind = kind_of(rule);
		const FileKindFacts &row = file_kind_facts(kind);
		given.insert(rule.kind);
		++rules;
		if (!typed_and_placed(kind) || std::string(row.resource_kind) != rule.kind)
			std::fprintf(stderr, "the runtime's %s %s (%s) is kind %d here\n", probe_of(rule).c_str(),
			             rule.magic, rule.kind, int(kind));
		TEST_EXPECT(typed_and_placed(kind));
		TEST_EXPECT(std::string(row.resource_kind) == rule.kind);
		TEST_EXPECT(file_kind_for_resource_kind(rule.kind) == kind);
	}
	TEST_EXPECT(rules > 0);
	std::printf("file_kind: %zu rules of the runtime's classifier\n", rules);
	for (size_t i = 0; i < kFileKindCount; ++i) {
		const FileKindFacts &row = file_kind_facts(FileKind(i));
		if (!*row.resource_kind) continue;
		if (!given.count(row.resource_kind))
			std::fprintf(stderr, "no rule gives kind %zu's %s\n", i, row.resource_kind);
		TEST_EXPECT(given.count(row.resource_kind) == 1);
	}
	TEST_EXPECT(file_kind_for_resource_kind("") == FileKind::Unknown &&
	            file_kind_for_resource_kind("no_such_kind") == FileKind::Unknown);
	return 0;
}

// Every row of the required-resources manifest, by each name it stands for, is a kind the game
// knows that it reads from somewhere; the boot table's archives and the expansion archive are
// archives.
static int test_required_files() {
	const int count = gameprofile::gameprofile_required_resource_count();
	TEST_EXPECT(count > 0);
	size_t names = 0, archives = 0;
	for (int i = 0; i < count; ++i) {
		const auto &row = *gameprofile::gameprofile_required_resource_at(i);
		for (const std::string &name : names_of(row)) {
			const FileKind kind = file_kind_for_required_name(name);
			++names;
			if (strutil::ends_with_icase(name, ".pff")) {
				TEST_EXPECT(kind == FileKind::Archive);
				++archives;
				continue;
			}
			if (!typed_and_placed(kind))
				std::fprintf(stderr, "the required %s (%s) is kind %d here\n", name.c_str(), row.role, int(kind));
			TEST_EXPECT(typed_and_placed(kind));
		}
	}
	TEST_EXPECT(archives == 4); // resource, localres, language and an expansion's
	std::printf("file_kind: %d manifest rows, %zu names, %zu of them archives\n", count, names, archives);
	// The `.bin` rows by name: string tables, but the music scripts and the raw markers.
	TEST_EXPECT(file_kind_for_required_name("gametext.bin") == FileKind::Strings);
	TEST_EXPECT(file_kind_for_required_name("menutxt.bin") == FileKind::Strings);
	TEST_EXPECT(file_kind_for_required_name("MENUMUS.BIN") == FileKind::MusicScript);
	TEST_EXPECT(file_kind_for_required_name("fgn2.bin") == FileKind::RawBin);
	TEST_EXPECT(file_kind_for_required_name("CC.BIN") == FileKind::CountryCode);
	TEST_EXPECT(file_kind_for_required_name("main.mnu") == FileKind::Menu);
	return 0;
}

// Where the files the boot reads apart from the archives belong: those it opens with the C
// library, or reads before any archive mounts, loose, and one it reads through the archives after
// they mount packed. Each is a manifest row, and the kind the requirement expects is the kind its
// name and content give it (a `.bin` by raw content).
static int test_boot_files() {
	struct Pinned {
		const char *name;
		bool loose;
	};
	const Pinned pinned[] = {
	        // Before the mount [orig: Game_LoadConfig @ 0x551480 via
	        // File_ParseASCIIFileWithCallback @ 0x53d980].
	        {"game.cfg", true},
	        {"assets.cd", true}, // [orig: Game_ReadAssetsCDFile @ 0x4a5800]
	        // fopen on every read, never the archives [orig: Game_ReadCCBinFile @ 0x4a5860].
	        {"CC.BIN", true},
	        {"filter.txt", true}, // [orig: ChatFilter_LoadFromFile @ 0x4fd640]
	        // After the mount, through the archives alone [orig: CEffectSystem_Init @ 0x5f6070
	        // through FileSystem_FileExists @ 0x75aa50].
	        {"fgn2.bin", false},
	};
	const std::vector<uint8_t> raw = {0x12, 0x34};
	for (const Pinned &file : pinned) {
		TEST_EXPECT(gameprofile::gameprofile_required_resource_find(file.name) != nullptr);
		const FileKind expected = file_kind_for_required_name(file.name);
		const bool peeked = resource_extension_for_name(file.name) == ".bin";
		const FileKind found = file_kind_for_file(file.name, peeked ? &raw : nullptr);
		const ArchiveSlot slot = file_kind_facts(found).archive_slot;
		const bool packed = slot == ArchiveSlot::Language || slot == ArchiveSlot::Localres ||
		                    slot == ArchiveSlot::Resource;
		if (expected != found || (file.loose ? slot != ArchiveSlot::Loose : !packed))
			std::fprintf(stderr, "%s is kind %d, expected %d, in slot %d\n", file.name, int(found),
			             int(expected), int(slot));
		TEST_EXPECT(expected == found);
		TEST_EXPECT(file.loose ? slot == ArchiveSlot::Loose : packed);
	}
	// The NovaWorld screens, the error page [orig: "nw_error.mnx" @ 0x558449] and the login's start
	// page (D-NET-31): a kind of their own. Retail ships them loose in its folder, where the front
	// door reads only under /d [orig: FileSystem_OpenFile @ 0x75b1c0; Game_InitSubsystems @
	// 0x4a6fac]: they belong with the menus.
	for (const char *name : {"nw_error.mnx", "nw_startup.mnx", "JOP_2_MAIN.MNX"}) {
		const FileKind kind = file_kind_for_name(name);
		TEST_EXPECT(kind == FileKind::NovaWorldScreen &&
		            file_kind_facts(kind).archive_slot == ArchiveSlot::Localres &&
		            file_kind_facts(kind).expansion_loose == ExpansionLoose::None);
	}
	return 0;
}

// What the static_asserts cannot say: no name gives an import source, an import's input or a
// material chunk, and none is a catalog kind; a material chunk packs with the art; Unknown names
// nothing and is read from nowhere; each whole name and extension a row lists types to the row by
// its name alone, in any case; a PNG is a texture by its name; a value past the last kind reads
// Unknown's row.
static int test_the_table() {
	for (const FileKind kind : {FileKind::ImportSource, FileKind::ImportInput, FileKind::MaterialChunk}) {
		const FileKindFacts &row = file_kind_facts(kind);
		TEST_EXPECT(!*row.resource_kind && !row.file_name && !row.file_names && !row.extensions);
	}
	TEST_EXPECT(file_kind_facts(FileKind::MaterialChunk).archive_slot == ArchiveSlot::Resource);
	const FileKindFacts &unknown = file_kind_facts(FileKind::Unknown);
	TEST_EXPECT(!*unknown.resource_kind && !unknown.file_name && !unknown.file_names && !unknown.extensions);
	TEST_EXPECT(unknown.archive_slot == ArchiveSlot::None);
	TEST_EXPECT(&file_kind_facts(FileKind::kCount) == &unknown);
	for (size_t i = 0; i < kFileKindCount; ++i) {
		const FileKindFacts &row = file_kind_facts(FileKind(i));
		TEST_EXPECT(row.kind == FileKind(i));
		if (row.file_name) {
			TEST_EXPECT(file_kind_for_name(row.file_name) == row.kind);
			TEST_EXPECT(file_kind_for_name(std::string("Data/") + strutil::to_upper(row.file_name)) == row.kind);
		}
		for (const char *const *name = row.file_names; name && *name; ++name)
			TEST_EXPECT(file_kind_for_name(*name) == row.kind);
		for (const char *const *extension = row.extensions; extension && *extension; ++extension)
			TEST_EXPECT(file_kind_for_name(std::string("x") + *extension) == row.kind);
	}
	TEST_EXPECT(file_kind_for_name("logo.png") == FileKind::Texture && file_kind_for_name("LOGO.PNG") == FileKind::Texture);
	// The runtime's classifier first: Avatars.def is its avatar database, not any other .def.
	TEST_EXPECT(file_kind_for_name("Avatars.def") == FileKind::AvatarDefs && file_kind_for_name("x.def") == FileKind::OtherDefs);
	TEST_EXPECT(file_kind_for_name("x.3di") == FileKind::Model && file_kind_for_name("noextension") == FileKind::Unknown);
	// A `.bin` by its content where it has one, else a raw table.
	const std::vector<uint8_t> rtxt = {'R', 'T', 'X', 'T', 0, 0, 0, 0};
	TEST_EXPECT(file_kind_for_file("menutxt.bin", &rtxt) == FileKind::Strings &&
	            file_kind_for_name("menutxt.bin") == FileKind::RawBin);
	// The two readers that end a line at CR LF alone, a pin each.
	TEST_EXPECT(file_kind_facts(FileKind::Credits).line_reader == LineReader::ConfigFile &&
	            file_kind_facts(FileKind::CharAttrDefs).line_reader == LineReader::ConfigFile &&
	            file_kind_facts(FileKind::ItemDefs).line_reader == LineReader::AsciiWalk &&
	            file_kind_facts(FileKind::Script).line_reader == LineReader::None);
	return 0;
}

// The table and master's other copies of its facts agree. The shader loader takes its file as
// stored (vfs_loader_takes_stored, a name's rule): exactly the files of the Shader kind. A packed
// slot's archive is the boot table's of its place, the mission list walks the slot the missions'
// kinds live in [orig: Mission_BuildMapListFromPFF @ 0x562910; runtime/mission/mission_catalog's
// localres/language walk], and the files the game reads from an expansion's folder by name
// (vfs_read_from_expansion_folder: its videos and music banks) are of kinds read loose from there.
static int test_master_copies() {
	size_t extensions = 0;
	for (size_t i = 0; i < kFileKindCount; ++i) {
		const FileKindFacts &row = file_kind_facts(FileKind(i));
		for (const char *const *extension = row.extensions; extension && *extension; ++extension) {
			++extensions;
			TEST_EXPECT(vfs_loader_takes_stored(std::string("x") + *extension) == (row.kind == FileKind::Shader));
		}
	}
	TEST_EXPECT(extensions > 0);
	TEST_EXPECT(file_kind_for_name("glass.fx") == FileKind::Shader && vfs_loader_takes_stored("glass.fx"));
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Language)) == kBootArchiveTable[0] &&
	            std::string(archive_slot_file_name(ArchiveSlot::Localres)) == kBootArchiveTable[1] &&
	            std::string(archive_slot_file_name(ArchiveSlot::Resource)) == kBootArchiveTable[2]);
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Language)) == "language.pff");
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Loose)).empty() &&
	            std::string(archive_slot_file_name(ArchiveSlot::None)).empty());
	const std::string walked = kBootArchiveTable[kArchiveSlotLocalres - kArchiveSlotLanguage];
	for (const FileKind kind : {FileKind::Mission, FileKind::MapProject})
		TEST_EXPECT(archive_slot_file_name(file_kind_facts(kind).archive_slot) == walked);
	for (const char *name : {"main.bik", "header.bik", "footer.bik", "prolog.bik", "intro.bik", "Mjxm.sbf", "Gjxm.sbf"}) {
		TEST_EXPECT(vfs_read_from_expansion_folder(name, "jxm"));
		const FileKindFacts &row = file_kind_facts(file_kind_for_name(name));
		TEST_EXPECT(row.archive_slot == ArchiveSlot::Loose && row.expansion_loose == ExpansionLoose::Folder);
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_runtime_formats();
	failures += test_required_files();
	failures += test_boot_files();
	failures += test_the_table();
	failures += test_master_copies();
	if (failures == 0) std::printf("file_kind: every runtime format and required file has a kind and a place\n");
	return failures == 0 ? 0 : 1;
}
