// The mission's 2D map (editor/preview/mission_map, ADR 0046 S23 C) over a real session: the kind's row (a Preview
// kind beside the mission's 3D view, which stays the kind a mission is read through), the CMAP's projection (its
// click-to-world law, hud::command_map_waypoint_world, the inverse the map's pixels take), the marks of the minted
// mission and the first framing, a hit and a box, a drag of a pin planned into the 3D view's move batch
// (mission_move_edits) and served as one undo step, the selection the 3D view shares, the commands and the wire; the
// edge cap (kMissionOutlineEdgesMax) and the wireframes' chunks over a mission of minted models at 00TRa's scale.

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_link_shapes.h>
#include <editor/preview/mission_map.h>
#include <editor/preview/mission_map_canvas.h>
#include <editor/preview/mission_map_outline.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_palette.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/hud/hud_map_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/mission_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"
#include "fixtures/minimal_3di_builder.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::FakeDevices;
using editor_test::NoProcess;

constexpr const char *kMission = "missions/synth_logic.bms";


struct Rig {
	test_temp::TempDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	FakeDevices devices;
	std::string path;

	explicit Rig(const char *name) : dir(name) {}

	// `attach`: the rig's own (headless) devices given to every target. `models`: the item table the mission's items
	// are in and the synth models their graphics draw (the pump's drawn by the synth crate, as the GUT device test
	// mints them), written into the project too.
	bool open(bool attach = true, bool models = false) {
		session.handle(request::new_project(dir.file("project"), "Maps"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &view = session.view();
		if (!test_io::write_file(view.project.root + "/" + kMission, test_io::read_file(test_paths_fixture("bms/synth_logic.bms"))))
			return false;
		if (models) {
			const std::vector<uint8_t> table = test_io::read_file(test_paths_fixture("def/items.def"));
			std::string items(table.begin(), table.end());
			for (size_t at = items.find("graphic pump\r\n"); at != std::string::npos; at = items.find("graphic pump\r\n"))
				items.replace(at, 12, "graphic crate");
			if (!test_io::write_text(view.project.root + "/defs/items.def", items)) return false;
			for (const char *model : { "crate.3di", "armory.3di", "shed.3di" })
				if (!test_io::write_file(view.project.root + "/models/" + model,
							test_io::read_file(test_paths_fixture(std::string("threedi/synth/") + model))))
					return false;
		}
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		path = session.document_for(kMission)->path();
		if (attach) devices.sync(session);
		return true;
	}
	const MissionMapViewport *map() {
		return static_cast<const MissionMapViewport *>(session.viewports().find(path, ViewportKind::Map));
	}
	const MissionViewport *mission() {
		return static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	}
	const Document *document() { return records_of(*session.document_for(kMission)); }
	ViewportContext context() { return viewport_context(session.view(), *map()); }
	void pump() { devices.sync(session); }
};

bool near(double a, double b, double slack) { return std::fabs(a - b) <= slack; }

using mission_test::first_of;

} // namespace

// The kind's row: a Preview kind of the mission beside its 3D view; the 3D view stays the kind a mission is read
// through when none is named, and the Preview window shows the map for a mission.
static int test_kind_row() {
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Map);
	TEST_EXPECT(row.role == ViewportRole::Preview && !row.as_saved && !row.part && !row.holds_for_gesture && row.canvas);
	TEST_EXPECT(row.devices == 1 && row.make != nullptr && !row.backdrop);
	TEST_EXPECT(viewport_kind_shows(ViewportKind::Map, DocumentTypeId::Mission));
	// Beside the 3D view: the workspace pins it no device until the Preview window draws it; the menu's and the
	// model's stand alone.
	TEST_EXPECT(viewport_kind_beside_picture(ViewportKind::Map) && !viewport_kind_beside_picture(ViewportKind::Menu) &&
	            !viewport_kind_beside_picture(ViewportKind::Model) && !viewport_kind_beside_picture(ViewportKind::Mission));
	TEST_EXPECT(preview_kind_of(DocumentTypeId::Mission) == ViewportKind::Map);
	TEST_EXPECT(main_viewport_kind(DocumentTypeId::Mission) == ViewportKind::Mission);
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::Mission) == ViewportKind::Mission);
	// A text type's Preview kind is still the one it is read through (its script device draws on no canvas).
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::Particles) == ViewportKind::Effect);
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::HudLayout) == ViewportKind::Hud);
	ViewportKind kind = ViewportKind::kCount;
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Map)) == "map" && viewport_kind_from_token("map", kind) &&
	            kind == ViewportKind::Map);
	std::printf("test_kind_row passed\n");
	return 0;
}

// The workspace's cache (set_pin_all_targets(false)): the Preview window shows the mission's map only where the Map
// tool asks (set_workspace's document `map`; the Windows menu's tick shows the kind shown before, S15), and stands aside
// for it until then, so its target is pinned no device; the window drawing it (its canvas's ask) gives it one.
static int test_workspace_pins() {
	Rig rig("opennova_editor_mission_map_pins");
	TEST_EXPECT(rig.open(false));
	TEST_EXPECT(rig.session.view().documents.preview_shown == ViewportKind::kCount &&
	            rig.session.view().documents.previews[ViewportKind::Map].path == rig.path);
	editor_test::handle_to_end(rig.session,
			request::set_workspace("{\"document\":{\"path\":\"" + rig.path + "\",\"map\":true},\"focus\":\"preview\"}"));
	TEST_EXPECT(rig.session.outcome().done());
	TEST_EXPECT(rig.session.view().documents.preview_shown == ViewportKind::Map &&
	            rig.session.view().documents.beside == rig.path);
	FakeDevices shown;
	shown.cache.set_pin_all_targets(false);
	shown.sync(rig.session);
	TEST_EXPECT(!shown.held(rig.path, ViewportKind::Map) && shown.cache.size() == 0);
	TEST_EXPECT(shown.cache.device(rig.path, ViewportKind::Map) == nullptr);
	shown.sync(rig.session);
	TEST_EXPECT(shown.held(rig.path, ViewportKind::Map) != nullptr && shown.cache.size() == 1);
	std::printf("test_workspace_pins passed\n");
	return 0;
}

