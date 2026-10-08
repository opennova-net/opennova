// S13 D5 (ADR 0046 S13): the rule the maintainer set for the editor's roadmap, that every
// resource format the game reads at runtime will be editable, made checkable so that no runtime
// format arrives untyped or unrouted. Every rule the runtime's classifier types a file by
// (base/resource_index's resource_kind_rules: a whole name, an extension, a `.bin`'s magic) and
// every row of the required-resources manifest (base/gameprofile: a literal name, each name a
// pattern stands for) maps to an asset kind that is not Unknown and has an archive slot (an
// archive or loose), but an archive, which the build makes; a runtime kind lands on the row that
// names its catalog token, and no row names a token the runtime does not give. The table's own
// shape (one row per kind, in order; a token, a runtime token, a file name or an extension named
// once) is its static_asserts'; here, what they cannot say: every document type is a row's, an
// import source and a material chunk are no runtime format and no name gives them (the scan does),
// the first packing nowhere, and Unknown is no runtime format and packs nowhere (S13 A8: the build
// leaves a file of no kind the game knows out, its name bound by no archive limit). A slot is
// pinned where the game reads a file apart from the archives: game.cfg, assets.cd, CC.BIN and
// filter.txt loose, fgn2.bin packed.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// A kind a runtime format may land on: one the game knows, which the build puts somewhere.
bool typed_and_routed(AssetKind kind) {
	return kind != AssetKind::Unknown && asset_kind_row(kind).archive_slot != ArchiveSlot::None;
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

// What the editor's classifier makes of a rule's file.
AssetKind kind_of(const ResourceKindRule &rule) {
	if (!*rule.magic) return classify_asset(probe_of(rule), nullptr);
	std::vector<uint8_t> bytes(rule.magic, rule.magic + std::strlen(rule.magic));
	bytes.resize(16, 0);
	return classify_asset(probe_of(rule), &bytes);
}

} // namespace

// Every rule of the runtime's classifier lands on the row naming its catalog token, typed and
// routed; every row naming a catalog token is one a rule gives.
static int test_runtime_formats() {
	std::set<std::string> given;
	size_t rules = 0;
	for (const ResourceKindRule &rule : resource_kind_rules()) {
		const AssetKind kind = kind_of(rule);
		const AssetKindRow &row = asset_kind_row(kind);
		given.insert(rule.kind);
		++rules;
		if (!typed_and_routed(kind) || std::string(row.runtime) != rule.kind)
			std::fprintf(stderr, "the runtime's %s %s (%s) is %s here\n", probe_of(rule).c_str(),
			             rule.magic, rule.kind, row.token);
		TEST_EXPECT(typed_and_routed(kind));
		TEST_EXPECT(std::string(row.runtime) == rule.kind);
		TEST_EXPECT(asset_kind_for_runtime(rule.kind) == kind);
	}
	TEST_EXPECT(rules > 0);
	std::printf("editor_asset_kinds: %zu rules of the runtime's classifier\n", rules);
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = asset_kind_row(AssetKind(i));
		if (!*row.runtime) continue;
		if (!given.count(row.runtime))
			std::fprintf(stderr, "no rule gives %s's %s\n", row.token, row.runtime);
		TEST_EXPECT(given.count(row.runtime) == 1);
	}
	return 0;
}

// Every row of the required-resources manifest, by each name it stands for, is a kind the game
// knows that the build puts somewhere; the boot table's archives and the expansion archive are
// archives, which the build makes.
static int test_required_files() {
	const int count = gameprofile::gameprofile_required_resource_count();
	TEST_EXPECT(count > 0);
	size_t names = 0, archives = 0;
	for (int i = 0; i < count; ++i) {
		const auto &row = *gameprofile::gameprofile_required_resource_at(i);
		for (const std::string &name : names_of(row)) {
			const AssetKind kind = expected_asset_kind_for_required_name(name);
			++names;
			if (strutil::ends_with_icase(name, ".pff")) {
				TEST_EXPECT(kind == AssetKind::Archive);
				++archives;
				continue;
			}
			if (!typed_and_routed(kind))
				std::fprintf(stderr, "the required %s (%s) is %s here\n", name.c_str(), row.role,
				             asset_kind_token(kind));
			TEST_EXPECT(typed_and_routed(kind));
		}
	}
	TEST_EXPECT(archives == 4); // resource, localres, language and an expansion's
	std::printf("editor_asset_kinds: %d manifest rows, %zu names, %zu of them archives\n", count,
	            names, archives);
	return 0;
}

