// ADR 0046 S18, the compare (documents/texture_compare): a texture beside the DXT texture made of it, the error of
// each level (texture_level_error), the pictures its views draw; through a session, the texture viewport's
// compare option on a TGA (its picture, the stats in the viewport query, a texel's before and after), a .dds no
// import makes refused in words, and a .dds an import makes weighed against its import's source.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_import.h>
#include <editor/documents/texture_compare.h>
#include <editor/documents/texture_image.h>
#include <editor/import/texture_import.h>
#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
using opennova::renderer::ImageImportSettings;
using opennova::renderer::encode_image;
using opennova::png::encode_png_rgba;

namespace {

// A smooth gradient of `w` x `h`, its alpha a ramp too where `alpha`.
std::vector<uint8_t> gradient(uint32_t w, uint32_t h, bool alpha) {
	std::vector<uint8_t> rgba(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t *t = &rgba[(size_t(y) * w + x) * 4];
			t[0] = uint8_t(x * 255 / std::max(1u, w - 1));
			t[1] = uint8_t(y * 255 / std::max(1u, h - 1));
			t[2] = uint8_t(128);
			t[3] = alpha ? uint8_t((x + y) * 255 / std::max(1u, w + h - 2)) : 255;
		}
	return rgba;
}

std::vector<uint8_t> tga(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	return out;
}

TextureLevel level_of(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
	TextureLevel level;
	level.width = w;
	level.height = h;
	level.rgba = std::move(rgba);
	return level;
}

int test_error() {
	const TextureLevel a = level_of(gradient(8, 8, true), 8, 8);
	const TextureLevelError same = texture_level_error(a, a);
	TEST_EXPECT(same.width == 8 && same.psnr_rgb == kTexturePsnrExact && same.psnr_alpha == kTexturePsnrExact &&
	            same.max_rgb == 0 && same.worst_rms == 0.0);
	// One texel's red off by 16 in the block at 4, 4: the worst block there, the largest error 16, the PSNR of
	// one squared error of 256 over 192 values.
	TextureLevel b = a;
	b.rgba[(size_t(5) * 8 + 6) * 4] = uint8_t(b.rgba[(size_t(5) * 8 + 6) * 4] + 16);
	const TextureLevelError one = texture_level_error(a, b);
	TEST_EXPECT(one.max_rgb == 16 && one.max_alpha == 0 && one.worst_x == 4 && one.worst_y == 4);
	TEST_EXPECT(std::fabs(one.psnr_rgb - 10.0 * std::log10(255.0 * 255.0 / (256.0 / 192.0))) < 1e-9);
	TEST_EXPECT(one.psnr_alpha == kTexturePsnrExact);
	// Sides that differ: no error made.
	TEST_EXPECT(texture_level_error(a, level_of(gradient(4, 4, true), 4, 4)).width == 0);
	std::printf("error: equal levels exact, one texel's error placed and weighed\n");
	return 0;
}

int test_compression() {
	// A 64 x 64 gradient with a graded alpha: DXT5, a full chain of seven levels, each weighed.
	const std::shared_ptr<const TextureImage> image = decode_texture("ramp.tga", tga(gradient(64, 64, true), 64, 64));
	TEST_EXPECT(image && image->decoded && image->alpha == TextureAlpha::Graded);
	if (!image) return 1;
	const TextureCompression five = compress_texture(*image);
	TEST_EXPECT(five.made && five.format == "DXT5" && five.against == "its own texels" && five.file_bytes > 4096 + 128);
	TEST_EXPECT(five.compressed && five.compressed->levels.size() == 7 && five.reference->levels.size() == 7 &&
	            five.errors.size() == 7);
	if (!five.made || five.errors.empty()) return 1;
	// A smooth gradient survives well; the error is not nothing.
	TEST_EXPECT(five.errors[0].psnr_rgb > 30.0 && five.errors[0].psnr_rgb < kTexturePsnrExact && five.errors[0].max_rgb > 0);
	TEST_EXPECT(five.errors[0].psnr_alpha > 30.0);
	// Opaque: DXT1, half the blocks' bytes; forced DXT5 the same texture.
	const std::shared_ptr<const TextureImage> solid = decode_texture("solid.tga", tga(gradient(64, 64, false), 64, 64));
	const TextureCompression one = compress_texture(*solid);
	TEST_EXPECT(one.made && one.format == "DXT1" && one.file_bytes < five.file_bytes);
	TEST_EXPECT(compress_texture(*solid, "dxt5").format == "DXT5");
	// A texture that does not load: none, and why.
	const std::shared_ptr<const TextureImage> broken = decode_texture("bad.dds", {1, 2, 3});
	const TextureCompression none = compress_texture(*broken);
	TEST_EXPECT(!none.made && !none.why.empty());
	// The pictures: the split's left quarter the reference's, the rest the compressed; the compressed alone; the
	// difference of a level against itself black and opaque.
	const auto split = texture_compare_picture(five, TextureCompareView::Split, 0.25f);
	TEST_EXPECT(split && split->levels.size() == 7);
	if (split) {
		const TextureLevel &top = split->levels[0];
		const auto texel = [](const TextureLevel &l, uint32_t x, uint32_t y) {
			return std::vector<uint8_t>(l.rgba.begin() + std::ptrdiff_t((size_t(y) * l.width + x) * 4),
			                            l.rgba.begin() + std::ptrdiff_t((size_t(y) * l.width + x) * 4 + 4));
		};
		TEST_EXPECT(texel(top, 15, 9) == texel(five.reference->levels[0], 15, 9));
		TEST_EXPECT(texel(top, 16, 9) == texel(five.compressed->levels[0], 16, 9));
	}
	TEST_EXPECT(texture_compare_picture(five, TextureCompareView::Compressed, 0.5f) == five.compressed);
	TEST_EXPECT(!texture_compare_picture(five, TextureCompareView::Off, 0.5f));
	TextureCompression itself = five;
	itself.compressed = itself.reference;
	const auto difference = texture_compare_picture(itself, TextureCompareView::Difference, 0.5f);
	TEST_EXPECT(difference && difference->levels[0].rgba[0] == 0 && difference->levels[0].rgba[3] == 255);
	// The wire, and the words.
	const JsonValue json = texture_compression_json(five);
	TEST_EXPECT(json.get_bool("made", false) && json.get_string("format", "") == "DXT5" && json.get("levels") &&
	            json.get("levels")->array.size() == 7 && json.get("levels")->array[0].get("worst_block"));
	TEST_EXPECT(texture_level_error_words(five.errors[0], true).rfind("PSNR ", 0) == 0);
	TextureCompareView view = TextureCompareView::Off;
	TEST_EXPECT(texture_compare_view_from_token("dds", view) && view == TextureCompareView::Compressed &&
	            !texture_compare_view_from_token("both", view));
	std::printf("compression: DXT5 and DXT1 by the alpha, a full chain weighed level by level, the pictures, the wire\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_texture_compare"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string ramp = "textures/ramp.tga";
	std::string root() const { return dir.file("project"); }
	const TextureViewport *viewport(const std::string &path) {
		return static_cast<const TextureViewport *>(session.viewports().find(path, ViewportKind::Texture));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue query(const std::string &name, const std::string &args_json) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(args_json, args, error);
		return session.query(name, args, error);
	}
	bool set_options(const std::string &path, const std::string &options) {
		editor_test::handle_to_end(session, request::set_viewport(path, "{\"kind\":\"texture\",\"options\":" + options + "}"));
		pump();
		return session.outcome().done();
	}
};

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Compare"));
	editor_test::create_missing_files(rig.session);
	const std::vector<uint8_t> pixels = gradient(64, 64, true);
	// A plain .dds: the gradient as the importer writes a DXT5 with its chain.
	opennova::RgbaImage ramp_image;
	ramp_image.width = ramp_image.height = 64;
	ramp_image.pixels = pixels;
	ImageImportSettings dds_settings;
	dds_settings.format = "dds";
	dds_settings.dds = "dxt5";
	dds_settings.mips = "full";
	std::vector<uint8_t> plain_dds;
	std::string why, note;
	TEST_EXPECT(encode_image(ramp_image, dds_settings, plain_dds, why, note));
	TEST_EXPECT(editor_test::write_bytes(rig.root() + "/" + rig.ramp, tga(pixels, 64, 64)) &&
	            editor_test::write_bytes(rig.root() + "/textures/plain.dds", plain_dds));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.ramp));
	rig.pump();
	const TextureViewport *viewport = rig.viewport(rig.ramp);
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && !viewport->compression());
	if (!viewport) return 1;
	// The split at a quarter: the picture the compare's, the stats on the wire.
	TEST_EXPECT(rig.set_options(rig.ramp, "{\"compare\":\"split\",\"split\":0.25}"));
	TEST_EXPECT(viewport->compression() && viewport->compression()->made && viewport->shown_error() &&
	            viewport->image() != viewport->source());
	editor_test::FakeDevice *device = rig.devices.held(rig.ramp, ViewportKind::Texture);
	TEST_EXPECT(device && device->last() == ViewportAction::Rebuild);
	JsonValue state = rig.query("viewport", "{\"path\":\"" + rig.ramp + "\",\"op\":\"state\"}");
	const JsonValue *body = state.get("body");
	const JsonValue *compare = body ? body->get("compare") : nullptr;
	TEST_EXPECT(compare && compare->get_bool("made", false) && compare->get_string("format", "") == "DXT5" &&
	            compare->get("levels") && compare->get("levels")->array.size() == 7);
	TEST_EXPECT(state.get("options") && state.get("options")->get_string("compare", "") == "split" &&
	            std::fabs(state.get("options")->get_number("split", 0) - 0.25) < 1e-6);
	// A texel says its value before and after.
	const JsonValue hit = rig.query("viewport", "{\"path\":\"" + rig.ramp + "\",\"op\":\"hit\",\"x\":400,\"y\":300}");
	TEST_EXPECT(hit.get_string("name", "").find("; before: R ") != std::string::npos &&
	            hit.get_string("name", "").find("; DXT5: R ") != std::string::npos);
	// The view changed, the compare kept (made once); the difference; off again, the texture itself.
	const std::shared_ptr<const TextureCompression> made = viewport->compression();
	TEST_EXPECT(rig.set_options(rig.ramp, "{\"compare\":\"difference\"}") && viewport->compression() == made);
	TEST_EXPECT(rig.set_options(rig.ramp, "{\"compare\":\"off\"}") && !viewport->compression() &&
	            viewport->image() == viewport->source());
	TEST_EXPECT(rig.set_options(rig.ramp, "{\"compare\":\"dds\"}") && viewport->compression() == made);
	// Refused: a compare of no such view, a split past the texture.
	editor_test::handle_to_end(rig.session,
	                           request::set_viewport(rig.ramp, "{\"kind\":\"texture\",\"options\":{\"compare\":\"both\"}}"));
	TEST_EXPECT(!rig.session.outcome().done());
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.ramp, "{\"kind\":\"texture\",\"options\":{\"split\":2}}"));
	TEST_EXPECT(!rig.session.outcome().done());

	// A .dds no import makes holds its compressed texels alone: none, and why.
	editor_test::handle_to_end(rig.session, request::open_document("textures/plain.dds"));
	TEST_EXPECT(rig.set_options("textures/plain.dds", "{\"compare\":\"split\"}"));
	const TextureViewport *plain = rig.viewport("textures/plain.dds");
	TEST_EXPECT(plain && plain->compression() && !plain->compression()->made &&
	            plain->compression()->why.find("holds its compressed texels alone") != std::string::npos);

	// A .dds an import makes, against its import's source as the import prepares it.
	opennova::RgbaImage source;
	source.width = source.height = 64;
	source.pixels = pixels;
	const std::string art = rig.dir.file("art");
	TEST_EXPECT(editor_test::write_bytes(art + "/skin.png", encode_png_rgba(source.pixels.data(), 64, 64)));
	const ImportResult imported = import_assets({{art + "/skin.png", {}}}, ProjectPaths::for_root(rig.root()),
	                                            *rig.session.view().project.document, false);
	TEST_EXPECT(imported.imported.size() == 1);
	if (imported.imported.size() != 1) return 1;
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::set_import_options(imported.imported[0], {{"format", "dds"}, {"dds", "dxt5"}}));
	rig.pump();
	const AssetEntry *output = rig.session.view().project.scan->find("skin.dds");
	TEST_EXPECT(output && output->imported_from == imported.imported[0]);
	if (!output) return 1;
	editor_test::handle_to_end(rig.session, request::open_document(output->relative_path));
	TEST_EXPECT(rig.set_options(output->relative_path, "{\"compare\":\"split\"}"));
	const TextureViewport *skin = rig.viewport(output->relative_path);
	TEST_EXPECT(skin && skin->compression() && skin->compression()->made &&
	            skin->compression()->against == imported.imported[0] + ", its import's source" &&
	            skin->compression()->format == "DXT5" && skin->compression()->file_bytes == output->size_bytes);
	if (skin && skin->compression() && skin->compression()->made && !skin->compression()->errors.empty())
		TEST_EXPECT(skin->compression()->errors[0].psnr_rgb > 30.0);
	std::printf("session: a TGA's split, stats and texels; the view changed on one compare; a plain .dds refused; an "
	            "imported .dds against its source\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_error();
	failures += test_compression();
	failures += test_session();
	if (failures == 0) std::printf("editor_texture_compare: all passed\n");
	return failures == 0 ? 0 : 1;
}
