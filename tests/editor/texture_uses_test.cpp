// ADR 0046 S18, the texture uses (graph/texture_uses over the session's index, session/texture_use_index):
// over a minted project, each texture's uses by role, its referrer and what that says of it: a model's rows
// (a diffuse row of an alpha-tested material, a detail row, a normal map), the .tga a model row names
// beside the .dds its loader opens (the .tga's uses said not to read it, the .dds's to), a terrain's
// colour map, a sky's cloud layer, an item's HUD image (alpha only), and a name the game opens itself
// (the scope's crosshair); the index made once while what it reads stands and again after an edit of a
// referrer; a texture's viewport showing it as a use draws it (a cut-out's test, the HUD's alpha alone)
// and as the file again; the texture_uses query.
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_import.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/texture_uses.h>
#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/env/env.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::vector<uint8_t> tga_bytes() {
	const std::vector<uint8_t> rgba(16, 200);
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), 2, 2, out, error);
	return out;
}

// The same texels as an A8R8G8B8 DDS.
std::vector<uint8_t> dds_bytes() {
	const std::vector<uint8_t> rgba(16, 200);
	std::vector<uint8_t> out;
	std::string error;
	opennova::dds::dds_write_a8r8g8b8(rgba.data(), 2, 2, out, error);
	return out;
}

const TextureUse *use_of(const std::vector<TextureUse> &uses, TextureRoleId role) {
	for (const TextureUse &use : uses)
		if (use.role == role) return &use;
	return nullptr;
}

