// ADR 0046 S18, the image importer's options as data (import/texture_import) and what a texture's uses ask
// of an import (graph/texture_import_needs, session/texture_import_state): the option rows and the values
// each takes; the pieces (the sizes asked, a halving by 2 x 2 boxes, an area average, each alpha, a green
// flipped); every format written and read back by the reader the game picks (a 24-bit TGA, a PCX, a DXT5
// DDS with its whole chain each level the ported codec's, a DXT1, an A8R8G8B8, an MDT, a PNG); over a
// minted project, a PNG imported to a TGA by default, what its uses ask (a model row's DDS, a colour
// map's 24-bit 1024 x 1024 TGA, a loading screen's PCX, a conflict), set_import_options writing the
// record and importing again under the new name, the old file gone, a value no row takes refused; the
// import_options query.
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/texture_image.h>
#include <editor/graph/texture_import_needs.h>
#include <editor/import/importer.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/texture_dxt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using io::JsonValue;

namespace {

// A `w` x `h` image of graded colours, its alpha falling left to right.
RgbaImage graded(int w, int h) {
	RgbaImage image;
	image.width = w;
	image.height = h;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			image.pixels.push_back(uint8_t(x * 255 / std::max(1, w - 1)));
			image.pixels.push_back(uint8_t(y * 255 / std::max(1, h - 1)));
			image.pixels.push_back(uint8_t((x + y) * 16));
			image.pixels.push_back(uint8_t(255 - x * 255 / std::max(1, w - 1)));
		}
	return image;
}

ImageImportSettings settings_of(const ImportOptions &options) { return image_import_settings(options); }

// The first level of a file read as the game's reader its name picks.
std::shared_ptr<const TextureImage> read_back(const std::string &name, const std::vector<uint8_t> &bytes) {
	return decode_texture(name, bytes);
}

int test_rows() {
	const std::vector<ImportOptionRow> &rows = image_import_option_rows();
	std::vector<std::string> keys;
	for (const ImportOptionRow &row : rows) keys.push_back(row.key);
	TEST_EXPECT(keys == std::vector<std::string>({"format", "name", "alpha", "size", "palette", "dds", "mips", "green"}));
	const ImportOptionRow *format = import_option_row(rows, "format");
	const ImportOptionRow *alpha = import_option_row(rows, "alpha");
	const ImportOptionRow *size = import_option_row(rows, "size");
	const ImportOptionRow *name = import_option_row(rows, "name");
	TEST_EXPECT(format && alpha && size && name && !import_option_row(rows, "colour"));
	if (!format || !alpha || !size || !name) return 1;
	TEST_EXPECT(format->fallback == "tga" && import_option_accepts(*format, "dds") && !import_option_accepts(*format, "bmp"));
	TEST_EXPECT(import_option_accepts(*alpha, "threshold:128") && import_option_accepts(*alpha, "key:#FF00ff") &&
	            !import_option_accepts(*alpha, "threshold:300") && !import_option_accepts(*alpha, "key:red"));
	TEST_EXPECT(import_option_accepts(*size, "512x256") && import_option_accepts(*size, "fit:1024x1024") &&
	            import_option_accepts(*size, "pow2_down") && !import_option_accepts(*size, "512") &&
	            !import_option_accepts(*size, "0x4"));
	TEST_EXPECT(import_option_accepts(*name, "Body.dds") && !import_option_accepts(*name, "a/b.tga") &&
	            !import_option_accepts(*name, "a_name_far_too_long.tga"));
	TEST_EXPECT(import_option_takes(*size) == "source, pow2_down, pow2_up, <W>x<H> or fit:<W>x<H>");
	// The importer's row carries them.
	const Importer *importer = importer_for("logo.png");
	TEST_EXPECT(importer && importer->version == kImageImporterVersion && importer->options.size() == rows.size() &&
	            importer->default_options.empty());
	std::printf("rows: eight options, each its values and forms\n");
	return 0;
}

