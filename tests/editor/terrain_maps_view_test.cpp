// The maps and the tiles in the mission's 3D view as they are made (ADR 0046 S25, the device patches' portable half):
// a colour stroke, its end and its undo, a surface stroke, a tile stamp and its undo each an Update of the picture
// (its builds stand, nothing read again whole), the view's device patches holding what changed (the colour map's
// texels as the device's loader takes them, the surface map's stamp, the placed tiles the game reads now) and the
// files they account for at their stamps; the picture's files take those stamps, so the device's read of them does not
// find them moved. A foliage stroke's end makes the picture again (its device takes no foliage patch yet).
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/terrain_maps_patch.h>
#include <editor/preview/tile_tools.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
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
	mark = device->taken.size();
	session.handle(request::tile_edit(m, til, plan_tile_stamp(5, 0, -200.0, 200.0, false)));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	const MapsDevicePatch *placed = last_of(*mission, MapsDevicePatch::Kind::Tiles);
	TEST_EXPECT(placed && placed->tiles.entries.size() == 1 && accounts_for(*placed, "m.til"));
	if (placed && placed->tiles.entries.size() == 1) TEST_EXPECT(placed->tiles.entries[0].tile_index == 5);
	// The ground holds the stamped tile at once (in place, not the terrain read again).
	TEST_EXPECT(mission->followed_ground(v).tiles() == 1 && mission->ground_reader().reads() == reads);
	// The stamp undone: an Update whose placement holds none.
	mark = device->taken.size();
	session.handle(request::undo(m));
	devices.sync(session);
	TEST_EXPECT(updated_since(mark));
	placed = last_of(*mission, MapsDevicePatch::Kind::Tiles);
	TEST_EXPECT(placed && placed->tiles.entries.empty());

	// A foliage stroke's end: the picture made again (no device patch takes a foliage map yet).
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
	TEST_EXPECT(mission->builds() > builds);
	std::printf("view: a colour stroke, its end, its undo, a surface stroke and a stamp each an Update with its patch; "
	            "a foliage stroke's end a Rebuild\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_view();
	if (failures == 0) std::printf("editor_terrain_maps_view: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