// The CMAP's projection: a pixel's mission point is where the game's commander map puts a click there
// (hud::command_map_waypoint_world over the CMAP's own render, an 800-wide window at its own design scale), and a
// point's pixel goes back to it; the scale is the CMAP's law, metres across = zoom x 65536 / 200.
static int test_projection() {
	MissionMapCamera camera;
	camera.center[0] = 100.5;
	camera.center[1] = -250.25;
	camera.zoom = 2.5f;
	const MissionMapView view = mission_map_view(camera, 800, 600);
	TEST_EXPECT(near(double(view.scale) * 800.0, 2.5 * 65536.0 / 200.0, 1e-2));
	TEST_EXPECT(view.middle_x == 400.0f && view.middle_y == 300.0f);
	opennova::hud::CommandMapView cmap;
	cmap.view.zoom = camera.zoom;
	opennova::hud::HudMinimapInput base;
	base.surface_w = 800.0f;
	base.surface_h = 600.0f;
	const int32_t cx = opennova::bms::to_fixed_16_16(camera.center[0]), cy = opennova::bms::to_fixed_16_16(camera.center[1]);
	cmap.render(opennova::hud::MapViewRect{ 0, 0, 800, 600 }, 800, cx, cy, 0, base);
	TEST_EXPECT(near(base.window_scale, view.scale, 1e-6));
	for (const auto &click : { std::pair<int, int>{ 123, 456 }, std::pair<int, int>{ 400, 300 }, std::pair<int, int>{ 799, 0 } }) {
		cmap.view.last_x = click.first;
		cmap.view.last_y = click.second;
		int32_t wx = 0, wy = 0;
		opennova::hud::command_map_waypoint_world(cmap, cx, cy, wx, wy);
		double x = 0.0, y = 0.0;
		view.unproject(float(click.first), float(click.second), x, y);
		TEST_EXPECT(near(x, double(wx) / 65536.0, 0.05) && near(y, double(wy) / 65536.0, 0.05));
		float px = 0.0f, py = 0.0f;
		view.project(x, y, px, py);
		TEST_EXPECT(near(px, click.first, 0.01) && near(py, click.second, 0.01));
	}
	// North up: a point north of the centre is above it, east of it to the right.
	float px = 0.0f, py = 0.0f;
	view.project(camera.center[0] + 50.0, camera.center[1] + 80.0, px, py);
	TEST_EXPECT(px > 400.0f && py < 300.0f);
	// A RotateMap180 mission's map turned half a turn about the centre (g_MapYaw180): the same point below the centre
	// and to its left, each pixel the north-up one's mirrored through the middle, and back.
	const MissionMapView turned = mission_map_view(camera, 800, 600, true);
	TEST_EXPECT(turned.flip_180 && turned.scale == view.scale && turned.middle_x == view.middle_x);
	float tx = 0.0f, ty = 0.0f;
	turned.project(camera.center[0] + 50.0, camera.center[1] + 80.0, tx, ty);
	TEST_EXPECT(tx < 400.0f && ty > 300.0f && near(tx, 800.0 - px, 0.01) && near(ty, 600.0 - py, 0.01));
	double bx = 0.0, by = 0.0;
	turned.unproject(tx, ty, bx, by);
	TEST_EXPECT(near(bx, camera.center[0] + 50.0, 1e-3) && near(by, camera.center[1] + 80.0, 1e-3));
	// The zoom for a width across: the law inverted, clamped to the map's range.
	TEST_EXPECT(near(mission_map_zoom_for(327.68 * 3.0, 640), 3.0, 1e-4));
	TEST_EXPECT(mission_map_zoom_for(1.0, 640) == kMissionMapZoomMin && mission_map_zoom_for(1e7, 640) == kMissionMapZoomMax);
	std::printf("test_projection passed\n");
	return 0;
}

// The map of the minted mission: ready, every entity and area a mark, the first framing showing them all; a hit at a
// pin names its record, a box over the picture takes every shown one; the selection the 3D view's.
// A tile set's texels as MED draws a tile: alpha not 0 opaque, 0 not drawn; the colour kept.
static int test_tile_texels() {
	std::vector<uint8_t> rgba = { 10, 20, 30, 0, 40, 50, 60, 1, 70, 80, 90, 255, 1, 2, 3, 128 };
	mission_map_tile_texels(rgba);
	TEST_EXPECT(rgba == std::vector<uint8_t>({ 10, 20, 30, 0, 40, 50, 60, 255, 70, 80, 90, 255, 1, 2, 3, 255 }));
	return 0;
}

static int test_marks_hit_box() {
	Rig rig("opennova_editor_mission_map_marks");
	TEST_EXPECT(rig.open());
	const MissionMapViewport *map = rig.map();
	TEST_EXPECT(map != nullptr);
	if (!map) return 1;
	TEST_EXPECT(map->status() == ViewportStatus::Ready);
	const ViewportContext context = rig.context();
	const std::vector<MissionMapMark> marks = map->marks(context.width, context.height);
	TEST_EXPECT(marks.size() == map->scene().entities().size() + map->scene().areas().size());
	size_t shown = 0;
	for (const MissionMapMark &mark : marks) shown += mark.shown ? 1 : 0;
	TEST_EXPECT(shown == marks.size());
	// A hit at the first item's pin.
	const NodeAddress item = first_of(*rig.document(), MissionKind::Item);
	const int index = map->scene().mark_index(item.row);
	TEST_EXPECT(index >= 0);
	if (index < 0) return 1;
	const ViewportHit hit = map->hit(context, marks[size_t(index)].px, marks[size_t(index)].py);
	TEST_EXPECT(hit.id == item.row && std::string(hit.kind) == "item" && hit.current);
	const std::vector<ViewportHit> boxed = map->box(context, -50.0f, -50.0f, float(context.width) + 50.0f,
			float(context.height) + 50.0f);
	TEST_EXPECT(boxed.size() == shown);
	// A click of the pin through the canvas: one SelectRecord, which the 3D view's frame shows selected too.
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(map->click(context, marks[size_t(index)].px, marks[size_t(index)].py, SelectMode::Replace, gathered, error));
	TEST_EXPECT(gathered.requests.size() == 1 && gathered.requests[0].kind == EditorRequestKind::SelectRecord);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	const MissionCanvasFrame frame = rig.mission()->canvas_frame(viewport_context(rig.session.view(), *rig.mission()));
	TEST_EXPECT(frame.primary == rig.mission()->scene().mark_index(item.row));
	const JsonValue body = rig.map()->body_json(ViewportInput{ rig.session.view(), rig.session.viewports().clock(),
	                                                             rig.session.document_for(kMission) });
	const JsonValue *selected = body.get("selected");
	TEST_EXPECT(selected && selected->array.size() == 1 && int64_t(selected->array[0].number) == int64_t(item.row));
	std::printf("test_marks_hit_box passed\n");
	return 0;
}

// A drag of a pin: the 3D view's move (mission_move_edits) to the point the drag goes to, one batch under one gesture,
// served as one undo step and undone to the bytes; a handle the map has not refused.
static int test_drag() {
	Rig rig("opennova_editor_mission_map_drag");
	TEST_EXPECT(rig.open());
	const MissionMapViewport *map = rig.map();
	if (!map) return 1;
	const ViewportContext context = rig.context();
	const Document &document = *rig.document();
	const NodeAddress item = first_of(document, MissionKind::Item);
	MissionPressed held;
	TEST_EXPECT(map->pressed(item, held));
	const std::string before = rig.document()->serialize().text;
	ViewportDrag drag;
	drag.id = item.row;
	drag.handle = "move";
	drag.by = true;
	drag.x = 20.0f;
	drag.y = -10.0f;
	drag.snap = 0.0f;
	drag.end = true;
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(map->drag(context, drag, gathered, error));
	TEST_EXPECT(gathered.requests.size() == 2 && gathered.requests[0].kind == EditorRequestKind::EditRecord &&
	            gathered.requests[1].kind == EditorRequestKind::EndEdit);
	if (gathered.requests.size() != 2) return 1;
	// The same batch the 3D view's move plans for the same displacement.
	const MissionMapView view = map->view(context.width, context.height);
	const double target[2] = { held.x + 20.0 * double(view.scale), held.y + 10.0 * double(view.scale) };
	std::vector<Edit> expected;
	TEST_EXPECT(mission_move_edits(document, { held }, 0, target, 0.0f, map->options().stick, context.device,
	                               gathered.requests[0].edits.front().gesture, expected));
	TEST_EXPECT(expected.size() == gathered.requests[0].edits.size());
	for (size_t i = 0; i < expected.size() && i < gathered.requests[0].edits.size(); ++i)
		TEST_EXPECT(expected[i].field == gathered.requests[0].edits[i].field &&
		            expected[i].value == gathered.requests[0].edits[i].value);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	const MissionEntityMark *moved = rig.map()->scene().entity(item.row);
	TEST_EXPECT(moved && near(moved->x, target[0], 1.0 / 32768.0) && near(moved->y, target[1], 1.0 / 32768.0));
	// The drag placed its one record again, no other.
	TEST_EXPECT(rig.map()->outlines_placed() == 1);
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == before);
	// The map moves a record; its height and heading are the 3D view's.
	drag.handle = "yaw";
	gathered.requests.clear();
	TEST_EXPECT(!rig.map()->drag(rig.context(), drag, gathered, error) && error.find("3D view") != std::string::npos);
	std::printf("test_drag passed\n");
	return 0;
}

