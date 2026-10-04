// ADR 0046 S18, a texture made from an image (import/texture_source, replace_texture) and a texture split
// (graph/rename_transaction plan_split, split_texture): the options that make a texture again as it is
// stored; a plain texture replaced by a PNG (the image an import source in art/, its record making the
// texture under its name, the plain file set aside under .replaced/), an import's output
// replaced by an image of its source's kind (the source written over) and by one of another (a new
// source, the old set aside), a name the project lacks made; the refusals; what a Replace will do said
// before anything is written (the dialog's preview: before and after, the stored form kept, what the uses
// ask, a cube map refused), a folder per set-aside, a failed record write putting everything back; a
// texture two files use split so one of them names a copy (a plain file copied; an import's output copied
// as its source); the source a paint program edits for each kind of texture.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/documents/texture_image.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/import/texture_source.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/env/env.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
	std::vector<uint8_t> out;
	for (int i = 0; i < w * h; ++i) out.insert(out.end(), {r, g, b, a});
	return out;
}

std::vector<uint8_t> tga32(const std::vector<uint8_t> &rgba, int w, int h) {
	std::vector<uint8_t> out;
	std::string why;
	tga::tga_write_rgba32(rgba.data(), uint32_t(w), uint32_t(h), out, why);
	return out;
}

std::vector<uint8_t> png(const std::vector<uint8_t> &rgba, int w, int h) { return encode_png_rgba(rgba.data(), uint32_t(w), uint32_t(h)); }

std::vector<uint8_t> read_bytes(const std::string &path) {
	std::vector<uint8_t> out;
	std::string error;
	read_file_bytes(path, out, error);
	return out;
}

// The first texel of a file as the game reads it.
std::vector<uint8_t> first_texel(const std::string &name, const std::vector<uint8_t> &bytes) {
	const std::shared_ptr<const TextureImage> image = decode_texture(name, bytes);
	if (!image || !image->loads || image->levels.empty()) return {};
	return std::vector<uint8_t>(image->levels[0].rgba.begin(), image->levels[0].rgba.begin() + 4);
}

ImportOptions record_of(const std::string &root, const std::string &source) {
	ImportSidecar record;
	Diagnostic error;
	load_import_sidecar(root + "/" + source + kImportSidecarSuffix, record, error);
	return record.options;
}

// How many set-aside folders under .replaced/ hold the file `relative`.
size_t set_aside_count(const std::string &root, const std::string &relative) {
	std::error_code ec;
	size_t count = 0;
	for (const auto &stamp : fs::directory_iterator(fs::path(root) / kReplacedFolder, ec))
		if (fs::is_regular_file(stamp.path() / relative, ec)) ++count;
	return count;
}

bool set_aside(const std::string &root, const std::string &relative) { return set_aside_count(root, relative) > 0; }

int test_options() {
	const std::vector<uint8_t> rgba = solid(4, 4, 10, 20, 30, 128);
	TEST_EXPECT(texture_reproducing_options("body.tga", tga32(rgba, 4, 4), "body.png", false) == ImportOptions({{"format", "tga"}}));
	std::vector<uint8_t> rgb;
	std::string why;
	TEST_EXPECT(tga::tga_write_rgb24(rgba.data(), 4, 4, rgb, why));
	TEST_EXPECT(texture_reproducing_options("map.tga", rgb, "map_src.tga", false) ==
	            ImportOptions({{"format", "tga24"}, {"name", "map.tga"}}));
	std::vector<uint8_t> dds;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(rgba.data(), 4, 4, dds, why));
	TEST_EXPECT(texture_reproducing_options("skin.dds", dds, "skin.png", false) == ImportOptions({{"dds", "argb"}, {"format", "dds"}}));
	TEST_EXPECT(texture_reproducing_options("isle_f.pcx", {}, "isle_f_src.pcx", true) ==
	            ImportOptions({{"format", "pcx"}, {"name", "isle_f.pcx"}, {"palette", "indices"}}));
	TEST_EXPECT(texture_reproducing_options("bump.mdt", {}, "bump.png", false) == ImportOptions({{"format", "mdt"}}));
	// A 24-bit PCX stays three planes of colour, whatever the source.
	RgbImage colours;
	colours.width = colours.height = 4;
	colours.pixels.assign(48, 77);
	std::vector<uint8_t> planes;
	TEST_EXPECT(encode_pcx_rgb(colours, planes, why));
	TEST_EXPECT(texture_reproducing_options("shot.pcx", planes, "shot.png", false) == ImportOptions({{"format", "pcx24"}}));
	TEST_EXPECT(texture_reproducing_options("shot.pcx", planes, "shot.pcx", true) == ImportOptions({{"format", "pcx24"}}));
	std::printf("options: a 32- and a 24-bit TGA, a DDS's form, an indexed and a 24-bit PCX, an MDT, the name\n");
	return 0;
}

