// The paint planner (ADR 0046 S25 B4): the maps' texels a dab reaches through the game's routing of a mission
// position into the terrain's atlas (preview/terrain_map_target: the sector grid's quadrant, the char and foliage
// maps' sample shift, the colour map over the atlas), and a stroke of the Paint tools through the session: the
// surface map's class, the foliage map's code and the colour map's colour laid on the maps' own texture documents,
// opened quietly with the mission kept active, the changed region the stroke's answer, the mission's Undo taking it
// back and its Save writing the maps in their form; a map no brush paints refused, saying why.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <editor/documents/terrain_document.h>
#include <editor/documents/texture_document.h>
#include <editor/preview/terrain_map_target.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_stroke.h>
#include <editor/session/view/session_view.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_encode.h>
#include <formats/trn/trn_io.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using io::JsonValue;

namespace {

std::string fixture(const std::string &name) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + name; }

bool contains(const std::string &text, const std::string &part) {
	if (text.find(part) != std::string::npos) return true;
	std::printf("  \"%s\" lacks \"%s\"\n", text.c_str(), part.c_str());
	return false;
}

TrnConfig tmap() {
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
	TrnConfig trn;
	std::string error;
	std::istringstream text(std::string(bytes.begin(), bytes.end()));
	load_trn(text, trn, error);
	return trn;
}

// The routing as the game takes it: the Tmap terrain's four sectors (grid 1 and 3 on its fourth row, 2 and 4 on its
// fifth, the origin -4) over a 256-texel char map (the sample shift 2) and the 1024-texel colour map.
int test_routing() {
	const TrnConfig trn = tmap();
	TEST_EXPECT(trn.origin_x == -4 && trn.sector_grid[3][3] == 1 && trn.sector_grid[3][4] == 3);
	const TerrainMapTarget surface = terrain_map_target(trn, TerrainPlane::Surface, 256, 256);
	TEST_EXPECT(surface.shifted && surface.shift == 2 && surface.step() == 4);
	int x = 0, y = 0;
	// (-256, 256): the cell (3, 3), quadrant slot 0, the atlas texel (256, 256), the map texel (64, 64).
	TEST_EXPECT(terrain_map_texel(surface, -256.0, 256.0, x, y) && x == 64 && y == 64);
	// (100, 100): the cell (4, 3), the grid's 3, slot 2 (+512 on the atlas's x): the atlas texel (612, 412).
	TEST_EXPECT(terrain_map_texel(surface, 100.0, 100.0, x, y) && x == 612 / 4 && y == 412 / 4);
	// An empty sector reads no texel.
	TEST_EXPECT(!terrain_map_texel(surface, 2000.0, 2000.0, x, y));
	const TerrainMapTarget colour = terrain_map_target(trn, TerrainPlane::Colour, 1024, 1024);
	TEST_EXPECT(!colour.shifted && colour.step() == 1 && terrain_map_texel(colour, 100.0, 100.0, x, y) && x == 612 && y == 412);
	// A one-texel click; a disc of 8 m on the char map (4-unit texels) and on the colour map (1-unit), hard-edged and
	// soft.
	TerrainBrush brush;
	brush.radius = 0.0f;
	TEST_EXPECT(terrain_map_dab(surface, -255.5, 255.5, brush, true).size() == 1);
	brush.radius = 8.0f;
	const std::vector<TerrainMapTexel> classes = terrain_map_dab(surface, -256.0, 256.0, brush, true);
	TEST_EXPECT(classes.size() >= 9 && classes.size() <= 16);
	for (const TerrainMapTexel &texel : classes) TEST_EXPECT(texel.weight == 1.0f);
	brush.falloff = 1.0f;
	const std::vector<TerrainMapTexel> colours = terrain_map_dab(colour, -256.0, 256.0, brush, false);
	TEST_EXPECT(colours.size() > 180 && colours.size() < 220);
	float least = 1.0f, most = 0.0f;
	for (const TerrainMapTexel &texel : colours) {
		least = std::min(least, texel.weight);
		most = std::max(most, texel.weight);
	}
	TEST_EXPECT(least < 0.2f && most > 0.9f);
	std::printf("routing: the game's quadrant routing, the char map's shift, the colour map's scale, the dabs\n");
	return 0;
}