// S24: the selected event's links on the map (preview/mission_link_shapes, its reach a circle seen from above), the
// badges on what the events name and the hover's words; none with the links option off.
static int test_links() {
	Rig rig("opennova_editor_mission_map_links");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	const MissionDocument &mission = static_cast<const MissionDocument &>(*rig.document());
	const NodeAddress event = first_of(mission, MissionKind::Event);
	const NodeAddress walker = first_of(mission, MissionKind::Organic);
	const NodeAddress zone = first_of(mission, MissionKind::Area);
	rig.session.handle(request::select_record(kMission, event));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const ViewportContext context = rig.context();
	std::unique_ptr<CanvasHalf> half = rig.map()->make_canvas();
	editor_test::Gathered gathered;
	half->follow(*rig.map(), context, gathered);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	OverlayList list = half->shapes(context, in);
	const auto count = [&](OverlayKind kind, uint32_t rgb, float size) {
		return std::count_if(list.shapes.begin(), list.shapes.end(), [&](const OverlayShape &shape) {
			return shape.kind == kind && shape.rgb == rgb && (size < 0.0f || shape.size == size);
		});
	};
	// Event 1 watches the walker in zone 20 (each rung where it stands on the map) and re-arms event 2 (no place).
	const std::vector<MissionMapMark> marks = rig.map()->marks(context.width, context.height);
	const MissionMapMark &at = marks[size_t(rig.map()->scene().mark_index(walker.row))];
	TEST_EXPECT(count(OverlayKind::Circle, kMissionWatchedRgb, 14.0f) == 2 && count(OverlayKind::Circle, kMissionActedRgb, -1.0f) == 0);
	bool rung = false;
	for (const OverlayShape &shape : list.shapes)
		rung = rung || (shape.kind == OverlayKind::Circle && shape.rgb == kMissionWatchedRgb && shape.points[0].x == at.px &&
		                shape.points[0].y == at.py);
	TEST_EXPECT(rung);
	// The badges: the walker, zone 20, both events (named by each other), and no other pin.
	TEST_EXPECT(count(OverlayKind::Text, kMissionBadgeRgb, -1.0f) == 2);
	// The hover over the walker's pin says which event names it.
	in.hovered = true;
	in.mouse = CanvasPoint{ at.px, at.py };
	const std::string tip = half->hover_tip(context, in);
	TEST_EXPECT(tip.find("Named by 1 event:\nEvent 1: When ") != std::string::npos);
	in.hovered = false;
	// The zone's ring is at its middle.
	const MissionMapMark &area = marks[size_t(rig.map()->scene().mark_index(zone.row))];
	bool middle = false;
	for (const OverlayShape &shape : list.shapes)
		middle = middle || (shape.kind == OverlayKind::Circle && shape.rgb == kMissionWatchedRgb &&
		                    std::fabs(shape.points[0].x - area.px) < 0.5f && std::fabs(shape.points[0].y - area.py) < 0.5f);
	TEST_EXPECT(middle);
	// Off: none.
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"marks": {"links": false}}})"));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	half->follow(*rig.map(), rig.context(), gathered);
	list = half->shapes(rig.context(), in);
	TEST_EXPECT(count(OverlayKind::Circle, kMissionWatchedRgb, 14.0f) == 0 && count(OverlayKind::Text, kMissionBadgeRgb, -1.0f) == 0);
	TEST_EXPECT(!rig.map()->options().links && rig.map()->options_json().get("marks")->get_bool("links", true) == false);
	std::printf("test_links passed\n");
	return 0;
}

// S24: coloured by group, the wireframes' edges and the pins' rings in each group's colour (an edge's colour moved in
// place, the chunks' serials moved), a legend of the groups in view; the select_group command selects the members of
// the named record's group; color_by on the wire.
static int test_group_colours() {
	Rig rig("opennova_editor_mission_map_groups");
	TEST_EXPECT(rig.open(true, true));
	for (int i = 0; i < 40 && rig.map() && rig.map()->outlines_pending(); ++i) rig.pump();
	const MissionMapViewport *map = rig.map();
	if (!map) return 1;
	const MissionDocument &mission = static_cast<const MissionDocument &>(*rig.document());
	const NodeAddress walker = first_of(mission, MissionKind::Organic);
	const std::vector<uint32_t> before = map->outline_rgb();
	TEST_EXPECT(!before.empty());
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"color_by": "group"}})"));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.map()->options().color_by == MissionColorBy::Group &&
	            rig.map()->options_json().get_string("color_by", "") == "group");
	// The walker's (group 1) and the guard's (group 2) edges in their groups' colours; none of a group-0 entity's.
	const std::vector<uint32_t> &after = rig.map()->outline_rgb();
	TEST_EXPECT(after.size() == before.size());
	const auto has = [&](uint32_t rgb) { return std::find(after.begin(), after.end(), rgb) != after.end(); };
	TEST_EXPECT(has(mission_group_rgb(1)) && has(mission_group_rgb(2)) && !has(mission_group_rgb(3)));
	TEST_EXPECT(mission_map_rgb(MissionPool::Organic, 2, 1, MissionColorBy::Group) == mission_group_rgb(1) &&
	            mission_map_rgb(MissionPool::Organic, 2, 0, MissionColorBy::Group) == kMissionOrganicRgb &&
	            mission_map_rgb(MissionPool::Organic, 2, 1, MissionColorBy::Team) == kMissionRedRgb);
	// The legend.
	const ViewportContext context = rig.context();
	std::unique_ptr<CanvasHalf> half = rig.map()->make_canvas();
	editor_test::Gathered gathered;
	half->follow(*rig.map(), context, gathered);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	const OverlayList list = half->shapes(context, in);
	const auto text = [&](const std::string &words) {
		return std::count_if(list.shapes.begin(), list.shapes.end(), [&](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Text && shape.text == words;
		});
	};
	TEST_EXPECT(text("Groups in view") == 1 && text("Group 1 (1)") == 1 && text("Group 2 (1)") == 1);
	// select_group: the walker's group's members (the walker alone), one SelectRecord.
	gathered.requests.clear();
	std::string error;
	TEST_EXPECT(rig.map()->command(context, "select_group", { walker.row }, gathered, error));
	TEST_EXPECT(gathered.requests.size() == 1 && gathered.requests[0].kind == EditorRequestKind::SelectRecord);
	gathered.requests.clear();
	TEST_EXPECT(!rig.map()->command(context, "select_group", { first_of(mission, MissionKind::Item).row }, gathered, error) &&
	            error.find("group") != std::string::npos);
	std::printf("test_group_colours passed\n");
	return 0;
}

