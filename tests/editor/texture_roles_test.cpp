// ADR 0046 S18, the texture roles: the catalog (documents/texture_roles: every way the game uses a
// texture, a row each in the enum's order with its token, words, loader, formats, size rule and witness,
// the research's table as data, pinned here), and the loaders' name rules and transforms
// (documents/texture_load_rules): the file each loader opens for a name and the reader that decodes it
// (STAGE's .dds sibling first, ARCHIVE's sibling at the last dot then a PCX's luminance alpha, HUD's
// .FULL and .ALPHA suffixes and its PCX white with alpha from blue, FILE's PCX for any name but a .TGA,
// MENU's by the last extension), and what each transform makes of the texels, against vectors worked
// by hand.
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_view.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/tga/tga.h>

#include <editor/project/project_document.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

TextureNameTest files_of(std::set<std::string> names) {
	return [names = std::move(names)](const std::string &name) {
		std::string lower;
		for (char c : name) lower.push_back(char(c >= 'A' && c <= 'Z' ? c + 32 : c));
		for (const std::string &held : names) {
			std::string other;
			for (char c : held) other.push_back(char(c >= 'A' && c <= 'Z' ? c + 32 : c));
			if (other == lower) return true;
		}
		return false;
	};
}

int test_catalog() {
	TEST_EXPECT(kTextureRoleCount == 46);
	std::set<std::string> tokens;
	for (size_t i = 0; i < kTextureRoleCount; ++i) {
		const TextureRoleRow &row = texture_role_row(static_cast<TextureRoleId>(i));
		TEST_EXPECT(static_cast<size_t>(row.id) == i && *row.token && *row.words && *row.witness);
		TEST_EXPECT(tokens.insert(row.token).second);
		TextureRoleId back = TextureRoleId::kCount;
		TEST_EXPECT(texture_role_from_token(row.token, back) && back == row.id);
		TEST_EXPECT(*texture_loader_token(row.loader) && *texture_role_group_words(row.group));
	}
	TextureRoleId none = TextureRoleId::kCount;
	TEST_EXPECT(!texture_role_from_token("no_such_role", none));
	// The rules the research witnessed, pinned.
	const TextureRoleRow &colour = texture_role_row(TextureRoleId::TerrainColourMap);
	TEST_EXPECT(colour.loader == TextureLoader::Tga && colour.size == TextureSizeRule::Exact && colour.width == 1024 &&
	            colour.height == 1024 && texture_size_words(colour) == "1024 x 1024");
	TEST_EXPECT(texture_role_takes(colour, ".TGA") && !texture_role_takes(colour, ".dds") && !texture_role_takes(colour, ".pcx"));
	const TextureRoleRow &foliage = texture_role_row(TextureRoleId::TerrainFoliageMap);
	TEST_EXPECT(foliage.size == TextureSizeRule::SquarePowerOfTwoAtMost && foliage.width == 1024 && !foliage.reads_alpha &&
	            foliage.loader == TextureLoader::Pcx8);
	TEST_EXPECT(texture_role_row(TextureRoleId::TerrainTileAtlas).size == TextureSizeRule::MultipleOf &&
	            texture_role_row(TextureRoleId::TerrainTileAtlas).width == 64);
	TEST_EXPECT(texture_role_row(TextureRoleId::ModelNormalMap).size == TextureSizeRule::AtMost &&
	            texture_role_row(TextureRoleId::ModelNormalMap).width == 512 &&
	            texture_role_takes(texture_role_row(TextureRoleId::ModelNormalMap), ".mdt") &&
	            !texture_role_takes(texture_role_row(TextureRoleId::ModelNormalMap), ".pcx"));
	TEST_EXPECT(texture_role_row(TextureRoleId::LoadingScreen).size == TextureSizeRule::Exact &&
	            texture_role_row(TextureRoleId::LoadingScreen).width == 800 &&
	            texture_role_row(TextureRoleId::LoadingScreen).height == 600);
	TEST_EXPECT(texture_role_takes(texture_role_row(TextureRoleId::ParticleGraphic), ".tga") &&
	            !texture_role_takes(texture_role_row(TextureRoleId::ParticleGraphic), ".dds"));
	TEST_EXPECT(texture_role_takes(texture_role_row(TextureRoleId::MenuImage), ".png") &&
	            !texture_role_takes(texture_role_row(TextureRoleId::ModelDiffuse), ".png"));
	TEST_EXPECT(texture_role_row(TextureRoleId::HudMfd).size == TextureSizeRule::PowerOfTwo);
	std::printf("catalog: %zu roles, each with a token, words, a loader, formats and a witness\n", kTextureRoleCount);
	return 0;
}