int test_uses() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_uses"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Uses"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	// A model of two materials: an alpha-tested one with a diffuse and a detail row, a plain one with a
	// normal map.
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/thing.o3d",
	                                    "o3d 1\nmodel THING\nmaterial VS_SKBASIC\nmatflags 1\nalphatest 128\n"
	                                    "texture body.tga 1 0\ntexture grain.tga 2 0\nmaterial FF_ST_OP\n"
	                                    "texture skin.mdt 3 4\nlod 0\npart 0 0 0 0\nstrip 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	const ImportResult imported =
			import_assets({{scene + "/thing.o3d", {}}}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/thing.3di"}));
	for (const char *name : {"body.tga", "body.dds", "grain.tga", "skin.mdt", "map.tga", "cloud.pcx", "stance.tga", "scopexh.tga"})
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/" + name,
		                                     std::string(name).find(".dds") != std::string::npos ? dds_bytes() : tga_bytes()));
	TEST_EXPECT(editor_test::write_text(root + "/terrains/isle.trn",
	                                    "polytrn_colormap map.tga\npolytrn_detailmap grain.tga\npolytrn_polydata isle.cpt\n"
	                                    "polytrn_sectorcount 1\npolytrn_sectors 0\n") &&
	            editor_test::write_text(root + "/defs/items.def",
	                                    "begin \"Brick\"\nid 100300\ntype building\nhud_image stance.tga\n"
	                                    "shadow shadow1.tga 3.5 5.4 0.0 0.0\nend\n"));
	{
		std::ostringstream text;
		opennova::env::Config config;
		config.sky_map1 = "cloud.pcx";
		config.sky_map2 = "cloud.pcx";
		std::string error;
		TEST_EXPECT(opennova::env::save_env(text, config, error) && editor_test::write_text(root + "/envs/day.env", text.str()));
	}
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const TextureUseIndex &index = *view.documents.texture_uses;

	// The .tga a model row names: its loader opens the .dds beside it, so the .tga reads for neither row.
	const std::vector<TextureUse> &tga = index.uses_of(view, "textures/body.tga");
	const TextureUse *diffuse = use_of(tga, TextureRoleId::ModelDiffuse);
	TEST_EXPECT(diffuse && !diffuse->reads_file && diffuse->served == "textures/body.dds" && diffuse->referrer == "models/thing.3di");
	TEST_EXPECT(diffuse && diffuse->context.material == 0 && diffuse->context.alpha_test() && diffuse->context.alpha_ref == 128 &&
	            diffuse->context.shader == "VS_SKBASIC");
	TEST_EXPECT(diffuse && diffuse->words == "Model diffuse: material 1 of thing.3di (VS_SKBASIC, cut-out above 128)");
	// The .dds: the same use, read.
	const std::vector<TextureUse> &dds = index.uses_of(view, "textures/body.dds");
	diffuse = use_of(dds, TextureRoleId::ModelDiffuse);
	TEST_EXPECT(dds.size() == 1 && diffuse && diffuse->reads_file && diffuse->load.reader == TextureFileReader::Dds);
	// The detail row's grain.tga, which the terrain's coefficient detail names too; the normal map.
	const std::vector<TextureUse> &grain = index.uses_of(view, "textures/grain.tga");
	TEST_EXPECT(grain.size() == 2 && use_of(grain, TextureRoleId::ModelDetail) &&
	            use_of(grain, TextureRoleId::TerrainDetailCoefficient));
	const std::vector<TextureUse> &skin = index.uses_of(view, "textures/skin.mdt");
	TEST_EXPECT(skin.size() == 1 && skin[0].role == TextureRoleId::ModelNormalMap && skin[0].context.material == 1);
	// A terrain's colour map; a sky's two cloud layers (ARCHIVE: a PCX's alpha from its palette).
	const std::vector<TextureUse> &map = index.uses_of(view, "textures/map.tga");
	TEST_EXPECT(map.size() == 1 && map[0].role == TextureRoleId::TerrainColourMap && map[0].field == "polytrn_colormap" &&
	            map[0].words == "Terrain colour map: isle.trn (polytrn_colormap)");
	const std::vector<TextureUse> &cloud = index.uses_of(view, "textures/cloud.pcx");
	TEST_EXPECT(cloud.size() == 2 && cloud[0].role == TextureRoleId::SkyCloud &&
	            cloud[0].load.transform == TextureLoadTransform::LuminanceAlpha);
	// An item's HUD image, alpha only; the scope's crosshair, a name the game opens itself.
	const std::vector<TextureUse> &stance = index.uses_of(view, "textures/stance.tga");
	TEST_EXPECT(stance.size() == 1 && stance[0].role == TextureRoleId::HudAlphaOnly && stance[0].context.hud_mode == 1 &&
	            stance[0].load.transform == TextureLoadTransform::AlphaOnly);
	const std::vector<TextureUse> &scope = index.uses_of(view, "textures/scopexh.tga");
	TEST_EXPECT(scope.size() == 1 && scope[0].fixed && scope[0].role == TextureRoleId::HudAlphaOnly &&
	            scope[0].fixed_for == "for the scope's crosshair" && scope[0].reads_file);
	// Made once while the graph and the documents stand.
	const uint64_t made = index.made();
	index.uses_of(view, "textures/body.tga");
	TEST_EXPECT(index.made() == made);
	// A referrer edited: made again (the item's HUD image now the scope's crosshair, which the game opens
	// by name too).
	editor_test::handle_to_end(session, request::open_document("defs/items.def"));
	const Document *items = session.document_for("defs/items.def");
	NodeAddress brick;
	TEST_EXPECT(items && find_definition(AssetGraph(), *items, "100300", brick));
	if (items && brick.row) {
		Edit set;
		set.address = brick;
		set.field = "hud_image";
		set.value = std::string("scopexh.tga");
		editor_test::handle_to_end(session, request::edit_record("defs/items.def", set));
		session.run_operations();
		TEST_EXPECT(index.uses_of(view, "textures/stance.tga").empty() && index.made() > made);
		const std::vector<TextureUse> &both = index.uses_of(view, "textures/scopexh.tga");
		TEST_EXPECT(both.size() == 2 && !both[0].fixed && both[1].fixed);
	}

	// The texture's picture as a use draws it: the .dds the alpha-tested diffuse loads, its texels cut out
	// at the material's reference (alpha 200, above 128: kept, opaque); the scope crosshair's alpha alone (an A8,
	// white under it);
	// back to the file as it is.
	const auto viewport_of = [&](const std::string &path) {
		return static_cast<const TextureViewport *>(session.viewports().find(path, ViewportKind::Texture));
	};
	editor_test::FakeDevices devices;
	const auto show_as = [&](const std::string &path, int as_used) {
		TextureViewportOptions options;
		options.as_used = as_used;
		editor_test::handle_to_end(session, request::set_viewport(path, texture_options_change(options)));
		devices.sync(session);
	};
	editor_test::handle_to_end(session, request::open_document("textures/body.dds"));
	devices.sync(session);
	show_as("textures/body.dds", 0);
	const TextureViewport *body = viewport_of("textures/body.dds");
	TEST_EXPECT(body && body->shown_use().index == 0 && body->shown_use().cutout == 128 && body->image() &&
	            body->image() != body->source() && body->image()->levels[0].rgba[3] == 255 &&
	            body->source()->levels[0].rgba[3] == 200);
	editor_test::handle_to_end(session, request::open_document("textures/scopexh.tga"));
	devices.sync(session);
	show_as("textures/scopexh.tga", 0);
	const TextureViewport *scope_view = viewport_of("textures/scopexh.tga");
	TEST_EXPECT(scope_view && scope_view->shown_use().transform == TextureLoadTransform::AlphaOnly && scope_view->image() &&
	            scope_view->image()->levels[0].rgba[0] == 255 && scope_view->image()->levels[0].rgba[3] == 200);
	{
		JsonValue state_args;
		std::string state_error;
		TEST_EXPECT(opennova::io::json_parse("{\"path\":\"textures/scopexh.tga\",\"op\":\"state\"}", state_args, state_error));
		const JsonValue state = session.query("viewport", state_args, state_error);
		const JsonValue *body_json = state.get("body");
		const JsonValue *as_used = body_json ? body_json->get("as_used") : nullptr;
		TEST_EXPECT(state_error.empty() && as_used && as_used->get_string("transform", "") == "alpha_only" &&
		            as_used->get_number("index", -1) == 0 && state.get("options") &&
		            state.get("options")->get_number("as_used", -1) == 0);
	}
	show_as("textures/scopexh.tga", -1);
	TEST_EXPECT(scope_view && scope_view->shown_use().index == -1 && scope_view->image() == scope_view->source());
	// A use past the texture's uses shows the file.
	show_as("textures/scopexh.tga", 9);
	TEST_EXPECT(scope_view && scope_view->shown_use().index == -1 && scope_view->image() == scope_view->source());

	// The wire.
	JsonValue args;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse("{\"path\":\"body.tga\"}", args, error));
	const JsonValue answer = session.query("texture_uses", args, error);
	const JsonValue *uses = answer.get("uses");
	TEST_EXPECT(error.empty() && answer.get_string("file", "") == "textures/body.tga" && uses && uses->array.size() == 1);
	if (uses && !uses->array.empty()) {
		const JsonValue &first = uses->array[0];
		TEST_EXPECT(first.get_string("role", "") == "model_diffuse" && !first.get_bool("reads_file", true) &&
		            first.get_string("served", "") == "textures/body.dds" && first.get("context") &&
		            first.get("context")->get_bool("alpha_test", false));
	}
	TEST_EXPECT(opennova::io::json_parse("{\"path\":\"defs/items.def\"}", args, error));
	session.query("texture_uses", args, error);
	TEST_EXPECT(!error.empty());
	// An item's shadow line names a TGA the game stores and never loads: no reference, so nothing missing
	// (shadow1.tga, which the install lacks too).
	for (const GraphEdge *edge : view.findings.graph->references_of("defs/items.def")) TEST_EXPECT(edge->value != "shadow1.tga");
	for (const Diagnostic &d : view.findings.diagnostics)
		TEST_EXPECT(!(d.code() == "reference.missing" && d.asset == "defs/items.def"));
	std::printf("uses: model rows by their material, a .tga beside its .dds, a terrain's map, a sky's clouds, a HUD "
	            "image, a name the game opens; made once; over the wire\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_uses();
	if (failures == 0) std::printf("editor_texture_uses: all passed\n");
	return failures == 0 ? 0 : 1;
}
