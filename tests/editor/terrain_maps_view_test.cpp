// The maps and the tiles in the mission's 3D view as they are made (ADR 0046 S25, the device patches' portable half):
// a colour stroke, its end and its undo, a surface stroke, a tile stamp and its undo each an Update of the picture
// (its builds stand, nothing read again whole), the view's device patches holding what changed (the colour map's
// texels as the device's loader takes them, the surface map's stamp, the placed tiles the game reads now) and the
// files they account for at their stamps; the picture's files take those stamps, so the device's read of them does not
// find them moved. A foliage stroke is an Update too: its patch the codes and the plane's rects whose foliage reads them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/preview/mission_ground_overlay.h>
#include <editor/preview/mission_map.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/terrain_maps_patch.h>
#include <editor/preview/terrain_patch.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/tile_tools.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string fixture(const std::string &name) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + name; }

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

const MapsDevicePatch *last_of(const MissionViewport &view, MapsDevicePatch::Kind kind) {
	const MapsDevicePatch *found = nullptr;
	for (const MapsDevicePatch &patch : view.maps_patches())
		if (patch.kind == kind) found = &patch;
	return found;
}

bool accounts_for(const MapsDevicePatch &patch, const std::string &name) {
	for (const FileStamp &file : patch.files)
		if (strutil::iequals(file.name, name) && file.stamp != 0) return true;
	return false;
}

