// Pins the asset registry (ADR 0046 d6): classification (shared with the runtime
// catalog, plus the editor-only kinds), the scan's exclusions, the import records (an
// output that is not there, a record whose source is gone: S9c), and the flat-name rules
// (the archives' length limit binding only a kind the build packs: S13 PR0). S13 D5: the
// kinds by the table's rows (asset_kinds): the sound banks named as the game names them (a
// .sbf the music bank, a .lwf the sound bank), a face, a score table, a wave and a map project.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

static std::vector<uint8_t> bytes_of(const char *text) {
	return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text));
}

// The entries in the order find reads them by: by key, then by path.
static bool sorted_by_key(const AssetScan &scan) {
	const auto before = [](const AssetEntry &a, const AssetEntry &b) {
		return a.key != b.key ? a.key < b.key : a.relative_path < b.relative_path;
	};
	return std::is_sorted(scan.entries.begin(), scan.entries.end(), before);
}

static int test_classification() {
	const std::vector<uint8_t> rtxt = bytes_of("RTXT....");
	const std::vector<uint8_t> scr = bytes_of("SCR0....");
	const std::vector<uint8_t> raw = bytes_of("\x01\x02\x03\x04");
	TEST_EXPECT(classify_asset("main.mnu", nullptr) == AssetKind::Menu);
	TEST_EXPECT(classify_asset("MENU_STYLE.MNS", nullptr) == AssetKind::MenuStyle);
	TEST_EXPECT(classify_asset("items.def", nullptr) == AssetKind::ItemDefs);
	TEST_EXPECT(classify_asset("Weapon.def", nullptr) == AssetKind::WeaponDefs);
	TEST_EXPECT(classify_asset("ammo.def", nullptr) == AssetKind::AmmoDefs);
	TEST_EXPECT(classify_asset("Avatars.def", nullptr) == AssetKind::AvatarDefs);
	TEST_EXPECT(classify_asset("hudpos.def", nullptr) == AssetKind::HudPosDefs);
	TEST_EXPECT(classify_asset("charattr.def", nullptr) == AssetKind::CharAttrDefs);
	TEST_EXPECT(classify_asset("mystuff.def", nullptr) == AssetKind::OtherDefs);
	TEST_EXPECT(classify_asset("gametext.bin", &rtxt) == AssetKind::Strings);
	TEST_EXPECT(classify_asset("menumus.bin", &scr) == AssetKind::MusicScript);
	TEST_EXPECT(classify_asset("fgn2.bin", &raw) == AssetKind::RawBin);
	TEST_EXPECT(classify_asset("gametext.bin", nullptr) == AssetKind::RawBin);
	TEST_EXPECT(classify_asset("x.3di", nullptr) == AssetKind::Model);
	TEST_EXPECT(classify_asset("x.bad", nullptr) == AssetKind::Animation);
	TEST_EXPECT(classify_asset("x.adm", nullptr) == AssetKind::AnimationMap);
	TEST_EXPECT(classify_asset("x.tga", nullptr) == AssetKind::Texture);
	TEST_EXPECT(classify_asset("x.PCX", nullptr) == AssetKind::Texture);
	TEST_EXPECT(classify_asset("x.dds", nullptr) == AssetKind::Texture);
	TEST_EXPECT(classify_asset("x.fnt", nullptr) == AssetKind::Font);
	TEST_EXPECT(classify_asset("x.bms", nullptr) == AssetKind::Mission);
	// The original mission editor's text, which the game never reads (S14): a kind of its own.
	TEST_EXPECT(classify_asset("briefing.MIS", nullptr) == AssetKind::MissionText &&
	            std::string(asset_kind_token(AssetKind::MissionText)) == "mission_text" &&
	            !asset_kind_packed(AssetKind::MissionText) && document_type_for(AssetKind::MissionText) == nullptr);
	TEST_EXPECT(classify_asset("x.trn", nullptr) == AssetKind::Terrain);
	TEST_EXPECT(classify_asset("x.cpt", nullptr) == AssetKind::TerrainPolyData);
	TEST_EXPECT(classify_asset("x.til", nullptr) == AssetKind::TileInfo);
	TEST_EXPECT(classify_asset("x.env", nullptr) == AssetKind::Environment);
	TEST_EXPECT(classify_asset("x.sbf", nullptr) == AssetKind::MusicBank);
	TEST_EXPECT(classify_asset("x.lwf", nullptr) == AssetKind::SoundBank);
	TEST_EXPECT(classify_asset("x.dbf", nullptr) == AssetKind::DialogBank);
	TEST_EXPECT(classify_asset("x.ptl", nullptr) == AssetKind::Particles);
	TEST_EXPECT(classify_asset("x.ptu", nullptr) == AssetKind::Particles);
	TEST_EXPECT(classify_asset("x.wac", nullptr) == AssetKind::Script);
	TEST_EXPECT(classify_asset("x.kda", nullptr) == AssetKind::Credits);
	TEST_EXPECT(classify_asset("nw_cdata.coo", nullptr) == AssetKind::StringTableCoo);
	TEST_EXPECT(classify_asset("intro.BIK", nullptr) == AssetKind::Video);
	TEST_EXPECT(classify_asset("player.sav", nullptr) == AssetKind::PlayerSave);
	TEST_EXPECT(classify_asset("x.fx", nullptr) == AssetKind::Shader);
	TEST_EXPECT(classify_asset("game.cfg", nullptr) == AssetKind::Config);
	TEST_EXPECT(classify_asset("gt.ssc", nullptr) == AssetKind::Config);
	TEST_EXPECT(classify_asset("assets.cd", nullptr) == AssetKind::Config);
	TEST_EXPECT(classify_asset("game.ini", nullptr) == AssetKind::Config);
	// score.ini by its whole name, before the .ini extension; the name without case.
	TEST_EXPECT(classify_asset("score.ini", nullptr) == AssetKind::Score);
	TEST_EXPECT(classify_asset("SCORE.INI", nullptr) == AssetKind::Score);
	TEST_EXPECT(classify_asset("head.grm", nullptr) == AssetKind::FaceAnimation);
	TEST_EXPECT(classify_asset("DltB086C.wav", nullptr) == AssetKind::Wave);
	TEST_EXPECT(classify_asset("ASP_G7.npz", nullptr) == AssetKind::MapProject);
	TEST_EXPECT(classify_asset("x.npj", nullptr) == AssetKind::MapProject);
	TEST_EXPECT(classify_asset("earlyerr.txt", nullptr) == AssetKind::Text);
	TEST_EXPECT(classify_asset("resource.pff", nullptr) == AssetKind::Archive);
	TEST_EXPECT(classify_asset("readme.docx", nullptr) == AssetKind::Unknown);
	TEST_EXPECT(asset_classification_needs_bytes("a.bin") && !asset_classification_needs_bytes("a.mnu"));
	// A model's .mdt normal map is a texture the TGA reader decodes; a file named by no
	// extension is a material chunk when its bytes hold one (an 8-byte header, then an NQ8B
	// chunk of a 2 x 2 image: S11f's texture by its bytes, S13 A8's kind of its own), and fits
	// one by its name, which cannot tell. A PNG is a texture by its name (S13 A8), and fits an
	// import source too, which its record makes it.
	TEST_EXPECT(classify_asset("bump.MDT", nullptr) == AssetKind::Texture && asset_name_fits_kind("bump.mdt", AssetKind::Texture));
	std::vector<uint8_t> chunk(8 + 8 + 28 + 2 * 2 * 4, 0);
	chunk[8] = 'N', chunk[9] = 'Q', chunk[10] = '8', chunk[11] = 'B';
	chunk[12] = uint8_t(chunk.size() - 16);
	chunk[28] = 2, chunk[32] = 2;
	TEST_EXPECT(is_material_chunk_container(chunk) && classify_asset("field.nq8", &chunk) == AssetKind::MaterialChunk);
	TEST_EXPECT(!is_material_chunk_container(raw) && classify_asset("field.nq8", &raw) == AssetKind::Unknown);
	TEST_EXPECT(classify_asset("field.nq8", nullptr) == AssetKind::Unknown && !asset_classification_needs_bytes("field.nq8"));
	TEST_EXPECT(asset_name_fits_kind("field.nq8", AssetKind::MaterialChunk) && !asset_name_fits_kind("field.nq8", AssetKind::Texture) &&
	            !asset_name_fits_kind("field.nq8", AssetKind::Model));
	TEST_EXPECT(classify_asset("main.mnu", &chunk) == AssetKind::Menu); // a typed name keeps its kind
	TEST_EXPECT(classify_asset("logo.png", nullptr) == AssetKind::Texture && asset_name_fits_kind("logo.png", AssetKind::Texture) &&
	            asset_name_fits_kind("logo.png", AssetKind::ImportSource) && !asset_name_fits_kind("logo.tga", AssetKind::ImportSource));

	TEST_EXPECT(expected_asset_kind_for_required_name("gametext.bin") == AssetKind::Strings);
	TEST_EXPECT(expected_asset_kind_for_required_name("menutxt.bin") == AssetKind::Strings);
	TEST_EXPECT(expected_asset_kind_for_required_name("MENUMUS.BIN") == AssetKind::MusicScript);
	TEST_EXPECT(expected_asset_kind_for_required_name("fgn2.bin") == AssetKind::RawBin);
	TEST_EXPECT(expected_asset_kind_for_required_name("main.mnu") == AssetKind::Menu);
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::ItemDefs)) == "item_defs");
	TEST_EXPECT(std::string(asset_kind_label(AssetKind::Strings)) == "String table");
	// The sound banks as the game names them: the .lwf the sound bank [orig: SoundBank_OpenFile @
	// 0x75caa0], the .sbf the music bank the music scripts stream.
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::SoundBank)) == "sound_bank" &&
	            std::string(asset_kind_label(AssetKind::SoundBank)) == "Sound bank");
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::MusicBank)) == "music_bank" &&
	            std::string(asset_kind_label(AssetKind::MusicBank)) == "Music bank");
	TEST_EXPECT(asset_kind_from_token("wave_bank") == AssetKind::Unknown); // no alias, pre-1.0
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::FaceAnimation)) == "face_animation" &&
	            std::string(asset_kind_label(AssetKind::FaceAnimation)) == "Face animation");
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::Score)) == "score" &&
	            std::string(asset_kind_label(AssetKind::Score)) == "Score table");
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::Wave)) == "wave" &&
	            std::string(asset_kind_token(AssetKind::MapProject)) == "map_project");
	// Every name a row lists types a file as that row's kind: its whole name, and a file of each
	// of its extensions (a .bin without its content a raw table, a .png an image source).
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = asset_kind_row(AssetKind(i));
		if (row.file_name) TEST_EXPECT(classify_asset(row.file_name, nullptr) == row.kind);
		for (const char *const *extension = row.extensions; extension && *extension; ++extension)
			TEST_EXPECT(classify_asset(std::string("x") + *extension, nullptr) == row.kind);
	}
	// Every kind's token reads back as that kind, the last one included, and a value past the last
	// is the Unknown row's.
	for (size_t i = 0; i < kAssetKindCount; ++i)
		TEST_EXPECT(asset_kind_from_token(asset_kind_token(AssetKind(i))) == AssetKind(i));
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::kCount)) == "unknown");
	return 0;
}