// A cube map of six 2 x 2 A8R8G8B8 faces.
std::vector<uint8_t> cube_dds() {
	const std::vector<uint8_t> texels(16, 90);
	std::vector<uint8_t> out;
	std::string why;
	dds::dds_write_a8r8g8b8(texels.data(), 2, 2, out, why);
	const std::vector<uint8_t> face(out.begin() + 128, out.end());
	for (int i = 0; i < 5; ++i) out.insert(out.end(), face.begin(), face.end());
	out[113] = 0xFE; // caps2: the cube map and its six faces
	return out;
}

size_t folders_in(const std::string &path) {
	std::error_code ec;
	size_t count = 0;
	for (const auto &each : fs::directory_iterator(path, ec))
		if (each.is_directory()) ++count;
	return count;
}

bool holds(const std::vector<std::string> &lines, const std::string &words) {
	for (const std::string &line : lines)
		if (line.find(words) != std::string::npos) return true;
	return false;
}

// What a Replace will do, said before anything is written (preview_texture_source, the dialog's: its changes,
// the texture before and after, its stored form kept, the forms its name offers) and closed by
// cancel_texture_source; what the uses ask of the image (a colour map's exact size, a foliage map's palette
// indices); a cube map refused; each set-aside a folder of its own; a record that cannot be written putting
// back what was done.
int test_preview() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_preview"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Preview"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string outside = dir.file("mine");
	const std::vector<uint8_t> rgba = solid(8, 8, 10, 20, 30);
	std::vector<uint8_t> rgb, map_pcx;
	std::string why;
	TEST_EXPECT(tga::tga_write_rgb24(rgba.data(), 8, 8, rgb, why));
	IndexedImage8 codes;
	codes.width = codes.height = 4;
	codes.indices.assign(16, 3);
	TEST_EXPECT(encode_pcx_indexed(codes, map_pcx, why));
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/body.tga", tga32(rgba, 8, 8)) &&
	            editor_test::write_bytes(root + "/textures/map.tga", rgb) &&
	            editor_test::write_bytes(root + "/textures/isle_f.pcx", map_pcx) &&
	            editor_test::write_bytes(root + "/textures/sky_cube.dds", cube_dds()) &&
	            editor_test::write_bytes(root + "/textures/one.tga", tga32(rgba, 8, 8)) &&
	            editor_test::write_bytes(root + "/textures/two.tga", tga32(rgba, 8, 8)) &&
	            editor_test::write_bytes(root + "/textures/five.tga", tga32(rgba, 8, 8)) &&
	            editor_test::write_text(root + "/terrains/isle.trn",
	                                    "polytrn_colormap map.tga\npolytrn_detailmap grain.tga\npolytrn_foliagemap isle_f.pcx\n"
	                                    "polytrn_polydata isle.cpt\n"
	                                    "polytrn_sectorcount 1\npolytrn_sectors 0\n") &&
	            editor_test::write_bytes(outside + "/new.png", png(solid(8, 8, 0, 200, 0), 8, 8)) &&
	            editor_test::write_bytes(outside + "/codes.pcx", map_pcx));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();

	// A plain TGA: before and after in words, its form kept, the set-aside said, nothing written yet.
	TextureSourcePlan plan = plan_texture_replace(paths, *view.project.scan, "textures/body.tga", "new.png",
	                                              read_bytes(outside + "/new.png"), {});
	TEST_EXPECT(plan.ok() && plan.before_words == "8 x 8, a 32-bit TGA" && plan.after_words == "8 x 8, a 32-bit TGA" &&
	            plan.made_name == "body.tga" && first_texel("body.tga", plan.made) == std::vector<uint8_t>({0, 200, 0, 255}) &&
	            holds(plan.changes, std::string("set aside in ") + kReplacedFolder));
	// The dialog's preview through the session: a colour map's exact size asked and said, its 24-bit form kept.
	session.handle(request::preview_texture_source("textures/map.tga", outside + "/new.png"));
	const DialogsView::TextureSourcePreview &preview = view.dialogs.texture_source;
	TEST_EXPECT(preview.open && preview.refusal.empty() && preview.texture == "textures/map.tga" &&
	            preview.forms == std::vector<std::string>({"tga", "tga24"}) && preview.form == "tga24");
	TEST_EXPECT(preview.before_words == "8 x 8, a 24-bit TGA" && preview.after_words == "1024 x 1024, a 24-bit TGA" &&
	            holds(preview.changes, "resized to 1024x1024"));
	TEST_EXPECT(preview.before && preview.after);
	TEST_EXPECT(!fs::exists(root + "/art/new.png") && fs::exists(root + "/textures/map.tga"));
	const uint64_t serial = preview.serial;
	session.handle(request::cancel_texture_source());
	TEST_EXPECT(!view.dialogs.texture_source.open && view.dialogs.texture_source.serial == serial);
	// A foliage map's palette indices: an image of colours refused, an indexed one keeping its indices.
	session.handle(request::preview_texture_source("textures/isle_f.pcx", outside + "/new.png"));
	TEST_EXPECT(preview.open && preview.refusal.find("palette indices") != std::string::npos);
	plan = plan_texture_replace(paths, *view.project.scan, "textures/isle_f.pcx", "codes.pcx", map_pcx, {},
	                            texture_use_asks(view, "textures/isle_f.pcx"));
	TEST_EXPECT(plan.ok() && plan.options.count("palette") && plan.options.at("palette") == "indices");
	// A cube map: refused for a Replace and for a source of its own.
	session.handle(request::preview_texture_source("textures/sky_cube.dds", outside + "/new.png"));
	TEST_EXPECT(preview.open && preview.refusal.find("cube map") != std::string::npos);
	session.handle(request::replace_texture("textures/sky_cube.dds", outside + "/new.png"));
	TEST_EXPECT(session.outcome().refused && fs::exists(root + "/textures/sky_cube.dds"));
	TEST_EXPECT(!plan_texture_source(paths, *view.project.scan, "textures/sky_cube.dds").ok());
	session.handle(request::cancel_texture_source());

	// Two set-asides at once: a folder each, never one written over.
	const std::vector<uint8_t> image = read_bytes(outside + "/new.png");
	const TextureSourcePlan one = plan_texture_replace(paths, *view.project.scan, "textures/one.tga", "one.png", image, {});
	const TextureSourcePlan two = plan_texture_replace(paths, *view.project.scan, "textures/two.tga", "two.png", image, {});
	std::vector<Diagnostic> findings;
	TEST_EXPECT(apply_texture_source(paths, one, findings) && apply_texture_source(paths, two, findings));
	TEST_EXPECT(folders_in(root + "/" + kReplacedFolder) == 2 && set_aside_count(root, "textures/one.tga") == 1 &&
	            set_aside_count(root, "textures/two.tga") == 1);
	// A record that cannot be written (its path a folder): the source taken away again, the texture put back.
	const TextureSourcePlan five = plan_texture_replace(paths, *view.project.scan, "textures/five.tga", "five.png", image, {});
	TEST_EXPECT(five.ok() && five.source == "art/five.png");
	std::error_code ec;
	fs::create_directories(root + "/art/five.png" + kImportSidecarSuffix, ec);
	findings.clear();
	TEST_EXPECT(!apply_texture_source(paths, five, findings) && !findings.empty());
	TEST_EXPECT(!fs::exists(root + "/art/five.png") && fs::exists(root + "/textures/five.tga") &&
	            set_aside_count(root, "textures/five.tga") == 0);
	fs::remove_all(root + "/art/five.png" + kImportSidecarSuffix, ec);
	// A source written over: its bytes put back.
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const AssetEntry *made = view.project.scan->find("one.tga");
	TEST_EXPECT(made && made->imported_from == "art/one.png");
	const std::vector<uint8_t> was = read_bytes(root + "/art/one.png");
	const TextureSourcePlan again =
			plan_texture_replace(paths, *view.project.scan, "one.tga", "one.png", png(solid(8, 8, 1, 2, 3), 8, 8), {});
	TEST_EXPECT(again.ok() && again.source == "art/one.png" && again.old_source.empty());
	fs::remove(root + "/art/one.png" + kImportSidecarSuffix, ec);
	fs::create_directories(root + "/art/one.png" + kImportSidecarSuffix, ec);
	findings.clear();
	TEST_EXPECT(!apply_texture_source(paths, again, findings) && read_bytes(root + "/art/one.png") == was);
	std::printf("preview: before and after, the form kept, an exact size and palette indices asked, a cube map refused, "
	            "a folder per set-aside, a failed record putting back the source and the texture\n");
	return 0;
}

