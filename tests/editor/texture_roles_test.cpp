// ADR 0046 S18, the texture roles: the catalog (documents/texture_roles: every way the game uses a
// texture, a row each in the enum's order with its token, words, loader, formats, size rule and witness,
// the research's table as data, pinned here), and the loaders' name rules and transforms
// (documents/texture_load_rules): the file each loader opens for a name and the reader that decodes it
// (STAGE's .dds sibling first, ARCHIVE's sibling at the last dot then a PCX's luminance alpha, HUD's
// .FULL and .ALPHA suffixes and its PCX white with alpha from blue, FILE's PCX for any name but a .TGA,
// MENU's by the last extension), and what each transform makes of the texels, against vectors worked
// by hand; the graph resolving each reference by its loader over a project, and every spelling of a texture
// reference the shipped referrers take resolved, none refusing a build.
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kinds.h>
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
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "common/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace renderer = opennova::renderer;
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
	TEST_EXPECT(renderer::kTextureRoleCount == 46);
	std::set<std::string> tokens;
	for (size_t i = 0; i < renderer::kTextureRoleCount; ++i) {
		const TextureRoleRow &row = texture_role_row(static_cast<renderer::TextureRoleId>(i));
		TEST_EXPECT(static_cast<size_t>(row.id) == i && *row.token && *row.words && *row.witness);
		TEST_EXPECT(tokens.insert(row.token).second);
		renderer::TextureRoleId back = renderer::TextureRoleId::kCount;
		TEST_EXPECT(texture_role_from_token(row.token, back) && back == row.id);
		TEST_EXPECT(*texture_loader_token(row.loader) && *texture_role_group_words(row.group));
	}
	renderer::TextureRoleId none = renderer::TextureRoleId::kCount;
	TEST_EXPECT(!texture_role_from_token("no_such_role", none));
	// Each row the engine's (renderer/texture_roles, whose rules renderer_texture_roles pins) with the words.
	for (size_t i = 0; i < renderer::kTextureRoleCount; ++i) {
		const renderer::TextureRoleId id = static_cast<renderer::TextureRoleId>(i);
		const renderer::TextureRole &engine = renderer::texture_role(id);
		const TextureRoleRow &row = texture_role_row(id);
		TEST_EXPECT(row.loader == engine.loader && row.formats == engine.formats && row.size == engine.size &&
		            row.width == engine.width && row.height == engine.height && row.reads_alpha == engine.reads_alpha);
	}
	TEST_EXPECT(texture_size_words(texture_role_row(renderer::TextureRoleId::TerrainColourMap)) == "1024 x 1024");
	TEST_EXPECT(texture_size_words(texture_role_row(renderer::TextureRoleId::ParticleGraphic))
	                    .find("256 for a bump, mod, mod2x, bumpadd or distort graphic") != std::string::npos);
	TEST_EXPECT(std::string(texture_loader_token(texture_role_row(renderer::TextureRoleId::SkyCloud).loader)) == "archive" &&
	            std::string(texture_loader_token(texture_role_row(renderer::TextureRoleId::HudAlphaOnly).loader)) == "hud" &&
	            std::string(texture_loader_token(texture_role_row(renderer::TextureRoleId::ParticleGraphic).loader)) == "ptl");
	std::printf("catalog: %zu roles, each with a token, words, a loader, formats and a witness\n", renderer::kTextureRoleCount);
	return 0;
}

