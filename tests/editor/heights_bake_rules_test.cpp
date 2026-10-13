// The heights' bake rules through the session (ADR 0046 S25, the heights lane's review round): a bake never started
// where it cannot land (a redo kept past the stroke it bakes) and landed as a step of its own where the stroke's step
// cannot take it (saved before the bake, Save over a kept redo), never looping; a bake the TrnGen port refuses (rough
// ground, D-TERRAIN-22) not tried again until the heights change, its finding said and the .cpt not saved; Save
// settling the bake before it chooses what to write, so the missions the landing moved are written with the ground;
// an entity edited while the bake was due kept where it was put; a mission's open drag never split by a follow; a
// closed mission whose moving entities are all height locked left closed; the terrain viewport's device patched in
// place like the mission view's; Flatten off the terrain.
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_heights_plan.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <formats/mission/bms_edit.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

// A mission on Tmap (under `environment`) holding one building (or `kind` of `type`) at each point (mission units), at
// z 4.
std::vector<uint8_t> mission_at(const std::vector<std::pair<float, float>> &points,
                                mission::EntityKind kind = mission::EntityKind::Building, int32_t type = 100701,
                                const char *environment = "day") {
	bms::File file;
	mission::BlankMission blank;
	blank.name = "Rules";
	blank.terrain = "Tmap";
	blank.environment = environment;
	std::string error;
	if (!mission::make_blank(file, blank, error)) return {};
	for (const auto &[x, y] : points) {
		mission::EntityTransform at;
		at.x = x;
		at.y = y;
		at.z = 4.0f;
		mission::add_entity(file, kind, type, at);
	}
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	if (!bms::write(file, bytes, error)) bytes.clear();
	return bytes;
}