int test_pieces() {
	uint32_t w = 0, h = 0;
	std::string why;
	TEST_EXPECT(image_target_size("pow2_down", 300, 129, w, h, why) && w == 256 && h == 128);
	TEST_EXPECT(image_target_size("pow2_up", 300, 129, w, h, why) && w == 512 && h == 256);
	TEST_EXPECT(image_target_size("fit:512x512", 1024, 256, w, h, why) && w == 512 && h == 128);
	TEST_EXPECT(image_target_size("fit:2048x2048", 100, 50, w, h, why) && w == 100 && h == 50);
	TEST_EXPECT(image_target_size("64x32", 7, 7, w, h, why) && w == 64 && h == 32);
	TEST_EXPECT(!image_target_size("huge", 7, 7, w, h, why) && !why.empty());
	// Halved: each texel the 2 x 2 box's sum shifted by two.
	const RgbaImage source = graded(4, 4);
	const RgbaImage half = resize_image(source, 2, 2);
	TEST_EXPECT(half.width == 2 && half.height == 2);
	for (int c = 0; c < 4; ++c) {
		const uint32_t sum = uint32_t(source.pixels[size_t(c)]) + source.pixels[size_t(4 + c)] + source.pixels[size_t(16 + c)] +
		                     source.pixels[size_t(20 + c)];
		TEST_EXPECT(half.pixels[size_t(c)] == uint8_t(sum >> 2));
	}
	// A third: each texel the average of what it covers; a size up repeats texels.
	RgbaImage flat;
	flat.width = 3;
	flat.height = 1;
	flat.pixels = {0, 0, 0, 255, 90, 90, 90, 255, 180, 180, 180, 255};
	const RgbaImage one = resize_image(flat, 1, 1);
	TEST_EXPECT(one.pixels == std::vector<uint8_t>({90, 90, 90, 255}));
	const RgbaImage up = resize_image(flat, 6, 1);
	TEST_EXPECT(up.pixels[0] == 0 && up.pixels[4] == 0 && up.pixels[8] == 90 && up.pixels[20] == 180);
	// The alphas.
	RgbaImage image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "opaque", why) && image.pixels[3] == 255 && image.pixels[15] == 255);
	image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "threshold:128", why) && image.pixels[3] == 255 && image.pixels[15] == 0);
	image = graded(4, 1);
	TEST_EXPECT(apply_image_alpha(image, "luminance", why) &&
	            image.pixels[7] == uint8_t((85u * (uint32_t(image.pixels[4]) + image.pixels[5] + image.pixels[6])) >> 8));
	image = graded(4, 1);
	const uint8_t key[3] = {image.pixels[4], image.pixels[5], image.pixels[6]};
	char hex[16];
	std::snprintf(hex, sizeof(hex), "key:#%02X%02X%02X", key[0], key[1], key[2]);
	TEST_EXPECT(apply_image_alpha(image, hex, why) && image.pixels[7] == 0 && image.pixels[3] == 255);
	TEST_EXPECT(!apply_image_alpha(image, "glow", why) && !why.empty());
	image = graded(2, 1);
	const uint8_t green = image.pixels[1];
	flip_image_green(image);
	TEST_EXPECT(image.pixels[1] == uint8_t(255 - green));
	std::printf("pieces: the sizes, a halving, an average, the alphas, the green\n");
	return 0;
}