int test_names() {
	// STAGE: the .dds sibling (cut 3 characters past the first '.') wins; else the TGA reader.
	TextureLoad load = texture_load(renderer::TextureLoader::Stage, "body.tga", files_of({"body.tga", "body.dds"}));
	TEST_EXPECT(load.file == "body.dds" && load.reader == TextureFileReader::Dds);
	load = texture_load(renderer::TextureLoader::Stage, "body.tga", files_of({"body.tga"}));
	TEST_EXPECT(load.file == "body.tga" && load.reader == TextureFileReader::Tga);
	load = texture_load(renderer::TextureLoader::Stage, "sky.pcx", files_of({"sky.pcx"}));
	TEST_EXPECT(load.file == "sky.pcx" && load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	// A normal map's .tga converted from its height; its .mdt as it is.
	load = texture_load(renderer::TextureLoader::Normal, "skin.tga", files_of({"skin.tga"}), 4);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::NormalFromHeight);
	load = texture_load(renderer::TextureLoader::Normal, "skin.MDT", files_of({"skin.MDT"}), 4);
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::None);
	// PLAIN's upper-case .PCX as written: white, alpha from blue; a lower-case one opaque colour.
	load = texture_load(renderer::TextureLoader::Plain, "MASK.PCX", files_of({"MASK.PCX"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::WhiteAlphaFromBlue);
	load = texture_load(renderer::TextureLoader::Plain, "mask.pcx", files_of({"mask.pcx"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	// ARCHIVE (renderer::texture_load_attempts): the .dds sibling first; else a PCX, its luminance its alpha
	// where the caller names it twice (a sky map's), else opaque (a scar's).
	load = texture_load(renderer::TextureLoader::ArchiveSelfAlpha, "cld.day.pcx", files_of({"cld.day.dds", "cld.day.pcx"}));
	TEST_EXPECT(load.file == "cld.day.dds" && load.reader == TextureFileReader::Dds);
	load = texture_load(renderer::TextureLoader::ArchiveSelfAlpha, "cloud01.pcx", files_of({"cloud01.pcx"}));
	TEST_EXPECT(load.file == "cloud01.pcx" && load.reader == TextureFileReader::Pcx &&
	            load.transform == TextureLoadTransform::LuminanceAlpha && load.alpha_source == "cloud01.pcx");
	load = texture_load(renderer::TextureLoader::Archive, "scorch1.pcx", files_of({"scorch1.pcx"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::None);
	load = texture_load(renderer::TextureLoader::Archive, "cloud01.bmp", files_of({"cloud01.bmp"}));
	TEST_EXPECT(load.file.empty() && load.reader == TextureFileReader::None);
	// HUD: the suffix decides the mode; a PCX white with alpha from blue; a file the project lacks, the name it
	// would open.
	load = texture_load(renderer::TextureLoader::HudColor, "stance.tga.FULL", files_of({"stance.tga"}));
	TEST_EXPECT(load.file == "stance.tga" && load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::None);
	load = texture_load(renderer::TextureLoader::HudColor, "frame.tga.alpha", files_of({"frame.tga"}));
	TEST_EXPECT(load.reader == TextureFileReader::Tga && load.transform == TextureLoadTransform::AlphaOnly);
	load = texture_load(renderer::TextureLoader::HudColor, "pip.pcx", files_of({"pip.pcx"}));
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::WhiteAlphaFromBlue);
	// A PCX in the HUD's alpha mode: its blue the alpha, then that alpha alone.
	load = texture_load(renderer::TextureLoader::HudColor, "pip.pcx", files_of({"pip.pcx"}), 0, 1);
	TEST_EXPECT(load.reader == TextureFileReader::Pcx && load.transform == TextureLoadTransform::BlueAlphaOnly);
	load = texture_load(renderer::TextureLoader::HudColor, "gone.tga", files_of({}));
	TEST_EXPECT(load.reader == TextureFileReader::Tga && !files_of({})(load.file));
	// FILE: .TGA through the TGA reader, any other name the PCX reader.
	TEST_EXPECT(texture_load(renderer::TextureLoader::File, "TSDicon.tga", files_of({})).reader == TextureFileReader::Tga);
	TEST_EXPECT(texture_load(renderer::TextureLoader::File, "NVGScale.bmp", files_of({})).reader == TextureFileReader::Pcx);
	// MENU: by the last extension; a .tga the files lack loads its .dds.
	load = texture_load(renderer::TextureLoader::Menu, "logo.tga", files_of({"logo.dds"}));
	TEST_EXPECT(load.file == "logo.dds" && load.reader == TextureFileReader::Dds);
	TEST_EXPECT(texture_load(renderer::TextureLoader::Menu, "art.png", files_of({"art.png"})).reader == TextureFileReader::Png);
	TEST_EXPECT(texture_load(renderer::TextureLoader::Menu, "art.bmp", files_of({"art.bmp"})).reader == TextureFileReader::None);
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
	// Luminance, from the PCX the archive loader reads: (85 x (30 + 60 + 90)) >> 8 = 59; (85 x 510) >> 8 = 169
	// (the sum held to 16 bits), each texel's by its index, as decode_pcx_luminance_alpha reads them.
	{
		opennova::IndexedImage8 indexed;
		indexed.width = 2;
		indexed.height = 1;
		indexed.indices = {3, 5};
		for (int i = 0; i < 256; ++i)
			for (int c = 0; c < 3; ++c) indexed.palette[i][c] = image.palette[size_t(i) * 3 + size_t(c)];
		std::vector<uint8_t> pcx;
		std::string error;
		TEST_EXPECT(opennova::encode_pcx_indexed(indexed, pcx, error));
		const std::shared_ptr<const TextureImage> decoded = decode_texture("sky.pcx", pcx);
		TEST_EXPECT(decoded && decoded->luminance == std::vector<uint8_t>({59, 169}));
		const std::shared_ptr<const TextureImage> lit = apply_load_transform(*decoded, TextureLoadTransform::LuminanceAlpha);
		TEST_EXPECT(lit->levels[0].rgba[3] == 59 && lit->levels[0].rgba[7] == 169 && lit->levels[0].rgba[0] == 30);
		// A PCX whose header names more texels than its bytes can describe: refused at once, nothing the
		// header's size allocated.
		std::vector<uint8_t> claim = pcx;
		claim[8] = 0xFE;
		claim[9] = 0x7F;
		claim[10] = 0xFE;
		claim[11] = 0x7F;
		const std::shared_ptr<const TextureImage> hostile = decode_texture("huge.pcx", claim);
		TEST_EXPECT(hostile && !hostile->loads && hostile->refusal.find("more pixels") != std::string::npos);
	}
	std::shared_ptr<const TextureImage> out;
	// White, alpha from blue.
	out = apply_load_transform(image, TextureLoadTransform::WhiteAlphaFromBlue);
	TEST_EXPECT(out->levels[0].rgba == std::vector<uint8_t>({255, 255, 255, 90, 255, 255, 255, 0}));
	// Alpha only: white under the alpha, the form an A8 takes under the HUD's alpha material.
	out = apply_load_transform(image, TextureLoadTransform::AlphaOnly);
	TEST_EXPECT(out->levels[0].rgba == std::vector<uint8_t>({255, 255, 255, 255, 255, 255, 255, 255}));
	// A PCX in the HUD's alpha mode: the blue as the alpha, white.
	out = apply_load_transform(image, TextureLoadTransform::BlueAlphaOnly);
	TEST_EXPECT(out->levels[0].rgba == std::vector<uint8_t>({255, 255, 255, 90, 255, 255, 255, 0}));
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
	TEST_EXPECT(error.empty() && size_t(answer.get_number("count", 0)) == renderer::kTextureRoleCount);
	const JsonValue *roles = answer.get("roles");
	TEST_EXPECT(roles && roles->is_array() && roles->array.size() == renderer::kTextureRoleCount);
	if (!roles || roles->array.size() != renderer::kTextureRoleCount) return 1;
	const JsonValue &colour = roles->array[size_t(renderer::TextureRoleId::TerrainColourMap)];
	TEST_EXPECT(colour.get_string("role", "") == "terrain_colour_map" && colour.get_string("loader", "") == "tga" &&
	            colour.get_string("group", "") == "Terrain");
	const JsonValue *formats = colour.get("formats");
	TEST_EXPECT(formats && formats->array.size() == 1 && formats->array[0].string == ".tga");
	const JsonValue *size = colour.get("size");
	TEST_EXPECT(size && size->get_string("rule", "") == "exact" && size->get_number("width", 0) == 1024 &&
	            size->get_string("words", "") == "1024 x 1024");
	TEST_EXPECT(colour.get_string("witness", "").find("PolyTrn_InitTextures") != std::string::npos);
	const JsonValue &foliage = roles->array[size_t(renderer::TextureRoleId::TerrainFoliageMap)];
	TEST_EXPECT(!foliage.get_bool("reads_alpha", true) && foliage.get_string("loader", "") == "pcx8");
	// A page of it.
	TEST_EXPECT(opennova::io::json_parse("{\"offset\":44,\"limit\":10}", args, error));
	const JsonValue page = session.query("texture_roles", args, error);
	TEST_EXPECT(page.get("roles") && page.get("roles")->array.size() == 2 &&
	            page.get("roles")->array[1].get_string("role", "") == "cinematic_fade");
	std::printf("query: texture_roles answers the %zu roles, a page at a time\n", renderer::kTextureRoleCount);
	return 0;
}

int test_reference_load() {
	// The argument a reference gives its loader: a model row's type, a role (with what the referrer's
	// content adds), or none.
	TEST_EXPECT(texture_arg_is_row_type(0) && texture_arg_is_row_type(18) && !texture_arg_is_row_type(-1) &&
	            !texture_arg_is_row_type(texture_role_arg(renderer::TextureRoleId::SkyCloud)));
	renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
	const int32_t colour = texture_role_arg(renderer::TextureRoleId::TerrainColourMap, kTextureArgGates);
	TEST_EXPECT(texture_arg_role(colour, role) && role == renderer::TextureRoleId::TerrainColourMap && texture_arg_gates(colour));
	TEST_EXPECT(!texture_arg_gates(texture_role_arg(renderer::TextureRoleId::TerrainBlendMap)) && !texture_arg_role(4, role) &&
	            !texture_arg_role(-1, role));
	const TextureNameTest files = files_of({"body.tga", "body.dds", "cld.pcx", "cld.dds", "ground.tga", "ground.dds",
	                                        "stance.tga", "trntile10.tga"});
	// A model row by its type: the .dds beside the name.
	TEST_EXPECT(texture_reference_load("body.tga", 0, files).file == "body.dds");
	// A sky map through ARCHIVE: its .dds too; a colour map through the TGA reader: the name alone.
	TEST_EXPECT(texture_reference_load("cld.pcx", texture_role_arg(renderer::TextureRoleId::SkyCloud), files).file == "cld.dds");
	// A sky map's name made .pcx first, as the environment's parser stores it.
	const TextureLoad haze =
			texture_reference_load("haze.tga", texture_role_arg(renderer::TextureRoleId::SkyCloud, kTextureArgPcx), files_of({"haze.pcx"}));
	TEST_EXPECT(haze.file == "haze.pcx" && haze.transform == TextureLoadTransform::LuminanceAlpha);
	TextureLoad load = texture_reference_load("ground.tga", colour, files);
	TEST_EXPECT(load.file == "ground.tga" && load.reader == TextureFileReader::Tga);
	// A mission's tile set: TGA for its extension.
	load = texture_reference_load("trntile10.bmp", texture_role_arg(renderer::TextureRoleId::TerrainTileAtlas, kTextureArgTileSet), files);
	TEST_EXPECT(load.file == "trntile10.TGA" && load.reader == TextureFileReader::Tga);
	// The HUD's alpha-only art: the suffix cut, alpha only.
	load = texture_reference_load("stance.tga", texture_role_arg(renderer::TextureRoleId::HudAlphaOnly), files);
	TEST_EXPECT(load.file == "stance.tga" && load.transform == TextureLoadTransform::AlphaOnly);
	// A use whose loader is not witnessed: the name as written, nothing else.
	load = texture_reference_load("cross.bmp", -1, files);
	TEST_EXPECT(load.file == "cross.bmp" && load.reader == TextureFileReader::None);
	std::printf("reference load: a row's type, a role's loader, a tile set's TGA, the name as written\n");
	return 0;
}

using editor_test::tga_bytes;

// The graph resolves each texture reference by its loader (ADR 0046 S18) over a project: a terrain's
// colour map read by the TGA reader alone, missing where only a PNG of its stem is (an alternate no
// loader reads), which refuses a build; its splat detail's .dds read for its .tga; its blend map missing
// refusing a build whenever its key names one; a sky map's .dds; an item's HUD image, and one written .dds
// the HUD's loader opens no file of.
int test_graph() {
	test_temp::TempDir dir{"opennova_editor_texture_roles"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const std::string root = dir.file("project");
	editor_test::handle_to_end(session, request::new_project(root, "Roles"));
	editor_test::create_missing_files(session);
	const std::string trn_head = "polytrn_detailmap detail.tga\npolytrn_polydata ground.cpt\npolytrn_sectorcount 1\n"
	                             "polytrn_sectors 0\n";
	TEST_EXPECT(test_io::write_text(root + "/terrains/a.trn", trn_head +
	                                                                  "polytrn_colormap ground.tga\npolytrn_detailmap_c1 splat.tga\n"
	                                                                  "polytrn_detailblendmap blend.tga\n") &&
	            test_io::write_text(root + "/terrains/b.trn",
	                                    trn_head + "polytrn_colormap bcol.tga\npolytrn_detailblendmap noblend.tga\n") &&
	            test_io::write_file(root + "/textures/ground.png", test_png::gradient_png(2, 2)) &&
	            test_io::write_file(root + "/textures/detail.tga", tga_bytes()) &&
	            test_io::write_file(root + "/textures/bcol.tga", tga_bytes()) &&
	            test_io::write_file(root + "/textures/splat.dds", tga_bytes()) &&
	            test_io::write_file(root + "/textures/cloud.dds", tga_bytes()) &&
	            test_io::write_file(root + "/textures/stance.tga", tga_bytes()) &&
	            test_io::write_file(root + "/textures/stance2.dds", tga_bytes()) &&
	            test_io::write_text(root + "/defs/items.def",
	                                    "begin \"Brick\"\nid 100300\ntype building\nhud_image stance.tga\nend\n"
	                                    "begin \"Pebble\"\nid 100301\ntype building\nhud_image stance2.dds\nend\n"));
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
	TEST_EXPECT(test_io::write_text(root + "/envs/sky.env", env) &&
	            test_io::write_file(root + "/textures/cloud2.png", test_png::gradient_png(2, 2)));
	// A sky map's extension made PCX as the game parses it: haze.tga finds haze.pcx; only.tga, which the
	// project holds as a TGA alone, finds nothing (the game opens only.dds or only.pcx).
	{
		std::ostringstream text;
		opennova::env::Config config;
		config.sky_map1 = "haze.tga";
		config.sky_map2 = "only.tga";
		std::string error;
		TEST_EXPECT(opennova::env::save_env(text, config, error) && test_io::write_text(root + "/envs/haze.env", text.str()) &&
		            test_io::write_file(root + "/textures/haze.pcx", tga_bytes()) &&
		            test_io::write_file(root + "/textures/only.tga", tga_bytes()));
	}
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
	TEST_EXPECT(resolved("envs/haze.env", "sky_map1") == "textures/haze.pcx");
	TEST_EXPECT(resolved("envs/haze.env", "sky_map2") == "<missing>");
	TEST_EXPECT(resolved("defs/items.def", "hud_image") == "textures/stance.tga");
	// What refuses a build: the colour map, and each blend map the key names, splat details or none (the key
	// alone turns the blend on).
	std::set<std::string> blocking, listed;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "reference.missing") continue;
		listed.insert(d.asset + " " + d.field);
		if (blocks_build(d)) blocking.insert(d.asset + " " + d.field);
	}
	TEST_EXPECT(blocking == std::set<std::string>({"terrains/a.trn polytrn_colormap", "terrains/a.trn polytrn_detailblendmap",
	                                               "terrains/b.trn polytrn_detailblendmap"}));
	TEST_EXPECT(listed.count("envs/sky.env sky_map2") && listed.count("defs/items.def hud_image") &&
	            listed.count("envs/haze.env sky_map2"));
	// Its words say what the game does; a name the project holds but the loader opens other files for says
	// which (the sky map's name made .pcx).
	size_t worded = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "reference.missing") continue;
		if (d.field == "polytrn_colormap") TEST_EXPECT(d.message.find("the mission aborts") != std::string::npos);
		if (d.field == "polytrn_detailblendmap") TEST_EXPECT(d.message.find("the key alone turns the blend on") != std::string::npos);
		if (d.field == "hud_image" || (d.asset == "envs/haze.env" && d.field == "sky_map2")) {
			++worded;
			std::printf("  %s\n", d.message.c_str());
			TEST_EXPECT(d.message.find("which the project has, but the game's loader for it opens ") != std::string::npos);
		}
		if (d.asset == "envs/haze.env" && d.field == "sky_map2")
			TEST_EXPECT(d.message.find("only.dds or only.pcx, which the project does not have") != std::string::npos);
	}
	TEST_EXPECT(worded == 2);
	std::printf("graph: %zu texture references found missing, %zu of them refusing a build\n", listed.size(), blocking.size());
	return 0;
}

// The texture references of the shipped referrers in the spellings they take (the census of JO:CA's 234 referrers
// and 2,579 textures), each resolved by its loader in a project holding every texture by its file's own name (an
// empty file each: what a reference resolves to is a name): a terrain's colour map and detail blend map, the two the
// game aborts the mission without, named as their file is (27 of the 30 terrains), in lower case over a file of mixed
// case (dvxg9_c.tga for Dvxg9_c.tga) and in mixed case over one of lower (Dvxg4_c.tga for dvxg4_c.tga, Dvxg5_m2.tga
// for dvxg5_m2.tga); a terrain that names no blend map (Dvxg5b.trn); its char, foliage and detail maps and its tile
// strip in another case than their files' (trntile10.tga for TRNTILE10.TGA); the sky maps' .pcx (Cloud01.pcx for
// cloud01.pcx); a mission's tile set named without its extension (trntile10); a particle graphic in another case;
// and a particle graphic and a HUD image whose files the install lacks (91 shipped references are so, every one of
// a loader that does not gate). Every reference but those two resolves, each colour map among them, and none
// refuses a build.
int test_shipped_references() {
	test_temp::TempDir dir{"opennova_editor_texture_roles_shipped"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const std::string root = dir.file("project");
	editor_test::handle_to_end(session, request::new_project(root, "Roles"));
	editor_test::create_missing_files(session);
	const auto terrain = [](const std::string &maps) {
		return "terrain_name     \"Isle\"\r\npolytrn_detailmap        Det_d4.tga\r\npolytrn_polydata         isle.cpt\r\n"
		       "polytrn_tilestrip       trntile10.tga\r\n" + maps + "polytrn_sectorcount\t\t1\r\npolytrn_sectors\t\t\t0\r\n";
	};
	TEST_EXPECT(test_io::write_text(root + "/terrains/exact.trn",
	                                terrain("polytrn_colormap         Isle_c.tga\r\npolytrn_detailblendmap   Isle_d1.tga\r\n"
	                                        "polytrn_charmap          Isle_m.pcx\r\npolytrn_foliagemap       Isle_f.pcx\r\n")) &&
	            test_io::write_text(root + "/terrains/lower.trn",
	                                terrain("polytrn_colormap         reef_c.tga\r\npolytrn_detailblendmap   Reef_d1.tga\r\n"
	                                        "polytrn_charmap          reef_m.pcx\r\npolytrn_foliagemap       reef_f.pcx\r\n")) &&
	            test_io::write_text(root + "/terrains/mixed.trn",
	                                terrain("polytrn_colormap         Cove_c.tga\r\npolytrn_detailblendmap   Cove_m2.tga\r\n")) &&
	            test_io::write_text(root + "/terrains/noblend.trn", terrain("polytrn_colormap         Isle_c.tga\r\n")));
	for (const char *file : {"Isle_c.tga", "Isle_d1.tga", "Isle_m.pcx", "Isle_f.pcx", "Reef_c.tga", "Reef_d1.tga", "Reef_m.pcx",
	                         "Reef_f.pcx", "cove_c.tga", "cove_m2.tga", "det_d4.tga", "TRNTILE10.TGA", "cloud01.pcx", "cloud01b.pcx",
	                         "CNFireBA.tga"})
		TEST_EXPECT(test_io::write_file(root + "/textures/" + file, {}));
	{
		std::ostringstream text;
		opennova::env::Config config;
		config.sky_map1 = "Cloud01.pcx";
		config.sky_map2 = "Cloud01b.pcx";
		std::string error;
		TEST_EXPECT(opennova::env::save_env(text, config, error) && test_io::write_text(root + "/envs/day.env", text.str()));
	}
	{
		opennova::bms::File file;
		opennova::mission::make_default(file);
		std::string why;
		TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "exact", why) &&
		            opennova::mission::set_header_string(file, "environment", "day", why) &&
		            opennova::mission::set_header_string(file, "terrain_tile", "trntile10", why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(file, bytes, why) && test_io::write_file(root + "/missions/landing.bms", bytes));
	}
	TEST_EXPECT(test_io::write_text(root + "/fx.ptl",
	                                "[effectdef]\n{\n\tid = FIRE;\n\tpdefs = flame, spark;\n}\n\n[particledef]\n{\n\tid = flame;\n"
	                                "\tgraphic1 = CNFIREBA.tga, additive;\n}\n\n[particledef]\n{\n\tid = spark;\n"
	                                "\tgraphic1 = gone.tga, additive;\n}\n") &&
	            test_io::write_text(root + "/defs/items.def", "begin \"Brick\"\nid 100300\ntype building\nhud_image soflam1.tga\nend\n"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const AssetGraph *graph = session.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr);
	if (!graph) return 1;
	const auto resolved = [&](const std::string &source, const std::string &value) {
		for (const GraphEdge *edge : graph->references_of(source))
			if (edge->kind == ReferenceKind::Texture && edge->value == value) {
				std::string file;
				return graph->resolve(*edge, &file) == ReferenceStatus::Present ? file : std::string("<missing>");
			}
		return std::string("<none>");
	};
	TEST_EXPECT(resolved("terrains/exact.trn", "Isle_c.tga") == "textures/Isle_c.tga" &&
	            resolved("terrains/exact.trn", "Isle_d1.tga") == "textures/Isle_d1.tga" &&
	            resolved("terrains/exact.trn", "Isle_m.pcx") == "textures/Isle_m.pcx" &&
	            resolved("terrains/exact.trn", "Isle_f.pcx") == "textures/Isle_f.pcx");
	TEST_EXPECT(resolved("terrains/lower.trn", "reef_c.tga") == "textures/Reef_c.tga" &&
	            resolved("terrains/lower.trn", "Reef_d1.tga") == "textures/Reef_d1.tga" &&
	            resolved("terrains/lower.trn", "reef_m.pcx") == "textures/Reef_m.pcx" &&
	            resolved("terrains/lower.trn", "reef_f.pcx") == "textures/Reef_f.pcx");
	TEST_EXPECT(resolved("terrains/mixed.trn", "Cove_c.tga") == "textures/cove_c.tga" &&
	            resolved("terrains/mixed.trn", "Cove_m2.tga") == "textures/cove_m2.tga");
	TEST_EXPECT(resolved("terrains/noblend.trn", "Isle_c.tga") == "textures/Isle_c.tga");
	for (const char *source : {"terrains/exact.trn", "terrains/lower.trn", "terrains/mixed.trn", "terrains/noblend.trn"})
		TEST_EXPECT(resolved(source, "Det_d4.tga") == "textures/det_d4.tga" &&
		            resolved(source, "trntile10.tga") == "textures/TRNTILE10.TGA");
	TEST_EXPECT(resolved("envs/day.env", "Cloud01.pcx") == "textures/cloud01.pcx" &&
	            resolved("envs/day.env", "Cloud01b.pcx") == "textures/cloud01b.pcx");
	TEST_EXPECT(resolved("missions/landing.bms", "trntile10") == "textures/TRNTILE10.TGA");
	TEST_EXPECT(resolved("fx.ptl", "CNFIREBA.tga") == "textures/CNFireBA.tga" && resolved("fx.ptl", "gone.tga") == "<missing>" &&
	            resolved("defs/items.def", "soflam1.tga") == "<missing>");
	// Each colour map resolves, the four of them; no texture reference refuses a build, the two missing listed.
	size_t colour_maps = 0;
	graph->for_each_edge([&](const GraphEdge &edge) {
		renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
		if (edge.kind != ReferenceKind::Texture || !texture_arg_role(edge.loader_arg, role) ||
		    role != renderer::TextureRoleId::TerrainColourMap)
			return;
		colour_maps += graph->resolve(edge) == ReferenceStatus::Present ? 1 : 0;
	});
	TEST_EXPECT(colour_maps == 4);
	size_t gating = 0, missing = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "reference.missing" || editor_test::reference_of(d).kind != ReferenceKind::Texture) continue;
		++missing;
		if (!blocks_build(d)) continue;
		std::fprintf(stderr, "refuses a build: %s %s: %s\n", d.asset.c_str(), d.field.c_str(), d.message.c_str());
		++gating;
	}
	TEST_EXPECT(gating == 0 && missing == 2);
	std::printf("shipped references: every spelling the shipped referrers take resolved by its loader, %zu colour maps; "
	            "the missing two listed, none refusing a build\n",
	            colour_maps);
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_catalog();
	failures += test_names();
	failures += test_transforms();
	failures += test_query();
	failures += test_reference_load();
	failures += test_graph();
	failures += test_shipped_references();
	if (failures == 0) std::printf("editor_texture_roles: all passed\n");
	return failures == 0 ? 0 : 1;
}