static int test_name_rules() {
	TEST_EXPECT(normalized_logical_name("main.mnu") == "MAIN.MNU");
	TEST_EXPECT(normalized_logical_name("Main.MNU  ") == "MAIN.MNU");
	// The whole text, however long: a filter compares a record's text by it, so a word past its 255th
	// byte is found (it stopped there before).
	const std::string long_text = std::string(300, 'x') + " needle";
	TEST_EXPECT(normalized_logical_name(long_text) == std::string(300, 'X') + " NEEDLE");
	TEST_EXPECT(normalized_logical_name(long_text).find(normalized_logical_name("needle")) != std::string::npos);
	TEST_EXPECT(normalized_logical_name(std::string("ab\0cd", 5)) == "AB");
	TEST_EXPECT(logical_name_fits_archive("sixteen_chars.pf"));
	TEST_EXPECT(!logical_name_fits_archive("seventeen_char.pff"));
	TEST_EXPECT(!logical_name_fits_archive("   "));
	return 0;
}

static int test_scan_exclusions_and_diagnostics() {
	editor_test::TempProjectDir dir("opennova_editor_asset_registry_test");
	const std::string root = dir.file("P");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "P", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_text(paths.root + "/menus/main.mnu", "<MENU/>"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/defs/items.def", "begin\nend\n"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/strings/gametext.bin", "RTXT...."));
	TEST_EXPECT(editor_test::write_text(paths.root + "/music/menumus.bin", "SCR0...."));
	TEST_EXPECT(editor_test::write_text(paths.root + "/ui/logo.png", "png"));
	// A sidecar is an import record, never a file of the project; its source is an image
	// source the build never packs, and an output it names that is not there is a
	// warning, not a file. A record whose source is gone lists nothing, not even an
	// output that is still under the cache.
	TEST_EXPECT(editor_test::write_text(paths.root + "/ui/logo.png.import",
	                                    "{\"schema_version\": 1, \"importer\": \"image\", \"version\": 1, \"options\": {}, "
	                                    "\"source_hash\": \"0\", \"outputs\": [\"logo.pcx\"]}"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/ui/gone.png.import",
	                                    "{\"schema_version\": 1, \"importer\": \"image\", \"version\": 1, \"options\": {}, "
	                                    "\"source_hash\": \"0\", \"outputs\": [\"gone.pcx\"]}"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/" + import_output_dir(paths, "ui/gone.png") + "/gone.pcx", "x"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/.opennova/build/play/1/x.mnu", "cache")); // skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/.git/config", "x"));                  // dot dir: skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/build/export/exported.mnu", "x"));    // export dir: skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/a/Same.tga", "x"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/b/same.TGA", "y"));                    // duplicate name
	TEST_EXPECT(editor_test::write_text(paths.root + "/textures/a_much_too_long_name.tga", "x")); // too long
	// A loose kind is copied beside the archives under any name: no finding.
	TEST_EXPECT(editor_test::write_text(paths.root + "/video/a_much_too_long_intro.bik", "x"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/notes/readme.docx", "x"));                // unknown kind

	const AssetScan scan = scan_project_assets(paths, doc);
	TEST_EXPECT(scan.entries.size() == 10);
	TEST_EXPECT(scan.find("a_much_too_long_intro.bik") &&
			scan.find("a_much_too_long_intro.bik")->kind == AssetKind::Video);
	TEST_EXPECT(scan.find("MAIN.MNU") != nullptr && scan.find("main.mnu")->kind == AssetKind::Menu);
	TEST_EXPECT(scan.find("main.mnu")->relative_path == "menus/main.mnu");
	TEST_EXPECT(scan.find("main.mnu")->size_bytes == 7);
	TEST_EXPECT(scan.find("main.mnu")->modified_ticks != 0); // the file system's own clock: any epoch
	TEST_EXPECT(scan.find("items.def")->kind == AssetKind::ItemDefs);
	TEST_EXPECT(scan.find("gametext.bin")->kind == AssetKind::Strings);
	TEST_EXPECT(scan.find("menumus.bin")->kind == AssetKind::MusicScript);
	TEST_EXPECT(scan.find("logo.png")->kind == AssetKind::ImportSource);
	TEST_EXPECT(scan.find("logo.png.import") == nullptr);
	TEST_EXPECT(scan.find("logo.pcx") == nullptr && scan.find("gone.pcx") == nullptr);
	TEST_EXPECT(scan.find("x.mnu") == nullptr);
	TEST_EXPECT(scan.find("exported.mnu") == nullptr);
	TEST_EXPECT(scan.find("config") == nullptr);
	TEST_EXPECT(scan.find("nothing.mnu") == nullptr);

	int duplicates = 0, too_long = 0, unknown = 0, output_missing = 0, orphan = 0, other = 0;
	for (const Diagnostic &d : scan.diagnostics) {
		if (d.code() == "asset.name.duplicate") ++duplicates;
		else if (d.code() == "asset.name.too_long" && d.asset == "textures/a_much_too_long_name.tga")
			++too_long;
		else if (d.code() == "asset.kind.unknown") ++unknown;
		else if (d.code() == "import.output_missing" && d.asset == "ui/logo.png" && d.severity == DiagnosticSeverity::Warning) ++output_missing;
		else if (d.code() == "import.orphan_record" && d.asset == "ui/gone.png.import" && d.severity == DiagnosticSeverity::Warning) ++orphan;
		else ++other;
	}
	TEST_EXPECT(duplicates == 1 && too_long == 1 && unknown == 1 && output_missing == 1 && orphan == 1 && other == 0);
	TEST_EXPECT(diagnostics_have_errors(scan.diagnostics));

	// Entries are ordered by normalized name, so the two "same" files are adjacent.
	bool adjacent = false;
	for (size_t i = 1; i < scan.entries.size(); ++i) {
		if (normalized_logical_name(scan.entries[i - 1].logical_name) == "SAME.TGA" &&
		    normalized_logical_name(scan.entries[i].logical_name) == "SAME.TGA")
			adjacent = true;
	}
	TEST_EXPECT(adjacent);

	// S13 D1: each entry keyed by its normalized name, the entries sorted by key then path (the
	// order find's binary search reads); each found by its logical name in any case (of the two
	// "same" files, the first by path) and by its path as written; a name or a path the scan
	// lacks, none.
	TEST_EXPECT(sorted_by_key(scan));
	for (const AssetEntry &entry : scan.entries) {
		TEST_EXPECT(entry.key == normalized_logical_name(entry.logical_name));
		const AssetEntry *named = scan.find(entry.logical_name);
		TEST_EXPECT(named && named->key == entry.key);
		TEST_EXPECT(named->relative_path <= entry.relative_path);
		TEST_EXPECT(scan.at_path(entry.relative_path) == &entry);
	}
	TEST_EXPECT(scan.find("same.tga") && scan.find("same.tga")->relative_path == "a/Same.tga");
	TEST_EXPECT(scan.find("menus/main.mnu") == nullptr); // a path is no logical name
	TEST_EXPECT(scan.at_path("main.mnu") == nullptr && scan.at_path("Menus/main.mnu") == nullptr);
	TEST_EXPECT(scan.at_path("menus/nothing.mnu") == nullptr);
	return 0;
}

// The scan's lookups over a scan made by hand: 2,000 files named in the reverse of their order,
// indexed (keyed, sorted, their paths indexed), each found by name and by path, in a copy of the
// scan too (the index holds places, not addresses); what it lacks, none. Its entries changed
// after index(): a file pushed is found by its name and its path, and a thousand erased are
// found no more while the others still are, nothing read past the entries (the lookups walk
// them until index() runs again).
static int test_lookups() {
	AssetScan scan;
	for (int i = 1999; i >= 0; --i) {
		char name[16];
		std::snprintf(name, sizeof(name), "f%04d.def", i);
		AssetEntry entry;
		entry.logical_name = name;
		entry.relative_path = std::string("defs/") + (i % 2 ? "odd/" : "even/") + name;
		entry.kind = AssetKind::ItemDefs;
		scan.entries.push_back(entry);
	}
	scan.index();
	TEST_EXPECT(sorted_by_key(scan) && scan.entries.front().logical_name == "f0000.def");
	const AssetScan copy = scan;
	const AssetScan *const scans[] = {&scan, &copy};
	for (const AssetScan *each : scans)
		for (const AssetEntry &entry : each->entries) {
			TEST_EXPECT(each->find(entry.logical_name) == &entry);
			TEST_EXPECT(each->find(normalized_logical_name(entry.logical_name)) == &entry);
			TEST_EXPECT(each->at_path(entry.relative_path) == &entry);
		}
	TEST_EXPECT(!scan.find("f2000.def") && !scan.find("") && !scan.find("f0000.de"));
	TEST_EXPECT(!scan.at_path("defs/f0000.def") && !scan.at_path("defs/odd/f0000.def"));
	TEST_EXPECT(!scan.at_path(""));
	AssetScan changed = scan;
	AssetEntry late;
	late.logical_name = "Late.def";
	late.relative_path = "defs/late/Late.def";
	late.kind = AssetKind::ItemDefs;
	changed.entries.push_back(late);
	TEST_EXPECT(changed.find("late.def") == &changed.entries.back());
	TEST_EXPECT(changed.at_path("defs/late/Late.def") == &changed.entries.back());
	TEST_EXPECT(changed.find("f0005.def") && changed.at_path("defs/odd/f0005.def"));
	changed.entries.erase(changed.entries.begin(), changed.entries.begin() + 1000);
	TEST_EXPECT(!changed.find("f0000.def") && !changed.at_path("defs/even/f0000.def"));
	TEST_EXPECT(changed.find("f1999.def") && changed.at_path("defs/odd/f1999.def"));
	TEST_EXPECT(changed.find("late.def") && changed.at_path("defs/late/Late.def"));
	changed.index();
	TEST_EXPECT(sorted_by_key(changed) && changed.entries.size() == 1001);
	const AssetEntry *late_entry = changed.at_path("defs/late/Late.def");
	TEST_EXPECT(late_entry && changed.find("LATE.DEF") == late_entry);
	return 0;
}

// The entry a request names, its case aside (AssetScan::named, what the session opens and edits by,
// ADR 0046 S17): the path as spelled, the path in another case, a name alone by find(); a path names
// its folder (the file of its name elsewhere is not it); two files answering to one spelling in
// another case (a file system that keeps case) are none, said ambiguous.
static int test_named() {
	AssetScan scan;
	const auto add = [&](const char *path) {
		AssetEntry entry;
		entry.relative_path = path;
		entry.logical_name = std::string(path).substr(std::string(path).find_last_of('/') + 1);
		entry.kind = AssetKind::Menu;
		scan.entries.push_back(entry);
	};
	add("menus/Extra.mnu");
	add("menus/Main.mnu");
	add("old/Twin.mnu");
	add("old/twin.mnu");
	scan.index();
	bool ambiguous = true;
	const AssetEntry *extra = scan.at_path("menus/Extra.mnu");
	TEST_EXPECT(extra && scan.named("menus/Extra.mnu", &ambiguous) == extra && !ambiguous);
	TEST_EXPECT(scan.named("MENUS/EXTRA.MNU", &ambiguous) == extra && !ambiguous);
	TEST_EXPECT(scan.named("extra.MNU") == extra);
	TEST_EXPECT(!scan.named("missions/old/Extra.mnu") && !scan.named("menus/nowhere.mnu"));
	TEST_EXPECT(scan.named("old/Twin.mnu", &ambiguous) == scan.at_path("old/Twin.mnu") && !ambiguous);
	TEST_EXPECT(!scan.named("OLD/TWIN.MNU", &ambiguous) && ambiguous);
	return 0;
}

static int test_empty_project_scans_clean() {
	editor_test::TempProjectDir dir("opennova_editor_asset_registry_empty_test");
	const std::string root = dir.file("E");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "E", "jo", doc, error));
	const AssetScan scan = scan_project_assets(ProjectPaths::for_root(root), doc);
	TEST_EXPECT(scan.entries.empty());
	TEST_EXPECT(scan.diagnostics.empty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_classification();
	failures += test_name_rules();
	failures += test_scan_exclusions_and_diagnostics();
	failures += test_lookups();
	failures += test_named();
	failures += test_empty_project_scans_clean();
	if (failures == 0) std::printf("editor_asset_registry: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