int test_strokes() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_map_paint");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Paint"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	// The colour map the .trn names, 1024 square, 24-bit with the TGA 2.0 footer as the shipped ones are.
	std::vector<uint8_t> grey(size_t(1024) * 1024 * 4, 128), colour_map;
	for (size_t i = 3; i < grey.size(); i += 4) grey[i] = 255; // opaque: the 24-bit form holds no alpha
	std::string why;
	tga::TgaForm form;
	form.bits = 24;
	form.descriptor = 0;
	form.footer = true;
	TEST_EXPECT(tga::tga_write(grey.data(), 1024, 1024, form, colour_map, why));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/mnml_c.tga", colour_map));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string m = v.project.scan->find("m.bms")->relative_path;
	session.handle(request::open_document(m));

	// Surface class 5 at one texel: the char map opened quietly, its texel the class, the region the answer.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{-256.0, 256.0}};
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done() && v.documents.active == m);
	const auto *chars = dynamic_cast<const TextureDocument *>(session.document_base_for("terrain/Tmap_m.pcx"));
	TEST_EXPECT(chars && chars->paintable() && chars->plane(0).u8(64, 64) == 5 && chars->dirty());
	const std::vector<TerrainStrokeRegion> &changed = session.outcome().stroke.changed;
	TEST_EXPECT(changed.size() == 1 && changed[0].x0 == 64 && changed[0].y0 == 64 && changed[0].x1 == 65 && changed[0].y1 == 65);
	TEST_EXPECT(contains(v.activity.status, "Painted the surface map in Tmap_m.pcx"));
	// The mission's Undo takes it back; Redo lays it again; the mission's Save writes the map in its form.
	session.handle(request::undo(m));
	TEST_EXPECT(chars->plane(0).u8(64, 64) != 5 && !chars->dirty());
	session.handle(request::redo(m));
	TEST_EXPECT(chars->plane(0).u8(64, 64) == 5);
	session.handle(request::save(m));
	TEST_EXPECT(!chars->dirty());
	const std::vector<uint8_t> saved = test_io::read_file(root + "/terrain/Tmap_m.pcx");
	IndexedImage8 image;
	TEST_EXPECT(decode_pcx_indexed(saved.data(), saved.size(), image, why) && image.indices[size_t(64) * 256 + 64] == 5);
	// The foliage map's code, a disc of 6 m: every texel it reaches the code.
	ask.plane = TerrainPlane::Foliage;
	ask.brush.tool = TerrainTool::Foliage;
	ask.brush.foliage = 254;
	ask.brush.radius = 6.0f;
	ask.brush_named = kBrushTool | kBrushFoliage | kBrushRadius;
	session.handle(request::terrain_stroke(m, ask));
	const auto *codes = dynamic_cast<const TextureDocument *>(session.document_base_for("terrain/Tmap_f.pcx"));
	TEST_EXPECT(session.outcome().done() && codes && codes->plane(0).u8(64, 64) == 254 && codes->plane(0).u8(63, 63) == 254);
	// The colour map's colour at full strength at one texel: the atlas texel (256, 256).
	ask.plane = TerrainPlane::Colour;
	ask.brush.tool = TerrainTool::Colour;
	ask.brush.colour = 0xFF2000;
	ask.brush.strength = 1.0f;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushColour | kBrushStrength | kBrushRadius;
	session.handle(request::terrain_stroke(m, ask));
	const auto *colours = dynamic_cast<const TextureDocument *>(session.document_base_for("terrain/mnml_c.tga"));
	TEST_EXPECT(session.outcome().done() && colours && colours->plane(0).format() == RasterFormat::Rgb8);
	const uint8_t *painted = colours->plane(0).texel(256, 256);
	TEST_EXPECT(painted[0] == 255 && painted[1] == 32 && painted[2] == 0 && colours->plane(0).texel(257, 256)[0] == 128);
	// A layer the blend map does not hold, and a map no brush paints: refused, saying why.
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/mnml_d1.tga", colour_map));
	editor_test::handle_to_end(session, request::rescan());
	ask.plane = TerrainPlane::Blend;
	ask.brush.tool = TerrainTool::Detail;
	ask.brush.layer = 3;
	ask.brush_named = kBrushTool | kBrushLayer;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "three detail layers"));
	std::vector<uint8_t> rle = colour_map;
	rle[2] = 10;
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/mnml_d1.tga", rle));
	session.handle(request::close_document("terrain/mnml_d1.tga"));
	editor_test::handle_to_end(session, request::rescan());
	ask.brush.layer = 1;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "no brush paints it"));
	std::printf("strokes: a class, a code and a colour laid where the game reads them, undone, saved; refusals\n");
	return 0;
}

