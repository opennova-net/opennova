// ADR 0046 S14: the mission viewport's portable half that reads no document type of its own: the
// scene (editor/preview/mission_scene) over a source a test holds, its marks and picks, the options'
// wire form (editor/preview/mission_options) and the overlays (editor/preview/mission_overlay).
//
// The scene reads everything once (every row counted), indexes rows, and places each entity in
// the presentation frame; a patch reads the changed rows alone: an entity's transform is a move
// (its stamp moves), its item a reshape, its team the overlays, an area's or a path's fields the
// overlays, the header row's terrain the header, an event's row nothing (and no row read); a row
// added or removed reshapes (everything read again, the delta from what differs). The marks: each
// entity at the pixel the camera projects its anchor to, an area at its centre (the ground where a
// device answers, else its z_min); a pick finds the front-most within the slop; a kind's marks off,
// or a mark past the mark range, is not shown and not picked; a box answers the records inside it,
// nearest first. The options round-trip and refuse what they do not take. The overlays: a glyph per
// pool, the team ring, the hover and selected rings, the primary's handles (a yaw-90 entity's yaw
// handle east of it), an area's footprint and its z box, a path's lines (closed unless it does not
// loop, thick when selected), the labels, the marquee, and a line clipped at the near plane. The
// labels' declutter (S15): what always draws, what draws first, what fits.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/mission_labels.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_overlay.h>
#include <formats/mission/bms.h>

#include "common/test_expect.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;