int test_names() {
	// STAGE: the .dds sibling (cut 3 characters past the first '.') wins; else the TGA reader.
	TextureLoad load = texture_load(TextureLoader::Stage, "body.tga", files_of({"body.tga", "body.dds"}));
	TEST_EXPECT(load.file == "body.dds" && load.reader == TextureFileReader::Dds);
	load = texture_load(TextureLoader::Stage, "body.tga", files_of({"body.tga"}));
	TEST_EXPECT(load.file == "body.tga" && load.reader == TextureFileReader::Tga);
	load = texture_load(TextureLoader::Stage, "sky.pcx", files_of({"sky.pcx"}));
	TEST_EXPECT(load.file == "sky.pcx" && load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	// A normal map's .tga converted from its height; its .mdt as it is.
	load = texture_load(TextureLoader::Normal, "skin.tga", files_of({"skin.tga"}), 4);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::NormalFromHeight);
	load = texture_load(TextureLoader::Normal, "skin.MDT", files_of({"skin.MDT"}), 4);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::None);
	// PLAIN's upper-case .PCX as written: white, alpha from blue; a lower-case one opaque colour.
	load = texture_load(TextureLoader::Plain, "MASK.PCX", files_of({"MASK.PCX"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::WhiteAlphaFromBlue);
	load = texture_load(TextureLoader::Plain, "mask.pcx", files_of({"mask.pcx"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	// ARCHIVE (renderer::texture_load_attempts): the .dds sibling first; else a PCX, its luminance its alpha
	// where the caller names it twice (a sky map's), else opaque (a scar's).
	load = texture_load(TextureLoader::Archive, "cld.day.pcx", files_of({"cld.day.dds", "cld.day.pcx"}), 0, -1,
	                    TextureRoleId::SkyCloud);
	TEST_EXPECT(load.file == "cld.day.dds" && load.reader == TextureFileReader::Dds);
	load = texture_load(TextureLoader::Archive, "cloud01.pcx", files_of({"cloud01.pcx"}), 0, -1, TextureRoleId::SkyCloud);
	TEST_EXPECT(load.file == "cloud01.pcx" && load.reader == TextureFileReader::Pcx &&
	            load.transform == TextureLoadTransform::LuminanceAlpha && load.alpha_source == "cloud01.pcx");
	load = texture_load(TextureLoader::Archive, "scorch1.pcx", files_of({"scorch1.pcx"}), 0, -1, TextureRoleId::ImpactScar);
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	load = texture_load(TextureLoader::Archive, "cloud01.bmp", files_of({"cloud01.bmp"}));
	TEST_EXPECT(load.file.empty() && load.reader == TextureFileReader::None);
	// HUD: the suffix decides the mode; a PCX white with alpha from blue; a file the project lacks, the name it
	// would open.
	load = texture_load(TextureLoader::Hud, "stance.tga.FULL", files_of({"stance.tga"}), 1);
	TEST_EXPECT(load.file == "stance.tga" && load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::None);
	load = texture_load(TextureLoader::Hud, "frame.tga.alpha", files_of({"frame.tga"}), 0);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::AlphaOnly);
	load = texture_load(TextureLoader::Hud, "pip.pcx", files_of({"pip.pcx"}), 0);
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::WhiteAlphaFromBlue);
	load = texture_load(TextureLoader::Hud, "gone.tga", files_of({}), 0);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && !files_of({})(load.file));
	// FILE: .TGA through the TGA reader, any other name the PCX reader.
	TEST_EXPECT(texture_load(TextureLoader::File, "TSDicon.tga", files_of({})).reader == TextureFileReader::Tga);
	TEST_EXPECT(texture_load(TextureLoader::File, "NVGScale.bmp", files_of({})).reader == TextureFileReader::Pcx);
	// MENU: by the last extension; a .tga the files lack loads its .dds.
	load = texture_load(TextureLoader::Menu, "logo.tga", files_of({"logo.dds"}));
	TEST_EXPECT(load.file == "logo.dds" && load.reader == TextureFileReader::Dds);
	TEST_EXPECT(texture_load(TextureLoader::Menu, "art.png", files_of({"art.png"})).reader == TextureFileReader::Png);
	TEST_EXPECT(texture_load(TextureLoader::Menu, "art.bmp", files_of({"art.bmp"})).reader == TextureFileReader::None);
	std::printf("names: STAGE, NORMAL, PLAIN, ARCHIVE, HUD, FILE and MENU open the files their witnesses say\n");
	return 0;
}

int test_transforms() {
	// A 2 x 1 indexed image: texel 0 index 3 (R 30, G 60, B 90), texel 1 index 5 (R 255, G 255, B 0).
	TextureImage image;
	image.levels.push_back({2, 1, {30, 60, 90, 255, 255, 255, 0, 255}});
	image.palette.assign(768, 0);
	image.palette[9] = 30;
	image.palette[10] = 60;
	image.palette[11] = 90;
	image.palette[15] = 255;
	image.palette[16] = 255;
	image.palette[17] = 0;
	image.indices = {3, 5};
	// Luminance: (85 x (30 + 60 + 90)) >> 8 = 59; (85 x 510) >> 8 = 169 (the sum held to 16 bits).
	std::shared_ptr<const TextureImage> out = apply_load_transform(image, TextureLoadTransform::LuminanceAlpha);
	TEST_EXPECT(out->levels[0].rgba[3] == 59 && out->levels[0].rgba[7] == 169 && out->levels[0].rgba[0] == 30);
	// White, alpha from blue.
	out = apply_load_transform(image, TextureLoadTransform::WhiteAlphaFromBlue);
	TEST_EXPECT(out->levels[0].rgba == std::vector<uint8_t>({255, 255, 255, 90, 255, 255, 255, 0}));
	// Alpha only: white under the alpha, the form an A8 takes under the HUD's alpha material.
	out = apply_load_transform(image, TextureLoadTransform::AlphaOnly);
	TEST_EXPECT(out->levels[0].rgba == std::vector<uint8_t>({255, 255, 255, 255, 255, 255, 255, 255}));
	// None: the texels as read.
	out = apply_load_transform(image, TextureLoadTransform::None);
	TEST_EXPECT(out->levels[0].rgba == image.levels[0].rgba);
	std::printf("transforms: luminance alpha, white with alpha from blue, alpha only\n");
	return 0;
}

int test_query() {
	// The catalog over the wire, no project open: every role in its order, each row's columns.
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	JsonValue args;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse("{\"limit\":200}", args, error));
	const JsonValue answer = session.query("texture_roles", args, error);
	TEST_EXPECT(error.empty() && size_t(answer.get_number("count", 0)) == kTextureRoleCount);
	const JsonValue *roles = answer.get("roles");
	TEST_EXPECT(roles && roles->is_array() && roles->array.size() == kTextureRoleCount);
	if (!roles || roles->array.size() != kTextureRoleCount) return 1;
	const JsonValue &colour = roles->array[size_t(TextureRoleId::TerrainColourMap)];
	TEST_EXPECT(colour.get_string("role", "") == "terrain_colour_map" && colour.get_string("loader", "") == "tga" &&
	            colour.get_string("group", "") == "Terrain");
	const JsonValue *formats = colour.get("formats");
	TEST_EXPECT(formats && formats->array.size() == 1 && formats->array[0].string == ".tga");
	const JsonValue *size = colour.get("size");
	TEST_EXPECT(size && size->get_string("rule", "") == "exact" && size->get_number("width", 0) == 1024 &&
	            size->get_string("words", "") == "1024 x 1024");
	TEST_EXPECT(colour.get_string("witness", "").find("PolyTrn_InitTextures") != std::string::npos);
	const JsonValue &foliage = roles->array[size_t(TextureRoleId::TerrainFoliageMap)];
	TEST_EXPECT(!foliage.get_bool("reads_alpha", true) && foliage.get_string("loader", "") == "pcx8");
	// A page of it.
	TEST_EXPECT(opennova::io::json_parse("{\"offset\":44,\"limit\":10}", args, error));
	const JsonValue page = session.query("texture_roles", args, error);
	TEST_EXPECT(page.get("roles") && page.get("roles")->array.size() == 2 &&
	            page.get("roles")->array[1].get_string("role", "") == "cinematic_fade");
	std::printf("query: texture_roles answers the %zu roles, a page at a time\n", kTextureRoleCount);
	return 0;
}

