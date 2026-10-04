// ADR 0046 S18, the texture thumbnails (preview/texture_thumbnails): a texture's bytes as a small
// picture (its proportions kept, averaged by alpha so a cut-out's hidden colour does not bleed, the DDS
// level nearest above its size taken, the use's loader's transform applied, a file the game cannot load
// said so); the PNG writer (import/png_encode) read back by the importer's reader; the session's cache
// (a picture asked for is made by a poll, once while the file's stamp stands, again when it moves); the
// wire: the texture_thumbnail query's facts and base64 PNG, a texture field's record JSON naming the
// file its loader opens and what that file is, a texture field's choices each the file it would load.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/import/png_decode.h>
#include <editor/import/png_encode.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/texture_dxt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

namespace renderer = opennova::renderer;

std::vector<uint8_t> tga_of(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	return out;
}

// A w x h image of one colour.
std::vector<uint8_t> solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i < size_t(w) * h; ++i) out.insert(out.end(), {r, g, b, a});
	return out;
}

std::vector<uint8_t> unbase64(const std::string &text) {
	std::vector<uint8_t> out;
	uint32_t chunk = 0;
	int bits = 0;
	for (const char c : text) {
		int v = -1;
		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '+') v = 62;
		else if (c == '/') v = 63;
		if (v < 0) continue;
		chunk = (chunk << 6) | uint32_t(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(uint8_t(chunk >> bits));
		}
	}
	return out;
}