// S24 B6: an area's edges and corners on the map over the wire's drag (as far as the pointer went, the area alone);
// its bottom and top refused (the 3D view's).
static int test_area_handles() {
	Rig rig("opennova_editor_mission_map_area_handles");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	const MissionDocument &mission = static_cast<const MissionDocument &>(*rig.document());
	const NodeAddress zone = first_of(mission, MissionKind::Area);
	const ViewportContext context = rig.context();
	const MissionMapView view = rig.map()->view(context.width, context.height);
	MissionPressed held;
	TEST_EXPECT(rig.map()->pressed(zone, held));
	ViewportDrag drag;
	drag.id = zone.row;
	drag.handle = "x_max";
	drag.by = true;
	drag.x = 10.0f;
	drag.y = 0.0f;
	drag.end = true;
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(rig.map()->drag(context, drag, gathered, error) && gathered.requests.size() == 2 &&
	            gathered.requests[0].edits.size() == 1 && gathered.requests[0].edits[0].field == "x_max");
	if (gathered.requests.size() == 2 && gathered.requests[0].edits.size() == 1) {
		const double x_max = std::get<double>(gathered.requests[0].edits[0].value);
		TEST_EXPECT(near(x_max, held.max[0] + 10.0 * double(view.scale), 1e-3));
	}
	drag.handle = "x_min_y_max";
	gathered.requests.clear();
	TEST_EXPECT(rig.map()->drag(context, drag, gathered, error) && !gathered.requests.empty() &&
	            gathered.requests[0].edits.size() == 1 && gathered.requests[0].edits[0].field == "x_min");
	drag.handle = "z_max";
	gathered.requests.clear();
	TEST_EXPECT(!rig.map()->drag(context, drag, gathered, error) && error.find("3D view") != std::string::npos);
	// An entity has no edge on the map.
	drag.id = first_of(mission, MissionKind::Item).row;
	drag.handle = "x_min";
	TEST_EXPECT(!rig.map()->drag(context, drag, gathered, error));
	// Esc mid-drag of the primary area's east edge on the canvas: what the drag wrote goes back where the press found it.
	{
		rig.session.handle(request::select_record(kMission, zone));
		rig.pump();
		const ViewportContext now = rig.context();
		std::unique_ptr<CanvasHalf> half = rig.map()->make_canvas();
		editor_test::Gathered got;
		half->follow(*rig.map(), now, got);
		const MissionMapView seen = rig.map()->view(now.width, now.height);
		double ex = 0.0, ny = 0.0;
		TEST_EXPECT(mission_map_handle_point(held, MissionHandle::XMax, ex, ny));
		CanvasInput in;
		in.width = now.width;
		in.height = now.height;
		in.hovered = true;
		in.keyboard.focused = true;
		seen.project(ex, ny, in.mouse.x, in.mouse.y);
		in.screen = in.mouse;
		const auto step = [&](CanvasInput frame) {
			got.requests.clear();
			half->follow(*rig.map(), rig.context(), got);
			got.requests.clear();
			half->input(rig.context(), frame, got);
			return got.requests;
		};
		CanvasInput press = in;
		press.pressed = true;
		press.down = true;
		step(press);
		CanvasInput moved = in;
		moved.down = true;
		moved.mouse.x += 40.0f;
		moved.screen = moved.mouse;
		std::vector<EditorRequest> wrote = step(moved);
		TEST_EXPECT(!wrote.empty() && editor_test::serve(rig.session, wrote));
		rig.pump();
		MissionPressed dragged;
		TEST_EXPECT(rig.map()->pressed(zone, dragged) && dragged.max[0] > held.max[0] + 1.0);
		CanvasInput escaped = moved;
		escaped.keyboard.escape = true;
		std::vector<EditorRequest> undone = step(escaped);
		bool restored = false;
		for (const EditorRequest &request : undone)
			for (const Edit &edit : request.edits)
				restored = restored || (edit.field == "x_max" && near(std::get<double>(edit.value), held.max[0], 1e-6));
		TEST_EXPECT(restored);
		TEST_EXPECT(editor_test::serve(rig.session, undone));
		rig.pump();
		TEST_EXPECT(rig.map()->pressed(zone, dragged) && near(dragged.max[0], held.max[0], 1e-6));
	}
	std::printf("test_area_handles passed\n");
	return 0;
}

// S24 B2: pick mode on the map: while the workspace's pick waits for event 1's zone, each area's pin is a candidate
// (rung), the hover says what to click, a `pick` of zone 20 sets the trigger's zone and ends the pick (one batch,
// then the workspace), and one of an entity is refused; the pick ends when its trigger goes.
static int test_pick() {
	Rig rig("opennova_editor_mission_map_pick");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	const MissionDocument &mission = static_cast<const MissionDocument &>(*rig.document());
	const NodeAddress event = first_of(mission, MissionKind::Event);
	NodeAddress trigger;
	for (const Document::Collection &list : mission.collections_of(event))
		if (list.spec.kind == node_kind(MissionKind::Trigger)) trigger = { event.row, list.spec.kind, list.ids[0] };
	const std::vector<const Node *> areas = mission.rows_of(MissionKind::Area);
	const auto ask_pick = [&](const std::string &pick) {
		rig.session.handle(request::set_workspace("{\"document\": {\"path\": \"" + rig.path + "\", \"pick\": " + pick + "}}"));
		return rig.session.outcome().done();
	};
	TEST_EXPECT(ask_pick("{\"record\": " + std::to_string(trigger.child) + ", \"slot\": 1}"));
	rig.pump();
	const ViewportContext context = rig.context();
	std::unique_ptr<CanvasHalf> half = rig.map()->make_canvas();
	editor_test::Gathered gathered;
	half->follow(*rig.map(), context, gathered);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	const OverlayList list = half->shapes(context, in);
	TEST_EXPECT(std::count_if(list.shapes.begin(), list.shapes.end(), [](const OverlayShape &shape) {
		return shape.kind == OverlayKind::Circle && shape.size == 13.0f && shape.rgb == kMissionCandidateRgb;
	}) == long(areas.size()));
	const std::vector<MissionMapMark> marks = rig.map()->marks(context.width, context.height);
	const MissionMapMark &pin = marks[size_t(rig.map()->scene().mark_index(areas[0]->id))];
	in.hovered = true;
	in.mouse = CanvasPoint{ pin.px, pin.py };
	TEST_EXPECT(half->hover_tip(context, in) == "Click an area for Zone (Esc cancels).");
	// The wire's pick: an entity refused, zone 30's area taken.
	std::string error;
	TEST_EXPECT(!rig.map()->command(context, "pick", { first_of(mission, MissionKind::Organic).row }, gathered, error) &&
	            error.find("an area") != std::string::npos);
	gathered.requests.clear();
	TEST_EXPECT(rig.map()->command(context, "pick", { areas[1]->id }, gathered, error) && gathered.requests.size() == 2 &&
	            gathered.requests[0].kind == EditorRequestKind::EditRecord && gathered.requests[0].keep_selection &&
	            gathered.requests[1].kind == EditorRequestKind::SetWorkspace);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	Value zone;
	TEST_EXPECT(rig.document()->get(trigger, "param2", zone) && std::get<int64_t>(zone) == 30);
	TEST_EXPECT(!rig.session.view().workspace.document(rig.path).pick.active());
	// No pick waits: refused.
	TEST_EXPECT(!rig.map()->command(rig.context(), "pick", { areas[0]->id }, gathered, error) &&
	            error.find("No pick waits") != std::string::npos);
	// A pick whose trigger goes ends with it.
	TEST_EXPECT(ask_pick("{\"record\": " + std::to_string(trigger.child) + ", \"slot\": 0}"));
	TEST_EXPECT(rig.session.view().workspace.document(rig.path).pick.active());
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = trigger;
	rig.session.handle(request::edit_record(kMission, std::vector<Edit>{ remove }));
	TEST_EXPECT(rig.session.outcome().done() && !rig.session.view().workspace.document(rig.path).pick.active());
	std::printf("test_pick passed\n");
	return 0;
}