int test_reference_load() {
	// The argument a reference gives its loader: a model row's type, a role (with what the referrer's
	// content adds), or none.
	TEST_EXPECT(texture_arg_is_row_type(0) && texture_arg_is_row_type(18) && !texture_arg_is_row_type(-1) &&
	            !texture_arg_is_row_type(texture_role_arg(TextureRoleId::SkyCloud)));
	TextureRoleId role = TextureRoleId::kCount;
	const int32_t colour = texture_role_arg(TextureRoleId::TerrainColourMap, kTextureArgGates);
	TEST_EXPECT(texture_arg_role(colour, role) && role == TextureRoleId::TerrainColourMap && texture_arg_gates(colour));
	TEST_EXPECT(!texture_arg_gates(texture_role_arg(TextureRoleId::TerrainBlendMap)) && !texture_arg_role(4, role) &&
	            !texture_arg_role(-1, role));
	const TextureNameTest files = files_of({"body.tga", "body.dds", "cld.pcx", "cld.dds", "ground.tga", "ground.dds",
	                                        "stance.tga", "trntile10.tga"});
	// A model row by its type: the .dds beside the name.
	TEST_EXPECT(texture_reference_load("body.tga", 0, files).file == "body.dds");
	// A sky map through ARCHIVE: its .dds too; a colour map through the TGA reader: the name alone.
	TEST_EXPECT(texture_reference_load("cld.pcx", texture_role_arg(TextureRoleId::SkyCloud), files).file == "cld.dds");
	TextureLoad load = texture_reference_load("ground.tga", colour, files);
	TEST_EXPECT(load.file == "ground.tga" && load.reader == TextureFileReader::Tga);
	// A mission's tile set: TGA for its extension.
	load = texture_reference_load("trntile10.bmp", texture_role_arg(TextureRoleId::TerrainTileAtlas, kTextureArgTileSet), files);
	TEST_EXPECT(load.file == "trntile10.TGA" && load.reader == TextureFileReader::Tga);
	// The HUD's alpha-only art: the suffix cut, alpha only.
	load = texture_reference_load("stance.tga", texture_role_arg(TextureRoleId::HudAlphaOnly), files);
	TEST_EXPECT(load.file == "stance.tga" && load.transform == TextureLoadTransform::AlphaOnly);
	// A use whose loader is not witnessed: the name as written, nothing else.
	load = texture_reference_load("cross.bmp", -1, files);
	TEST_EXPECT(load.file == "cross.bmp" && load.reader == TextureFileReader::None);
	std::printf("reference load: a row's type, a role's loader, a tile set's TGA, the name as written\n");
	return 0;
}