int test_formats() {
	const RgbaImage image = graded(8, 8);
	std::vector<uint8_t> bytes;
	std::string why, note;
	// A 24-bit TGA: three bytes a texel, bottom row first, read opaque.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "tga24"}}), bytes, why, note));
	tga::TgaHeader header;
	TEST_EXPECT(tga::tga_read_header(bytes.data(), bytes.size(), header) && header.image_type == 2 && header.bits == 24 &&
	            header.descriptor == 0 && bytes.size() == 18 + 8 * 8 * 3);
	std::shared_ptr<const TextureImage> read = read_back("map.tga", bytes);
	TEST_EXPECT(read && read->loads && read->width() == 8 && read->alpha == TextureAlpha::None);
	if (read && read->loads)
		TEST_EXPECT(read->levels[0].rgba[0] == image.pixels[0] && read->levels[0].rgba[1] == image.pixels[1] &&
		            read->levels[0].rgba[3] == 255);
	// A 32-bit TGA keeps the alpha; an MDT is its bytes.
	TEST_EXPECT(encode_image(image, settings_of({}), bytes, why, note));
	read = read_back("a.tga", bytes);
	TEST_EXPECT(read && read->loads && read->levels[0].rgba == image.pixels);
	std::vector<uint8_t> mdt;
	TEST_EXPECT(encode_image(image, settings_of({{"format", "mdt"}}), mdt, why, note) && mdt == bytes);
	// A PCX: the colours kept (64 here), the alpha dropped and said; exact refused past 256 colours.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "pcx"}}), bytes, why, note) && !note.empty());
	read = read_back("a.pcx", bytes);
	TEST_EXPECT(read && read->loads && read->levels[0].rgba[0] == image.pixels[0] && read->levels[0].rgba[3] == 255);
	TEST_EXPECT(!encode_image(graded(32, 32), settings_of({{"format", "pcx"}, {"palette", "exact"}}), bytes, why, note) &&
	            why.find("256") != std::string::npos);
	// A DXT5 DDS of every level to 1 x 1, each the ported codec's (the D3DX box filter of the level before,
	// decoded from its own blocks): what D3DX reads of it, and the game's decode of the first level.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}}), bytes, why, note));
	dds::DdsImage dds;
	TEST_EXPECT(dds::dds_read(bytes.data(), bytes.size(), dds, why) && dds.loads && std::string(dds.format.name) == "DXT5" &&
	            dds.levels.size() == 4);
	const std::vector<renderer::DxtSurface> chain =
	        renderer::build_dxt_texture_levels(image.pixels.data(), 8, 8, renderer::TextureDxtFormat::Dxt5, 4);
	TEST_EXPECT(chain.size() == 4);
	for (size_t i = 0; i < chain.size() && i < dds.levels.size(); ++i)
		TEST_EXPECT(std::vector<uint8_t>(bytes.begin() + long(dds.levels[i].offset),
		                                 bytes.begin() + long(dds.levels[i].offset + dds.levels[i].bytes)) == chain[i].blocks);
	read = read_back("a.dds", bytes);
	TEST_EXPECT(read && read->loads && read->levels.size() == 4);
	// DXT1, one level; A8R8G8B8.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}, {"dds", "dxt1"}, {"mips", "none"}}), bytes, why, note) &&
	            dds::dds_read(bytes.data(), bytes.size(), dds, why) && std::string(dds.format.name) == "DXT1" &&
	            dds.levels.size() == 1);
	TEST_EXPECT(encode_image(image, settings_of({{"format", "dds"}, {"dds", "argb"}}), bytes, why, note) &&
	            dds::dds_read(bytes.data(), bytes.size(), dds, why) && std::string(dds.format.name) == "A8R8G8B8" &&
	            dds.levels.size() == 1 && dds.levels[0].rgba == image.pixels);
	// A PNG.
	TEST_EXPECT(encode_image(image, settings_of({{"format", "png"}}), bytes, why, note));
	read = read_back("a.png", bytes);
	TEST_EXPECT(read && read->loads && read->levels[0].rgba == image.pixels);
	TEST_EXPECT(image_import_output_name("art/logo.png", settings_of({{"format", "tga24"}})) == "logo.tga" &&
	            image_import_output_name("art/logo.png", settings_of({{"name", "Sky.pcx"}})) == "Sky.pcx");
	std::printf("formats: a 24-bit and a 32-bit TGA, an MDT, a PCX, DXT5 with its chain, DXT1, A8R8G8B8, a PNG\n");
	return 0;
}

std::string png_of(const RgbaImage &image) {
	const std::vector<uint8_t> bytes = encode_png_rgba(image.pixels.data(), uint32_t(image.width), uint32_t(image.height));
	return std::string(bytes.begin(), bytes.end());
}

const AssetEntry *output_of(const SessionView &view, const std::string &source) {
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.imported_from == source) return &entry;
	return nullptr;
}