int test_pictures() {
	// 256 x 128 fits 128 x 64; a solid colour stays that colour.
	std::shared_ptr<TextureThumbnail> picture =
			make_texture_thumbnail("wall.tga", tga_of(solid(256, 128, 10, 20, 30, 255), 256, 128), TextureLoadTransform::None);
	TEST_EXPECT(picture->state == TextureThumbnail::State::Ready && picture->width == 128 && picture->height == 64 &&
	            picture->source_width == 256 && picture->source_height == 128 && !picture->translucent);
	TEST_EXPECT(picture->rgba[0] == 10 && picture->rgba[1] == 20 && picture->rgba[2] == 30 && picture->rgba[3] == 255 &&
	            picture->format == "TGA image" && picture->average[0] == 10);
	// A small one keeps its size.
	picture = make_texture_thumbnail("dot.tga", tga_of(solid(3, 2, 1, 2, 3, 4), 3, 2), TextureLoadTransform::None);
	TEST_EXPECT(picture->width == 3 && picture->height == 2 && picture->translucent);
	// Averaged by alpha: a hidden red texel beside an opaque blue one is blue, half covering.
	const std::vector<uint8_t> two = {255, 0, 0, 0, 0, 0, 255, 255};
	picture = make_texture_thumbnail("cut.tga", tga_of(two, 2, 1), TextureLoadTransform::None, 1);
	TEST_EXPECT(picture->width == 1 && picture->height == 1 && picture->rgba[0] == 0 && picture->rgba[2] == 255 &&
	            picture->rgba[3] == 128);
	// What the use's loader makes of it: the HUD's alpha alone.
	picture = make_texture_thumbnail("hud.tga", tga_of(solid(4, 4, 200, 100, 50, 60), 4, 4), TextureLoadTransform::AlphaOnly);
	TEST_EXPECT(picture->rgba[0] == 255 && picture->rgba[1] == 255 && picture->rgba[3] == 60 && picture->transform == TextureLoadTransform::AlphaOnly);
	// A DXT5 chain: the level nearest above the picture (32 for a side of 32), its texels the codec's.
	const std::vector<uint8_t> source = solid(128, 128, 0, 255, 0, 255);
	const std::vector<renderer::DxtSurface> levels =
			renderer::build_dxt_texture_levels(source.data(), 128, 128, renderer::TextureDxtFormat::Dxt5, 8);
	std::vector<std::vector<uint8_t>> blocks;
	for (const renderer::DxtSurface &level : levels) blocks.push_back(level.blocks);
	std::vector<uint8_t> dds;
	std::string error;
	TEST_EXPECT(opennova::dds::dds_write_dxt(opennova::dds::dds_fourcc('D', 'X', 'T', '5'), 128, 128, blocks, dds, error));
	picture = make_texture_thumbnail("grass.dds", dds, TextureLoadTransform::None, 32);
	TEST_EXPECT(picture->state == TextureThumbnail::State::Ready && picture->width == 32 && picture->levels == levels.size() &&
	            picture->rgba[1] > 240 && picture->rgba[0] < 16);
	// Cut short: the game cannot load it, and why.
	dds.resize(dds.size() - 4);
	picture = make_texture_thumbnail("cut.dds", dds, TextureLoadTransform::None);
	TEST_EXPECT(picture->state == TextureThumbnail::State::Unloadable && !picture->refusal.empty() && picture->rgba.empty());
	// The PNG writer, read back by the importer's reader.
	const std::vector<uint8_t> rgba = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
	const std::vector<uint8_t> png = encode_png_rgba(rgba.data(), 3, 2);
	opennova::RgbaImage read;
	TEST_EXPECT(decode_png(png, read, error) && read.width == 3 && read.height == 2 && read.pixels == rgba);
	TEST_EXPECT(encode_png_rgba(rgba.data(), 0, 2).empty());
	std::printf("pictures: fitted, averaged by alpha, transformed, a DDS level taken, a refusal said; the PNG writer read back\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_texture_thumbnails"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	JsonValue query(const std::string &name, const std::string &args_json, std::string *error_out = nullptr) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(args_json, args, error);
		JsonValue answer = session.query(name, args, error);
		if (error_out) *error_out = error;
		return answer;
	}
};

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Thumbnails"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_bytes(rig.root() + "/textures/stance.tga", tga_of(solid(8, 4, 90, 80, 70, 200), 8, 4)) &&
	            editor_test::write_bytes(rig.root() + "/textures/other.tga", tga_of(solid(2, 2, 1, 1, 1, 255), 2, 2)) &&
	            editor_test::write_text(rig.root() + "/defs/items.def",
	                                    "begin \"Brick\"\nid 100300\ntype building\nhud_image stance.tga\nend\n"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.session.run_operations();
	TextureThumbnails &thumbnails = *rig.view().documents.thumbnails;
	// Asked as a window draws: none yet, queued; a poll makes it; asked again, the same picture.
	TEST_EXPECT(!thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::None) && thumbnails.pending());
	rig.session.poll();
	std::shared_ptr<const TextureThumbnail> made = thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::None);
	TEST_EXPECT(made && made->width == 8 && made->height == 4 && made->serial == 1 && !thumbnails.pending() && thumbnails.made() == 1);
	rig.session.poll();
	TEST_EXPECT(thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::None) == made && thumbnails.made() == 1);
	// Another transform is another picture; a file the project lacks none.
	TEST_EXPECT(!thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::AlphaOnly));
	TEST_EXPECT(!thumbnails.get(rig.view(), "textures/none.tga", TextureLoadTransform::None));
	rig.session.poll();
	TEST_EXPECT(thumbnails.made() == 2);
	// The file changed (its stamp moved): the last picture answers until a poll makes the new one.
	TEST_EXPECT(editor_test::write_bytes(rig.root() + "/textures/stance.tga", tga_of(solid(16, 16, 9, 9, 9, 255), 16, 16)));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.session.run_operations();
	TEST_EXPECT(thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::None) == made && thumbnails.pending());
	rig.session.poll();
	made = thumbnails.get(rig.view(), "textures/stance.tga", TextureLoadTransform::None);
	TEST_EXPECT(made && made->width == 16 && made->serial == 3);

	// The wire: the query's facts and its PNG.
	std::string error;
	JsonValue answer = rig.query("texture_thumbnail", "{\"path\":\"stance.tga\",\"transform\":\"alpha_only\"}", &error);
	TEST_EXPECT(error.empty() && answer.get_string("file", "") == "textures/stance.tga" &&
	            answer.get_string("state", "") == "ready" && answer.get_string("transform", "") == "alpha_only" &&
	            answer.get_number("source_width", 0) == 16 && answer.get_string("format", "") == "TGA image");
	opennova::RgbaImage png;
	TEST_EXPECT(decode_png(unbase64(answer.get_string("png", "")), png, error) && png.width == 16 && png.height == 16 &&
	            png.pixels[0] == 255 && png.pixels[3] == 255);
	rig.query("texture_thumbnail", "{\"path\":\"defs/items.def\"}", &error);
	TEST_EXPECT(!error.empty());
	rig.query("texture_thumbnail", "{\"path\":\"stance.tga\",\"transform\":\"sideways\"}", &error);
	TEST_EXPECT(!error.empty());

	// A texture field's record: the file its loader opens, what the loader makes of it, what it is.
	editor_test::handle_to_end(rig.session, request::open_document("defs/items.def"));
	const JsonValue document = rig.query("document", "{\"path\":\"defs/items.def\"}");
	const JsonValue *rows = document.get("rows");
	TEST_EXPECT(rows && !rows->array.empty());
	const int64_t id = rows && !rows->array.empty() ? int64_t(rows->array[0].get_number("id", 0)) : 0;
	const JsonValue record = rig.query("record", "{\"path\":\"defs/items.def\",\"id\":" + std::to_string(id) + "}");
	const JsonValue *texture = nullptr;
	if (const JsonValue *fields = record.get("fields"))
		for (const JsonValue &field : fields->array)
			if (field.get_string("id", "") == "hud_image") texture = field.get("texture");
	TEST_EXPECT(texture && texture->get_string("file", "") == "textures/stance.tga" &&
	            texture->get_string("transform", "") == "alpha_only" && texture->get_string("status", "") == "present" &&
	            texture->get_bool("loads", false) && texture->get_number("width", 0) == 16);
	// Its choices: each texture by the file it would load, and what that is.
	const JsonValue choices = rig.query("reference_choices", "{\"path\":\"defs/items.def\",\"id\":" + std::to_string(id) +
	                                                                 ",\"field\":\"hud_image\"}");
	bool other = false;
	if (const JsonValue *list = choices.get("choices"))
		for (const JsonValue &choice : list->array)
			if (choice.get_string("name", "") == "other.tga")
				other = choice.get_string("served", "") == "textures/other.tga" && choice.get("texture") &&
				        choice.get("texture")->get_number("width", 0) == 2;
	TEST_EXPECT(other);
	// Closed, the project's pictures go.
	editor_test::handle_to_end(rig.session, request::close_project());
	TEST_EXPECT(thumbnails.held() == 0 && thumbnails.held_bytes() == 0);
	std::printf("session: made by a poll, kept while the stamp stands, again when it moves; the query, a field's "
	            "and its choices' textures over the wire\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_pictures();
	failures += test_session();
	if (failures == 0) std::printf("editor_texture_thumbnails: all passed\n");
	return failures == 0 ? 0 : 1;
}