int test_replace() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_replace"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Replace"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::string outside = dir.file("mine");
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/body.tga", tga32(solid(8, 8, 200, 0, 0), 8, 8)) &&
	            editor_test::write_bytes(outside + "/skin.png", png(solid(8, 8, 0, 200, 0), 8, 8)) &&
	            editor_test::write_bytes(outside + "/skin2.png", png(solid(8, 8, 0, 0, 200), 8, 8)) &&
	            editor_test::write_bytes(outside + "/paint.tga", tga32(solid(8, 8, 50, 60, 70), 8, 8)) &&
	            editor_test::write_text(outside + "/notes.txt", "x"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();

	// A plain texture replaced by a PNG: the PNG an import source in art/, the texture its output under its
	// own name, the plain file set aside.
	editor_test::handle_to_end(session, request::replace_texture("textures/body.tga", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/skin.png");
	TEST_EXPECT(!fs::exists(root + "/textures/body.tga") && set_aside(root, "textures/body.tga"));
	TEST_EXPECT(record_of(root, "art/skin.png") == ImportOptions({{"format", "tga"}, {"name", "body.tga"}}));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({0, 200, 0, 255}));

	// An import's output replaced by an image of its source's kind: the source written over.
	editor_test::handle_to_end(session, request::replace_texture("body.tga", outside + "/skin2.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/skin.png" && set_aside(root, "art/skin.png"));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({0, 0, 200, 255}));
	// By one of another kind: a TGA source of a name of its own, the old source and its record set aside.
	editor_test::handle_to_end(session, request::replace_texture("body.tga", outside + "/paint.tga"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/paint.tga" && !fs::exists(root + "/art/skin.png") &&
	            set_aside(root, "art/skin.png.import"));
	TEST_EXPECT(record_of(root, "art/paint.tga") == ImportOptions({{"format", "tga"}, {"name", "body.tga"}}));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({50, 60, 70, 255}));

	// A name the project lacks (a field's missing texture): made under it (art/skin.png free again, its old
	// source set aside).
	editor_test::handle_to_end(session, request::replace_texture("grass.tga", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *grass = view.project.scan->find("grass.tga");
	TEST_EXPECT(grass && grass->imported_from == "art/skin.png" &&
	            record_of(root, "art/skin.png") == ImportOptions({{"format", "tga"}, {"name", "grass.tga"}}));

	// Refused, nothing written: an image the importer does not read, a file that is no texture, an option
	// no row takes.
	session.handle(request::replace_texture("grass.tga", outside + "/notes.txt"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().code() == "texture.replace");
	session.handle(request::replace_texture("project.opennova", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().refused);
	session.handle(request::replace_texture("grass.tga", outside + "/skin.png", {{"shine", "on"}}));
	TEST_EXPECT(session.outcome().refused);

	// The source a paint program edits: an output's own; a plain TGA's a copy beside in art/ under a name of
	// its own; a DDS's a PNG of its first level.
	{
		TextureSourcePlan plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "body.tga");
		TEST_EXPECT(plan.ok() && plan.source == "art/paint.tga" && plan.bytes.empty());
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/rock.tga", tga32(solid(4, 4, 1, 2, 3), 4, 4)));
		std::vector<uint8_t> dds;
		std::string why;
		const std::vector<uint8_t> blue = solid(4, 4, 0, 0, 255);
		TEST_EXPECT(dds::dds_write_a8r8g8b8(blue.data(), 4, 4, dds, why) && editor_test::write_bytes(root + "/textures/sky.dds", dds));
		editor_test::handle_to_end(session, request::rescan());
		session.run_operations();
		plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "textures/rock.tga");
		TEST_EXPECT(plan.ok() && plan.source == "art/rock_src.tga" && plan.replaced == "textures/rock.tga" &&
		            plan.options == ImportOptions({{"format", "tga"}, {"name", "rock.tga"}}));
		plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "sky.dds");
		TEST_EXPECT(plan.ok() && plan.source == "art/sky.png" && plan.options == ImportOptions({{"dds", "argb"}, {"format", "dds"}}));
		ImageSource image;
		TEST_EXPECT(decode_image_source("sky.png", plan.bytes, image, why) && image.image.pixels == blue);
	}
	std::printf("replace: a plain texture, an output by its source's kind and another, a missing name, the refusals, "
	            "the sources to edit\n");
	return 0;
}

int test_split() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_split"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Split"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	// Particle files name the graphics (a particle's TGA, read by its name alone).
	const auto particles = [](const std::string &id, const std::vector<std::string> &graphics) {
		std::string text = "[particledef]\r\n{\r\n\tid = " + id + ";\r\n";
		for (size_t i = 0; i < graphics.size(); ++i)
			text += "\tgraphic" + std::to_string(i + 1) + " = " + graphics[i] + ", blend;\r\n";
		return text + "}\r\n";
	};
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/cloud.tga", tga32(solid(4, 4, 9, 9, 9), 4, 4)) &&
	            editor_test::write_text(root + "/fx/a.ptl", particles("a", {"cloud.tga"})) &&
	            editor_test::write_text(root + "/fx/b.ptl", particles("b", {"cloud.tga", "cloud.tga"})));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	// A plain file: copied, b.ptl's two layers moved to the copy, a.ptl's left.
	editor_test::handle_to_end(session, request::split_texture("textures/cloud.tga", "cloud_2.tga", {"fx/b.ptl"}));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	TEST_EXPECT(fs::exists(root + "/textures/cloud.tga") && fs::exists(root + "/textures/cloud_2.tga"));
	std::string text, error;
	TEST_EXPECT(read_file_text(root + "/fx/b.ptl", text, error) && text == particles("b", {"cloud_2.tga", "cloud_2.tga"}));
	TEST_EXPECT(read_file_text(root + "/fx/a.ptl", text, error) && text == particles("a", {"cloud.tga"}));
	// An import's output: its source copied beside it, the copy's record naming the new file.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/haze.png", png(solid(4, 4, 7, 7, 7), 4, 4)));
	ImportSidecar record;
	record.importer = "image";
	record.version = kImageImporterVersion;
	record.options = {{"format", "tga"}};
	Diagnostic saved;
	TEST_EXPECT(save_import_sidecar(root + "/art/haze.png" + kImportSidecarSuffix, record, saved));
	TEST_EXPECT(editor_test::write_text(root + "/fx/c.ptl", particles("c", {"haze.tga"})) &&
	            editor_test::write_text(root + "/fx/d.ptl", particles("d", {"haze.tga"})));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("haze.tga") && view.project.scan->find("haze.tga")->imported_from == "art/haze.png");
	editor_test::handle_to_end(session, request::split_texture("haze.tga", "haze_2.tga", {"fx/d.ptl"}));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *copy = view.project.scan->find("haze_2.tga");
	TEST_EXPECT(copy && copy->imported_from == "art/haze_2.png" &&
	            record_of(root, "art/haze_2.png") == ImportOptions({{"format", "tga"}, {"name", "haze_2.tga"}}));
	TEST_EXPECT(read_file_text(root + "/fx/d.ptl", text, error) && text == particles("d", {"haze_2.tga"}));
	TEST_EXPECT(read_file_text(root + "/fx/c.ptl", text, error) && text == particles("c", {"haze.tga"}));
	// Refused: no referrer named, one that does not use it, a name taken.
	session.handle(request::split_texture("textures/cloud.tga", "cloud_3.tga", {}));
	session.run_operations();
	TEST_EXPECT(!fs::exists(root + "/textures/cloud_3.tga"));
	session.handle(request::split_texture("textures/cloud.tga", "cloud_3.tga", {"fx/c.ptl"}));
	session.run_operations();
	TEST_EXPECT(!fs::exists(root + "/textures/cloud_3.tga"));
	session.handle(request::split_texture("textures/cloud.tga", "haze.tga", {"fx/a.ptl"}));
	session.run_operations();
	TEST_EXPECT(read_file_text(root + "/fx/a.ptl", text, error) && text == particles("a", {"cloud.tga"}));
	// In an expansion, a name the base game serves is taken too (a copy of it would stand in for the base's
	// file for every use): refused, and the copy's name the editor offers passes over it.
	const std::vector<std::string> base_files = {"CLOUD_3.TGA"};
	const BaseNames base{&base_files};
	TEST_EXPECT(view.findings.graph != nullptr);
	if (view.findings.graph) {
		const RenamePlan in_base = plan_split(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "textures/cloud.tga",
		                                      "cloud_3.tga", {"fx/a.ptl"}, &base);
		TEST_EXPECT(!in_base.ok() && in_base.refusals.back().message.find("base game") != std::string::npos);
		TEST_EXPECT(plan_split(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "textures/cloud.tga", "cloud_3.tga",
		                       {"fx/a.ptl"})
		                    .ok());
	}
	TEST_EXPECT(free_texture_copy_name(view, "textures/cloud.tga") == "cloud_3.tga"); // cloud_2.tga is the project's
	TEST_EXPECT(free_texture_copy_name(*view.project.scan, base, "textures/cloud.tga") == "cloud_4.tga");
	std::printf("split: a plain file copied, an output's source copied, the uses moved, the refusals, a base game's name\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_options();
	failures += test_replace();
	failures += test_preview();
	failures += test_split();
	if (failures == 0) std::printf("editor_texture_replace: all passed\n");
	return failures == 0 ? 0 : 1;
}
