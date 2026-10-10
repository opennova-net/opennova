// ADR 0046 S18, a texture as the game draws it: the chain the game builds of a texture made from pixels
// (texture_game_chain), the halvings of the object texture detail (texture_halved), a normal map lit
// (texture_lit_normals), and a model row's alpha in words (texture_alpha_meaning_words); through a
// session the texture viewport's level, detail, normals and light options, and a Phong diffuse shown as its use
// draws it, opaque, its alpha the specular brightness.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_import.h>
#include <editor/documents/texture_roles.h>
#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/texture_dxt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
namespace renderer = opennova::renderer;
using opennova::io::JsonValue;

namespace {

TextureLevel level_of(uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
	TextureLevel level;
	level.width = w;
	level.height = h;
	level.rgba = std::move(rgba);
	return level;
}

int test_pieces() {
	// The chain the game builds: 8 x 8 ends at 4 x 4, its texel the box filter's of the four under it.
	std::vector<uint8_t> rgba(8 * 8 * 4, 0);
	for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = uint8_t((i / 4) % 2 ? 200 : 100), rgba[i + 3] = 255;
	const std::vector<TextureLevel> chain = texture_game_chain(level_of(8, 8, rgba), 2);
	TEST_EXPECT(chain.size() == 2 && chain[1].width == 4 && chain[1].height == 4 && chain[1].rgba[0] == 150 && chain[1].rgba[3] == 255);
	TEST_EXPECT(texture_game_chain(level_of(8, 8, rgba), 0).size() == 4);
	// Each level the box filter of the bytes the level before was stored as, as D3DXFilterTexture filters
	// (renderer::extend_box_chain), not of the filter's floats carried down: on this texture the two part
	// at some level.
	std::vector<uint8_t> noise(16 * 16 * 4);
	uint32_t seed = 12345u;
	for (uint8_t &byte : noise) seed = seed * 1103515245u + 12345u, byte = uint8_t(seed >> 16);
	const std::vector<TextureLevel> stored = texture_game_chain(level_of(16, 16, noise), 0);
	TEST_EXPECT(stored.size() == 5);
	std::vector<renderer::DxtColor> carried = renderer::decode_rgba8(noise.data(), 16, 16);
	bool parted = false;
	for (size_t i = 1; i < stored.size(); ++i) {
		const TextureLevel &above = stored[i - 1];
		TEST_EXPECT(stored[i].rgba == renderer::box_filter_half_rgba8(above.rgba.data(), above.width, above.height));
		carried = renderer::box_filter_half(carried, above.width, above.height);
		parted = parted || renderer::encode_rgba8(carried) != stored[i].rgba;
	}
	TEST_EXPECT(parted);
	// A halving as the game's: the truncated mean of each 2 x 2 block (101 and 102 make 101).
	std::vector<uint8_t> odd(4 * 4 * 4, 255);
	for (size_t i = 0; i < odd.size(); i += 4) odd[i] = uint8_t((i / 4) % 2 ? 102 : 101);
	const TextureLevel half = texture_halved(level_of(4, 4, odd), 1);
	TEST_EXPECT(half.width == 2 && half.height == 2 && half.rgba[0] == 101);
	TEST_EXPECT(texture_halved(level_of(4, 4, odd), 2).width == 1);
	// A normal map lit: a flat normal the same grey whatever the light's direction; a normal leaning toward the
	// light brighter than one leaning away.
	const std::vector<uint8_t> flat = {128, 128, 255, 255};
	const uint8_t from_right = texture_lit_normals(level_of(1, 1, flat), 0.0f).rgba[0];
	TEST_EXPECT(std::abs(int(from_right) - int(texture_lit_normals(level_of(1, 1, flat), 90.0f).rgba[0])) <= 1 && from_right > 150 &&
	            from_right < 200);
	const std::vector<uint8_t> right = {230, 128, 200, 255}, left = {26, 128, 200, 255};
	TEST_EXPECT(texture_lit_normals(level_of(1, 1, right), 0.0f).rgba[0] > texture_lit_normals(level_of(1, 1, left), 0.0f).rgba[0]);
	TEST_EXPECT(texture_lit_normals(level_of(1, 1, right), 0.0f).rgba[3] == 255);
	// A model row's alpha in words (what it is, renderer::texture_row_alpha_meaning, renderer_texture_roles pins).
	using M = renderer::TextureAlphaMeaning;
	TEST_EXPECT(texture_alpha_meaning_words(M::Specular, "VS_PHONGT", 0, false) ==
	            "the specular brightness VS_PHONGT reads, not transparency: the surface is opaque");
	TEST_EXPECT(texture_alpha_meaning_words(M::CutOut, "FF_ST_OP", 128, false).find("above 128") != std::string::npos);
	std::printf("pieces: the game's chain, a halving, normals lit, a model row's alpha in words\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_texture_game_view"};
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
	JsonValue state(const std::string &path) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse("{\"path\":\"" + path + "\",\"op\":\"state\"}", args, error);
		return session.query("viewport", args, error);
	}
	bool set_options(const std::string &path, const std::string &options) {
		editor_test::handle_to_end(session, request::set_viewport(path, "{\"kind\":\"texture\",\"options\":" + options + "}"));
		pump();
		return session.outcome().done();
	}
};

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Game view"));
	editor_test::create_missing_files(rig.session);
	// A 16 x 16 diffuse whose alpha is graded, on a VS_PHONGT material in slot 1.
	std::vector<uint8_t> rgba(16 * 16 * 4);
	for (size_t i = 0; i < rgba.size(); i += 4) {
		rgba[i] = 90;
		rgba[i + 1] = 120;
		rgba[i + 2] = 150;
		rgba[i + 3] = uint8_t(i / 4);
	}
	std::vector<uint8_t> tga;
	std::string error;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba.data(), 16, 16, tga, error) &&
	            editor_test::write_bytes(rig.root() + "/textures/crate.tga", tga));
	const std::string scene = rig.dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/crate.o3d",
	                                    "o3d 2\nmodel CRATE\nmaterial VS_PHONGT\ntexture crate.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                                    "mesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	ImportChoice model;
	model.path = scene + "/crate.o3d";
	TEST_EXPECT(import_assets({model}, ProjectPaths::for_root(rig.root()), *rig.session.view().project.document, false).imported.size() == 1);
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document("textures/crate.tga"));
	rig.pump();
	const std::string path = "textures/crate.tga";
	const TextureViewport *viewport = rig.viewport(path);
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && viewport->image()->levels.size() == 1);
	if (!viewport) return 1;
	// As its Phong use draws it: opaque, its alpha said the specular brightness.
	TEST_EXPECT(rig.set_options(path, "{\"as_used\":0}"));
	TEST_EXPECT(viewport->shown_use().model_row && viewport->shown_use().alpha == renderer::TextureAlphaMeaning::Specular &&
	            viewport->image()->levels[0].rgba[3] == 255 && viewport->image()->levels[0].rgba[3 + 4 * 7] == 255);
	JsonValue state = rig.state(path);
	const JsonValue *body = state.get("body");
	TEST_EXPECT(body && body->get("as_used") && body->get("as_used")->get_string("alpha", "") == "specular" &&
	            body->get("as_used")->get_string("alpha_words", "").find("specular brightness VS_PHONGT") != std::string::npos);
	// The game's chain: a level past the first builds it (16 down to 4 x 4, three levels).
	TEST_EXPECT(rig.set_options(path, "{\"as_used\":-1,\"level\":2}"));
	TEST_EXPECT(viewport->image()->levels.size() == 3 && viewport->shown_level() == 2 && viewport->image()->levels[2].width == 4);
	editor_test::FakeDevice *device = rig.devices.held(path, ViewportKind::Texture);
	TEST_EXPECT(device && device->last() == ViewportAction::Rebuild);
	// The lowest object texture detail: a diffuse halved twice, 4 x 4.
	TEST_EXPECT(rig.set_options(path, "{\"level\":0,\"detail\":0}"));
	TEST_EXPECT(viewport->shown_detail() == 0 && viewport->image()->width() == 4 && viewport->shown_device().halvings == 2);
	state = rig.state(path);
	body = state.get("body");
	TEST_EXPECT(body && body->get("detail") && body->get("detail")->get_number("width", 0) == 4 &&
	            body->get("detail")->get_string("format", "") == "A8R8G8B8");
	// Full detail: as stored.
	TEST_EXPECT(rig.set_options(path, "{\"detail\":3}") && viewport->image()->width() == 16 && viewport->shown_detail() == 3);
	// Normals lit: every texel a grey, opaque; the light moved, lit again.
	TEST_EXPECT(rig.set_options(path, "{\"detail\":-1,\"channels\":\"normals\",\"light\":45}"));
	const TextureLevel &lit = viewport->image()->levels[0];
	TEST_EXPECT(lit.rgba[0] == lit.rgba[1] && lit.rgba[1] == lit.rgba[2] && lit.rgba[3] == 255);
	const uint8_t at45 = lit.rgba[0];
	TEST_EXPECT(rig.set_options(path, "{\"light\":225}") && viewport->image()->levels[0].rgba[0] != at45);
	// Refused: a detail past 3, a light of no number.
	editor_test::handle_to_end(rig.session, request::set_viewport(path, "{\"kind\":\"texture\",\"options\":{\"detail\":5}}"));
	TEST_EXPECT(!rig.session.outcome().done());
	editor_test::handle_to_end(rig.session, request::set_viewport(path, "{\"kind\":\"texture\",\"options\":{\"light\":\"x\"}}"));
	TEST_EXPECT(!rig.session.outcome().done());
	std::printf("session: a Phong diffuse as its use draws it, the game's chain, a detail's halvings, normals lit\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_pieces();
	failures += test_session();
	if (failures == 0) std::printf("editor_texture_game_view: all passed\n");
	return failures == 0 ? 0 : 1;
}