std::vector<uint8_t> tga_bytes() {
	const std::vector<uint8_t> rgba(16, 200);
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), 2, 2, out, error);
	return out;
}

// The graph resolves each texture reference by its loader (ADR 0046 S18) over a project: a terrain's
// colour map read by the TGA reader alone, missing where only a PNG of its stem is (an alternate no
// loader reads), which refuses a build; its splat detail's .dds read for its .tga; its blend map
// missing refusing a build only once splat details are authored; a sky map's .dds; an item's HUD image.
int test_graph() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_roles"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const std::string root = dir.file("project");
	editor_test::handle_to_end(session, request::new_project(root, "Roles"));
	editor_test::create_missing_files(session);
	const std::string trn_head = "polytrn_detailmap detail.tga\npolytrn_polydata ground.cpt\npolytrn_sectorcount 1\n"
	                             "polytrn_sectors 0\n";
	TEST_EXPECT(editor_test::write_text(root + "/terrains/a.trn", trn_head +
	                                                                  "polytrn_colormap ground.tga\npolytrn_detailmap_c1 splat.tga\n"
	                                                                  "polytrn_detailblendmap blend.tga\n") &&
	            editor_test::write_text(root + "/terrains/b.trn",
	                                    trn_head + "polytrn_colormap bcol.tga\npolytrn_detailblendmap noblend.tga\n") &&
	            editor_test::write_bytes(root + "/textures/ground.png", editor_test::gradient_png(2, 2)) &&
	            editor_test::write_bytes(root + "/textures/detail.tga", tga_bytes()) &&
	            editor_test::write_bytes(root + "/textures/bcol.tga", tga_bytes()) &&
	            editor_test::write_bytes(root + "/textures/splat.dds", tga_bytes()) &&
	            editor_test::write_bytes(root + "/textures/cloud.dds", tga_bytes()) &&
	            editor_test::write_bytes(root + "/textures/stance.tga", tga_bytes()) &&
	            editor_test::write_text(root + "/defs/items.def",
	                                    "begin \"Brick\"\nid 100300\ntype building\nhud_image stance.tga\nend\n"));
	std::string env;
	{
		std::ostringstream text;
		opennova::env::Config config;
		config.sky_map1 = "cloud.pcx";
		config.sky_map2 = "cloud2.pcx";
		std::string error;
		TEST_EXPECT(opennova::env::save_env(text, config, error));
		env = text.str();
	}
	TEST_EXPECT(editor_test::write_text(root + "/envs/sky.env", env) &&
	            editor_test::write_bytes(root + "/textures/cloud2.png", editor_test::gradient_png(2, 2)));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const AssetGraph *graph = session.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr);
	if (!graph) return 1;
	const auto resolved = [&](const std::string &source, const std::string &field) {
		for (const GraphEdge *edge : graph->references_of(source))
			if (edge->field == field) {
				std::string file;
				return graph->resolve(*edge, &file) == ReferenceStatus::Present ? file : std::string("<missing>");
			}
		return std::string("<none>");
	};
	TEST_EXPECT(resolved("terrains/a.trn", "polytrn_colormap") == "<missing>");
	TEST_EXPECT(resolved("terrains/a.trn", "polytrn_detailmap") == "textures/detail.tga");
	TEST_EXPECT(resolved("terrains/a.trn", "polytrn_detailmap_c1") == "textures/splat.dds");
	TEST_EXPECT(resolved("terrains/b.trn", "polytrn_colormap") == "textures/bcol.tga");
	TEST_EXPECT(resolved("envs/sky.env", "sky_map1") == "textures/cloud.dds");
	TEST_EXPECT(resolved("envs/sky.env", "sky_map2") == "<missing>");
	TEST_EXPECT(resolved("defs/items.def", "hud_image") == "textures/stance.tga");
	// What refuses a build: the colour map, and a's blend map (its splat detail authored); not b's.
	std::set<std::string> blocking, listed;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "reference.missing") continue;
		listed.insert(d.asset + " " + d.field);
		if (blocks_build(d)) blocking.insert(d.asset + " " + d.field);
	}
	TEST_EXPECT(blocking == std::set<std::string>({"terrains/a.trn polytrn_colormap", "terrains/a.trn polytrn_detailblendmap"}));
	TEST_EXPECT(listed.count("terrains/b.trn polytrn_detailblendmap") && listed.count("envs/sky.env sky_map2"));
	// Its words say what the game does.
	for (const Diagnostic &d : session.view().findings.diagnostics)
		if (d.code() == "reference.missing" && d.field == "polytrn_colormap")
			TEST_EXPECT(d.message.find("the mission aborts") != std::string::npos);
	std::printf("graph: %zu texture references found missing, %zu of them refusing a build\n", listed.size(), blocking.size());
	return 0;
}