// S24 B7: the 3D view's keys on the map: Delete removes the selection (one batch), Ctrl+D copies it a snap step east
// (the 3D view's duplicate), an arrow nudges it a snap step on the map's axes (one gesture while it is held).
static int test_keys() {
	Rig rig("opennova_editor_mission_map_keys");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	const MissionDocument &mission = static_cast<const MissionDocument &>(*rig.document());
	const NodeAddress item = first_of(mission, MissionKind::Item);
	rig.session.handle(request::select_record(kMission, item));
	rig.pump();
	std::unique_ptr<CanvasHalf> half = rig.map()->make_canvas();
	editor_test::Gathered gathered;
	const ViewportContext context = rig.context();
	half->follow(*rig.map(), context, gathered);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	in.keyboard.focused = true;
	const auto keyed = [&](const std::function<void(CanvasKeyboard &)> &set) {
		CanvasInput frame = in;
		set(frame.keyboard);
		gathered.requests.clear();
		half->input(context, frame, gathered);
		return gathered.requests;
	};
	std::vector<EditorRequest> raised = keyed([](CanvasKeyboard &k) { k.remove = true; });
	TEST_EXPECT(raised.size() == 1 && raised[0].kind == EditorRequestKind::EditRecord && raised[0].edits.size() == 1 &&
	            raised[0].edits[0].operation == EditOperation::Remove && raised[0].edits[0].address == item);
	raised = keyed([](CanvasKeyboard &k) { k.duplicate = true; });
	TEST_EXPECT(raised.size() == 1 && raised[0].kind == EditorRequestKind::EditInViewport &&
	            raised[0].command.name == "duplicate" && raised[0].command.by.size() == 2 &&
	            raised[0].command.by[0] == double(rig.map()->options().snap));
	MissionPressed held;
	TEST_EXPECT(rig.map()->pressed(item, held));
	raised = keyed([](CanvasKeyboard &k) {
		k.arrow_x = 1;
		k.arrow_held = true;
	});
	bool east = false;
	for (const EditorRequest &request : raised)
		for (const Edit &edit : request.edits)
			east = east || (edit.field == "x" && std::fabs(std::get<double>(edit.value) - (held.x + double(rig.map()->options().snap))) < 1e-3);
	TEST_EXPECT(east);
	// Up moves north up the picture and Down south (arrow_y is +1 for Down); a RotateMap180 mission's map the other
	// way. Each press on a canvas of its own, so no nudge before it carries over.
	const double snap = double(rig.map()->options().snap);
	const auto nudged_y = [&](int arrow_y) {
		std::unique_ptr<CanvasHalf> fresh = rig.map()->make_canvas();
		editor_test::Gathered got;
		const ViewportContext now = rig.context();
		fresh->follow(*rig.map(), now, got);
		CanvasInput frame = in;
		frame.width = now.width;
		frame.height = now.height;
		frame.keyboard.arrow_y = arrow_y;
		frame.keyboard.arrow_held = true;
		got.requests.clear();
		fresh->input(now, frame, got);
		for (const EditorRequest &request : got.requests)
			for (const Edit &edit : request.edits)
				if (edit.field == "y") return std::get<double>(edit.value) - held.y;
		return 0.0;
	};
	TEST_EXPECT(std::fabs(nudged_y(-1) - snap) < 1e-3 && std::fabs(nudged_y(1) + snap) < 1e-3);
	{
		Edit turn;
		turn.operation = EditOperation::Set;
		turn.address = first_of(*rig.document(), MissionKind::Mission);
		turn.field = "attrib_flags";
		turn.value = int64_t(0x20);
		rig.session.handle(request::edit_record(kMission, turn));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		rig.session.handle(request::select_record(kMission, item));
		rig.pump();
		TEST_EXPECT(rig.map()->ground().flip_180);
		TEST_EXPECT(std::fabs(nudged_y(-1) + snap) < 1e-3 && std::fabs(nudged_y(1) - snap) < 1e-3);
		rig.session.handle(request::undo(kMission));
		rig.pump();
	}
	std::printf("test_keys passed\n");
	return 0;
}

// The commands (frame, the CMAP's zoom steps), each a SetViewport of the camera; the wire's camera and options, and
// their refusals.
static int test_commands_and_wire() {
	Rig rig("opennova_editor_mission_map_wire");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	const float zoom = rig.map()->camera().zoom;
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(rig.map()->command(rig.context(), "zoom_in", {}, gathered, error) && gathered.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	TEST_EXPECT(near(rig.map()->camera().zoom, zoom * opennova::hud::kMapViewZoomInStep, 1e-4));
	gathered.requests.clear();
	TEST_EXPECT(rig.map()->command(rig.context(), "zoom_out", {}, gathered, error) && editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	TEST_EXPECT(near(rig.map()->camera().zoom, zoom, 1e-3));
	gathered.requests.clear();
	TEST_EXPECT(!rig.map()->command(rig.context(), "top", {}, gathered, error) && gathered.requests.empty());
	// The camera on the wire, set and refused.
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "camera": {"center": [10, 20], "zoom": 5}})"));
	rig.pump();
	TEST_EXPECT(rig.session.outcome().done() && rig.map()->camera().center[0] == 10.0 &&
	            rig.map()->camera().center[1] == 20.0 && rig.map()->camera().zoom == 5.0f);
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "camera": {"zoom": 400}})"));
	TEST_EXPECT(!rig.session.outcome().done() && rig.map()->camera().zoom == 5.0f);
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"marks": {"items": false}, "grid": false}})"));
	rig.pump();
	TEST_EXPECT(rig.session.outcome().done() && !rig.map()->options().items && !rig.map()->options().grid);
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"bogus": 1}})"));
	TEST_EXPECT(!rig.session.outcome().done());
	const JsonValue camera = rig.map()->camera_json();
	TEST_EXPECT(camera.get("zoom") && camera.get("zoom")->number == 5.0 && camera.get("scale"));
	// Frame: every entity and area on the picture again.
	gathered.requests.clear();
	TEST_EXPECT(rig.map()->command(rig.context(), "frame", {}, gathered, error) && editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"marks": {"items": true}}})"));
	rig.pump();
	const ViewportContext context = rig.context();
	for (const MissionMapMark &mark : rig.map()->marks(context.width, context.height)) TEST_EXPECT(mark.shown);
	std::printf("test_commands_and_wire passed\n");
	return 0;
}

// The grid's origin: the first marker whose record's type is 2043 (the Map Centerpoint), as the game's HUD init scans
// for it; none in the minted mission, a marker of that item added gives the map its place.
static int test_grid_origin() {
	Rig rig("opennova_editor_mission_map_grid");
	TEST_EXPECT(rig.open());
	if (!rig.map()) return 1;
	TEST_EXPECT(!rig.map()->ground().grid_origin && rig.map()->ground().terrain == rig.map()->scene().header().terrain);
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, node_kind(MissionKind::Marker), 0 };
	add.field = "item";
	add.value = int64_t(102043);
	rig.session.handle(request::edit_record(kMission, add));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const MissionEntityMark *centre = nullptr;
	for (const MissionEntityMark &entity : rig.map()->scene().entities())
		if (entity.item == 102043) centre = &entity;
	TEST_EXPECT(centre != nullptr);
	if (!centre) return 1;
	TEST_EXPECT(rig.map()->ground().grid_origin && rig.map()->ground().grid_x == centre->x &&
	            rig.map()->ground().grid_y == centre->y);
	std::printf("test_grid_origin passed\n");
	return 0;
}

