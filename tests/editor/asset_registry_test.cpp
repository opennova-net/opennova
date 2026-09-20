// Pins the asset registry (ADR 0046 d6): classification (shared with the runtime
// catalog, plus the editor-only kinds), the scan's exclusions, and the flat-name rules.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/project/project_document.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

static std::vector<uint8_t> bytes_of(const char *text) {
	return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text));
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
	TEST_EXPECT(classify_asset("x.trn", nullptr) == AssetKind::Terrain);
	TEST_EXPECT(classify_asset("x.cpt", nullptr) == AssetKind::TerrainPolyData);
	TEST_EXPECT(classify_asset("x.til", nullptr) == AssetKind::TileInfo);
	TEST_EXPECT(classify_asset("x.env", nullptr) == AssetKind::Environment);
	TEST_EXPECT(classify_asset("x.sbf", nullptr) == AssetKind::SoundBank);
	TEST_EXPECT(classify_asset("x.lwf", nullptr) == AssetKind::WaveBank);
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
	TEST_EXPECT(classify_asset("earlyerr.txt", nullptr) == AssetKind::Text);
	TEST_EXPECT(classify_asset("resource.pff", nullptr) == AssetKind::Archive);
	TEST_EXPECT(classify_asset("readme.docx", nullptr) == AssetKind::Unknown);
	TEST_EXPECT(asset_classification_needs_bytes("a.bin") && !asset_classification_needs_bytes("a.mnu"));

	TEST_EXPECT(expected_asset_kind_for_required_name("gametext.bin") == AssetKind::Strings);
	TEST_EXPECT(expected_asset_kind_for_required_name("menutxt.bin") == AssetKind::Strings);
	TEST_EXPECT(expected_asset_kind_for_required_name("MENUMUS.BIN") == AssetKind::MusicScript);
	TEST_EXPECT(expected_asset_kind_for_required_name("fgn2.bin") == AssetKind::RawBin);
	TEST_EXPECT(expected_asset_kind_for_required_name("main.mnu") == AssetKind::Menu);
	TEST_EXPECT(std::string(asset_kind_token(AssetKind::ItemDefs)) == "item_defs");
	TEST_EXPECT(std::string(asset_kind_label(AssetKind::Strings)) == "String table");
	return 0;
}

static int test_name_rules() {
	TEST_EXPECT(normalized_logical_name("main.mnu") == "MAIN.MNU");
	TEST_EXPECT(normalized_logical_name("Main.MNU  ") == "MAIN.MNU");
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
	TEST_EXPECT(editor_test::write_text(paths.root + "/ui/logo.png.import", "{}"));  // sidecar: skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/.opennova/build/play/1/x.mnu", "cache")); // skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/.git/config", "x"));                  // dot dir: skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/build/export/exported.mnu", "x"));    // export dir: skipped
	TEST_EXPECT(editor_test::write_text(paths.root + "/a/Same.tga", "x"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/b/same.TGA", "y"));                    // duplicate name
	TEST_EXPECT(editor_test::write_text(paths.root + "/textures/a_much_too_long_name.tga", "x")); // too long

	const AssetScan scan = scan_project_assets(paths, doc);
	TEST_EXPECT(scan.entries.size() == 8);
	TEST_EXPECT(scan.find("MAIN.MNU") != nullptr && scan.find("main.mnu")->kind == AssetKind::Menu);
	TEST_EXPECT(scan.find("main.mnu")->relative_path == "menus/main.mnu");
	TEST_EXPECT(scan.find("main.mnu")->size_bytes == 7);
	TEST_EXPECT(scan.find("main.mnu")->modified_time > 0);
	TEST_EXPECT(scan.find("items.def")->kind == AssetKind::ItemDefs);
	TEST_EXPECT(scan.find("gametext.bin")->kind == AssetKind::Strings);
	TEST_EXPECT(scan.find("menumus.bin")->kind == AssetKind::MusicScript);
	TEST_EXPECT(scan.find("logo.png")->kind == AssetKind::Unknown);
	TEST_EXPECT(scan.find("logo.png.import") == nullptr);
	TEST_EXPECT(scan.find("x.mnu") == nullptr);
	TEST_EXPECT(scan.find("exported.mnu") == nullptr);
	TEST_EXPECT(scan.find("config") == nullptr);
	TEST_EXPECT(scan.find("nothing.mnu") == nullptr);

	int duplicates = 0, too_long = 0, unknown = 0, other = 0;
	for (const Diagnostic &d : scan.diagnostics) {
		if (d.code == "asset.name.duplicate") ++duplicates;
		else if (d.code == "asset.name.too_long") ++too_long;
		else if (d.code == "asset.kind.unknown") ++unknown;
		else ++other;
	}
	TEST_EXPECT(duplicates == 1 && too_long == 1 && unknown == 1 && other == 0);
	TEST_EXPECT(diagnostics_have_errors(scan.diagnostics));

	// Entries are ordered by normalized name, so the two "same" files are adjacent.
	bool adjacent = false;
	for (size_t i = 1; i < scan.entries.size(); ++i) {
		if (normalized_logical_name(scan.entries[i - 1].logical_name) == "SAME.TGA" &&
		    normalized_logical_name(scan.entries[i].logical_name) == "SAME.TGA")
			adjacent = true;
	}
	TEST_EXPECT(adjacent);
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
	failures += test_empty_project_scans_clean();
	if (failures == 0) std::printf("editor_asset_registry: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