int test_view() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_maps_view");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Maps"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	// The colour map the .trn names, 1024 square, 24-bit with the TGA 2.0 footer as the shipped ones are.
	std::vector<uint8_t> grey(size_t(1024) * 1024 * 4, 128), colour_map;
	for (size_t i = 3; i < grey.size(); i += 4) grey[i] = 255; // a 24-bit TGA holds no alpha
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
	devices.sync(session);
	const auto *mission = static_cast<const MissionViewport *>(session.viewports().find(m, ViewportKind::Mission));
	TEST_EXPECT(mission && devices.last(m, ViewportKind::Mission) == ViewportAction::Rebuild);
	if (!mission) return 1;
	// The device read the maps and the tiles' names (the files a game load of the terrain reads), so a move of one the
	// picture does not take is a Rebuild.
	editor_test::FakeDevice *device = devices.held(m, ViewportKind::Mission);
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->reads = {"mnml_c.tga", "Tmap_m.pcx", "Tmap_f.pcx", "m.til"};
	devices.sync(session);
	const uint64_t builds = mission->builds();
	// What the device took since `from`: Updates alone, one at least (Keep left out), and nothing built again.
	const auto updated_since = [&](size_t from) {
		const std::vector<ViewportAction> taken = device->since(from);
		return !taken.empty() && mission->builds() == builds &&
		       std::all_of(taken.begin(), taken.end(), [](ViewportAction a) { return a == ViewportAction::Update; });
	};
	size_t mark = device->taken.size();

	// The colour map's colour at full strength over a disc, its stroke open: an Update, the patch the painted texels.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Colour;
	ask.brush.tool = TerrainTool::Colour;
	ask.brush.colour = 0xFF2000;
	ask.brush.strength = 1.0f;
	ask.brush.radius = 4.0f;
	ask.brush_named = kBrushTool | kBrushColour | kBrushStrength | kBrushRadius;
	ask.points = {{-256.0, 256.0}};
	ask.gesture = 5151;
	ask.end = false;
	mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	const MapsDevicePatch *colour = last_of(*mission, MapsDevicePatch::Kind::Colour);
	TEST_EXPECT(colour != nullptr);
	if (colour) {
		TEST_EXPECT(colour->width == 1024 && colour->height == 1024 && colour->w > 0 && colour->h > 0 &&
		            colour->rgba.size() == size_t(colour->w) * size_t(colour->h) * 4 && accounts_for(*colour, "mnml_c.tga"));
		bool painted = false;
		for (size_t i = 0; i + 3 < colour->rgba.size(); i += 4)
			painted = painted || (colour->rgba[i] == 255 && colour->rgba[i + 1] == 32 && colour->rgba[i + 2] == 0 &&
			                      colour->rgba[i + 3] == 255);
		TEST_EXPECT(painted);
	}
	// Its end: still an Update (the device patched the colour map in place: no picture made again).
	ask.points = {{-252.0, 256.0}};
	ask.end = true;
	mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	// The undo: an Update whose patch holds the map as it was.
	mark = device->taken.size();
	session.handle(request::undo(m));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	colour = last_of(*mission, MapsDevicePatch::Kind::Colour);
	TEST_EXPECT(colour && !colour->rgba.empty() && colour->rgba[0] == 128 && colour->rgba[3] == 255);

	// A surface stroke: an Update, its patch the surface map's stamp alone (the device draws nothing of it).
	ask = TerrainStrokeAsk();
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{-256.0, 256.0}};
	mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	const MapsDevicePatch *surface = last_of(*mission, MapsDevicePatch::Kind::Surface);
	TEST_EXPECT(surface && surface->rgba.empty() && accounts_for(*surface, "Tmap_m.pcx"));
	// The ground overlays take a stroke's region in place (the review's m5), and what they show is what a whole
	// rebuild over the same ground shows: its texels and its legend's shares.
	const auto same_overlay = [&](const MissionOverlayImage &a, const MissionOverlayImage &b) {
		if (a.kind != b.kind || a.width != b.width || a.height != b.height || a.rgba != b.rgba || a.legend.size() != b.legend.size())
			return false;
		for (size_t i = 0; i < a.legend.size(); ++i)
			if (a.legend[i].key != b.legend[i].key || std::abs(a.legend[i].share - b.legend[i].share) > 1e-12) return false;
		return true;
	};
	for (const char *kind : {"surfaces", "foliage"}) {
		session.handle(request::set_viewport(m, std::string(R"({"kind": "mission", "options": {"overlay": ")") + kind + "\"}}"));
		devices.sync(session);
		const uint64_t made = mission->overlay_serial();
		const bool surfaces = std::string(kind) == "surfaces";
		TerrainStrokeAsk dab;
		dab.plane = surfaces ? TerrainPlane::Surface : TerrainPlane::Foliage;
		dab.brush.tool = surfaces ? TerrainTool::Surface : TerrainTool::Foliage;
		dab.brush.surface = 9;
		dab.brush.foliage = 253;
		dab.brush.radius = 6.0f;
		dab.brush_named = kBrushTool | kBrushSurface | kBrushFoliage | kBrushRadius;
		dab.points = {{-240.0, 240.0}};
		session.handle(request::terrain_stroke(m, dab));
		TEST_EXPECT(session.outcome().done());
		devices.sync(session);
		devices.sync(session);
		const MissionOverlayImage whole = mission_ground_overlay(mission->ground_reader(), mission->overlay().kind);
		const bool same = mission->overlay_serial() > made && same_overlay(mission->overlay(), whole);
		if (!same) std::fprintf(stderr, "  the %s overlay patched differs from it made whole\n", kind);
		TEST_EXPECT(same);
	}
	session.handle(request::set_viewport(m, R"({"kind": "mission", "options": {"overlay": "none"}})"));
	devices.sync(session);

	// A stamp: the mission's .til made and opened, the tile laid; an Update whose patch is the placement the game reads.
	std::string error;
	JsonValue args = JsonValue::make_object();
	args.set("op", io::json_string("tiles"));
	args.set("path", io::json_string(m));
	const JsonValue tiles = session.query("terrain", args, error);
	const std::string til = tiles.get_string("path", "");
	TEST_EXPECT(error.empty() && contains(til, "m.til"));
	// The ground read once before the stamp (a hover's), so the stamp's tiles go into it in place.
	TEST_EXPECT(mission->followed_ground(v).terrain() && mission->ground_reader().tiles() == 0);
	const int reads = mission->ground_reader().reads();
	// The 2D map's ground too: a tile edit retiles it, never a read of the whole terrain.
	session.viewports().attach(m, ViewportKind::Map);
	const auto *map = static_cast<const MissionMapViewport *>(session.viewports().follow_one(v, m, ViewportKind::Map));
	TEST_EXPECT(map && map->followed_terrain(v).terrain());
	const int map_reads = map ? map->followed_terrain(v).reads() : 0;
	mark = device->taken.size();
	session.handle(request::tile_edit(m, til, plan_tile_stamp(5, 0, -200.0, 200.0, false)));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	const MapsDevicePatch *placed = last_of(*mission, MapsDevicePatch::Kind::Tiles);
	TEST_EXPECT(placed && placed->tiles.entries.size() == 1 && accounts_for(*placed, "m.til"));
	if (placed && placed->tiles.entries.size() == 1) TEST_EXPECT(placed->tiles.entries[0].tile_index == 5);
	// The ground holds the stamped tile at once (in place, not the terrain read again), the 2D map's alike.
	TEST_EXPECT(mission->followed_ground(v).tiles() == 1 && mission->ground_reader().reads() == reads);
	TEST_EXPECT(map && map->followed_terrain(v).tiles() == 1 && map->followed_terrain(v).reads() == map_reads);
	// The stamp undone: an Update whose placement holds none.
	mark = device->taken.size();
	session.handle(request::undo(m));
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	placed = last_of(*mission, MapsDevicePatch::Kind::Tiles);
	TEST_EXPECT(placed && placed->tiles.entries.empty());

	// A foliage stroke: an Update, its patch the codes of the foliage map the ground read and the plane's rects whose
	// foliage reads them (the sector at grid (3, 3) drawing quadrant 1, around (-256, 256): the plane's z is -y).
	const uint64_t built = mission->builds();
	ask = TerrainStrokeAsk();
	ask.plane = TerrainPlane::Foliage;
	ask.brush.tool = TerrainTool::Foliage;
	ask.brush.foliage = 254;
	ask.brush.radius = 2.0f;
	ask.brush_named = kBrushTool | kBrushFoliage | kBrushRadius;
	ask.points = {{-256.0, 256.0}};
	ask.gesture = 5252;
	ask.end = true;
	mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(updated_since(mark) && mission->builds() == built);
	const MapsDevicePatch *foliage = last_of(*mission, MapsDevicePatch::Kind::Foliage);
	TEST_EXPECT(foliage != nullptr);
	if (foliage) {
		TEST_EXPECT(foliage->width == 256 && foliage->indices.size() == size_t(foliage->w) * size_t(foliage->h) &&
		            accounts_for(*foliage, "Tmap_f.pcx"));
		TEST_EXPECT(std::count(foliage->indices.begin(), foliage->indices.end(), uint8_t(254)) > 0);
		bool over = false;
		for (const opennova::terrain::TerrainWorldRect &rect : foliage->world)
			over = over || (rect.x0 <= -256 && -256 < rect.x1 && rect.z0 <= -256 && -256 < rect.z1);
		TEST_EXPECT(!foliage->world.empty() && over);
	}
	std::printf("view: a colour stroke, its end, its undo, a surface stroke, a stamp and a foliage stroke each an Update "
	            "with its patch\n");
	return 0;
}