// The synth crate (one box) seen from above: its LOD 0's edges the four sides of its plan (a face's diagonal lies in its
// face's plane, an upright has no length from above, the top lands on the bottom), its hull the plan's four corners;
// every point's words the vertex's (p2, -p0, p1), the words the device's model holds it at.
static int test_outline_of_a_box() {
	const std::vector<uint8_t> bytes = test_io::read_file(test_paths_fixture("threedi/synth/crate.3di"));
	opennova::threedi::Threedi3di3 parsed{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &parsed) == 0);
	MissionModelOutline outline;
	TEST_EXPECT(mission_model_outline(parsed, outline));
	double lo[2] = { 1e9, 1e9 }, hi[2] = { -1e9, -1e9 };
	const opennova::threedi::ThreediLod &lod = parsed.lods[0];
	for (uint32_t i = 0; i < lod.vertices.count; ++i) {
		double words[3];
		mission_vertex_words(lod.vertices.items[i].position, words);
		for (int axis = 0; axis < 2; ++axis) {
			lo[axis] = std::min(lo[axis], words[axis]);
			hi[axis] = std::max(hi[axis], words[axis]);
		}
	}
	opennova::threedi::threedi_3di3_free(&parsed);
	std::printf("crate: %zu edges, %zu hull corners, %zu triangles, %zu points\n", outline.edges.size() / 6,
			outline.hull.size() / 2, outline.triangles.size() / 9, outline.points.size() / 3);
	TEST_EXPECT(outline.edges.size() / 6 == 4 && outline.hull.size() / 2 == 4);
	const auto on = [](double v, double a, double b) { return near(v, a, 1e-4) || near(v, b, 1e-4); };
	for (size_t i = 0; i + 1 < outline.hull.size(); i += 2)
		TEST_EXPECT(on(outline.hull[i], lo[0], hi[0]) && on(outline.hull[i + 1], lo[1], hi[1]));
	for (size_t i = 0; i + 5 < outline.edges.size(); i += 6) {
		// Each side of the plan: along one axis, at the other's edge.
		const bool along_forward = near(outline.edges[i + 1], outline.edges[i + 4], 1e-4);
		const bool along_left = near(outline.edges[i], outline.edges[i + 3], 1e-4);
		TEST_EXPECT(along_forward != along_left);
		TEST_EXPECT(along_forward ? on(outline.edges[i + 1], lo[1], hi[1]) : on(outline.edges[i], lo[0], hi[0]));
	}
	TEST_EXPECT(near(outline.reach, std::max(std::max(std::hypot(lo[0], lo[1]), std::hypot(hi[0], hi[1])),
	                                         std::max(std::hypot(lo[0], hi[1]), std::hypot(hi[0], lo[1]))), 1e-4));
	// From the side: the box's elevation, four sides of (forward, up), its hull the four corners.
	opennova::threedi::Threedi3di3 again{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &again) == 0);
	MissionModelOutline side;
	TEST_EXPECT(mission_model_outline(again, side, MissionOutlineView::Side) && side.view == MissionOutlineView::Side);
	double up_lo = 1e9, up_hi = -1e9;
	for (uint32_t i = 0; i < again.lods[0].vertices.count; ++i) {
		double words[3];
		mission_vertex_words(again.lods[0].vertices.items[i].position, words);
		up_lo = std::min(up_lo, words[2]);
		up_hi = std::max(up_hi, words[2]);
	}
	opennova::threedi::threedi_3di3_free(&again);
	TEST_EXPECT(side.edges.size() / 6 == 4 && side.hull.size() / 2 == 4);
	for (size_t i = 0; i + 1 < side.hull.size(); i += 2)
		TEST_EXPECT(on(side.hull[i], lo[0], hi[0]) && on(side.hull[i + 1], up_lo, up_hi));
	std::printf("test_outline_of_a_box passed\n");
	return 0;
}

// The models on the map: each entity whose item draws a model of the project placed as the game places it (its
// footprint's corners the model's carried by the placement matrix at its angles and SCALE, mission_anchor_offset),
// its wireframe's edges the device's lines; close enough to read, picked by its footprint away from its anchor and
// boxed by a box over a corner of it; far, a pin again; its kind hidden, its lines go.
static int test_footprints() {
	Rig rig("opennova_editor_mission_map_footprints");
	TEST_EXPECT(rig.open(true, true));
	if (!rig.map()) return 1;
	for (int i = 0; i < 50 && rig.map()->outlines_pending() > 0; ++i) rig.pump();
	TEST_EXPECT(rig.map()->outlines_pending() == 0 && rig.map()->outline_files_read() > 0);
	const MissionScene &scene = rig.map()->scene();
	const std::vector<MissionMapFootprint> &footprints = rig.map()->footprints();
	TEST_EXPECT(footprints.size() == scene.entities().size());
	// The crate (the pumps' items): four edges.
	int crate = -1;
	size_t outlined = 0;
	for (size_t i = 0; i < footprints.size(); ++i) {
		if (!footprints[i].outline) continue;
		++outlined;
		if (crate < 0 && footprints[i].outline->edges.size() == 24 && scene.entities()[i].pool == MissionPool::Item)
			crate = int(i);
	}
	std::printf("footprints: %zu of %zu entities, %zu edges drawn\n", outlined, footprints.size(),
			rig.map()->outline_rgb().size());
	TEST_EXPECT(crate >= 0 && outlined > 1);
	if (crate < 0) return 1;
	const MissionEntityMark entity = scene.entities()[size_t(crate)];
	const MissionMapFootprint &footprint = footprints[size_t(crate)];
	for (size_t k = 0; k + 1 < footprint.outline->hull.size(); k += 2) {
		const double words[3] = { footprint.outline->hull[k], footprint.outline->hull[k + 1], 0.0 };
		double offset[3];
		mission_anchor_offset(words, 0, double(entity.pitch), double(entity.yaw), double(entity.roll), offset);
		bool found = false;
		for (size_t h = 0; h + 1 < footprint.hull.size(); h += 2)
			found = found || (near(footprint.hull[h], entity.x + offset[0], 1e-3) &&
			                  near(footprint.hull[h + 1], entity.y + offset[1], 1e-3));
		TEST_EXPECT(found);
	}
	const size_t edges = rig.map()->outline_rgb().size();
	TEST_EXPECT(rig.map()->outline_lines().size() == edges * 4 && edges >= 4);
	// The largest model close: 120 pixels across the picture.
	size_t largest = 0;
	for (size_t i = 0; i < footprints.size(); ++i)
		if (footprints[i].outline && footprints[i].reach > footprints[largest].reach) largest = i;
	const MissionEntityMark big = scene.entities()[largest];
	const MissionMapFootprint shape = footprints[largest];
	const ViewportState picture = rig.map()->size();
	const double metres = shape.reach * 2.0 * double(picture.width) / 120.0;
	char camera[200];
	std::snprintf(camera, sizeof(camera), R"({"kind": "map", "camera": {"center": [%.6f, %.6f], "zoom": %.6f}})",
			shape.centre[0], shape.centre[1], std::max(double(kMissionMapZoomMin), double(mission_map_zoom_for(metres, picture.width))));
	rig.session.handle(request::set_viewport(kMission, camera));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const ViewportContext context = rig.context();
	const MissionMapView view = rig.map()->view(context.width, context.height);
	const std::vector<MissionMapMark> marks = rig.map()->marks(context.width, context.height);
	const MissionMapMark &mark = marks[largest];
	TEST_EXPECT(mark.outlined && mark.footprint == &rig.map()->footprints()[largest] && mark.shown);
	// A press on the model near its first corner, past the pin's slop from its anchor: the largest model's, or one on
	// it (a smaller footprint over it takes the press first).
	float cx = 0.0f, cy = 0.0f, ax = 0.0f, ay = 0.0f;
	double px = shape.centre[0] + 0.9 * (shape.hull[0] - shape.centre[0]);
	double py = shape.centre[1] + 0.9 * (shape.hull[1] - shape.centre[1]);
	view.project(px, py, cx, cy);
	view.project(big.x, big.y, ax, ay);
	TEST_EXPECT(std::hypot(cx - ax, cy - ay) > kMissionPickSlop);
	const ViewportHit hit = rig.map()->hit(context, cx, cy);
	std::printf("press at %.1f,%.1f (anchor %.1f,%.1f): record %llu (the largest %llu)\n", cx, cy, ax, ay,
			(unsigned long long)hit.id, (unsigned long long)big.row);
	TEST_EXPECT(hit.id == big.row);
	// Nothing near the model: no record.
	TEST_EXPECT(rig.map()->hit(context, 2.0f, 2.0f).id == 0 || rig.map()->hit(context, 2.0f, 2.0f).id != big.row);
	// A box over that corner alone, the anchor outside it.
	float kx = 0.0f, ky = 0.0f;
	view.project(shape.hull[0], shape.hull[1], kx, ky);
	bool boxed = false;
	for (const ViewportHit &each : rig.map()->box(context, kx - 4.0f, ky - 4.0f, kx + 4.0f, ky + 4.0f))
		boxed = boxed || each.id == big.row;
	TEST_EXPECT(boxed && !(ax >= kx - 4.0f && ax <= kx + 4.0f && ay >= ky - 4.0f && ay <= ky + 4.0f));
	// The same box with the map turned half a turn (the header's RotateMap180): the corner where the turned view draws
	// it, the box's corners unprojected west of east and south of north whatever the turn, the footprint taken by the
	// wire's box and by the canvas's box alike.
	{
		const NodeAddress header = first_of(*rig.document(), MissionKind::Mission);
		Edit turn;
		turn.operation = EditOperation::Set;
		turn.address = header;
		turn.field = "attrib_flags";
		turn.value = int64_t(0x20);
		rig.session.handle(request::edit_record(kMission, turn));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		const ViewportContext turned_context = rig.context();
		const MissionMapView turned = rig.map()->view(turned_context.width, turned_context.height);
		TEST_EXPECT(turned.flip_180);
		float tx = 0.0f, ty = 0.0f, tax = 0.0f, tay = 0.0f;
		turned.project(shape.hull[0], shape.hull[1], tx, ty);
		turned.project(big.x, big.y, tax, tay);
		bool turned_boxed = false;
		for (const ViewportHit &each : rig.map()->box(turned_context, tx - 4.0f, ty - 4.0f, tx + 4.0f, ty + 4.0f))
			turned_boxed = turned_boxed || each.id == big.row;
		TEST_EXPECT(turned_boxed && !(tax >= tx - 4.0f && tax <= tx + 4.0f && tay >= ty - 4.0f && tay <= ty + 4.0f));
		const std::vector<MissionMapMark> turned_marks = rig.map()->marks(turned_context.width, turned_context.height);
		bool canvas_boxed = false;
		for (const NodeAddress &record : mission_map_box_records(turned_marks, turned, CanvasPoint{ tx + 4.0f, ty + 4.0f },
				     CanvasPoint{ tx - 4.0f, ty - 4.0f }))
			canvas_boxed = canvas_boxed || record.row == big.row;
		TEST_EXPECT(canvas_boxed);
		rig.session.handle(request::undo(kMission));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(!rig.map()->view(turned_context.width, turned_context.height).flip_180);
	}
	// Far: the crate a speck, a pin again.
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "camera": {"zoom": 40}})"));
	rig.pump();
	TEST_EXPECT(!rig.map()->marks(context.width, context.height)[size_t(crate)].outlined);
	// The items hidden: their lines go.
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "map", "options": {"marks": {"items": false}}})"));
	rig.pump();
	TEST_EXPECT(rig.map()->outline_rgb().size() < edges);
	std::printf("test_footprints passed\n");
	return 0;
}