// The whole input raised to full-range noise, due for its bake: ground the TrnGen port refuses (D-TERRAIN-22).
struct RoughInput : RasterEditPayload {
	const char *token() const override { return "test.rough"; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override {
		(void)error;
		std::vector<uint16_t> rough(size_t(kHeightsSide) * kHeightsSide);
		uint32_t seed = 0x2545F491u;
		for (uint16_t &h : rough) {
			seed = seed * 1664525u + 1013904223u;
			h = static_cast<uint16_t>(seed >> 16);
		}
		canvas.write(kHeightsInput).set_rect_u16(0, 0, kHeightsSide, kHeightsSide, rough.data(), size_t(kHeightsSide));
		auto state = std::make_shared<HeightsState>(*static_cast<const HeightsState *>(canvas.side()));
		state->unbaked = {HeightsRect{0, 0, kHeightsSide, kHeightsSide}};
		canvas.set_side(std::move(state));
		return true;
	}
};

// A project with Tmap, its .trn and missions on it.
struct Rig {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root, trn, cpt;
	explicit Rig(const char *name, const std::vector<std::pair<std::string, std::vector<std::pair<float, float>>>> &missions = {})
	    : dir(name) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Rules"));
		const SessionView &v = session.view();
		root = v.project.root;
		editor_test::write_bytes(root + "/terrain/Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
		editor_test::write_bytes(root + "/terrain/Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
		editor_test::write_text(root + "/day.env", "env");
		for (const auto &[file, points] : missions) editor_test::write_bytes(root + "/missions/" + file, mission_at(points));
		editor_test::set_missions(session, true);
		editor_test::handle_to_end(session, request::rescan());
		read_through(60.0);
		trn = v.project.scan->find("Tmap.trn")->relative_path;
		cpt = v.project.scan->find("Tmap.cpt")->relative_path;
	}
	bool read_through(double seconds) {
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
			session.poll();
			const SessionView &v = session.view();
			if (v.findings.graph && v.activity.validation.read && !v.activity.validation.files_unread) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}
	std::string path_of(const char *name) const { return session.view().project.scan->find(name)->relative_path; }
	const DocumentBase *open_at(const std::string &path) {
		for (const auto &document : session.view().documents.open)
			if (document->path() == path) return document.get();
		return nullptr;
	}
	const TerrainHeightsDocument *heights() { return dynamic_cast<const TerrainHeightsDocument *>(open_at(cpt)); }
	// One Raise sample in the view `view` (a mission or the .trn).
	void raise(const std::string &view, double x, double y, uint64_t gesture, bool end, float radius = 8.0f) {
		TerrainStrokeAsk ask;
		ask.plane = TerrainPlane::Heights;
		ask.brush.tool = TerrainTool::Raise;
		ask.brush.radius = radius;
		ask.brush.strength = 1.0f;
		ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
		ask.points = {{x, y}};
		ask.gesture = gesture;
		ask.end = end;
		session.handle(request::terrain_stroke(view, ask));
	}
	// The terrain query's bake op over the view `view`.
	io::JsonValue bake(const std::string &view) {
		io::JsonValue args = io::JsonValue::make_object();
		args.set("path", io::json_string(view));
		args.set("op", io::json_string("bake"));
		std::string error;
		return session.query("terrain", args, error);
	}
	double started(const std::string &view) {
		const io::JsonValue answer = bake(view);
		return answer.get("started") ? answer.get("started")->number : -1.0;
	}
	// Polls until the heights have no region due and a bake state, at most `seconds`.
	bool settled(double seconds) {
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
			session.poll();
			if (const TerrainHeightsDocument *h = heights())
				if (h->heights().unbaked.empty() && h->heights().bake) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}
	// Polls `count` times, a little apart (what a loop would show in).
	void idle(int count) {
		for (int i = 0; i < count; ++i) {
			session.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	std::vector<double> heights_of(const std::string &mission) {
		std::vector<double> out;
		if (const auto *document = dynamic_cast<const MissionDocument *>(open_at(mission)))
			for (const MissionEntityRead &read : mission_entities(*document)) out.push_back(read.z);
		return out;
	}
};

// Two strokes, the second before the first's bake: the bake of both lands in the second's step. One Undo leaves the
// first's regions due with the second to redo: no bake starts while that redo is kept (it would bring the step the bake
// belongs to back), however long the session polls. A Redo brings the landed state back; a Save over the kept redo
// lands the bake as a step of its own ("Baked Tmap.cpt") and writes the baked tiles, never the old ones.
int test_redo_waits() {
	Rig rig("opennova_editor_heights_rules_redo");
	rig.session.handle(request::open_document(rig.trn));
	rig.raise(rig.trn, -200.0, 200.0, 801, true);
	rig.raise(rig.trn, -150.0, 200.0, 802, true);
	TEST_EXPECT(rig.settled(180.0));
	const TerrainHeightsDocument *heights = rig.heights();
	TEST_EXPECT(heights && heights->can_undo());
	rig.session.handle(request::undo(rig.cpt));
	heights = rig.heights();
	TEST_EXPECT(heights && heights->can_redo() && !heights->heights().unbaked.empty());
	const double started = rig.started(rig.trn);
	rig.idle(40);
	TEST_EXPECT(rig.started(rig.trn) == started); // no bake started, none looping
	TEST_EXPECT(!rig.heights()->heights().unbaked.empty() && rig.heights()->can_redo());
	rig.session.handle(request::redo(rig.cpt));
	TEST_EXPECT(rig.heights()->heights().unbaked.empty());
	rig.session.handle(request::undo(rig.cpt));
	TEST_EXPECT(!rig.heights()->heights().unbaked.empty());
	// Save: the bake lands as a step of its own, and the file holds the baked tiles.
	rig.session.handle(request::save(rig.cpt));
	heights = rig.heights();
	TEST_EXPECT(heights && !heights->dirty() && heights->heights().unbaked.empty() && !heights->can_redo());
	TEST_EXPECT(heights->undo_words() == "Baked Tmap.cpt");
	CptFile saved, composed;
	std::string why;
	const std::vector<uint8_t> bytes = test_io::read_file(rig.root + "/" + rig.cpt);
	TEST_EXPECT(load_cpt(bytes.data(), bytes.size(), saved, why) && heights->compose_cpt(composed));
	bool same_tiles = saved.tiles.size() == composed.tiles.size();
	for (size_t i = 0; same_tiles && i < saved.tiles.size(); ++i)
		same_tiles = saved.tiles[i].vertex_indices == composed.tiles[i].vertex_indices &&
		             saved.tiles[i].lods[0].indices == composed.tiles[i].lods[0].indices;
	TEST_EXPECT(same_tiles && saved.depth_buffer == composed.depth_buffer);
	// One Undo takes the bake's own step, the next the stroke.
	rig.session.handle(request::undo(rig.cpt));
	TEST_EXPECT(!rig.heights()->heights().unbaked.empty() && rig.heights()->dirty());
	std::printf("redo: no bake over a kept redo, Save lands it alone with the baked tiles\n");
	return 0;
}

// A stroke saved before its bake lands (the stroke's step the saved checkpoint): the bake lands once, as a step of its
// own carrying the stroke's link, and the missions follow; nothing loops.
int test_saved_before_landing() {
	Rig rig("opennova_editor_heights_rules_saved", {{"m.bms", {{-200.0f, 200.0f}}}});
	const std::string m = rig.path_of("m.bms");
	rig.session.handle(request::open_document(m));
	const std::vector<double> before = rig.heights_of(m);
	rig.raise(m, -200.0, 200.0, 811, true);
	const TerrainHeightsDocument *heights = rig.heights();
	TEST_EXPECT(heights && !heights->heights().unbaked.empty());
	// The .cpt saved while its regions are due (its own save settles it): written baked.
	rig.session.handle(request::save(rig.cpt));
	heights = rig.heights();
	TEST_EXPECT(heights && heights->heights().unbaked.empty() && !heights->dirty());
	const std::vector<double> after = rig.heights_of(m);
	TEST_EXPECT(before.size() == 1 && after.size() == 1 && after[0] > before[0]);
	const double started = rig.started(m);
	rig.idle(30);
	TEST_EXPECT(rig.started(m) == started);
	std::printf("saved: the bake lands before the write, the mission follows, nothing loops\n");
	return 0;
}

// Ground the bake refuses: tried once, its finding said, the .cpt not saved (its sculpted heights over its old tiles
// would part the drawn ground and the collision); its heights changed back, the refusal goes.
int test_refused() {
	Rig rig("opennova_editor_heights_rules_refused");
	rig.session.handle(request::open_document(rig.trn));
	rig.raise(rig.trn, -200.0, 200.0, 821, true); // the .cpt opened through the stroke, its seed made
	TEST_EXPECT(rig.settled(180.0));
	const std::vector<uint8_t> on_disk = test_io::read_file(rig.root + "/" + rig.cpt);
	Edit rough;
	rough.operation = EditOperation::Apply;
	rough.payload = std::make_shared<RoughInput>();
	rig.session.handle(request::edit_record(rig.cpt, rough));
	TEST_EXPECT(rig.session.outcome().done());
	// Polled until the bake gives up: once.
	const auto start = std::chrono::steady_clock::now();
	while (rig.heights() && rig.heights()->bake_refusal().empty() &&
	       std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 180.0)
		rig.idle(1);
	const TerrainHeightsDocument *heights = rig.heights();
	TEST_EXPECT(heights && heights->bake_refusal().find("too rough") != std::string::npos);
	const double started = rig.started(rig.trn);
	rig.idle(40);
	TEST_EXPECT(rig.started(rig.trn) == started);
	bool finding = false;
	for (const Diagnostic &d : validate_terrain_heights_file(*heights)) finding = finding || d.code() == "terrain_heights.bake_refused";
	TEST_EXPECT(finding);
	TEST_EXPECT(rig.bake(rig.trn).get("refused") && !rig.bake(rig.trn).get("refused")->string.empty());
	// Save refuses the .cpt.
	rig.session.handle(request::save(rig.cpt));
	TEST_EXPECT(rig.heights()->dirty() && test_io::read_file(rig.root + "/" + rig.cpt) == on_disk);
	// Undone: the refusal goes.
	rig.session.handle(request::undo(rig.cpt));
	rig.idle(2);
	TEST_EXPECT(rig.heights()->bake_refusal().empty());
	std::printf("refused: tried once, said, not saved; undone, gone\n");
	return 0;
}

// Save of the .cpt alone before its bake lands, a clean mission on it: the landing moves the mission, which the save
// writes too (the ground and the entities on it on disk together).
int test_save_writes_followers() {
	Rig rig("opennova_editor_heights_rules_followers", {{"m.bms", {{-200.0f, 200.0f}}}});
	const std::string m = rig.path_of("m.bms");
	const std::vector<uint8_t> mission_before = test_io::read_file(rig.root + "/" + m);
	rig.session.handle(request::open_document(rig.trn));
	rig.raise(rig.trn, -200.0, 200.0, 831, true);
	TEST_EXPECT(rig.heights() && !rig.heights()->heights().unbaked.empty() && !rig.open_at(m));
	rig.session.handle(request::save(rig.cpt));
	TEST_EXPECT(rig.heights() && !rig.heights()->dirty());
	const DocumentBase *mission = rig.open_at(m);
	TEST_EXPECT(mission && !mission->dirty()); // moved by the landing, written with the .cpt
	TEST_EXPECT(test_io::read_file(rig.root + "/" + m) != mission_before);
	std::printf("followers: Save settles first and writes the missions the landing moved\n");
	return 0;
}

// An entity set while the bake was due was put against the ground the views showed (the sculpted input): the landing
// leaves it where it was put, and moves the others.
int test_placed_while_due() {
	Rig rig("opennova_editor_heights_rules_placed", {{"m.bms", {{-200.0f, 200.0f}, {-198.0f, 202.0f}}}});
	const std::string m = rig.path_of("m.bms");
	rig.session.handle(request::open_document(m));
	const std::vector<double> before = rig.heights_of(m);
	rig.raise(m, -200.0, 200.0, 841, true);
	// The second building set onto the sculpted hill before the bake lands.
	const auto *mission = dynamic_cast<const MissionDocument *>(rig.open_at(m));
	TEST_EXPECT(mission && mission_entities(*mission).size() == 2);
	if (!mission) return 1;
	Edit set;
	set.address = mission_entities(*mission)[1].address;
	set.field = "z";
	set.value = 30.0;
	rig.session.handle(request::edit_record(m, set));
	TEST_EXPECT(rig.session.outcome().done());
	TEST_EXPECT(rig.settled(180.0));
	const std::vector<double> after = rig.heights_of(m);
	TEST_EXPECT(after.size() == 2 && after[0] > before[0] && after[1] == 30.0);
	std::printf("placed: an entity set while the bake was due stays where it was put\n");
	return 0;
}

// A drag open in a mission when the landing comes: the mission's follow waits for the drag's end, then lands as a
// step of its own (the drag kept one step).
int test_drag_defers() {
	Rig rig("opennova_editor_heights_rules_drag", {{"m.bms", {{-200.0f, 200.0f}, {300.0f, -300.0f}}}});
	const std::string m = rig.path_of("m.bms");
	rig.session.handle(request::open_document(m));
	const std::vector<double> before = rig.heights_of(m);
	rig.raise(m, -200.0, 200.0, 851, true);
	// A drag of the far building's x begun (its gesture open).
	const auto *mission = dynamic_cast<const MissionDocument *>(rig.open_at(m));
	TEST_EXPECT(mission != nullptr);
	if (!mission) return 1;
	Edit drag;
	drag.address = mission_entities(*mission)[1].address;
	drag.field = "x";
	drag.value = 301.0;
	drag.gesture = 9851;
	rig.session.handle(request::edit_record(m, drag));
	TEST_EXPECT(rig.settled(180.0));
	TEST_EXPECT(rig.heights_of(m)[0] == before[0]); // waiting on the drag
	rig.session.handle(request::end_edit(m, 9851));
	rig.idle(2);
	TEST_EXPECT(rig.heights_of(m)[0] > before[0]);
	// The drag one step, the follow another: one Undo in the mission takes the follow (and the stroke with it).
	rig.session.handle(request::undo(m));
	TEST_EXPECT(rig.heights_of(m)[0] == before[0]);
	std::printf("drag: a follow waits for a drag open in its mission\n");
	return 0;
}

// A closed mission whose only entity the change moves is height locked (its .mis says): the follow reads its locks
// and leaves it closed, where one beside it whose item is not locked is opened and followed. (Items: the .mis read
// alone keeps every entity's lock in the item pool.)
int test_locked_stays_closed() {
	Rig rig("opennova_editor_heights_rules_locked");
	for (const char *file : {"/missions/m.bms", "/missions/free.bms"})
		editor_test::write_bytes(rig.root + file, mission_at({{-200.0f, 200.0f}}, mission::EntityKind::Item, 106102));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.read_through(60.0);
	const std::string m = rig.path_of("m.bms"), free = rig.path_of("free.bms");
	rig.session.handle(request::open_document(m));
	const auto *mission = dynamic_cast<const MissionDocument *>(rig.open_at(m));
	TEST_EXPECT(mission != nullptr);
	if (!mission) return 1;
	Edit lock;
	lock.address = mission_entities(*mission)[0].address;
	lock.field = "height_lock";
	lock.value = int64_t(1);
	rig.session.handle(request::edit_record(m, lock));
	editor_test::handle_to_end(rig.session, request::save(m));
	rig.session.handle(request::close_document(m));
	TEST_EXPECT(!rig.open_at(m));
	rig.session.handle(request::open_document(rig.trn));
	rig.raise(rig.trn, -200.0, 200.0, 861, true);
	TEST_EXPECT(rig.settled(180.0));
	TEST_EXPECT(!rig.open_at(m));
	TEST_EXPECT(rig.open_at(free) && rig.heights_of(free).size() == 1 && rig.heights_of(free)[0] != 4.0);
	std::printf("locked: a closed mission whose movers are locked stays closed\n");
	return 0;
}

// The missions a landing moves are those whose ground the .cpt is as the game resolves it for each, the routing a
// stroke in its own view takes (its .env's polytrn_polydata over the .trn's, D-TERRAIN-18): one on Tmap whose .env names
// another .cpt is left closed, and the open missions the lock op counts route the same.
int test_env_routing() {
	Rig rig("opennova_editor_heights_rules_env");
	editor_test::write_text(rig.root + "/away.env", "polytrn_polydata Away.cpt\r\n");
	editor_test::write_bytes(rig.root + "/missions/here.bms", mission_at({{-200.0f, 200.0f}}));
	editor_test::write_bytes(rig.root + "/missions/away.bms",
	                         mission_at({{-200.0f, 200.0f}}, mission::EntityKind::Building, 100701, "away"));
	editor_test::write_bytes(rig.root + "/missions/near.bms",
	                         mission_at({{-200.0f, 200.0f}}, mission::EntityKind::Building, 100701, "away"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.read_through(60.0);
	const std::string here = rig.path_of("here.bms"), away = rig.path_of("away.bms"), near = rig.path_of("near.bms");
	// An open mission on the other .cpt: no follower of a stroke in the .trn's view, as its own strokes edit Away.cpt.
	rig.session.handle(request::open_document(near));
	rig.session.handle(request::open_document(rig.trn));
	// The .trn's view drawn under no mission (its option "none"): its brush edits the terrain's own files (under a
	// mission, that mission's: the first, away.bms, would route it to Away.cpt as its picture does).
	rig.session.handle(request::set_viewport(rig.trn, R"({"kind": "terrain", "options": {"mission": "none"}})"));
	TEST_EXPECT(rig.session.outcome().done() && rig.session.view().workspace.document(rig.trn).terrain_mission == "none");
	io::JsonValue args = io::JsonValue::make_object();
	args.set("path", io::json_string(rig.trn));
	args.set("op", io::json_string("lock"));
	args.set("x", io::json_number(-200.0));
	args.set("y", io::json_number(200.0));
	args.set("radius", io::json_number(8.0));
	std::string error;
	const io::JsonValue lock = rig.session.query("terrain", args, error);
	TEST_EXPECT(error.empty() && lock.get("follow") && lock.get("follow")->number == 0.0);
	const double near_z = rig.heights_of(near).empty() ? -1.0 : rig.heights_of(near)[0];
	rig.raise(rig.trn, -200.0, 200.0, 881, true);
	TEST_EXPECT(rig.settled(180.0));
	TEST_EXPECT(rig.open_at(here) && rig.heights_of(here).size() == 1 && rig.heights_of(here)[0] != 4.0);
	TEST_EXPECT(!rig.open_at(away));
	TEST_EXPECT(rig.heights_of(near).size() == 1 && rig.heights_of(near)[0] == near_z);
	std::printf("env: the missions followed are those the .cpt is the ground of as the game routes each\n");
	return 0;
}

// The terrain viewport (the .trn's own 3D view) takes a stroke as the mission view does: an Update with the heights
// patches queued for its device, never a Rebuild.
int test_terrain_view_device() {
	Rig rig("opennova_editor_heights_rules_view");
	editor_test::FakeDevices devices;
	rig.session.handle(request::open_document(rig.trn));
	devices.sync(rig.session);
	const auto *view = static_cast<const TerrainViewport *>(rig.session.viewports().find(rig.trn, ViewportKind::Terrain));
	TEST_EXPECT(view != nullptr);
	if (!view) return 1;
	rig.raise(rig.trn, -200.0, 200.0, 871, false);
	devices.sync(rig.session);
	rig.raise(rig.trn, -196.0, 200.0, 871, false);
	devices.sync(rig.session);
	TEST_EXPECT(devices.last(rig.trn, ViewportKind::Terrain) == ViewportAction::Update);
	TEST_EXPECT(!view->heights_patches().empty() && view->heights_patches().back().path == rig.cpt);
	std::printf("view: the terrain viewport's device takes the stroke in place\n");
	return 0;
}

// Flatten whose first sample is off the terrain: towards the height under the strongest texel its disc reaches, not
// to 0; a disc reaching no ground is refused.
int test_flatten_off_terrain() {
	Rig rig("opennova_editor_heights_rules_flatten");
	rig.session.handle(request::open_document(rig.trn));
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Flatten;
	ask.brush.radius = 24.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-1500.0, 1500.0}};
	ask.gesture = 881;
	ask.end = true;
	rig.session.handle(request::terrain_stroke(rig.trn, ask));
	TEST_EXPECT(!rig.session.outcome().done()); // nothing under it
	// Centre 8 units off the island's west edge (mission x -512), the disc reaching its edge.
	ask.points = {{-520.0, 200.0}};
	ask.gesture = 882;
	rig.session.handle(request::terrain_stroke(rig.trn, ask));
	TEST_EXPECT(rig.session.outcome().done());
	const TerrainHeightsDocument *heights = rig.heights();
	TEST_EXPECT(heights != nullptr);
	if (!heights) return 1;
	CptFile shipped;
	std::string why;
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("terrain/tmap/Tmap.cpt"));
	TEST_EXPECT(load_cpt(bytes.data(), bytes.size(), shipped, why));
	// The edge texel under the disc's strongest weight keeps its height (the target is its own), none goes to 0.
	bool zeroed = false;
	for (int y = 300; y < 330; ++y)
		for (int x = 0; x < 16; ++x)
			zeroed = zeroed || (heights->live_height(x, y) == 0 && shipped.depth_buffer[size_t(y) * 1024 + size_t(x)] != 0);
	TEST_EXPECT(!zeroed);
	std::printf("flatten: off the terrain, towards the ground the disc reaches; none under it, refused\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_redo_waits();
	failures += test_saved_before_landing();
	failures += test_refused();
	failures += test_save_writes_followers();
	failures += test_placed_while_due();
	failures += test_drag_defers();
	failures += test_locked_stays_closed();
	failures += test_env_routing();
	failures += test_terrain_view_device();
	failures += test_flatten_off_terrain();
	if (failures == 0) std::printf("editor_heights_bake_rules: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