// An imported terrain (S20): its surface map's source, a palette PNG whose indices are the classes, takes the stroke
// in the output's place as an index (the import reads it so), opened quietly and written again as a palette PNG.
int test_imported_source() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_map_paint_import");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Isle"));
	const SessionView &v = session.view();
	const std::string images = dir.file("images");
	// An island of 8-bit grey heights, a colour map, and a surface map of grass (class 2) as a palette PNG.
	constexpr int kSide = 1024, kMap = 512;
	std::vector<uint8_t> heights(size_t(kSide) * kSide), colours(size_t(kSide) * kSide * 4, 120), classes(size_t(kMap) * kMap, 2);
	for (int y = 0; y < kSide; ++y)
		for (int x = 0; x < kSide; ++x)
			heights[size_t(y) * kSide + x] = uint8_t(std::max(0.0, 200.0 * (1.0 - std::hypot(x - 512.0, y - 512.0) / 400.0)));
	std::vector<uint8_t> ramp;
	for (int i = 0; i < 256; ++i) ramp.insert(ramp.end(), 3, uint8_t(i));
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", png::encode_png_gray(heights.data(), kSide, kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", png::encode_png_rgba(colours.data(), kSide, kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/surface.png", png::encode_png_indexed(classes.data(), kMap, kMap, ramp.data(), 256)));
	// A detail blend map's weights (the .tset's blendmap key, S25 B4): the first layer full everywhere.
	std::vector<uint8_t> weights;
	for (int i = 0; i < kMap * kMap; ++i) weights.insert(weights.end(), {255, 0, 0, 255});
	TEST_EXPECT(editor_test::write_bytes(images + "/blend.png", png::encode_png_rgba(weights.data(), kMap, kMap)));
	ActionOutcome outcome = editor_test::handle_to_end(session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"surface", images + "/surface.png"},
	         {"blendmap", images + "/blend.png"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused);
	editor_test::handle_to_end(session, request::create_file("isle.env", "environment"));
	editor_test::set_missions(session, true);
	outcome = editor_test::handle_to_end(session,
	        request::create_file("isle1.bms", "mission", {{"terrain", "isle.trn"}, {"environment", "isle.env"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	const AssetEntry *entry = v.project.scan->find("isle1.bms");
	TEST_EXPECT(entry != nullptr);
	if (!entry) return 1;
	const std::string m = entry->relative_path;
	session.handle(request::open_document(m));
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{8.0, 8.0}};
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && session.outcome().stroke.changed.size() == 1);
	const std::string source = session.outcome().stroke.changed.front().path;
	TEST_EXPECT(contains(source, "isle_surface.png"));
	const auto *painted = dynamic_cast<const TextureDocument *>(session.document_base_for(source));
	TEST_EXPECT(painted && painted->plane(0).format() == RasterFormat::U8);
	size_t fives = 0;
	for (int y = 0; y < kMap; ++y)
		for (int x = 0; x < kMap; ++x) fives += painted->plane(0).u8(x, y) == 5 ? 1 : 0;
	TEST_EXPECT(fives == 1);
	// Fill (S25): the click's texel and every texel joined to it that holds its class take the brush's; the class 5
	// texel, holding another, stays (the fill goes round it). A fill on that texel takes it alone. Undo takes each back.
	ask.brush.surface = 7;
	ask.brush.fill = true;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius | kBrushFill;
	ask.points = {{100.0, 100.0}, {120.0, 120.0}};
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done() && contains(v.activity.status, "Filled the surface map in isle_surface.png: 262143 texels"));
	size_t sevens = 0;
	for (int y = 0; y < kMap; ++y)
		for (int x = 0; x < kMap; ++x) sevens += painted->plane(0).u8(x, y) == 7 ? 1 : 0;
	TEST_EXPECT(sevens == size_t(kMap) * kMap - 1);
	ask.brush.surface = 9;
	ask.points = {{8.0, 8.0}};
	session.handle(request::terrain_stroke(m, ask));
	const std::vector<TerrainStrokeRegion> &filled = session.outcome().stroke.changed;
	TEST_EXPECT(session.outcome().done() && filled.size() == 1 && filled[0].x1 - filled[0].x0 == 1 && filled[0].y1 - filled[0].y0 == 1);
	session.handle(request::undo(m));
	session.handle(request::undo(m));
	size_t twos = 0;
	for (int y = 0; y < kMap; ++y)
		for (int x = 0; x < kMap; ++x) twos += painted->plane(0).u8(x, y) == 2 ? 1 : 0;
	TEST_EXPECT(twos == size_t(kMap) * kMap - 1);
	// A gesture's later samples lay nothing: a drag (the wire's too) fills only where it was pressed.
	ask.brush.surface = 11;
	ask.points = {{100.0, 100.0}};
	ask.gesture = 0;
	ask.end = false;
	session.handle(request::terrain_stroke(m, ask));
	const uint64_t held = session.outcome().gesture;
	TEST_EXPECT(session.outcome().done() && held != 0);
	ask.gesture = held;
	ask.points = {{8.0, 8.0}};
	ask.end = true;
	session.handle(request::terrain_stroke(m, ask));
	size_t elevens = 0;
	for (int y = 0; y < kMap; ++y)
		for (int x = 0; x < kMap; ++x) elevens += painted->plane(0).u8(x, y) == 11 ? 1 : 0;
	TEST_EXPECT(session.outcome().done() && elevens == size_t(kMap) * kMap - 1);
	session.handle(request::undo(m));
	ask.gesture = 0;
	ask.brush.fill = false;
	// Written again in its form: a palette PNG whose indices the import reads as the classes.
	const SerializeResult saved = painted->serialize();
	TEST_EXPECT(saved.text.size() > 26 && uint8_t(saved.text[25]) == 3);
	// The blend map made from the set's blendmap, 32-bit at its own size; a Detail stroke paints its source's layer.
	const AssetEntry *made = v.project.scan->find("isle_d1.tga");
	TEST_EXPECT(made != nullptr);
	if (!made) return 1;
	const std::vector<uint8_t> d1 = test_io::read_file(v.project.root + "/" + made->relative_path);
	std::vector<uint8_t> expected;
	std::string why;
	TEST_EXPECT(tga::tga_write_rgba32(weights.data(), kMap, kMap, expected, why) && d1 == expected);
	ask.plane = TerrainPlane::Blend;
	ask.brush.tool = TerrainTool::Detail;
	ask.brush.layer = 1;
	ask.brush.strength = 1.0f;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushLayer | kBrushStrength | kBrushRadius;
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && session.outcome().stroke.changed.size() == 1 &&
	            contains(session.outcome().stroke.changed.front().path, "isle_blendmap.png"));
	const auto *blended = dynamic_cast<const TextureDocument *>(session.document_base_for(session.outcome().stroke.changed.front().path));
	size_t layered = 0;
	for (int y = 0; blended && y < kMap; ++y)
		for (int x = 0; x < kMap; ++x) {
			const uint8_t *texel = blended->plane(0).texel(x, y);
			// The layer's channel full, the others empty, the alpha (which no splat reads) as it was.
			layered += texel[0] == 0 && texel[1] == 255 && texel[2] == 0 && texel[3] == 255 ? 1 : 0;
		}
	TEST_EXPECT(layered == 1);
	// Add (S25 B4): the set names no foliagemap, so the terrain has none: Add makes isle_foliagemap.png beside the set
	// (grey, every level 0), writes the key, and the import made again makes isle_f.pcx and names it in the .trn; a
	// stroke then paints the source in its place.
	TEST_EXPECT(v.project.scan->find("isle_f.pcx") == nullptr);
	TerrainStrokeAsk add;
	add.plane = TerrainPlane::Foliage;
	add.add = true;
	outcome = editor_test::handle_to_end(session, request::terrain_stroke(m, add));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused && v.project.scan->find("isle_foliagemap.png") != nullptr &&
	            v.project.scan->find("isle_f.pcx") != nullptr);
	ask.plane = TerrainPlane::Foliage;
	ask.brush.tool = TerrainTool::Foliage;
	ask.brush.foliage = 3;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushFoliage | kBrushRadius;
	ask.points = {{8.0, 8.0}};
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && session.outcome().stroke.changed.size() == 1 &&
	            contains(session.outcome().stroke.changed.front().path, "isle_foliagemap.png"));
	std::printf("imported: the surface map's palette source painted with a class, written again as a palette PNG; the "
	            "blendmap's source painted with a layer; a foliage map added to the set\n");
	return 0;
}