// Where the build puts the files the boot reads apart from the archives: those it opens with the
// C library, or reads before any archive mounts, must reach the build loose, and one it reads
// through the archives after they mount must be packed. Each is a manifest row, and the kind the
// requirement expects is the kind the scan gives it (a `.bin` by raw content).
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
		const AssetKind expected = expected_asset_kind_for_required_name(file.name);
		const bool peeked = asset_classification_needs_bytes(file.name);
		const AssetKind scanned = classify_asset(file.name, peeked ? &raw : nullptr);
		const ArchiveSlot slot = asset_kind_row(scanned).archive_slot;
		const bool packed = slot == ArchiveSlot::Language || slot == ArchiveSlot::Localres ||
		                    slot == ArchiveSlot::Resource;
		if (expected != scanned || (file.loose ? slot != ArchiveSlot::Loose : !packed))
			std::fprintf(stderr, "%s is %s, expected %s, in slot %d\n", file.name,
			             asset_kind_token(scanned), asset_kind_token(expected), int(slot));
		TEST_EXPECT(expected == scanned);
		TEST_EXPECT(file.loose ? slot == ArchiveSlot::Loose : packed);
	}
	TEST_EXPECT(expected_asset_kind_for_required_name("CC.BIN") == AssetKind::CountryCode);
	TEST_EXPECT(expected_asset_kind_for_required_name("fgn2.bin") == AssetKind::RawBin);
	// The NovaWorld screens, the error page [orig: "nw_error.mnx" @ 0x558449] and the login's start
	// page (D-NET-31): a kind of their own (S13 A8: no kind before, so no build carried them). Retail
	// ships them loose in its folder, where the front door reads only under /d [orig: FileSystem_OpenFile
	// @ 0x75b1c0; Game_InitSubsystems @ 0x4a6fac]: a build packs them with the menus (S16).
	for (const char *name : {"nw_error.mnx", "nw_startup.mnx", "JOP_2_MAIN.MNX"}) {
		const AssetKind kind = classify_asset(name, nullptr);
		TEST_EXPECT(kind == AssetKind::NovaWorldScreen && asset_kind_row(kind).archive_slot == ArchiveSlot::Localres &&
		            asset_kind_row(kind).expansion_loose == ExpansionLoose::None);
	}
	return 0;
}

// What the static_asserts cannot say: every document type is a row's (and opens it); an import
// source packs nowhere and binds the archives' name limit (its outputs take its name), a material
// chunk packs with the art, and neither is a runtime format or a name's; Unknown is no runtime
// format and packs nowhere, its name bound by nothing (S13 A8), as an archive's; a PNG is a
// texture by its name.
static int test_the_table() {
	std::set<DocumentTypeId> edited;
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKind kind = AssetKind(i);
		const AssetKindRow &row = asset_kind_row(kind);
		TEST_EXPECT(row.kind == kind && asset_kind_from_token(row.token) == kind);
		// A kind a document type edits is one the build packs, but the project's notes, which the text type
		// opens and the game never reads.
		if (row.document != DocumentTypeId::None) {
			edited.insert(row.document);
			const DocumentType *type = document_type(row.document);
			TEST_EXPECT(type && document_type_for(kind) == type && asset_kind_packed(kind) == (kind != AssetKind::Notes));
		} else {
			TEST_EXPECT(document_type_for(kind) == nullptr);
		}
		const bool left_out = kind == AssetKind::Unknown || kind == AssetKind::Archive || kind == AssetKind::ImportSource ||
		                      kind == AssetKind::ImportInput || kind == AssetKind::MissionText || kind == AssetKind::Notes;
		TEST_EXPECT(asset_kind_packed(kind) == !left_out);
	}
	TEST_EXPECT(edited.size() == kDocumentTypeCount);
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentType *type = document_type(DocumentTypeId(id));
		TEST_EXPECT(type && type->id == DocumentTypeId(id));
	}
	TEST_EXPECT(document_type(DocumentTypeId::None) == nullptr);
	for (const AssetKind kind : {AssetKind::ImportSource, AssetKind::MaterialChunk}) {
		const AssetKindRow &row = asset_kind_row(kind);
		TEST_EXPECT(!*row.runtime && !row.file_name && !row.file_names && !row.extensions);
	}
	// The project's notes: the text type opens them, no build packs them, and no archive's name limit binds them.
	TEST_EXPECT(document_type_for(AssetKind::Notes) == document_type_for(AssetKind::Text) &&
	            !asset_kind_packed(AssetKind::Notes) && !archive_name_limit_binds(AssetKind::Notes));
	TEST_EXPECT(archive_name_limit_binds(AssetKind::ImportSource) &&
	            asset_kind_row(AssetKind::MaterialChunk).archive_slot == ArchiveSlot::Resource);
	const AssetKindRow &unknown = asset_kind_row(AssetKind::Unknown);
	TEST_EXPECT(!*unknown.runtime && !unknown.file_name && !unknown.file_names && !unknown.extensions);
	TEST_EXPECT(!asset_kind_packed(AssetKind::Unknown) && unknown.archive_slot == ArchiveSlot::None);
	TEST_EXPECT(!archive_name_limit_binds(AssetKind::Unknown) && !archive_name_limit_binds(AssetKind::Archive));
	TEST_EXPECT(asset_kind_for_name("logo.png") == AssetKind::Texture && asset_kind_for_name("LOGO.PNG") == AssetKind::Texture);
	TEST_EXPECT(&asset_kind_row(AssetKind::kCount) == &unknown);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_runtime_formats();
	failures += test_required_files();
	failures += test_boot_files();
	failures += test_the_table();
	if (failures == 0)
		std::printf("editor_asset_kinds: every runtime format and required file has a kind and a "
		            "slot\n");
	return failures == 0 ? 0 : 1;
}