namespace {

constexpr NodeKind kItemKind = 1, kBuildingKind = 2, kMarkerKind = 3, kOrganicKind = 4, kAreaKind = 6, kPathKind = 5;
constexpr NodeId kHeaderRow = 1, kEventRow = 90;

// A source over what it holds: three entities, one area, one path.
struct Source final : MissionSceneSource {
	MissionSceneHeader head;
	std::vector<MissionEntityMark> entity_rows;
	std::vector<MissionAreaMark> area_rows;
	std::vector<MissionPathMark> path_rows;
	bool header(MissionSceneHeader &out) const override {
		out = head;
		return true;
	}
	bool header_row(NodeId row) const override { return row == kHeaderRow; }
	void entities(std::vector<MissionEntityMark> &out) const override { out = entity_rows; }
	void areas(std::vector<MissionAreaMark> &out) const override { out = area_rows; }
	void paths(std::vector<MissionPathMark> &out) const override {
		out.clear();
		for (const MissionPathMark &path : path_rows)
			if (!path.stops.empty()) out.push_back(path);
	}
	// One row's read, as the document's is: it knows no place in a pool (its index 0).
	bool entity(NodeId row, MissionEntityMark &out) const override {
		for (const MissionEntityMark &each : entity_rows)
			if (each.row == row) {
				out = each;
				out.index = 0;
				return true;
			}
		return false;
	}
	bool area(NodeId row, MissionAreaMark &out) const override {
		for (const MissionAreaMark &each : area_rows)
			if (each.row == row) {
				out = each;
				out.index = 0;
				return true;
			}
		return false;
	}
	bool path(NodeId row, MissionPathMark &out) const override {
		for (const MissionPathMark &each : path_rows)
			if (each.row == row && !each.stops.empty()) {
				out = each;
				return true;
			}
		return false;
	}
	MissionEntityMark *entity_at(NodeId row) {
		for (MissionEntityMark &each : entity_rows)
			if (each.row == row) return &each;
		return nullptr;
	}
};

MissionEntityMark entity_of(NodeId row, NodeKind kind, MissionPool pool, int index, int64_t item, double x, double y,
		double z, int yaw, int team) {
	MissionEntityMark mark;
	mark.row = row;
	mark.kind = kind;
	mark.pool = pool;
	mark.index = index;
	mark.item = item;
	mark.x = x;
	mark.y = y;
	mark.z = z;
	mark.yaw = yaw;
	mark.team = team;
	return mark;
}

Source make_source() {
	Source source;
	source.head.terrain = "Tmap";
	source.head.environment = "synth_full";
	source.head.start_time = 720;
	source.entity_rows = {
		entity_of(10, kItemKind, MissionPool::Item, 0, 100300, 0.0, 0.0, 0.0, 0, 1),
		entity_of(11, kBuildingKind, MissionPool::Building, 7, 105004, 100.0, 0.0, 0.0, 90, 0),
		entity_of(12, kMarkerKind, MissionPool::Marker, 0, 4093, 0.0, 100.0, 0.0, 0, 2),
		entity_of(13, kOrganicKind, MissionPool::Organic, 2, 110001, -50.0, -50.0, 2.0, 180, 2),
	};
	MissionAreaMark area;
	area.row = 20;
	area.kind = kAreaKind;
	area.zone = 5;
	area.index = 3;
	area.min[0] = 10.0;
	area.max[0] = 30.0;
	area.min[1] = 10.0;
	area.max[1] = 30.0;
	area.min[2] = 0.0;
	area.max[2] = 8.0;
	area.constrains_z = true;
	source.area_rows = { area };
	MissionPathMark path;
	path.row = 30;
	path.kind = kPathKind;
	path.index = 3;
	path.flags = uint32_t(opennova::bms::WaypointFlags::DoesNotLoop) | uint32_t(opennova::bms::WaypointFlags::RedTeam);
	path.stops = { 12, 10, 0 };
	MissionPathMark empty;
	empty.row = 31;
	empty.kind = kPathKind;
	empty.index = 4;
	source.path_rows = { path, empty };
	return source;
}

RowChanges changed(std::initializer_list<NodeId> rows) {
	RowChanges changes;
	changes.changed = rows;
	std::sort(changes.changed.begin(), changes.changed.end());
	return changes;
}

bool near(double a, double b, double slack = 1e-4) { return std::fabs(a - b) <= slack; }

// A camera above the origin looking down and north, 640 x 480.
OrbitCamera camera_over(double x, double y, double z, float distance) {
	OrbitCamera camera;
	camera.target = mission_scene_point(x, y, z);
	camera.yaw = 0.0f;
	camera.pitch = 1.2f;
	camera.distance = distance;
	camera.far_plane = 5000.0f;
	return camera;
}

int test_scene() {
	Source source = make_source();
	MissionScene scene;
	TEST_EXPECT(scene.rows_read() == 0 && scene.serial() == 0 && scene.entities().empty());
	scene.read(source);
	// The header, four entities, one area and the one path with a stop: six rows and the header.
	TEST_EXPECT(scene.rows_read() == 7 && scene.serial() == 1);
	TEST_EXPECT(scene.header().terrain == "Tmap" && scene.header().start_time == 720);
	TEST_EXPECT(scene.entities().size() == 4 && scene.areas().size() == 1 && scene.paths().size() == 1);
	TEST_EXPECT(scene.count(MissionPool::Item) == 1 && scene.count(MissionPool::Organic) == 1);
	const MissionEntityMark *building = scene.entity(11);
	TEST_EXPECT(building && building->pool == MissionPool::Building && building->yaw == 90 && building->stamp == 1);
	TEST_EXPECT(building->at.x == 100.0f && building->at.y == 0.0f && building->at.z == 0.0f);
	const MissionEntityMark *marker = scene.entity(12);
	TEST_EXPECT(marker && marker->at.x == 0.0f && marker->at.z == -100.0f); // north is -z
	TEST_EXPECT(scene.area(20) && scene.area(20)->zone == 5 && scene.path(30) && scene.path(30)->stops.size() == 3);
	TEST_EXPECT(!scene.entity(20) && !scene.area(10) && !scene.path(31));

	// An entity's transform: a move, its stamp moved, one row read; its item: a reshape; its team:
	// the overlays; an event's row: nothing, no row read.
	source.entity_at(11)->x = 120.0;
	MissionSceneDelta delta = scene.patch(changed({ 11 }), source);
	TEST_EXPECT(delta.moved && !delta.reshaped && !delta.header && !delta.overlays && scene.rows_read() == 8);
	TEST_EXPECT(scene.entity(11)->x == 120.0 && scene.entity(11)->at.x == 120.0f && scene.entity(11)->stamp == 2 && scene.serial() == 2);
	source.entity_at(11)->item = 105005;
	delta = scene.patch(changed({ 11 }), source);
	TEST_EXPECT(delta.reshaped && !delta.moved && scene.entity(11)->item == 105005 && scene.entity(11)->stamp == 3);
	source.entity_at(10)->team = 2;
	delta = scene.patch(changed({ 10 }), source);
	TEST_EXPECT(delta.overlays && !delta.moved && !delta.reshaped && scene.entity(10)->stamp == 1 && scene.rows_read() == 10);
	// Its attributes or its group (what the placement reads beside the item): a reshape, its stamp
	// moved. A row's read keeps its place in its pool (the whole read's: a one-row read knows none).
	source.entity_at(11)->attributes = 0x01000000u; // NoShadow
	delta = scene.patch(changed({ 11 }), source);
	TEST_EXPECT(delta.reshaped && !delta.moved && scene.entity(11)->attributes == 0x01000000u && scene.entity(11)->stamp == 5);
	source.entity_at(11)->group = 3;
	delta = scene.patch(changed({ 11 }), source);
	TEST_EXPECT(delta.reshaped && scene.entity(11)->group == 3 && scene.entity(11)->stamp == 6);
	TEST_EXPECT(scene.entity(11)->index == 7 && scene.entity(13)->index == 2);
	TEST_EXPECT(scene.mark_index(11) == 1 && scene.mark_index(20) == 4 && scene.mark_index(30) == -1);
	const size_t read = scene.rows_read();
	const uint64_t serial = scene.serial();
	delta = scene.patch(changed({ kEventRow }), source);
	TEST_EXPECT(!delta.any() && scene.rows_read() == read && scene.serial() == serial);
	// The header row changed with the same terrain: nothing but the read; with another: the header.
	delta = scene.patch(changed({ kHeaderRow }), source);
	TEST_EXPECT(!delta.any() && scene.rows_read() == read + 1);
	source.head.terrain = "Bmap";
	delta = scene.patch(changed({ kHeaderRow }), source);
	TEST_EXPECT(delta.header && !delta.reshaped && scene.header().terrain == "Bmap");
	// An area's bounds, a path's stops: the overlays; the empty path given a stop is listed; the
	// listed path losing its stops is dropped.
	source.area_rows[0].max[0] = 40.0;
	delta = scene.patch(changed({ 20 }), source);
	TEST_EXPECT(delta.overlays && !delta.moved && scene.area(20)->max[0] == 40.0 && scene.area(20)->index == 3);
	source.path_rows[1].stops = { 13 };
	delta = scene.patch(changed({ 31 }), source);
	TEST_EXPECT(delta.overlays && scene.paths().size() == 2 && scene.path(31) && scene.paths()[1].index == 4);
	source.path_rows[0].stops.clear();
	delta = scene.patch(changed({ 30 }), source);
	TEST_EXPECT(delta.overlays && scene.paths().size() == 1 && !scene.path(30) && scene.path(31));
	// A row added: a reshape, everything read again; the same for one removed; an order alone the
	// overlays.
	source.entity_rows.push_back(entity_of(14, kItemKind, MissionPool::Item, 1, 100301, 5.0, 5.0, 0.0, 0, 0));
	RowChanges added;
	added.added = { 14 };
	const size_t before = scene.rows_read();
	delta = scene.patch(added, source);
	TEST_EXPECT(delta.reshaped && !delta.moved && scene.entities().size() == 5 && scene.entity(14) &&
			scene.rows_read() == before + 1 + 5 + 1 + 1);
	TEST_EXPECT(scene.entity(10)->stamp == 1); // a row as it was keeps its stamp
	source.entity_rows.erase(source.entity_rows.begin() + 3); // the organic
	RowChanges removed;
	removed.removed = { 13 };
	delta = scene.patch(removed, source);
	TEST_EXPECT(delta.reshaped && scene.entities().size() == 4 && !scene.entity(13));
	RowChanges reordered;
	reordered.reordered = true;
	delta = scene.patch(reordered, source);
	TEST_EXPECT(!delta.reshaped && !delta.moved && delta.overlays);
	scene.clear();
	TEST_EXPECT(scene.entities().empty() && scene.header().terrain.empty());
	std::printf("test_scene passed\n");
	return 0;
}

int test_marks() {
	Source source = make_source();
	MissionScene scene;
	scene.read(source);
	MissionViewportOptions options;
	const OrbitCamera camera = camera_over(0.0, 0.0, 0.0, 400.0f);
	const int width = 640, height = 480;
	std::vector<MissionMark> marks = mission_marks(scene, options, camera, width, height, nullptr);
	TEST_EXPECT(marks.size() == 5 && marks[4].area == 0 && marks[4].entity == -1 && std::string(marks[4].kind) == "area");
	for (size_t i = 0; i < 4; ++i) {
		const MissionMark &mark = marks[i];
		float x = 0.0f, y = 0.0f;
		TEST_EXPECT(mark.entity == int(i) && mark.record.row == scene.entities()[i].row && mark.shown);
		TEST_EXPECT(camera.project(scene.entities()[i].at, width, height, x, y) && x == mark.x && y == mark.y);
		TEST_EXPECT(pick_mission_mark(marks, mark.x + 3.0f, mark.y - 3.0f) == int(i));
	}
	TEST_EXPECT(std::string(marks[0].kind) == "item" && std::string(marks[1].kind) == "building" &&
			std::string(marks[2].kind) == "marker" && std::string(marks[3].kind) == "organic");
	// The area's anchor at its centre at z_min with no device, at the ground with one.
	TEST_EXPECT(marks[4].at.x == 20.0f && marks[4].at.y == 0.0f && marks[4].at.z == -20.0f);
	editor_test::FakeDevice device;
	device.ground = [](double, double) { return 3.5; };
	marks = mission_marks(scene, options, camera, width, height, &device);
	TEST_EXPECT(marks[4].at.y == 3.5f);
	TEST_EXPECT(pick_mission_mark(marks, -100.0f, -100.0f) == -1);
	// Two marks on one ray: the nearer wins.
	source.entity_rows.push_back(entity_of(15, kItemKind, MissionPool::Item, 1, 1, 0.0, 0.0, 50.0, 0, 0));
	scene.read(source);
	OrbitCamera above = camera_over(0.0, 0.0, 0.0, 300.0f);
	above.pitch = kOrbitPitchLimit; // straight down: the origin and 50 above it on one ray
	marks = mission_marks(scene, options, above, width, height, nullptr);
	TEST_EXPECT(marks[0].shown && marks[4].shown && std::fabs(marks[0].x - marks[4].x) < 0.5f);
	TEST_EXPECT(pick_mission_mark(marks, marks[0].x, marks[0].y) == 4 && marks[4].depth < marks[0].depth);
	// A kind's marks off: not shown, not picked; past the range the same; the range 0 is none.
	options.items = false;
	marks = mission_marks(scene, options, above, width, height, nullptr);
	TEST_EXPECT(!marks[0].shown && !marks[4].shown && marks[1].shown && pick_mission_mark(marks, marks[0].x, marks[0].y) == -1);
	options.items = true;
	options.mark_range = 280.0f;
	marks = mission_marks(scene, options, above, width, height, nullptr);
	TEST_EXPECT(!marks[0].shown && marks[4].shown); // the origin is 300 away, the one above it 250
	options.mark_range = 0.0f;
	marks = mission_marks(scene, options, above, width, height, nullptr);
	TEST_EXPECT(marks[0].shown);
	// A box: the records inside it, nearest first.
	const std::vector<NodeAddress> boxed = mission_box_records(marks, CanvasPoint{ marks[0].x + 2.0f, marks[0].y + 2.0f },
			CanvasPoint{ marks[0].x - 2.0f, marks[0].y - 2.0f });
	TEST_EXPECT(boxed.size() == 2 && boxed[0].row == 15 && boxed[1].row == 10);
	TEST_EXPECT(mission_box_records(marks, CanvasPoint{ 0.0f, 0.0f }, CanvasPoint{ float(width), float(height) }).size() ==
			scene.entities().size() + 1);
	TEST_EXPECT(mission_box_records(marks, CanvasPoint{ -5.0f, -5.0f }, CanvasPoint{ -1.0f, -1.0f }).empty());
	std::printf("test_marks passed\n");
	return 0;
}

int test_options() {
	MissionViewportOptions options;
	options.labels = true;
	options.water = false;
	options.mark_range = 250.0f;
	options.stick = false;
	options.time = 6.5;
	const opennova::io::JsonValue json = mission_options_to_json(options);
	TEST_EXPECT(json.get("show")->get_bool("water", true) == false && json.get("marks")->get_bool("labels", false) &&
			json.get_number("mark_range", 0) == 250.0 && json.get_bool("stick", true) == false && json.get_number("time", 0) == 6.5);
	MissionViewportOptions read;
	std::string error;
	TEST_EXPECT(mission_options_from_json(json, read, error) && read == options);
	TEST_EXPECT(mission_options_to_json(MissionViewportOptions()).get("time")->is_null());
	const auto refused = [&](const char *text, const char *says) {
		opennova::io::JsonValue bad;
		opennova::io::json_parse(text, bad, error);
		MissionViewportOptions held = read;
		const bool ok = !mission_options_from_json(bad, held, error) && error.find(says) != std::string::npos && held == read;
		if (!ok) std::printf("  %s: %s\n", text, error.c_str());
		return ok;
	};
	TEST_EXPECT(refused(R"({"show": {"fog": true}})", "Unknown options.show member"));
	TEST_EXPECT(refused(R"({"marks": {"items": 1}})", "options.marks.items is true or false"));
	TEST_EXPECT(refused(R"({"mark_range": -1})", "options.mark_range"));
	TEST_EXPECT(refused(R"({"time": 25})", "options.time"));
	TEST_EXPECT(refused(R"({"stick": "yes"})", "options.stick"));
	TEST_EXPECT(refused(R"({"camera": {}})", "Unknown options member"));
	TEST_EXPECT(refused(R"({"show": {"sky": false}, "time": -3})", "options.time"));
	opennova::io::JsonValue partial;
	opennova::io::json_parse(R"({"time": null, "marks": {"paths": false}})", partial, error);
	TEST_EXPECT(mission_options_from_json(partial, read, error) && read.time == -1.0 && !read.paths && read.labels);
	std::printf("test_options passed\n");
	return 0;
}

int test_overlays() {
	Source source = make_source();
	MissionScene scene;
	scene.read(source);
	MissionViewportOptions options;
	const OrbitCamera camera = camera_over(0.0, 0.0, 0.0, 400.0f);
	const int width = 640, height = 480;
	const std::vector<MissionMark> marks = mission_marks(scene, options, camera, width, height, nullptr);
	MissionOverlayInput in;
	in.scene = &scene;
	in.options = &options;
	in.camera = &camera;
	in.width = width;
	in.height = height;
	in.marks = &marks;
	// The shapes of a kind and a role (a Circle: a ring, never the yaw handle's filled disc).
	const auto count = [](const OverlayList &list, OverlayKind kind, OverlayRole role, float thickness = -1.0f) {
		return std::count_if(list.shapes.begin(), list.shapes.end(), [&](const OverlayShape &shape) {
			return shape.kind == kind && shape.role == role && (thickness < 0.0f || shape.thickness == thickness) &&
					!(kind == OverlayKind::Circle && shape.filled);
		});
	};
	const auto glyphs = [](const OverlayList &list, OverlayGlyph glyph, uint32_t rgb) {
		return std::count_if(list.shapes.begin(), list.shapes.end(), [&](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Marker && shape.glyph == glyph && shape.rgb == rgb;
		});
	};
	// The four glyphs, the team rings (three entities on a team), the area's footprint and its z box
	// (4 + 8 + 4 lines), the path's two lines (one stop names no marker; it does not loop), no label.
	OverlayList list = mission_overlay_shapes(in);
	TEST_EXPECT(count(list, OverlayKind::Quad, OverlayRole::Normal) == 1);
	TEST_EXPECT(glyphs(list, OverlayGlyph::Square, kMissionBuildingRgb) == 1 && glyphs(list, OverlayGlyph::Cross, kMissionMarkerRgb) == 1 &&
			glyphs(list, OverlayGlyph::Dot, kMissionOrganicRgb) == 1 && glyphs(list, OverlayGlyph::Square, kMissionAreaRgb) == 1);
	TEST_EXPECT(count(list, OverlayKind::Circle, OverlayRole::Normal) == 3);
	const auto lines = [&](uint32_t rgb, float thickness) {
		return std::count_if(list.shapes.begin(), list.shapes.end(), [&](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Line && shape.rgb == rgb && shape.thickness == thickness;
		});
	};
	TEST_EXPECT(lines(kMissionAreaRgb, 1.0f) == 16 && lines(kMissionRedRgb, 1.0f) == 1 && count(list, OverlayKind::Text, OverlayRole::Normal) == 0);
	TEST_EXPECT(count(list, OverlayKind::Rect, OverlayRole::Marquee) == 0);
	// The path looping and selected by its marker: two lines, thick; its flag's blue.
	source.path_rows[0].flags = uint32_t(opennova::bms::WaypointFlags::BlueTeam);
	scene.read(source);
	std::vector<NodeId> selected_rows = { 12 };
	in.selected_rows = &selected_rows;
	list = mission_overlay_shapes(in);
	TEST_EXPECT(lines(kMissionBlueRgb, 2.5f) == 2);
	in.selected_rows = nullptr;
	// Hovered: a Hover ring; the primary: a thick Selected ring, its handles (the yaw handle of the
	// yaw-90 building east of its anchor, the height handle above it); another selected: a thinner
	// ring; each over a dark ring (S15); labels beside those alone, beside every shown mark with the
	// option.
	in.hover = 0;
	in.primary = 1;
	std::vector<int> selected = { 2 };
	in.selected = &selected;
	in.handle_reach = 10.0f;
	in.title = [&](const NodeAddress &record) { return "row " + std::to_string(record.row); };
	list = mission_overlay_shapes(in);
	TEST_EXPECT(count(list, OverlayKind::Circle, OverlayRole::Hover) == 1);
	TEST_EXPECT(count(list, OverlayKind::Circle, OverlayRole::Selected, 2.5f) == 1 && count(list, OverlayKind::Circle, OverlayRole::Selected, 1.5f) == 1);
	TEST_EXPECT(count(list, OverlayKind::Circle, OverlayRole::Normal) == 3 + 3); // the team rings, a dark ring under each of the three
	TEST_EXPECT(count(list, OverlayKind::Text, OverlayRole::Normal) == 3);
	const PreviewVec3 yaw = mission_yaw_handle(scene.entities()[1], 10.0f);
	TEST_EXPECT(near(yaw.x, 110.0) && near(yaw.y, 0.0) && near(yaw.z, 0.0)); // yaw 90 faces east
	const PreviewVec3 north = mission_yaw_handle(scene.entities()[0], 10.0f);
	TEST_EXPECT(near(north.x, 0.0) && near(north.z, -10.0)); // yaw 0 faces north
	const PreviewVec3 up = mission_height_handle(scene.entities()[1], 10.0f);
	TEST_EXPECT(near(up.y, 10.0) && near(up.x, 100.0));
	float hx = 0.0f, hy = 0.0f;
	TEST_EXPECT(camera.project(yaw, width, height, hx, hy));
	bool disc = false;
	for (const OverlayShape &shape : list.shapes)
		disc = disc || (shape.kind == OverlayKind::Circle && shape.filled && shape.role == OverlayRole::Selected && shape.points[0].x == hx);
	TEST_EXPECT(disc && hx > marks[1].x); // the yaw handle's disc is drawn east of the building on the picture
	TEST_EXPECT(count(list, OverlayKind::Marker, OverlayRole::Selected) == 1); // the height handle's square
	options.labels = true;
	list = mission_overlay_shapes(in);
	TEST_EXPECT(count(list, OverlayKind::Text, OverlayRole::Normal) == 5);
	options.labels = false;
	// The area primary: its four edge handles; the marquee drawn.
	in.primary = 4;
	in.hover = -1;
	selected.clear();
	in.marquee = true;
	in.marquee_from = CanvasPoint{ 50.0f, 60.0f };
	in.marquee_to = CanvasPoint{ 10.0f, 20.0f };
	list = mission_overlay_shapes(in);
	TEST_EXPECT(count(list, OverlayKind::Marker, OverlayRole::Selected) == 4);
	TEST_EXPECT(count(list, OverlayKind::Rect, OverlayRole::Marquee) == 1 && list.shapes.back().points[0].x == 10.0f &&
			list.shapes.back().points[1].y == 60.0f);
	const PreviewVec3 edge = mission_area_edge_middle(scene.areas()[0], MissionHandle::XMax, 0.0);
	TEST_EXPECT(near(edge.x, 30.0) && near(edge.z, -20.0));
	// A line clipped at the near plane: one end behind the eye still draws to the plane; both
	// behind, nothing.
	OrbitCamera low = camera_over(0.0, 0.0, 0.0, 5.0f);
	low.pitch = 0.0f;
	CanvasPoint from, to;
	TEST_EXPECT(mission_project_segment(low, width, height, mission_scene_point(0.0, -50.0, 0.0), mission_scene_point(0.0, 50.0, 0.0), from, to));
	TEST_EXPECT(!mission_project_segment(low, width, height, mission_scene_point(0.0, -50.0, 0.0), mission_scene_point(0.0, -60.0, 0.0), from, to));
	// The paths off: no path line; a mark not shown draws nothing.
	options.paths = false;
	options.buildings = false;
	const std::vector<MissionMark> fewer = mission_marks(scene, options, camera, width, height, nullptr);
	in.marks = &fewer;
	in.primary = -1;
	list = mission_overlay_shapes(in);
	TEST_EXPECT(lines(kMissionBlueRgb, 1.0f) == 0 && glyphs(list, OverlayGlyph::Square, kMissionBuildingRgb) == 0);
	std::printf("test_overlays passed\n");
	return 0;
}

// The labels' declutter (S15): the hovered mark's and the primary's always, even over each other; the
// other selected before the rest but only where they fit, each nearest first; the rest after.
int test_label_picks() {
	const auto candidate = [](float x, float y, float depth, size_t length, bool always, bool first) {
		MissionLabelCandidate label;
		label.x = x;
		label.y = y;
		label.depth = depth;
		label.length = length;
		label.always = always;
		label.first = first;
		return label;
	};
	const std::vector<MissionLabelCandidate> candidates = {
		candidate(0.0f, 0.0f, 50.0f, 10, true, false), // the primary
		candidate(5.0f, 0.0f, 10.0f, 10, false, true), // a selected one over it: not drawn
		candidate(0.0f, 100.0f, 30.0f, 10, false, true), // a selected one clear of it
		candidate(0.0f, 102.0f, 1.0f, 5, false, false), // nearer, but over a selected one's
		candidate(0.0f, 200.0f, 5.0f, 5, false, false), // clear
		candidate(2.0f, 0.0f, 60.0f, 10, true, false), // the hovered, over the primary's: drawn
	};
	const std::vector<size_t> picks = mission_label_picks(candidates);
	TEST_EXPECT((picks == std::vector<size_t>{ 0, 5, 2, 4 }));
	std::printf("test_label_picks passed\n");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_scene() == 0);
	TEST_EXPECT(test_marks() == 0);
	TEST_EXPECT(test_options() == 0);
	TEST_EXPECT(test_overlays() == 0);
	TEST_EXPECT(test_label_picks() == 0);
	std::printf("editor_mission_scene: all tests passed\n");
	return 0;
}