// The palette's pictures (S23 C): an item's model seen from the side, made off the frame: asked and queued (none yet),
// made by a step, then answered while its file's stamp stands (made once); a file that is no model reads as none.
// The header's terrain, tile set or environment edited: the map's device loads its ground again (a Rebuild), the
// 3D view's alike; another header field (the wind) reaches neither as a Rebuild.
static int test_header_rebuilds() {
	Rig rig("opennova_editor_mission_map_header");
	TEST_EXPECT(rig.open());
	const MissionMapViewport *map = rig.map();
	TEST_EXPECT(map != nullptr && rig.devices.last(rig.path, ViewportKind::Map) == ViewportAction::Rebuild);
	rig.pump();
	TEST_EXPECT(rig.devices.last(rig.path, ViewportKind::Map) == ViewportAction::Keep);
	const NodeAddress mission = first_of(*rig.document(), MissionKind::Mission);
	TEST_EXPECT(mission.row != 0);
	bool served = true;
	const auto set = [&](const char *field, Value value) -> ViewportAction {
		Edit edit;
		edit.operation = EditOperation::Set;
		edit.address = mission;
		edit.field = field;
		edit.value = std::move(value);
		rig.session.handle(request::edit_record(kMission, edit));
		served = served && rig.session.outcome().done();
		rig.pump();
		return rig.devices.last(rig.path, ViewportKind::Map);
	};
	TEST_EXPECT(set("terrain", std::string("Bmap")) == ViewportAction::Rebuild);
	TEST_EXPECT(map->scene().header().terrain == "Bmap");
	TEST_EXPECT(set("environment", std::string("other.env")) == ViewportAction::Rebuild);
	TEST_EXPECT(set("wind_speed", int64_t(7)) != ViewportAction::Rebuild);
	// The header's RotateMap180 (attribute 0x20): the map turned half a turn, the device's CMAP pass with it (an
	// Update: the terrain stands).
	TEST_EXPECT(!map->ground().flip_180 && !map->view(800, 600).flip_180);
	TEST_EXPECT(set("attrib_flags", int64_t(0x20)) == ViewportAction::Update);
	TEST_EXPECT(map->ground().flip_180 && map->view(800, 600).flip_180);
	TEST_EXPECT(served);
	std::printf("test_header_rebuilds passed\n");
	return 0;
}

static int test_palette_pictures() {
	Rig rig("opennova_editor_mission_map_palette");
	TEST_EXPECT(rig.open(true, true));
	const SessionView &view = rig.session.view();
	MissionPalettePictures pictures;
	TEST_EXPECT(pictures.get(view, "models/crate.3di") == nullptr && pictures.pending());
	TEST_EXPECT(pictures.step(view, 1000000) && !pictures.pending() && pictures.made() == 1);
	const auto crate = pictures.get(view, "models/crate.3di");
	TEST_EXPECT(crate && crate->read && crate->lines.size() == 16 && crate->hi[0] > crate->lo[0] && crate->hi[1] > crate->lo[1]);
	TEST_EXPECT(!pictures.pending() && !pictures.step(view, 1000000) && pictures.made() == 1);
	pictures.get(view, "missions/synth_logic.bms");
	TEST_EXPECT(pictures.step(view, 1000000));
	const auto none = pictures.get(view, "missions/synth_logic.bms");
	TEST_EXPECT(none && !none->read && none->lines.empty());
	std::printf("test_palette_pictures passed\n");
	return 0;
}

// A model of a field of unit boxes on each LOD (`boxes[l]` on LOD l, two units apart in rows of eight, its thresholds
// falling to 0), through the engine's own construction API and 3DI writer: seen from above a box is its plan's four
// sides (the crate's), so LOD l's plan has 4 x boxes[l] edges.
std::vector<uint8_t> box_field_model(const char *name, const std::vector<int> &boxes) {
	synth3di::Model m;
	m.name = name;
	const int material = m.add_material("FF_ST_OP", "field.tga");
	for (size_t l = 0; l < boxes.size(); ++l) {
		const int lod = m.add_lod(int32_t(32 * (boxes.size() - 1 - l)));
		const int part = m.add_part(lod, 0, synth3di::Vec3{});
		for (int b = 0; b < boxes[l]; ++b) {
			const double x = 2.0 * (b % 8), y = 2.0 * (b / 8);
			m.add_box(lod, part, material, synth3di::Box{ { x, y, 0.0 }, { x + 1.0, y + 1.0, 1.0 } });
		}
		m.add_panm(lod, 0, 0);
	}
	std::vector<uint8_t> bytes;
	if (!synth3di::mint(m, bytes)) bytes.clear();
	return bytes;
}

