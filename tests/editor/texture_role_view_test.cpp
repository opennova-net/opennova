// ADR 0046 S18, each texture role in its own picture (preview/texture_role_view): a terrain blend map's weights as
// the terrain normalizes them and the splat details they weigh, a foliage map's codes and the foliage each grows, a
// particle graphic as its atlas page holds it and its share of the page; through a session the texture viewport
// showing each as its use, its legend on the wire.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/texture_role_view.h>
#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
namespace renderer = opennova::renderer;
using opennova::io::JsonValue;

namespace {

std::shared_ptr<const TextureImage> image_of(uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
	auto out = std::make_shared<TextureImage>();
	out->loads = out->decoded = true;
	TextureLevel level;
	level.width = w;
	level.height = h;
	level.rgba = std::move(rgba);
	out->levels.push_back(std::move(level));
	return out;
}

// An 8-bit PCX of `w` x `h` holding `indices` (each below 0xC0, so none reads as a run), its palette entry i
// (i, 2i, 3i).
std::vector<uint8_t> pcx8(uint16_t w, uint16_t h, const std::vector<uint8_t> &indices) {
	std::vector<uint8_t> out(128, 0);
	out[0] = 0x0A;
	out[1] = 5;
	out[2] = 1;
	out[3] = 8;
	out[8] = uint8_t(w - 1);
	out[9] = uint8_t((w - 1) >> 8);
	out[10] = uint8_t(h - 1);
	out[11] = uint8_t((h - 1) >> 8);
	out[65] = 1;
	out[66] = uint8_t(w);
	out[67] = uint8_t(w >> 8);
	out.insert(out.end(), indices.begin(), indices.end());
	out.push_back(0x0C);
	for (int i = 0; i < 256; ++i) {
		out.push_back(uint8_t(i));
		out.push_back(uint8_t(2 * i));
		out.push_back(uint8_t(3 * i));
	}
	return out;
}

std::vector<uint8_t> tga(uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba) {
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	return out;
}

int test_pieces() {
	// A blend map's weights: (100, 50, 0) scaled by 65535 / 150 = 436, then >> 8: (170, 85, 0); a texel of none red;
	// its alpha kept.
	const auto blend = image_of(2, 1, {100, 50, 0, 77, 0, 0, 0, 9});
	const auto weights = texture_role_texels(blend, renderer::TextureRoleId::TerrainBlendMap, -1);
	TEST_EXPECT(weights != blend && weights->levels[0].rgba == std::vector<uint8_t>({170, 85, 0, 77, 255, 0, 0, 9}));
	opennova::TrnConfig terrain;
	terrain.detailmap_c1 = "grass.tga";
	terrain.detailmap_c2 = "rock.tga";
	TextureRoleView view = texture_role_view(*blend, *weights, renderer::TextureRoleId::TerrainBlendMap, -1, &terrain);
	TEST_EXPECT(view.legend.size() == 3 && view.legend[0].key == "red" && view.legend[0].rgb[0] == 255 &&
	            view.legend[0].words == "weighs polytrn_detailmap_c1, grass.tga" &&
	            view.legend[2].words == "weighs polytrn_detailmap_c3, which the terrain does not name" && view.words.empty());
	// Red's mean weight: (170 + 255) / (2 x 255).
	TEST_EXPECT(view.legend.size() == 3 && std::abs(view.legend[0].share - 425.0 / 510.0) < 1e-9);
	terrain.detailmap_c1.clear();
	view = texture_role_view(*blend, *weights, renderer::TextureRoleId::TerrainBlendMap, -1, &terrain);
	TEST_EXPECT(view.words.find("names no polytrn_detailmap_c1: the game draws no splat") != std::string::npos);
	// A foliage map's codes: 5 selects both definitions (the second's second code), 7 the second, 9 none, 0 never.
	auto foliage = std::make_shared<TextureImage>(*image_of(2, 2, std::vector<uint8_t>(16, 0)));
	foliage->indices = {0, 5, 5, 9};
	foliage->palette.assign(256 * 3, 0);
	foliage->palette[5 * 3] = 50;
	opennova::FoliageDef grass, bush;
	grass.graphic = "grass.3di";
	grass.match[0] = 5;
	bush.graphic = "bush.3di";
	bush.match[0] = 7;
	bush.match[1] = 5;
	terrain.foliage_defs = {grass, bush};
	view = texture_role_view(*foliage, *foliage, renderer::TextureRoleId::TerrainFoliageMap, -1, &terrain);
	TEST_EXPECT(view.legend.size() == 3 && view.legend[0].key == "0" && view.legend[0].words.find("code 0 matches no") != std::string::npos &&
	            view.legend[1].key == "5" && view.legend[1].rgb[0] == 50 && view.legend[1].share == 0.5 &&
	            view.legend[1].words == "grows foliage 1 (grass.3di), foliage 2 (bush.3di)" && view.legend[2].key == "9" &&
	            view.legend[2].words.find("no definition of the terrain matches it") != std::string::npos);
	// A particle graphic as its page holds it (renderer::particle_atlas_paged_frame, which
	// renderer_particle_atlas_contract pins): a quarter of a percent of a 1024 page, its alpha cleared for an additive
	// one; a bump's page a normal map of its blue; one as wide as its page held by none.
	const auto spark = image_of(32, 32, std::vector<uint8_t>(32 * 32 * 4, 200));
	const auto paged = texture_role_texels(spark, renderer::TextureRoleId::ParticleGraphic, 1);
	TEST_EXPECT(paged != spark && paged->width() == 32 && paged->levels.size() == 1);
	view = texture_role_view(*spark, *paged, renderer::TextureRoleId::ParticleGraphic, 1, nullptr);
	TEST_EXPECT(view.title.empty() && view.words.find("Alone on a 1024 x 1024 atlas page of its mode") != std::string::npos &&
	            view.words.find("it takes 0.1% of the page; the page holds it with its alpha cleared") != std::string::npos);
	const auto bump = texture_role_texels(spark, renderer::TextureRoleId::ParticleGraphic, 3);
	TEST_EXPECT(bump != spark);
	view = texture_role_view(*spark, *bump, renderer::TextureRoleId::ParticleGraphic, 3, nullptr);
	TEST_EXPECT(view.words.find("256 x 256") != std::string::npos && view.words.find("normal map of its blue") != std::string::npos);
	const auto wide = image_of(1024, 1, std::vector<uint8_t>(1024 * 4, 9));
	view = texture_role_view(*wide, *wide, renderer::TextureRoleId::ParticleGraphic, 1, nullptr);
	TEST_EXPECT(view.words.find("No 1024 x 1024 atlas page of its mode") != std::string::npos);
	// Any other role: nothing.
	TEST_EXPECT(texture_role_texels(blend, renderer::TextureRoleId::ModelDiffuse, -1) == blend &&
	            texture_role_view(*blend, *blend, renderer::TextureRoleId::ModelDiffuse, -1, nullptr).empty());
	std::printf("pieces: a blend map's weights and details, a foliage map's codes, a particle graphic's page\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_texture_role_view"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	std::string root() const { return dir.file("project"); }
	const TextureViewport *viewport(const std::string &path) {
		return static_cast<const TextureViewport *>(session.viewports().find(path, ViewportKind::Texture));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue role_view(const std::string &path) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse("{\"path\":\"" + path + "\",\"op\":\"state\"}", args, error);
		const JsonValue state = session.query("viewport", args, error);
		const JsonValue *body = state.get("body");
		const JsonValue *as_used = body ? body->get("as_used") : nullptr;
		const JsonValue *view = as_used ? as_used->get("role_view") : nullptr;
		return view ? *view : JsonValue::make_null();
	}
	bool show_use(const std::string &path) {
		editor_test::handle_to_end(session, request::open_document(path));
		pump();
		editor_test::handle_to_end(session, request::set_viewport(path, "{\"kind\":\"texture\",\"options\":{\"as_used\":0}}"));
		pump();
		return session.outcome().done();
	}
};

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Roles"));
	editor_test::create_missing_files(rig.session);
	const std::string t = rig.root() + "/textures/";
	TEST_EXPECT(editor_test::write_bytes(t + "blend.tga", tga(2, 1, {100, 50, 0, 255, 0, 0, 0, 255})) &&
	            editor_test::write_bytes(t + "fol.pcx", pcx8(2, 2, {0, 5, 5, 9})) &&
	            editor_test::write_bytes(t + "spark.tga", tga(16, 16, std::vector<uint8_t>(16 * 16 * 4, 200))));
	TEST_EXPECT(editor_test::write_text(rig.root() + "/terrains/isle.trn",
	                                    "polytrn_detailmap detail.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\n"
	                                    "polytrn_sectors 0\npolytrn_colormap colour.tga\npolytrn_detailmap_c1 grass.tga\n"
	                                    "polytrn_detailmap_c2 rock.tga\n"
	                                    "polytrn_detailmap_c3 sand.tga\npolytrn_detailblendmap blend.tga\npolytrn_foliagemap fol.pcx\n"
	                                    "foliage\n  graphic grass.3di\n  match 5\nend\n") &&
	            editor_test::write_text(rig.root() + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = FLASH;\n\tpdefs = spark;\n}\n\n[particledef]\n{\n\tid = spark;\n"
	                                    "\tgraphic1 = spark.tga, additive;\n}\n"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();
	// The blend map as its terrain's splat reads it: its weights, each naming the detail it weighs.
	TEST_EXPECT(rig.show_use("textures/blend.tga"));
	const TextureViewport *viewport = rig.viewport("textures/blend.tga");
	TEST_EXPECT(viewport && viewport->shown_use().role == renderer::TextureRoleId::TerrainBlendMap && viewport->image() &&
	            viewport->image()->levels[0].rgba == std::vector<uint8_t>({170, 85, 0, 255, 255, 0, 0, 255}));
	JsonValue view = rig.role_view("textures/blend.tga");
	const JsonValue *legend = view.get("legend");
	TEST_EXPECT(legend && legend->array.size() == 3 &&
	            legend->array[2].get_string("words", "") == "weighs polytrn_detailmap_c3, sand.tga");
	// The foliage map: its codes and what grows on each.
	TEST_EXPECT(rig.show_use("textures/fol.pcx"));
	view = rig.role_view("textures/fol.pcx");
	legend = view.get("legend");
	TEST_EXPECT(legend && legend->array.size() == 3 && legend->array[1].get_string("key", "") == "5" &&
	            legend->array[1].get_string("words", "") == "grows foliage 1 (grass.3di)" && legend->array[1].get_number("share", 0) == 0.5);
	// The terrain written again: the legend read again.
	TEST_EXPECT(editor_test::write_text(rig.root() + "/terrains/isle.trn",
	                                    "polytrn_detailmap detail.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\n"
	                                    "polytrn_sectors 0\npolytrn_colormap colour.tga\npolytrn_foliagemap fol.pcx\nfoliage\n"
	                                    "  graphic tall_grass.3di\n  match 5 9\n"
	                                    "end\n"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();
	view = rig.role_view("textures/fol.pcx");
	legend = view.get("legend");
	TEST_EXPECT(legend && legend->array.size() == 3 &&
	            legend->array[2].get_string("words", "") == "grows foliage 1 (tall_grass.3di)");
	// The particle graphic: as its additive page holds it, alpha cleared, and its share of the page.
	TEST_EXPECT(rig.show_use("textures/spark.tga"));
	viewport = rig.viewport("textures/spark.tga");
	TEST_EXPECT(viewport && viewport->shown_use().blend_mode == 1 && viewport->image() &&
	            viewport->image()->levels[0].rgba[3] == 0);
	view = rig.role_view("textures/spark.tga");
	TEST_EXPECT(view.get_string("words", "").find("Alone on a 1024 x 1024 atlas page of its mode") != std::string::npos);
	std::printf("session: each use's picture and legend, the legend read again as its terrain moves\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_pieces();
	failures += test_session();
	if (failures == 0) std::printf("editor_texture_role_view: all passed\n");
	return failures == 0 ? 0 : 1;
}