// The retail leg (OPENNOVA_JO_DIR): the install's referrers of textures (its terrains, environments,
// particle files, HUD layout, catalogs, menus and missions) and every texture it ships, by name (an
// empty file each: what a reference resolves to is a name), in a project: every texture reference
// resolved by its loader, counted by role; none the game would abort the mission over (every shipped
// terrain's colour map, and its blend map where splat details are authored, is there).
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's texture references by role)");
		return 0;
	}
	editor_test::TempProjectDir dir{"opennova_editor_texture_roles_retail"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const std::string root = dir.file("project");
	editor_test::handle_to_end(session, request::new_project(root, "Roles"));
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	std::string why;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	const std::set<AssetKind> referrers = {AssetKind::Terrain,    AssetKind::Environment, AssetKind::Particles,
	                                       AssetKind::HudPosDefs, AssetKind::ItemDefs,    AssetKind::WeaponDefs,
	                                       AssetKind::AmmoDefs,   AssetKind::Menu,        AssetKind::MenuStyle,
	                                       AssetKind::Mission};
	size_t textures = 0, files = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		const AssetKind kind = classify_asset(name, nullptr);
		if (kind == AssetKind::Texture) {
			textures += editor_test::write_bytes(root + "/textures/" + name, {}) ? 1 : 0;
		} else if (referrers.count(kind)) {
			std::vector<uint8_t> bytes;
			if (mount.read_file_raw(name, bytes) && editor_test::write_bytes(root + "/" + asset_kind_token(kind) + "/" + name, bytes))
				++files;
		}
	}
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const AssetGraph *graph = session.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr && textures > 1000 && files > 100);
	if (!graph) return 1;
	// Each role's references and those its loader finds nothing for.
	std::map<std::string, std::pair<size_t, size_t>> by_role;
	std::map<std::string, std::vector<std::string>> examples;
	size_t unwitnessed = 0;
	graph->for_each_edge([&](const GraphEdge &edge) {
		if (edge.kind != ReferenceKind::Texture) return;
		TextureRoleId role = TextureRoleId::kCount;
		const std::string token = texture_arg_role(edge.loader_arg, role) ? texture_role_row(role).token
		                          : texture_arg_is_row_type(edge.loader_arg) ? "model_row"
		                                                                     : "not_witnessed";
		unwitnessed += token == std::string("not_witnessed") ? 1 : 0;
		auto &[count, missing] = by_role[token];
		++count;
		if (graph->resolve(edge) != ReferenceStatus::Missing) return;
		++missing;
		if (examples[token].size() < 4) examples[token].push_back(basename_of(edge.source) + ": " + edge.value);
	});
	for (const auto &[role, counts] : by_role) {
		std::string some;
		for (const std::string &example : examples[role]) some += (some.empty() ? " (" : ", ") + example;
		std::printf("retail: %-28s %6zu references, %5zu missing%s\n", role.c_str(), counts.first, counts.second,
		            some.empty() ? "" : (some + ")").c_str());
	}
	// The game aborts no shipped mission over its terrain's maps.
	size_t gating = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics)
		if (d.code() == "reference.missing" && blocks_build(d) && editor_test::reference_of(d).kind == ReferenceKind::Texture) {
			std::fprintf(stderr, "retail: %s %s: %s\n", d.asset.c_str(), d.field.c_str(), d.message.c_str());
			++gating;
		}
	TEST_EXPECT(gating == 0);
	TEST_EXPECT(by_role.count("terrain_colour_map") && by_role["terrain_colour_map"].second == 0);
	std::printf("retail: %zu textures and %zu referrers; %zu references of a loader not witnessed yet; none refusing a build\n",
	            textures, files, unwitnessed);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_catalog();
	failures += test_names();
	failures += test_transforms();
	failures += test_query();
	failures += test_reference_load();
	failures += test_graph();
	failures += test_retail();
	if (failures == 0) std::printf("editor_texture_roles: all passed\n");
	return failures == 0 ? 0 : 1;
}