int test_project() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_import"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Import"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	// Three PNGs imported as a modder imports them: a model's skin, a terrain's colour map, a loading
	// screen; and one no reference names.
	const std::string art = dir.file("art");
	for (const char *name : {"body.png", "isle_c.png", "m01.png", "logo.png"})
		TEST_EXPECT(editor_test::write_text(art + "/" + name, png_of(graded(16, 8))));
	const ImportResult imported = import_assets({{art + "/body.png", {}}, {art + "/isle_c.png", {}}, {art + "/m01.png", {}},
	                                             {art + "/logo.png", {}}},
	                                            paths, *view.project.document, false);
	TEST_EXPECT(imported.diagnostics.empty() && imported.imported.size() == 4);
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/thing.o3d",
	                                    "o3d 1\nmodel THING\nmaterial VS_SKBASIC\ntexture body.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                                    "strip 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	TEST_EXPECT(import_assets({{scene + "/thing.o3d", {}}}, paths, *view.project.document, false).imported.size() == 1);
	TEST_EXPECT(editor_test::write_text(root + "/terrains/isle.trn",
	                                    "polytrn_colormap isle_c.tga\npolytrn_detailmap logo.tga\npolytrn_polydata isle.cpt\n"
	                                    "polytrn_sectorcount 1\npolytrn_sectors 0\n"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	// Imported by default as a 32-bit TGA of the source's stem.
	std::string body_source, isle_source, m01_source, logo_source;
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (entry.kind != AssetKind::ImportSource) continue;
		const std::string name = basename_of(entry.relative_path);
		if (name == "body.png") body_source = entry.relative_path;
		if (name == "isle_c.png") isle_source = entry.relative_path;
		if (name == "m01.png") m01_source = entry.relative_path;
		if (name == "logo.png") logo_source = entry.relative_path;
	}
	TEST_EXPECT(!body_source.empty() && !isle_source.empty() && !m01_source.empty() && !logo_source.empty());
	const AssetEntry *logo = output_of(view, logo_source);
	TEST_EXPECT(logo && logo->logical_name == "logo.tga" && logo->kind == AssetKind::Texture);

	// What the uses ask: the model row's body.tga, DXT5 beside it; the colour map, a 24-bit 1024 x 1024 TGA;
	// the coefficient detail, a TGA whose sides are powers of two.
	TextureImportState state;
	std::string error;
	TEST_EXPECT(texture_import_state(view, body_source, state, error));
	TEST_EXPECT(state.needs.options == ImportOptions({{"format", "dds"}}) && state.needs.conflicts.empty() &&
	            state.needs.reasons.size() == 1 && state.needs.reasons[0].find("reads body.dds before body.tga") != std::string::npos);
	TEST_EXPECT(texture_import_state(view, "isle_c.tga", state, error) && state.source == isle_source);
	TEST_EXPECT(state.needs.options == ImportOptions({{"format", "tga24"}, {"size", "1024x1024"}}));
	TEST_EXPECT(texture_import_state(view, logo_source, state, error) &&
	            state.needs.options == ImportOptions({{"format", "tga"}, {"size", "pow2_down"}}));
	// Nothing names m01 yet: it asks nothing. A loading screen names it; a particle graphic too: no one
	// file serves both.
	TEST_EXPECT(texture_import_state(view, m01_source, state, error) && state.needs.options.empty() && state.needs.uses == 0);
	TEST_EXPECT(!texture_import_state(view, "terrains/isle.trn", state, error) && !error.empty());

	// set_import_options: the record written, imported again under the name its format gives, the old
	// file gone.
	editor_test::handle_to_end(session, request::set_import_options(body_source, {{"format", "dds"}}));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	ImportSidecar sidecar;
	Diagnostic read_error;
	TEST_EXPECT(load_import_sidecar(root + "/" + body_source + kImportSidecarSuffix, sidecar, read_error) &&
	            sidecar.options == ImportOptions({{"format", "dds"}}) && sidecar.outputs == std::vector<std::string>({"body.dds"}));
	const AssetEntry *body = output_of(view, body_source);
	TEST_EXPECT(body && body->logical_name == "body.dds" && !view.project.scan->find("body.tga"));
	// The model row now loads it (its loader reads body.dds for body.tga): nothing missing.
	for (const Diagnostic &d : view.findings.diagnostics)
		TEST_EXPECT(!(d.code() == "reference.missing" && d.asset == "models/thing.3di"));
	// An empty value goes back to the default; a key or a value no row takes is refused, nothing written.
	editor_test::handle_to_end(session, request::set_import_options(body_source, {{"format", ""}}));
	session.run_operations();
	TEST_EXPECT(load_import_sidecar(root + "/" + body_source + kImportSidecarSuffix, sidecar, read_error) && sidecar.options.empty());
	session.handle(request::set_import_options(body_source, {{"format", "bmp"}}));
	TEST_EXPECT(session.outcome().refused && !session.outcome().findings.empty() &&
	            session.outcome().findings[0].code() == "import.option");
	session.handle(request::set_import_options(body_source, {{"shine", "on"}}));
	TEST_EXPECT(session.outcome().refused);
	session.handle(request::set_import_options("terrains/isle.trn", {{"format", "tga"}}));
	TEST_EXPECT(session.outcome().refused);
	TEST_EXPECT(load_import_sidecar(root + "/" + body_source + kImportSidecarSuffix, sidecar, read_error) && sidecar.options.empty());

	// The wire: the query by an output's name.
	JsonValue args;
	TEST_EXPECT(io::json_parse("{\"path\":\"isle_c.tga\"}", args, error));
	const JsonValue answer = session.query("import_options", args, error);
	TEST_EXPECT(error.empty() && answer.get_string("source", "") == isle_source && answer.get_string("importer", "") == "image");
	const JsonValue *rows = answer.get("rows");
	const JsonValue *needs = answer.get("needs");
	TEST_EXPECT(rows && rows->array.size() == 8 && needs && needs->get("options") &&
	            needs->get("options")->get_string("format", "") == "tga24");
	if (rows && rows->array.size() == 8) {
		TEST_EXPECT(rows->array[0].get_string("key", "") == "format" && rows->array[0].get_bool("applies_now", false));
		TEST_EXPECT(rows->array[4].get_string("key", "") == "palette" && !rows->array[4].get_bool("applies_now", true));
	}
	TEST_EXPECT(answer.get("effective") && answer.get("effective")->get_string("format", "") == "tga");
	TEST_EXPECT(io::json_parse("{\"path\":\"terrains/isle.trn\"}", args, error));
	session.query("import_options", args, error);
	TEST_EXPECT(!error.empty());
	std::printf("project: imported as a TGA, its uses' needs, set_import_options, refusals, the query\n");
	return 0;
}