// An imported island (S20's import of a terrain set's images: 8-bit grey heights, a colour map, a surface map of grass
// (class 2) as a palette PNG, a foliage map of code 0 as a grey PNG) and a mission on it: the mission's path ("" where
// a step failed).
std::string isle_project(ProjectSession &session, editor_test::TempProjectDir &dir) {
	const SessionView &v = session.view();
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Isle"));
	const std::string images = dir.file("images");
	constexpr int kSide = 1024, kMap = 512;
	std::vector<uint8_t> heights(size_t(kSide) * kSide), colours(size_t(kSide) * kSide * 4, 120), classes(size_t(kMap) * kMap, 2),
	        codes(size_t(kMap) * kMap, 0);
	for (int y = 0; y < kSide; ++y)
		for (int x = 0; x < kSide; ++x)
			heights[size_t(y) * kSide + x] = uint8_t(std::max(0.0, 200.0 * (1.0 - std::hypot(x - 512.0, y - 512.0) / 400.0)));
	std::vector<uint8_t> ramp;
	for (int i = 0; i < 256; ++i) ramp.insert(ramp.end(), 3, uint8_t(i));
	bool written = editor_test::write_bytes(images + "/height.png", png::encode_png_gray(heights.data(), kSide, kSide));
	written = editor_test::write_bytes(images + "/colour.png", png::encode_png_rgba(colours.data(), kSide, kSide)) && written;
	written = editor_test::write_bytes(images + "/surface.png", png::encode_png_indexed(classes.data(), kMap, kMap, ramp.data(), 256)) &&
	          written;
	written = editor_test::write_bytes(images + "/foliage.png", png::encode_png_gray(codes.data(), kMap, kMap)) && written;
	ActionOutcome outcome = editor_test::handle_to_end(session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"surface", images + "/surface.png"},
	         {"foliagemap", images + "/foliage.png"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	if (!written || outcome.refused) return std::string();
	editor_test::handle_to_end(session, request::create_file("isle.env", "environment"));
	editor_test::set_missions(session, true);
	outcome = editor_test::handle_to_end(session,
	        request::create_file("isle1.bms", "mission", {{"terrain", "isle.trn"}, {"environment", "isle.env"}}));
	const AssetEntry *entry = v.project.scan->find("isle1.bms");
	return entry ? entry->relative_path : std::string();
}

// An imported terrain (S25, the review's M5): its surface and foliage maps are painted in their source images, which
// stand for the maps the import makes. A surface stroke on the source reaches the view's ground (its class there, an
// Update), and a foliage stroke is an Update whose device patch holds the codes, nothing built again at its end.
int test_imported() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_maps_view_import");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const SessionView &v = session.view();
	const std::string m = isle_project(session, dir);
	TEST_EXPECT(!m.empty());
	if (m.empty()) return 1;
	session.handle(request::open_document(m));
	devices.sync(session);
	const auto *mission = static_cast<const MissionViewport *>(session.viewports().find(m, ViewportKind::Mission));
	editor_test::FakeDevice *device = devices.held(m, ViewportKind::Mission);
	TEST_EXPECT(mission && device);
	if (!mission || !device) return 1;
	device->reads = {"isle_c.tga", "isle_m.pcx", "isle_f.pcx", "isle1.til"};
	devices.sync(session);
	TEST_EXPECT(mission->followed_ground(v).terrain());
	const uint64_t builds = mission->builds();
	const auto updated_since = [&](size_t from) {
		const std::vector<ViewportAction> taken = device->since(from);
		return !taken.empty() && mission->builds() == builds &&
		       std::all_of(taken.begin(), taken.end(), [](ViewportAction a) { return a == ViewportAction::Update; });
	};
	// A surface stroke on the source: the view's ground reads its class there, an Update.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{8.0, 8.0}};
	size_t mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && contains(session.outcome().stroke.changed.front().path, "isle_surface.png"));
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(updated_since(mark) && mission->followed_ground(v).terrain_at(8.0, 8.0, 0.0).surface == 5);
	// A foliage stroke on the source: an Update, its device patch the codes, nothing built again at its end.
	ask.plane = TerrainPlane::Foliage;
	ask.brush.tool = TerrainTool::Foliage;
	ask.brush.foliage = 3;
	ask.brush_named = kBrushTool | kBrushFoliage | kBrushRadius;
	ask.gesture = 7373;
	ask.end = true;
	mark = device->taken.size();
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	const MapsDevicePatch *foliage = last_of(*mission, MapsDevicePatch::Kind::Foliage);
	TEST_EXPECT(foliage && std::count(foliage->indices.begin(), foliage->indices.end(), uint8_t(3)) == 1 &&
	            !foliage->world.empty());
	// A colour stroke on the source, whose texels' alpha is 120: the device patch shows them opaque, as the import's
	// 24-bit colour map holds them (the review's NIT).
	ask.plane = TerrainPlane::Colour;
	ask.brush.tool = TerrainTool::Colour;
	ask.brush.colour = 0x30A050;
	ask.brush.strength = 1.0f;
	ask.brush.radius = 4.0f;
	ask.brush_named = kBrushTool | kBrushColour | kBrushStrength | kBrushRadius;
	ask.gesture = 7474;
	session.handle(request::terrain_stroke(m, ask));
	for (const Diagnostic &d : session.outcome().findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(session.outcome().done() && contains(session.outcome().stroke.changed.front().path, ".png"));
	devices.sync(session);
	devices.sync(session);
	const MapsDevicePatch *colour = last_of(*mission, MapsDevicePatch::Kind::Colour);
	bool opaque = colour && !colour->rgba.empty(), painted = false;
	for (size_t i = 0; colour && i + 3 < colour->rgba.size(); i += 4) {
		opaque = opaque && colour->rgba[i + 3] == 255;
		painted = painted || (colour->rgba[i] == 0x30 && colour->rgba[i + 1] == 0xA0 && colour->rgba[i + 2] == 0x50);
	}
	TEST_EXPECT(opaque && painted);
	std::printf("imported: a surface source's stroke reaches the ground, a foliage source's is an Update with its codes, a "
	            "colour source's texels shown opaque\n");
	return 0;
}