// Add (S25 B4) on an authored terrain naming no surface map and no foliage map: each made beside the .trn in the game's
// form (the char map 8-bit, every texel class 1; the foliage map every texel 0) and named by its field, an edit from
// the mission's view that its Undo takes back; a stroke then paints it. A second Add, the colour map's and an empty
// stroke refused.
int test_add_maps() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_map_add");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Add"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const std::vector<uint8_t> trn_bytes = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
	std::string trn_text, line;
	std::istringstream lines(std::string(trn_bytes.begin(), trn_bytes.end()));
	while (std::getline(lines, line))
		if (line.rfind("polytrn_charmap", 0) != 0 && line.rfind("polytrn_foliagemap", 0) != 0) trn_text += line + "\n";
	TEST_EXPECT(editor_test::write_text(root + "/terrain/Tmap.trn", trn_text));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt"))));
	std::vector<uint8_t> grey(size_t(1024) * 1024 * 4, 128), colour_map;
	for (size_t i = 3; i < grey.size(); i += 4) grey[i] = 255; // a 24-bit TGA holds no alpha
	std::string why;
	TEST_EXPECT(tga::tga_write_rgb24(grey.data(), 1024, 1024, colour_map, why));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/mnml_c.tga", colour_map));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string m = v.project.scan->find("m.bms")->relative_path;
	session.handle(request::open_document(m));

	// A stroke on a map the terrain names none of is refused, saying Add makes one.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{-256.0, 256.0}};
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "names no surface map"));
	TerrainStrokeAsk add;
	add.plane = TerrainPlane::Surface;
	add.add = true;
	session.handle(request::terrain_stroke(m, add));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && v.documents.active == m);
	const std::vector<uint8_t> made = test_io::read_file(root + "/terrain/Tmap_m.pcx");
	IndexedImage8 image;
	TEST_EXPECT(decode_pcx_indexed(made.data(), made.size(), image, why) && image.width == 512 && image.height == 512 &&
	            std::all_of(image.indices.begin(), image.indices.end(), [](uint8_t i) { return i == 1; }) &&
	            image.palette[2][1] == 210);
	const auto *terrain = dynamic_cast<const TerrainDocument *>(session.document_base_for("terrain/Tmap.trn"));
	TEST_EXPECT(terrain && terrain->dirty() && terrain->config() && terrain->config()->charmap == "Tmap_m.pcx");
	// A second Add is refused: the terrain has one.
	session.handle(request::terrain_stroke(m, add));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "already"));
	// The stroke paints it now: the 512-texel map's texel (128, 128) under the atlas texel (256, 256).
	session.handle(request::terrain_stroke(m, ask));
	const auto *chars = dynamic_cast<const TextureDocument *>(session.document_base_for("terrain/Tmap_m.pcx"));
	TEST_EXPECT(session.outcome().done() && chars && chars->plane(0).u8(128, 128) == 5);
	// The foliage map: every texel 0, 256 square.
	add.plane = TerrainPlane::Foliage;
	session.handle(request::terrain_stroke(m, add));
	TEST_EXPECT(session.outcome().done() && terrain->config()->foliagemap == "Tmap_f.pcx");
	const std::vector<uint8_t> codes = test_io::read_file(root + "/terrain/Tmap_f.pcx");
	TEST_EXPECT(decode_pcx_indexed(codes.data(), codes.size(), image, why) && image.width == 256 &&
	            std::all_of(image.indices.begin(), image.indices.end(), [](uint8_t i) { return i == 0; }));
	// The colour map is every terrain's own; an add names no points.
	add.plane = TerrainPlane::Colour;
	session.handle(request::terrain_stroke(m, add));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "Add makes a surface"));
	TerrainStrokeAsk read;
	std::string error;
	io::JsonValue wire = terrain_stroke_to_json(add);
	TEST_EXPECT(terrain_stroke_from_json(wire, read, error) && read == add);
	JsonValue points = JsonValue::make_array();
	JsonValue point = JsonValue::make_array();
	point.push(io::json_number(0.0));
	point.push(io::json_number(0.0));
	points.push(point);
	wire.set("points", points);
	TEST_EXPECT(!terrain_stroke_from_json(wire, read, error) && contains(error, "add names no points"));
	// The mission's Undo takes the foliage map's name back (its file stays, Undo file's to take).
	session.handle(request::undo(m));
	TEST_EXPECT(terrain->config()->foliagemap.empty() && terrain->config()->charmap == "Tmap_m.pcx");
	std::printf("add: a surface map and a foliage map made beside an authored .trn and named in it; refusals\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_routing() + test_strokes() + test_imported_source() + test_add_maps();
	if (failures == 0) std::printf("editor_terrain_map_paint: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