// The edge cap and the chunks over a mission of the scale S23 C measured the map on (JO:CA's 00TRa, measured once:
// 1,333 entity records, 141 models outlined, six of them drawn by their fewest plan, 79,903 lines in 20 chunks; its
// models' plans drawn by LOD 0 within the cap, by the first coarser LOD within it, by an only LOD past it and by the
// fewest of several past it), over minted models, one of each: a post (LOD 0 within the cap), a palm (LOD 0 and 1
// past it, drawn by LOD 2, the first within it, though LOD 3 has fewer), a ruin (one LOD, past it, drawn by that) and
// a hut (every LOD past it, drawn by the fewest, LOD 2), placed in turn by 1,331 records over the mission (every
// seventh pitched, every seventh from the fourth rolled: a tilted entity's footprint the hull of its every point), then
// an item of no graphic and one of a model the project lacks: 1,333 records. Its outlines read over the follows, a few
// each within the map's budgets, to the end: every model outlined by the plan the cap gives it, the lines its records'
// plans' edges, kMissionMapLinesChunk edges a chunk; a tilted record's footprint not its level one's. Then a drag of the first outlined item over 20 samples of one gesture: each
// sample's edit served and followed places that one record again and moves the chunks its edges lie in alone.
static int test_edge_cap_and_chunks() {
	Rig rig("opennova_editor_mission_map_cap");
	rig.session.handle(request::new_project(rig.dir.file("project"), "Maps"));
	rig.session.run_operations();
	editor_test::create_missing_files(rig.session);
	const std::string project = rig.session.view().project.root;
	struct Shape {
		const char *name;
		int64_t item;
		std::vector<int> boxes; // by LOD
		int lod;                // the LOD its plan is drawn by
	};
	const Shape shapes[] = { { "post", 106300, { 1 }, 0 }, { "palm", 106301, { 60, 50, 10, 2 }, 2 },
		{ "ruin", 106302, { 50 }, 0 }, { "hut", 106303, { 70, 52, 49, 55 }, 2 } };
	std::string items;
	for (const Shape &shape : shapes) {
		TEST_EXPECT(test_io::write_file(project + "/models/" + shape.name + ".3di", box_field_model(shape.name, shape.boxes)));
		items += "begin \"" + std::string(shape.name) + "\"\nid " + std::to_string(shape.item) + "\ntype decoration\ngraphic " +
		         shape.name + "\nend\n";
	}
	items += "begin \"Bare\"\nid 106304\ntype decoration\nend\nbegin \"Lost\"\nid 106305\ntype decoration\ngraphic lost\nend\n";
	TEST_EXPECT(test_io::write_text(project + "/defs/items.def", items));
	// The four models in turn over a grid 20 units apart, some tilted, then the two items that draw none.
	opennova::bms::File file;
	opennova::mission::make_default(file);
	const int64_t order[] = { 106300, 106301, 106302, 106303 };
	for (int i = 0; i < 1333; ++i) {
		const int64_t item = i < 1331 ? order[i % 4] : 106304 + (i - 1331);
		opennova::bms::Entity entity = opennova::mission::new_entity(opennova::mission::EntityKind::Item, int(item), 100 + i);
		entity.x = opennova::bms::to_fixed_16_16(-400.0 + 20.0 * (i % 40));
		entity.y = opennova::bms::to_fixed_16_16(-400.0 + 20.0 * (i / 40));
		entity.pitch = int16_t(i % 7 == 0 ? 10 : 0);
		entity.roll = int16_t(i % 7 == 3 ? 5 : 0);
		file.items.push_back(entity);
	}
	opennova::mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	std::string error;
	TEST_EXPECT(opennova::bms::write(file, bytes, error) && test_io::write_file(project + "/missions/field.bms", bytes));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.session.handle(request::open_document("missions/field.bms"));
	TEST_EXPECT(rig.session.outcome().done());
	if (!rig.session.outcome().done()) return 1;
	rig.path = rig.session.document_for("missions/field.bms")->path();
	rig.pump();
	const MissionMapViewport *map = rig.map();
	TEST_EXPECT(map && map->status() == ViewportStatus::Ready);
	if (!map) return 1;
	int follows = 0;
	do {
		rig.pump();
		++follows;
	} while ((map->outlines_pending() > 0 || map->outline_lines().empty()) && follows < 4000);
	TEST_EXPECT(map->outlines_pending() == 0);
	// Each model outlined by the plan the cap gives it; every plan within the cap but those whose every LOD's plan is
	// past it (the ruin and the hut, drawn by the fewest); the two items of no model outlined by none.
	const std::vector<MissionEntityMark> &entities = map->scene().entities();
	TEST_EXPECT(entities.size() == 1333 && map->footprints().size() == entities.size());
	std::set<const MissionModelOutline *> outlined;
	size_t past_cap = 0, edges = 0, tilted = 0;
	for (size_t i = 0; i < entities.size() && i < map->footprints().size(); ++i) {
		const MissionMapFootprint &footprint = map->footprints()[i];
		const Shape *shape = nullptr;
		for (const Shape &each : shapes) shape = each.item == entities[i].item ? &each : shape;
		TEST_EXPECT((shape != nullptr) == (footprint.outline != nullptr));
		if (!shape || !footprint.outline) continue;
		TEST_EXPECT(footprint.outline->lod == shape->lod &&
		            footprint.outline->edges.size() / 6 == size_t(4 * shape->boxes[size_t(shape->lod)]));
		edges += footprint.outline->edges.size() / 6;
		// A tilted record's footprint: the hull of its every point placed, not its level hull's.
		if (entities[i].pitch != 0.0f || entities[i].roll != 0.0f) {
			const MissionMapFootprint level = mission_map_footprint(footprint.outline, entities[i].x, entities[i].y, 0.0,
					double(entities[i].yaw), 0.0, 0);
			TEST_EXPECT(footprint.hull.size() >= 6 && footprint.hull != level.hull);
			++tilted;
		}
		if (outlined.insert(footprint.outline.get()).second && footprint.outline->edges.size() / 6 > kMissionOutlineEdgesMax)
			++past_cap;
	}
	const size_t lines = map->outline_lines().size() / 4;
	std::printf("cap: %zu entities, %zu outlined, %zu past the %zu-edge cap, %zu lines in %zu chunks, %d follows\n",
	            entities.size(), outlined.size(), past_cap, kMissionOutlineEdgesMax, lines, map->outline_chunks().size(), follows);
	TEST_EXPECT(outlined.size() == 4 && past_cap == 2 && tilted == 381);
	// 333 posts, palms and ruins and 332 huts: 4, 40, 200 and 196 edges each.
	TEST_EXPECT(lines == edges && lines == 333 * (4 + 40 + 200) + 332 * 196);
	TEST_EXPECT(map->outline_chunks().size() ==
	            (lines + MissionMapViewport::kMissionMapLinesChunk - 1) / MissionMapViewport::kMissionMapLinesChunk);
	// A drag of the first outlined item over 20 samples of one gesture, then its end.
	NodeAddress dragged;
	for (size_t i = 0; i < entities.size() && !dragged.row; ++i)
		if (map->footprints()[i].outline && entities[i].pool == MissionPool::Item)
			dragged = NodeAddress{ entities[i].row, entities[i].kind, 0 };
	TEST_EXPECT(dragged.row != 0);
	if (!dragged.row) return 1;
	const std::vector<uint64_t> chunks_before = map->outline_chunks();
	size_t chunks_moved = 0;
	for (int sample = 1; sample <= 20; ++sample) {
		ViewportDrag drag;
		drag.id = dragged.row;
		drag.handle = "move";
		drag.by = true;
		drag.x = 2.0f * float(sample);
		drag.y = 0.0f;
		drag.snap = 0.0f;
		drag.end = sample == 20;
		editor_test::Gathered gathered;
		TEST_EXPECT(map->drag(rig.context(), drag, gathered, error));
		TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
		rig.pump();
		TEST_EXPECT(map->outlines_placed() == 1);
	}
	const std::vector<uint64_t> &chunks_after = map->outline_chunks();
	TEST_EXPECT(chunks_after.size() == chunks_before.size());
	for (size_t i = 0; i < chunks_after.size() && i < chunks_before.size(); ++i) chunks_moved += chunks_after[i] != chunks_before[i];
	TEST_EXPECT(chunks_moved >= 1 && chunks_moved <= 2);
	std::printf("cap drag: 20 samples, %zu of %zu chunks moved\n", chunks_moved, chunks_after.size());
	std::printf("test_edge_cap_and_chunks passed\n");
	return 0;
}

int main() {
	int failed = 0;
	failed += test_kind_row();
	failed += test_workspace_pins();
	failed += test_projection();
	failed += test_tile_texels();
	failed += test_marks_hit_box();
	failed += test_drag();
	failed += test_links();
	failed += test_group_colours();
	failed += test_area_handles();
	failed += test_pick();
	failed += test_keys();
	failed += test_commands_and_wire();
	failed += test_grid_origin();
	failed += test_outline_of_a_box();
	failed += test_footprints();
	failed += test_header_rebuilds();
	failed += test_palette_pictures();
	failed += test_edge_cap_and_chunks();
	return failed == 0 ? 0 : 1;
}