// The ground at (x, y) as the terrain query's sample reads it for the view `path`: its surface class (-1: none) and
// foliage code, and whose ground answered.
struct Sampled {
	int surface = -1;
	int code = -1;
	std::string from;
};
Sampled sample(ProjectSession &session, const std::string &path, double x, double y) {
	JsonValue args = JsonValue::make_object();
	args.set("path", io::json_string(path));
	args.set("op", io::json_string("sample"));
	args.set("x", io::json_number(x));
	args.set("y", io::json_number(y));
	std::string error;
	const JsonValue answer = session.query("terrain", args, error);
	Sampled out;
	if (!error.empty()) std::fprintf(stderr, "  sample: %s\n", error.c_str());
	const JsonValue *ground = answer.get("ground");
	if (!ground) return out;
	out.surface = int(ground->get_number("surface", -1.0));
	if (const JsonValue *foliage = ground->get("foliage"); foliage && foliage->is_object())
		out.code = int(foliage->get_number("code", -1.0));
	out.from = answer.get_string("from", "");
	return out;
}

// The painting feedback (2026-10-13): the terrain query's sample reads unsaved paint. On an imported terrain the maps
// are painted in the set's source images, and the import's outputs move only as the set is saved and imported again: a
// sample answers from the view's own ground, which holds the paint (patched in place, or laid over a ground read whole
// after the stroke: a view first followed once the stroke is in), never from the outputs read again.
int test_sample_unsaved() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_maps_view_sample");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const SessionView &v = session.view();
	const std::string m = isle_project(session, dir);
	TEST_EXPECT(!m.empty());
	if (m.empty()) return 1;
	session.handle(request::open_document(m));
	// A class painted and a code painted, before any save (no Shell draws the view: its first follow, the sample's, comes
	// after them).
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 5;
	ask.brush.foliage = 3;
	ask.brush.radius = 0.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushFoliage | kBrushRadius;
	ask.points = {{8.0, 8.0}};
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done() && contains(session.outcome().stroke.changed.front().path, "isle_surface.png"));
	ask.plane = TerrainPlane::Foliage;
	ask.brush.tool = TerrainTool::Foliage;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done() && contains(session.outcome().stroke.changed.front().path, "isle_foliage"));
	const Sampled painted = sample(session, m, 8.0, 8.0);
	if (painted.surface != 5 || painted.code != 3)
		std::fprintf(stderr, "  sampled surface %d, code %d from %s\n", painted.surface, painted.code, painted.from.c_str());
	TEST_EXPECT(painted.surface == 5 && painted.code == 3 && painted.from == "view");
	TEST_EXPECT(sample(session, m, 40.0, 40.0).surface == 2);
	// The outputs stand as imported: a ground read over the project's files alone reads the old class, and the open
	// sources laid over it (the query's rule where no view holds a ground) read the paint.
	const auto *mission = static_cast<const MissionViewport *>(session.viewports().find(m, ViewportKind::Mission));
	TEST_EXPECT(mission != nullptr);
	if (!mission) return 1;
	MissionGround read;
	read.follow(v.findings.assets, v.findings.assets->generation(), mission->scene().header(), "isle1");
	TEST_EXPECT(read.terrain() && read.terrain_at(8.0, 8.0, 0.0).surface == 2);
	TEST_EXPECT(lay_open_sources(v, m, read) && read.terrain_at(8.0, 8.0, 0.0).surface == 5 &&
	            read.terrain_at(8.0, 8.0, 0.0).foliage_code == 3);
	// Undone, the samples read the class as imported again.
	session.handle(request::undo(m));
	session.handle(request::undo(m));
	const Sampled undone = sample(session, m, 8.0, 8.0);
	TEST_EXPECT(undone.surface == 2 && undone.code == 0);
	std::printf("sample: unsaved paint of an imported terrain's sources read from the view's ground, the outputs as imported\n");
	return 0;
}