int test_needs_conflict() {
	// A loading screen and a particle graphic of one name: no one file serves both.
	TextureUse screen;
	screen.role = TextureRoleId::LoadingScreen;
	screen.name_written = "m01.pcx";
	screen.words = "Mission loading screen: m01.bms";
	TextureUse graphic;
	graphic.role = TextureRoleId::ParticleGraphic;
	graphic.name_written = "m01.tga";
	graphic.words = "Particle graphic: puff in fx.ptl";
	TextureImportNeeds needs = texture_import_needs({screen}, "m01.png");
	TEST_EXPECT(needs.options == ImportOptions({{"format", "pcx"}, {"size", "800x600"}}) && needs.conflicts.empty());
	needs = texture_import_needs({screen, graphic}, "m01.png");
	TEST_EXPECT(needs.options.empty() && needs.conflicts.size() == 1 &&
	            needs.conflicts[0].find("No one file serves them all") != std::string::npos);
	// A foliage map's indices: a conflict of its own.
	TextureUse foliage;
	foliage.role = TextureRoleId::TerrainFoliageMap;
	foliage.name_written = "isle_f.pcx";
	foliage.words = "Terrain foliage map: isle.trn";
	needs = texture_import_needs({foliage}, "isle_f.png");
	TEST_EXPECT(needs.options.empty() && needs.conflicts.size() == 1);
	// A name of another stem asks for it.
	needs = texture_import_needs({screen}, "screen.png");
	TEST_EXPECT(needs.options.count("name") && needs.options["name"] == "m01.pcx");
	std::printf("needs: a loading screen's PCX, two uses no one file serves, a foliage map, another stem\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_rows();
	failures += test_pieces();
	failures += test_formats();
	failures += test_needs_conflict();
	failures += test_project();
	if (failures == 0) std::printf("editor_texture_import: all passed\n");
	return failures == 0 ? 0 : 1;
}