// The painting feedback (2026-10-13): while the brush paints the surface or the foliage map in a view (its Terrain pane
// shown, its tool Surface or Foliage), the mission's 3D view and the terrain viewport show that map's overlay, the
// person's own again as the tool moves away or the pane hides; an overlay the person picks meanwhile stays. The options
// say what shows, the body's brush_overlay why; the legend's fold is an option of the canvas's alone (no Update).
int test_brush_overlay() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_brush_overlay");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Overlay"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string m = v.project.scan->find("m.bms")->relative_path;
	session.handle(request::open_document(m));
	devices.sync(session);
	const auto *mission = static_cast<const MissionViewport *>(session.viewports().find(m, ViewportKind::Mission));
	TEST_EXPECT(mission && mission->options().overlay == MissionGroundOverlay::None);
	if (!mission) return 1;
	const auto state = [&](const std::string &path, const char *kind) {
		JsonValue args = JsonValue::make_object();
		args.set("path", io::json_string(path));
		args.set("kind", io::json_string(kind));
		args.set("op", io::json_string("state"));
		std::string error;
		const JsonValue answer = session.query("viewport", args, error);
		if (!error.empty()) std::fprintf(stderr, "  viewport state: %s\n", error.c_str());
		return answer;
	};
	const auto shown = [&](const std::string &path, const char *kind) {
		const JsonValue answer = state(path, kind);
		const JsonValue *options = answer.get("options");
		return options ? options->get_string("overlay", "") : std::string();
	};
	const auto brushed = [&](const std::string &path, const char *kind) {
		const JsonValue answer = state(path, kind);
		const JsonValue *body = answer.get("body");
		const JsonValue *why = body ? body->get("brush_overlay") : nullptr;
		return !why || why->is_null() ? std::string("null")
		                              : why->get_string("overlay", "") + " then " + why->get_string("returns_to", "");
	};
	const auto brush = [&](const char *tool) {
		session.handle(request::set_workspace(std::string(R"({"terrain_brush": {"tool": ")") + tool + "\"}}"));
		const bool done = session.outcome().done();
		devices.sync(session);
		return done;
	};
	// The Surface tool with the pane hidden: nothing; the pane shown: the surface classes, a picture the device takes.
	TEST_EXPECT(brush("surface"));
	TEST_EXPECT(mission->options().overlay == MissionGroundOverlay::None);
	session.handle(request::set_workspace(R"({"document": {"terrain": true}})"));
	devices.sync(session);
	TEST_EXPECT(mission->options().overlay == MissionGroundOverlay::Surfaces && mission->overlay().kind == MissionGroundOverlay::Surfaces &&
	            !mission->overlay().empty());
	TEST_EXPECT(shown(m, "mission") == "surfaces" && brushed(m, "mission") == "surfaces then none");
	// Foliage: its overlay; a tool of another plane: the person's own (none) again.
	TEST_EXPECT(brush("foliage"));
	TEST_EXPECT(shown(m, "mission") == "foliage" && brushed(m, "mission") == "foliage then none");
	TEST_EXPECT(brush("raise"));
	TEST_EXPECT(shown(m, "mission") == "none" && brushed(m, "mission") == "null" &&
	            mission->overlay().kind == MissionGroundOverlay::None);
	// The pane hidden while painting: the person's own again.
	TEST_EXPECT(brush("surface"));
	TEST_EXPECT(shown(m, "mission") == "surfaces");
	session.handle(request::set_workspace(R"({"document": {"terrain": false}})"));
	devices.sync(session);
	TEST_EXPECT(shown(m, "mission") == "none");
	// The person's choice while painting stays: Foliage picked over the surface classes, kept as the tool moves away.
	session.handle(request::set_workspace(R"({"document": {"terrain": true}})"));
	devices.sync(session);
	TEST_EXPECT(shown(m, "mission") == "surfaces");
	session.handle(request::set_viewport(m, R"({"kind": "mission", "options": {"overlay": "foliage"}})"));
	devices.sync(session);
	TEST_EXPECT(shown(m, "mission") == "foliage");
	TEST_EXPECT(brush("raise"));
	TEST_EXPECT(shown(m, "mission") == "foliage" && brushed(m, "mission") == "null");
	// The person's own overlay is what painting returns to.
	TEST_EXPECT(brush("surface"));
	TEST_EXPECT(shown(m, "mission") == "surfaces" && brushed(m, "mission") == "surfaces then foliage");
	TEST_EXPECT(brush("none"));
	TEST_EXPECT(shown(m, "mission") == "foliage");
	// The legend folded and opened: the option on the wire, no Update of the device.
	editor_test::FakeDevice *device = devices.held(m, ViewportKind::Mission);
	TEST_EXPECT(device != nullptr);
	const size_t taken = device ? device->taken.size() : 0;
	session.handle(request::set_viewport(m, R"({"kind": "mission", "options": {"legend": false}})"));
	devices.sync(session);
	TEST_EXPECT(!mission->options().legend && device && std::none_of(device->taken.begin() + std::ptrdiff_t(taken), device->taken.end(),
	                                                                   [](ViewportAction a) { return a != ViewportAction::Keep; }));
	const JsonValue folded = state(m, "mission");
	TEST_EXPECT(folded.get("options") && !folded.get("options")->get_bool("legend", true));
	session.handle(request::set_viewport(m, R"({"kind": "mission", "options": {"legend": 1}})"));
	TEST_EXPECT(!session.outcome().done());
	session.handle(request::set_viewport(m, R"({"kind": "mission", "options": {"legend": true}})"));
	TEST_EXPECT(mission->options().legend);

	// The terrain viewport alike: its Terrain pane and the brush's Foliage tool show the foliage overlay.
	const std::string t = v.project.scan->find("Tmap.trn")->relative_path;
	session.handle(request::open_document(t));
	devices.sync(session);
	TEST_EXPECT(session.viewports().find(t, ViewportKind::Terrain) != nullptr && shown(t, "terrain") == "none");
	session.handle(request::set_workspace(R"({"document": {"terrain": true}, "terrain_brush": {"tool": "foliage"}})"));
	devices.sync(session);
	TEST_EXPECT(shown(t, "terrain") == "foliage" && brushed(t, "terrain") == "foliage then none");
	session.handle(request::set_viewport(t, R"({"kind": "terrain", "options": {"legend": false}})"));
	const JsonValue terrain = state(t, "terrain");
	TEST_EXPECT(terrain.get("options") && !terrain.get("options")->get_bool("legend", true));
	TEST_EXPECT(brush("none"));
	TEST_EXPECT(shown(t, "terrain") == "none" && brushed(t, "terrain") == "null");
	std::printf("brush overlay: Surface and Foliage show their map's overlay in the mission view and the terrain view, the "
	            "person's own after, theirs kept; the legend's fold an option\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_view();
	failures += test_imported();
	failures += test_sample_unsaved();
	failures += test_brush_overlay();
	if (failures == 0) std::printf("editor_terrain_maps_view: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
