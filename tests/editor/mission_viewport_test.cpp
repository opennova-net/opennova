// The mission viewport (editor/preview/mission_viewport, ADR 0046 S14) over a real session, a project
// holding the minted mission (fixtures/bms/synth_logic.bms) and fake devices: its kind's row (Main,
// the rows as they stand, held for a gesture, a canvas; the type's main and default kind); what it
// shows (no project, no mission, ready) and what its follow comes to (a Rebuild on open, Keep on a
// pump with nothing changed); what each change set comes to (an entity's x an Update with builds()
// standing, an event's field Keep with rows_read() standing, an Add a Rebuild, the header's terrain
// a Rebuild, a reload a Rebuild); a Rebuild held under an open gesture and issued at its end; a file
// the device read moving its stamp a Rebuild, and the files it misses as notes; its camera (the
// wire's yaw a compass heading against presentation_forward_from_angles, the first framing moving
// the Viewports concern once, a SetViewport of its camera and options, refused members named; a framing
// within the mission's fog, the demo round's bug 12); its
// hits and boxes (each entity at its projected pixel, the front-most, a kind's marks off and the
// mark range dropping marks, a box's records); and its envelope (the body's counts, a page of items,
// the notes). S14 V10: a drop's item facts and its one batch (on the plane, over a device's ground
// with the model's anchor baked in), its refusals; the ground command. S14 V11, the retail leg
// (--retail, OPENNOVA_JO_DIR): every shipped mission in a project, each opened in its viewport. DI-07:
// the ground under a point in the game's words over a minted terrain with a char map (the class, its
// footstep slots and effects row at known places, a placed tile, the water plane, the ocean), and the
// hit and the canvas carrying it.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/bam.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_palette.h>
#include <editor/preview/mission_place.h>
#include <editor/preview/mission_viewport.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <formats/pcx/pcx_io.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/til/til_io.h>
#include <formats/trn/charmap_legend.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/world/ammo_table.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <runtime/world/model_geometry.h>
#include <runtime/world/presentation_frame.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::FakeDevice;
using editor_test::FakeDevices;
using editor_test::NoProcess;

constexpr const char *kMission = "missions/synth_logic.bms";

std::string fixture(const std::string &rel) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel;
}

// An item catalog for the drop: the pump drawn by two items, the armory a building, the shed a
// person, the crate (a model with a `ground` user point) an object.
constexpr const char *kDropItems = "begin \"Drop Pump\"\nid 106100\ntype object\ngraphic pump\nend\n"
								   "begin \"Drop Scaled Pump\"\nid 106103\ntype object\ngraphic pump\nscale 1.5\nend\n"
								   "begin \"Drop Armory\"\nid 106101\ntype building\ngraphic armory\nend\n"
								   "begin \"Drop Rifleman\"\nid 106102\ntype person\ngraphic shed\nend\n"
								   "begin \"Drop Crate\"\nid 106190\ntype object\ngraphic crate\nend\n"
								   "begin \"Marker Alpha\"\nid 100001\ntype marker\nend\n";

// The preferences in memory, whose saves fail while `fail` is set (a settings file another program
// holds, a read-only profile).
struct FlakyStore : MemoryPreferencesStore {
	bool fail = false;
	bool save(const Preferences &preferences, Diagnostic &error) override {
		if (!fail) return MemoryPreferencesStore::save(preferences, error);
		error.message = "the settings file is held by another program";
		return false;
	}
};

// A session over a project holding the minted mission, the mission open and its viewport followed
// once through the fake devices.
struct Rig {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	FlakyStore preferences;
	ProjectSession session{ platform, preferences };
	FakeDevices devices;
	std::string path;

	explicit Rig(const char *name) : dir(name) {}

	// `items`: the drop's item catalog and the synth models it draws, written into the project too.
	bool open(bool open_mission = true, bool items = false) {
		session.handle(request::new_project(dir.file("project"), "Missions"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &view = session.view();
		if (!editor_test::write_bytes(view.project.root + "/" + kMission, test_io::read_file(fixture("bms/synth_logic.bms"))))
			return false;
		if (items) {
			if (!editor_test::write_text(view.project.root + "/defs/items.def", kDropItems)) return false;
			for (const char *model : { "pump.3di", "armory.3di", "shed.3di", "crate.3di" })
				if (!editor_test::write_bytes(view.project.root + "/models/" + model,
							test_io::read_file(fixture(std::string("threedi/synth/") + model))))
					return false;
		}
		session.handle(request::rescan());
		session.run_operations();
		if (!open_mission) return true;
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		path = session.document_for(kMission)->path();
		devices.sync(session);
		return true;
	}
	const MissionViewport *viewport() {
		return static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	}
	FakeDevice *device() { return devices.held(path, ViewportKind::Mission); }
	const Document *document() { return records_of(*session.document_for(kMission)); }
	ViewportContext context() { return viewport_context(session.view(), *viewport()); }
	// A pump: the devices synced over the session's view.
	void pump() { devices.sync(session); }
	// The last action the mission's device took.
	ViewportAction last() { return devices.last(path, ViewportKind::Mission); }
};

NodeAddress first_of(const Document &document, MissionKind kind) {
	const std::vector<const Node *> rows = static_cast<const MissionDocument &>(document).rows_of(kind);
	return rows.empty() ? NodeAddress() : NodeAddress{ rows.front()->id, rows.front()->kind, 0 };
}

Edit set_of(const NodeAddress &record, const char *field, Value value) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool near(double a, double b, double slack = 1e-3) { return std::fabs(a - b) <= slack; }

// What a refused request said ("" for none).
std::string refusal(ProjectSession &session) {
	return session.outcome().findings.empty() ? std::string() : session.outcome().findings.front().message;
}

} // namespace

static int test_kind_row() {
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Mission);
	TEST_EXPECT(row.role == ViewportRole::Main && !row.as_saved && !row.part && row.holds_for_gesture && row.canvas);
	TEST_EXPECT(row.devices == 2 && row.make != nullptr);
	TEST_EXPECT(viewport_kind_shows(ViewportKind::Mission, DocumentTypeId::Mission));
	TEST_EXPECT(main_viewport_kind(DocumentTypeId::Mission) == ViewportKind::Mission);
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::Mission) == ViewportKind::Mission);
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Mission)) == "mission");
	ViewportKind kind = ViewportKind::kCount;
	TEST_EXPECT(viewport_kind_from_token("mission", kind) && kind == ViewportKind::Mission);
	std::printf("test_kind_row passed\n");
	return 0;
}

static int test_status_and_follow() {
	// No project: the kind's reason.
	{
		NoProcess platform;
		MemoryPreferencesStore preferences;
		ProjectSession session(platform, preferences);
		const JsonValue empty = editor_test::empty_viewport_json(session.view(), ViewportKind::Mission);
		TEST_EXPECT(empty.get_string("status", "") == "empty" && empty.get_string("reason", "") == "no_project");
	}
	Rig rig("opennova_editor_mission_viewport_follow");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && std::string(viewport->reason()) == "ready");
	// Opened: read whole once, its first Rebuild taken by the device.
	TEST_EXPECT(viewport->scene().entities().size() == 12 && viewport->scene().areas().size() == 2);
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->builds() == 1);
	TEST_EXPECT(viewport->followed_change() == ChangeClass::Loaded);
	const size_t read = viewport->scene().rows_read();
	// A pump with nothing changed: Keep, nothing read.
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Keep && viewport->builds() == 1 && viewport->scene().rows_read() == read);
	// The mission closed: the viewport goes with it.
	rig.session.handle(request::close_document(kMission));
	rig.pump();
	TEST_EXPECT(rig.session.viewports().find(rig.path, ViewportKind::Mission) == nullptr);
	std::printf("test_status_and_follow passed\n");
	return 0;
}

static int test_change_sets() {
	Rig rig("opennova_editor_mission_viewport_changes");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	const Document &document = *rig.document();
	const NodeAddress item = first_of(document, MissionKind::Item);
	const NodeAddress event = first_of(document, MissionKind::Event);
	const NodeAddress mission = first_of(document, MissionKind::Mission);
	TEST_EXPECT(item.row && event.row && mission.row);
	const uint64_t builds = viewport->builds();
	size_t read = viewport->scene().rows_read();
	// An entity's x: an Update, the one row read, the build generation standing.
	rig.session.handle(request::edit_record(kMission, set_of(item, "x", 123.5)));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Update && viewport->builds() == builds);
	TEST_EXPECT(viewport->scene().rows_read() == read + 1 && viewport->scene().entity(item.row)->x == 123.5);
	read = viewport->scene().rows_read();
	// An event's field: Keep, nothing read.
	rig.session.handle(request::edit_record(kMission, set_of(event, "delay", int64_t(3))));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Keep && viewport->scene().rows_read() == read);
	// An entity added: a Rebuild, everything read again.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, node_kind(MissionKind::Item), 0 };
	add.field = "item";
	add.value = int64_t(100301);
	rig.session.handle(request::edit_record(kMission, add));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->builds() == builds + 1);
	TEST_EXPECT(viewport->scene().entities().size() == 13);
	// The header's terrain: a Rebuild.
	rig.session.handle(request::edit_record(kMission, set_of(mission, "terrain", std::string("Bmap"))));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->scene().header().terrain == "Bmap");
	// Undo of each the same class: the terrain back (Rebuild), the add undone (Rebuild), the event
	// (Keep), the x (Update).
	rig.session.handle(request::undo(kMission));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->scene().header().terrain != "Bmap");
	rig.session.handle(request::undo(kMission));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->scene().entities().size() == 12);
	rig.session.handle(request::undo(kMission));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Keep);
	const uint64_t before_x = viewport->builds();
	rig.session.handle(request::undo(kMission));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Update && viewport->builds() == before_x && viewport->scene().entity(item.row)->x != 123.5);
	// A reload: a Rebuild.
	rig.session.handle(request::reload_document(kMission));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild);
	std::printf("test_change_sets passed\n");
	return 0;
}

static int test_rebuild_held_for_a_gesture() {
	Rig rig("opennova_editor_mission_viewport_gesture");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	const Document &document = *rig.document();
	const NodeAddress item = first_of(document, MissionKind::Item);
	const uint64_t builds = viewport->builds();
	// A move under a gesture: Updates.
	const uint64_t gesture = next_edit_gesture();
	Edit move = set_of(item, "x", 50.0);
	move.gesture = gesture;
	rig.session.handle(request::edit_record(kMission, move));
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Update && viewport->builds() == builds);
	// An Add while the gesture is open: the Rebuild held (the picture the device holds stands).
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, node_kind(MissionKind::Item), 0 };
	add.field = "item";
	add.value = int64_t(100301);
	add.gesture = gesture;
	rig.session.handle(request::edit_record(kMission, add));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(viewport->held() && viewport->builds() == builds && rig.last() != ViewportAction::Rebuild);
	// Its end: the Rebuild issued.
	rig.session.handle(request::end_edit(kMission));
	rig.pump();
	TEST_EXPECT(!viewport->held() && rig.last() == ViewportAction::Rebuild && viewport->builds() == builds + 1);
	std::printf("test_rebuild_held_for_a_gesture passed\n");
	return 0;
}

static int test_files() {
	Rig rig("opennova_editor_mission_viewport_files");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	FakeDevice *device = rig.device();
	TEST_EXPECT(device);
	// The device read a file as it built: the file moving its stamp builds again.
	const std::string root = rig.session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/Tmap.trn", "version 1\n"));
	editor_test::backdate(root + "/Tmap.trn", std::chrono::seconds(10));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	device->reads = { "Tmap.trn" };
	rig.pump();
	rig.pump();
	const uint64_t builds = viewport->builds();
	TEST_EXPECT(editor_test::write_text(root + "/Tmap.trn", "version 2\n"));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild && viewport->builds() == builds + 1);
	// The files it misses: its notes.
	device->missing = { "Tmap_c1.pcx", "barrel.3di" };
	rig.pump();
	TEST_EXPECT(viewport->missing().size() == 2);
	const JsonValue envelope = viewport_to_json(rig.session.view(), *viewport, JsonPage());
	const JsonValue *notes = envelope.get("notes");
	TEST_EXPECT(notes && notes->is_array() && notes->array.size() >= 2);
	TEST_EXPECT(notes->array[0].get_string("code", "") == "file.missing" && notes->array[0].get_string("name", "") == "Tmap_c1.pcx");
	TEST_EXPECT(envelope.get("body")->get_number("missing", 0) == 2.0);
	std::printf("test_files passed\n");
	return 0;
}

static int test_camera_frame() {
	Rig rig("opennova_editor_mission_viewport_camera");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	// The first framing: north, 35 degrees down, within the frame distance; the state moved once.
	const uint64_t state = viewport->state_serial();
	TEST_EXPECT(near(mission_camera_heading(viewport->camera()), 0.0) && near(mission_camera_pitch(viewport->camera()), 35.0, 0.01));
	TEST_EXPECT(viewport->camera().distance <= kMissionFrameDistance);
	rig.pump();
	TEST_EXPECT(viewport->state_serial() == state);
	// The wire's yaw is the compass heading: the camera set to 90 looks east, as an entity of yaw 90
	// faces (presentation_forward_from_angles).
	JsonValue change = JsonValue::make_object();
	change.set("kind", opennova::io::json_string("mission"));
	JsonValue camera = JsonValue::make_object();
	camera.set("yaw", opennova::io::json_number(90.0));
	camera.set("pitch", opennova::io::json_number(0.0));
	change.set("camera", camera);
	rig.session.handle(request::set_viewport(kMission, opennova::io::json_write(change)));
	TEST_EXPECT(rig.session.outcome().done());
	PreviewVec3 right, up, back;
	viewport->camera().axes(right, up, back);
	float forward[3];
	opennova::world::presentation_forward_from_angles(90.0f, 0.0f, forward);
	TEST_EXPECT(near(-back.x, forward[0], 1e-4) && near(-back.z, forward[2], 1e-4));
	TEST_EXPECT(near(mission_camera_heading(viewport->camera()), 90.0, 1e-3));
	const JsonValue wire = viewport->camera_json();
	TEST_EXPECT(near(wire.get_number("yaw", -1), 90.0, 1e-3) && wire.get("eye") && wire.get("fov"));
	// The options set; a member refused is named and nothing applies.
	change = JsonValue::make_object();
	change.set("kind", opennova::io::json_string("mission"));
	JsonValue options = JsonValue::make_object();
	JsonValue marks = JsonValue::make_object();
	marks.set("labels", JsonValue::make_bool(true));
	options.set("marks", marks);
	options.set("stick", JsonValue::make_bool(false));
	change.set("options", options);
	rig.session.handle(request::set_viewport(kMission, opennova::io::json_write(change)));
	TEST_EXPECT(rig.session.outcome().done() && viewport->options().labels && !viewport->options().stick);
	rig.pump();
	TEST_EXPECT(rig.last() == ViewportAction::Update);
	change = JsonValue::make_object();
	change.set("kind", opennova::io::json_string("mission"));
	options = JsonValue::make_object();
	options.set("fog", JsonValue::make_bool(true));
	change.set("options", options);
	rig.session.handle(request::set_viewport(kMission, opennova::io::json_write(change)));
	TEST_EXPECT(!rig.session.outcome().done() && refusal(rig.session).find("options") != std::string::npos);
	change = JsonValue::make_object();
	change.set("kind", opennova::io::json_string("mission"));
	camera = JsonValue::make_object();
	camera.set("eye", JsonValue::make_array());
	change.set("camera", camera);
	rig.session.handle(request::set_viewport(kMission, opennova::io::json_write(change)));
	TEST_EXPECT(!rig.session.outcome().done() && refusal(rig.session).find("eye") != std::string::npos);
	// The top command: straight down, north up.
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(viewport->command(rig.context(), "top", {}, gathered, error) && gathered.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	TEST_EXPECT(near(mission_camera_heading(viewport->camera()), 0.0, 1e-3) && viewport->camera().pitch >= kOrbitPitchLimit - 1e-4f);
	std::printf("test_camera_frame passed\n");
	return 0;
}

static int test_hit_and_box() {
	Rig rig("opennova_editor_mission_viewport_hit");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	const ViewportContext context = rig.context();
	TEST_EXPECT(context.width > 0 && context.height > 0);
	const std::vector<MissionMark> marks = viewport->marks(context.width, context.height, context.device);
	TEST_EXPECT(marks.size() == 14);
	// Each shown entity hit at its projected pixel answers its record (the front-most where two
	// coincide).
	int hits = 0;
	for (const MissionMark &mark : marks) {
		if (!mark.shown) continue;
		const ViewportHit hit = viewport->hit(context, mark.x, mark.y);
		TEST_EXPECT(hit.current && hit.index >= 0 && hit.id != 0);
		const MissionMark &found = marks[size_t(hit.index)];
		TEST_EXPECT(found.depth <= mark.depth + 1e-3f);
		TEST_EXPECT(hit.kind == found.kind && !hit.name.empty());
		++hits;
	}
	TEST_EXPECT(hits > 0);
	TEST_EXPECT(viewport->hit(context, -50.0f, -50.0f).index == -1);
	// A kind's marks off: no hit on them.
	JsonValue change = JsonValue::make_object();
	change.set("kind", opennova::io::json_string("mission"));
	JsonValue options = JsonValue::make_object();
	JsonValue shown = JsonValue::make_object();
	shown.set("items", JsonValue::make_bool(false));
	options.set("marks", shown);
	change.set("options", options);
	rig.session.handle(request::set_viewport(kMission, opennova::io::json_write(change)));
	TEST_EXPECT(rig.session.outcome().done());
	for (const MissionMark &mark : viewport->marks(context.width, context.height, context.device))
		if (std::string(mark.kind) == "item") TEST_EXPECT(!mark.shown && viewport->hit(context, mark.x, mark.y).index != int(mark.entity) );
	// A box over the whole picture: every shown record, nearest first.
	const std::vector<ViewportHit> boxed = viewport->box(context, 0.0f, 0.0f, float(context.width), float(context.height));
	size_t shown_count = 0;
	for (const MissionMark &mark : viewport->marks(context.width, context.height, context.device)) shown_count += mark.shown ? 1 : 0;
	TEST_EXPECT(boxed.size() == shown_count);
	for (size_t i = 1; i < boxed.size(); ++i) {
		const std::vector<MissionMark> now = viewport->marks(context.width, context.height, context.device);
		TEST_EXPECT(now[size_t(boxed[i - 1].index)].depth <= now[size_t(boxed[i].index)].depth);
	}
	std::printf("test_hit_and_box passed\n");
	return 0;
}

// The polish: an entity's bound sphere about its position (the model's GHDR radius by the item's SCALE,
// padded, none without a collision block: mission_item_bound_radius, the game's entity+0), read once per
// item while the graph and its files stand and again when a file it read moves (a SCALE edited in the
// open catalog, the model written again: the review's M1), each file parsed once by a cache's facts; a
// pump clicked well above its anchor, past the slop, with no device to say, is picked through the
// viewport's hit by its sphere, and the facts a drop reads carry the bound (the scaled pump's by its
// SCALE).
static int test_sphere_picking() {
	Rig rig("opennova_editor_mission_viewport_spheres");
	TEST_EXPECT(rig.open(true, true));
	const MissionViewport *viewport = rig.viewport();
	const SessionView &view = rig.session.view();
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("threedi/synth/pump.3di"));
	opennova::threedi::Threedi3di3 model{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
	const bool collision = model.collision != nullptr;
	const int32_t radius_q16 = opennova::world::model_bound_radius_q16_from_3di(model);
	opennova::threedi::threedi_3di3_free(&model);
	TEST_EXPECT(collision && radius_q16 > 0);
	const double pump = mission_item_bound_radius(radius_q16, true, 0, false, 0);
	TEST_EXPECT(near(pump, double(radius_q16 + 0x1000) / 65536.0, 1e-9));
	TEST_EXPECT(mission_item_bound_radius(radius_q16, false, 0, false, 0) == 0.0);
	const auto &radii = viewport->items().radii();
	TEST_EXPECT(radii.count(106100) == 1 && near(radii.at(106100), pump, 1e-4));
	MissionItemFacts facts;
	std::string error;
	TEST_EXPECT(mission_item_facts(view, 106100, facts, error) && near(facts.radius, pump, 1e-9));
	TEST_EXPECT(mission_item_facts(view, 106103, facts, error) &&
			near(facts.radius, mission_item_bound_radius(radius_q16, true, int32_t(1.5 * 65536.0), false, 0), 1e-9) &&
			facts.radius > pump * 1.4);
	TEST_EXPECT(mission_item_facts(view, 100001, facts, error) && facts.radius == 0.0); // a marker draws no model
	// A cache's facts parse each file once while it stands (the review's L3): asked again, nothing read.
	MissionItemCache cache;
	TEST_EXPECT(cache.facts(view, 106103, facts, error) && cache.files_read() == 2); // the pump and the catalog
	TEST_EXPECT(cache.facts(view, 106100, facts, error) && cache.facts(view, 106103, facts, error) && cache.files_read() == 2);
	// An edit of the mission alone (the asset source's generation moves, no file the items read does):
	// no item asked again, no file read again.
	const size_t asked = viewport->items().items_asked(), read = viewport->items().files_read();
	TEST_EXPECT(asked > 0 && read > 0);
	const NodeAddress some_item = first_of(*rig.document(), MissionKind::Item);
	const uint64_t serial = viewport->scene().serial();
	rig.session.handle(request::edit_record(kMission, set_of(some_item, "x", 77.25)));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(viewport->scene().serial() != serial); // the cache asked over the scene moved, not skipped
	TEST_EXPECT(viewport->items().items_asked() == asked && viewport->items().files_read() == read);
	// The pump's SCALE set to 3 in the open catalog (the review's M1: no edge or symbol moves, so the graph
	// stands): the catalog read again, the pumps asked again, their bound tripled as the game scales it.
	rig.session.handle(request::open_document("defs/items.def"));
	TEST_EXPECT(rig.session.outcome().done());
	const Document *catalog = records_of(*rig.session.document_for("defs/items.def"));
	NodeAddress pump_row;
	TEST_EXPECT(catalog && find_definition(AssetGraph(), *catalog, "106100", pump_row));
	rig.session.handle(request::edit_record("defs/items.def", set_of(pump_row, "scale_q16", 3.0)));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const double scaled = mission_item_bound_radius(radius_q16, true, 3 * 65536, false, 0);
	TEST_EXPECT(radii.count(106100) == 1 && near(radii.at(106100), scaled, 1e-4) && scaled > pump * 2.9);
	TEST_EXPECT(viewport->items().files_read() == read + 1 && viewport->items().items_asked() > asked);
	rig.session.handle(request::undo("defs/items.def"));
	rig.pump();
	TEST_EXPECT(near(radii.at(106100), pump, 1e-4));
	// The pump's model written again with twice its GHDR radius and the project scanned again (no edge
	// moves): the model read again, the bound grown.
	{
		opennova::threedi::Threedi3di3 larger{};
		TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &larger) == 0);
		larger.header.max_radius_fp16 = radius_q16 * 2;
		std::vector<uint8_t> written;
		TEST_EXPECT(opennova::threedi::threedi_3di3_write_memory(&larger, written) == 0);
		opennova::threedi::threedi_3di3_free(&larger);
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/pump.3di", written));
		rig.session.handle(request::rescan());
		rig.session.run_operations();
		rig.pump();
		TEST_EXPECT(near(radii.at(106100), mission_item_bound_radius(radius_q16 * 2, true, 0, false, 0), 1e-4));
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/pump.3di", bytes));
		rig.session.handle(request::rescan());
		rig.session.run_operations();
		rig.pump();
		TEST_EXPECT(near(radii.at(106100), pump, 1e-4));
	}
	// A pump framed close, clicked above its anchor by most of its radius: the pump's.
	const MissionEntityMark *target = nullptr;
	for (const MissionEntityMark &entity : viewport->scene().entities())
		if (!target && entity.item == 106100) target = &entity;
	TEST_EXPECT(target != nullptr);
	OrbitCamera camera = viewport->camera();
	camera.target = target->at;
	camera.distance = float(pump) * 8.0f;
	rig.session.handle(request::set_viewport(kMission, mission_camera_change(camera)));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const ViewportContext context = rig.context();
	const std::vector<MissionMark> marks = viewport->marks(context.width, context.height, context.device);
	const int index = viewport->scene().mark_index(target->row);
	TEST_EXPECT(index >= 0 && marks[size_t(index)].shown && near(marks[size_t(index)].radius, pump, 1e-4));
	float x = 0.0f, y = 0.0f;
	TEST_EXPECT(viewport->camera().project(mission_scene_point(target->x, target->y, target->z + pump * 0.7), context.width,
			context.height, x, y));
	TEST_EXPECT(std::fabs(y - marks[size_t(index)].y) > 2.0f * kMissionPickSlop);
	const ViewportHit hit = viewport->hit(context, x, y);
	TEST_EXPECT(hit.index == index && hit.id == target->row && hit.kind == "item");
	std::printf("test_sphere_picking passed\n");
	return 0;
}

static int test_envelope() {
	Rig rig("opennova_editor_mission_viewport_envelope");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	JsonPage page;
	page.limit = 5;
	const JsonValue envelope = viewport_to_json(rig.session.view(), *viewport, page);
	TEST_EXPECT(envelope.get_string("kind", "") == "mission" && envelope.get_string("status", "") == "ready");
	const JsonValue *body = envelope.get("body");
	TEST_EXPECT(body && body->get_string("terrain", "") == viewport->scene().header().terrain);
	const JsonValue *counts = body->get("counts");
	TEST_EXPECT(counts && counts->get_number("items", 0) == 3.0 && counts->get_number("buildings", 0) == 2.0 &&
			counts->get_number("markers", 0) == 5.0 && counts->get_number("organics", 0) == 2.0 && counts->get_number("areas", 0) == 2.0);
	TEST_EXPECT(body->get_bool("ground", true) == false);
	const JsonValue *items = envelope.get("items");
	TEST_EXPECT(items && items->is_array() && items->array.size() == 5);
	const JsonValue &first = items->array[0];
	TEST_EXPECT(first.get_number("index", -1) == 0.0 && first.get_number("id", 0) != 0.0 && first.get("at") && first.get("at")->is_array());
	TEST_EXPECT(first.get_string("kind", "") == "item" && first.get("item") && first.get("yaw"));
	TEST_EXPECT(envelope.get("notes") && envelope.get("notes")->is_array());
	std::printf("test_envelope passed\n");
	return 0;
}

// The synth crate with its `ground` user point moved off the origin (every synth model has it at the
// origin), minted through the engine's own 3DI writer into the project's models/crate.3di and
// rescanned: the point authored at (0.25, -0.5, 0.75) (x forward, y left, z up), which is the mission
// frame's (-0.5, -0.25, 0.75) as the drop bakes it (threedi_user_point_position, the placer's
// godot_vec3, godot_to_bms_position).
constexpr double kCrateAnchor[3] = { -0.5, -0.25, 0.75 };
static bool mint_anchored_crate(Rig &rig) {
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("threedi/synth/crate.3di"));
	opennova::threedi::Threedi3di3 model{};
	if (opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) != 0) return false;
	bool moved = false;
	for (size_t i = 0; i < model.user_point_count; ++i) {
		opennova::threedi::ThreediUserPoint &point = model.user_points[i];
		if (std::string(point.name) != "ground") continue;
		point.x = int32_t(0.25 * 65536.0);
		point.y = int32_t(-0.5 * 65536.0);
		point.z = int32_t(0.75 * 65536.0);
		moved = true;
	}
	std::vector<uint8_t> written;
	const bool wrote = moved && opennova::threedi::threedi_3di3_write_memory(&model, written) == 0;
	opennova::threedi::threedi_3di3_free(&model);
	if (!wrote || !editor_test::write_bytes(rig.session.view().project.root + "/models/crate.3di", written)) return false;
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.pump();
	return true;
}

// A drop (S14 V10): the item facts the project's graph gives (its name, its TYPE's pool, its model,
// that model's ground anchor), the items a model draws; an item dropped at the picture's middle over
// no device is one batch (an Add of the pool its TYPE puts it in with the item, then x, y and z on
// batch_made(0)) placing it on the plane through the camera's target, one undo step; a model file of
// one item drops that item; one several items draw is refused naming them, a file that is no model
// and an item no catalog defines too, nothing written; over a device's ground the stored position is
// the ground point less the model's anchor.
static int test_drop() {
	Rig rig("opennova_editor_mission_viewport_drop");
	TEST_EXPECT(rig.open(true, true));
	TEST_EXPECT(mint_anchored_crate(rig));
	const MissionViewport *viewport = rig.viewport();
	const SessionView &view = rig.session.view();
	std::string error;
	MissionItemFacts facts;
	TEST_EXPECT(mission_item_facts(view, 106190, facts, error) && facts.name == "Drop Crate" && facts.pool == MissionKind::Item);
	TEST_EXPECT(facts.model == "models/crate.3di");
	const double *crate = kCrateAnchor;
	for (int i = 0; i < 3; ++i) TEST_EXPECT(near(facts.anchor[i], crate[i], 1e-5));
	// A model whose `ground` point is its origin: no anchor.
	TEST_EXPECT(mission_item_facts(view, 106101, facts, error) && facts.pool == MissionKind::Building && facts.model == "models/armory.3di");
	TEST_EXPECT(facts.anchor[0] == 0.0 && facts.anchor[1] == 0.0 && facts.anchor[2] == 0.0);
	TEST_EXPECT(mission_item_facts(view, 106102, facts, error) && facts.pool == MissionKind::Organic);
	TEST_EXPECT(!mission_item_facts(view, 999999, facts, error) && error.find("999999") != std::string::npos);
	TEST_EXPECT(mission_items_of_model(view, "models/pump.3di") == std::vector<int64_t>({ 106100, 106103 }));
	TEST_EXPECT(mission_items_of_model(view, "crate.3di") == std::vector<int64_t>({ 106190 }));

	// The armory by its item at the picture's middle, over no device: the camera's target.
	const ViewportContext context = rig.context();
	const Document &document = *rig.document();
	const size_t buildings = static_cast<const MissionDocument &>(document).rows_of(MissionKind::Building).size();
	ViewportDrop drop;
	drop.reference = "item";
	drop.name = "106101";
	drop.x = float(context.width) * 0.5f;
	drop.y = float(context.height) * 0.5f;
	editor_test::Gathered gathered;
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1);
	if (gathered.requests.size() != 1) return 1;
	const std::vector<Edit> &edits = gathered.requests[0].edits;
	TEST_EXPECT(edits.size() == 5 && edits[0].operation == EditOperation::Add &&
			edits[0].address.kind == node_kind(MissionKind::Building) && edits[0].field == "item" &&
			std::get<int64_t>(edits[0].value) == 106101);
	double target[3];
	preview_to_mission(viewport->camera().target, target);
	for (size_t i = 1; i < 4 && edits.size() == 5; ++i) {
		TEST_EXPECT(edits[i].address.row == batch_made(0) && edits[i].operation == EditOperation::Set);
		TEST_EXPECT(near(std::get<double>(edits[i].value), target[i - 1], 1e-2));
	}
	// S15: facing the way the camera looks: north (0) on the first framing.
	TEST_EXPECT(edits.size() == 5 && edits[1].field == "x" && edits[2].field == "y" && edits[3].field == "z" &&
			edits[4].field == "yaw" && edits[4].address.row == batch_made(0) && std::get<int64_t>(edits[4].value) == 0);
	const std::string before = rig.document()->serialize().text;
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	TEST_EXPECT(static_cast<const MissionDocument &>(*rig.document()).rows_of(MissionKind::Building).size() == buildings + 1);
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == before);
	// The camera turned (the wire's camera yaw is a compass heading, docs/mcp.md): to look east (90),
	// placed facing east, 90; to look south-west (225), 225; to look west (-90), 270.
	for (const auto &[camera, faces] : { std::pair<const char *, int64_t>{ R"({"kind": "mission", "camera": {"yaw": 90}})", 90 },
			std::pair<const char *, int64_t>{ R"({"kind": "mission", "camera": {"yaw": 225}})", 225 },
			std::pair<const char *, int64_t>{ R"({"kind": "mission", "camera": {"yaw": -90}})", 270 } }) {
		rig.session.handle(request::set_viewport(kMission, camera));
		rig.pump();
		gathered.requests.clear();
		TEST_EXPECT(rig.session.outcome().done() && viewport->drop(rig.context(), drop, gathered, error) && gathered.requests.size() == 1);
		TEST_EXPECT(gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 5 &&
				std::get<int64_t>(gathered.requests[0].edits[4].value) == faces);
	}
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "mission", "camera": {"yaw": 0}})"));
	rig.pump();
	// A model file of one item drops that item.
	drop.reference.clear();
	drop.name.clear();
	drop.file = "armory.3di";
	gathered.requests.clear();
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1 &&
			gathered.requests[0].edits.size() == 5 && std::get<int64_t>(gathered.requests[0].edits[0].value) == 106101);
	// Refused, nothing planned: a model two items draw (naming both), a file that is no model, an item
	// no catalog defines.
	gathered.requests.clear();
	drop.file = "pump.3di";
	TEST_EXPECT(!viewport->drop(context, drop, gathered, error) && error.find("Drop Pump (106100)") != std::string::npos &&
			error.find("Drop Scaled Pump (106103)") != std::string::npos);
	drop.file = "synth_logic.bms";
	TEST_EXPECT(!viewport->drop(context, drop, gathered, error) && error.find("is no model") != std::string::npos);
	drop.file.clear();
	drop.reference = "item";
	drop.name = "999999";
	TEST_EXPECT(!viewport->drop(context, drop, gathered, error) && error.find("999999") != std::string::npos);
	TEST_EXPECT(gathered.requests.empty());

	// Over a device's ground (z = 5 + x / 100): the crate stored at the ground point less its anchor.
	rig.session.viewports().set_devices(&rig.devices.cache);
	FakeDevice *device = rig.device();
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->ground = [](double x, double) { return 5.0 + x / 100.0; };
	const ViewportContext grounded = rig.context();
	TEST_EXPECT(grounded.device == device);
	double point[3];
	bool on_terrain = false;
	TEST_EXPECT(mission_ground_point(grounded, viewport->camera(), drop.x, drop.y, target[2], point, &on_terrain) && on_terrain);
	drop.name = "106190";
	gathered.requests.clear();
	TEST_EXPECT(viewport->drop(grounded, drop, gathered, error) && gathered.requests.size() == 1);
	if (gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 5) {
		const std::vector<Edit> &placed = gathered.requests[0].edits;
		TEST_EXPECT(std::get<int64_t>(placed[0].value) == 106190 && placed[0].address.kind == node_kind(MissionKind::Item));
		for (int i = 0; i < 3; ++i) TEST_EXPECT(near(std::get<double>(placed[size_t(i) + 1].value), point[i] - crate[i], 1e-6));
	}
	rig.session.viewports().set_devices(nullptr);
	std::printf("test_drop passed\n");
	return 0;
}

// The ground command (S14 V10): each named entity set down on the device's ground under it, its z the
// ground's less its model's anchor height, one batch; the selection when none is named; refused with
// no ground under it (no device) and with nothing named or selected.
static int test_ground_command() {
	Rig rig("opennova_editor_mission_viewport_ground");
	TEST_EXPECT(rig.open(true, true));
	TEST_EXPECT(mint_anchored_crate(rig));
	const MissionViewport *viewport = rig.viewport();
	const Document &document = *rig.document();
	const NodeAddress item = first_of(document, MissionKind::Item);
	const NodeAddress building = first_of(document, MissionKind::Building);
	std::string error;
	editor_test::Gathered gathered;
	// No device: no ground.
	TEST_EXPECT(!viewport->command(rig.context(), "ground", { item.row }, gathered, error) &&
			error.find("no ground") != std::string::npos);
	rig.session.viewports().set_devices(&rig.devices.cache);
	FakeDevice *device = rig.device();
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->ground = [](double, double) { return 7.0; };
	// The pump's `ground` point is its origin: set down at the ground's height.
	TEST_EXPECT(viewport->command(rig.context(), "ground", { item.row }, gathered, error) && gathered.requests.size() == 1);
	if (gathered.requests.size() == 1) {
		const std::vector<Edit> &edits = gathered.requests[0].edits;
		TEST_EXPECT(edits.size() == 1 && edits[0].address.row == item.row && edits[0].field == "z" &&
				near(std::get<double>(edits[0].value), 7.0, 1e-9));
	}
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	TEST_EXPECT(near(viewport->scene().entity(item.row)->z, 7.0, 1.0 / 65536.0));
	// Set down already: nothing planned.
	gathered.requests.clear();
	TEST_EXPECT(viewport->command(rig.context(), "ground", { item.row }, gathered, error) && gathered.requests.empty());
	// The anchored crate dropped, then set down: its height the ground's less its anchor's (only the
	// height: the game's vertical terrain conform).
	ViewportDrop drop;
	drop.reference = "item";
	drop.name = "106190";
	drop.x = float(rig.context().width) * 0.5f;
	drop.y = float(rig.context().height) * 0.5f;
	TEST_EXPECT(viewport->drop(rig.context(), drop, gathered, error) && editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	NodeId crate = 0;
	for (const Node *row : static_cast<const MissionDocument &>(*rig.document()).rows_of(MissionKind::Item)) {
		const MissionEntityMark *mark = viewport->scene().entity(row->id);
		if (mark && mark->item == 106190) crate = row->id;
	}
	TEST_EXPECT(crate != 0);
	if (crate == 0) return 1;
	gathered.requests.clear();
	TEST_EXPECT(near(viewport->scene().entity(crate)->z, 7.0 - kCrateAnchor[2], 1.0 / 65536.0));
	device->ground = [](double, double) { return 9.0; };
	TEST_EXPECT(viewport->command(rig.context(), "ground", { crate }, gathered, error) && gathered.requests.size() == 1 &&
			gathered.requests[0].edits.size() == 1 &&
			near(std::get<double>(gathered.requests[0].edits[0].value), 9.0 - kCrateAnchor[2], 1e-9));
	gathered.requests.clear();
	// The selection when none is named.
	rig.session.handle(request::select_record(kMission, building));
	TEST_EXPECT(viewport->command(rig.context(), "ground", {}, gathered, error) && gathered.requests.size() == 1 &&
			gathered.requests[0].edits.size() == 1 && gathered.requests[0].edits[0].address.row == building.row);
	rig.session.handle(request::select_record(kMission, NodeAddress()));
	gathered.requests.clear();
	TEST_EXPECT(!viewport->command(rig.context(), "ground", {}, gathered, error) && error.find("No entity") != std::string::npos);
	rig.session.viewports().set_devices(nullptr);
	std::printf("test_ground_command passed\n");
	return 0;
}

// A viewport query of the mission's, its args as JSON text ("" error when it answered).
static JsonValue ask(Rig &rig, const std::string &args, std::string &error) {
	JsonValue json;
	std::string parse_error;
	opennova::io::json_parse(args, json, parse_error);
	error.clear();
	return rig.session.query("viewport", json, error);
}

// The Place tool's palette (S15): the drop catalog's items by name in their TYPE's groups (People the
// rifleman, Objects the crate and the two pumps, Buildings the armory, Markers the marker), each with
// the model its graphic loads and the pool a placed one lands in; a search by name and by model's file;
// the recently placed first (in their own group as well); through the viewport query's palette op (a
// page of the groups' rows, a search), where a served drop of an item puts it first among the recently
// placed the preferences keep; a menu has no palette.
static int test_palette() {
	Rig rig("opennova_editor_mission_viewport_palette");
	TEST_EXPECT(rig.open(true, true));
	const AssetGraph *graph = rig.session.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr);
	if (!graph) return 1;
	MissionPalette palette = mission_palette(*graph, "", {});
	TEST_EXPECT(palette.count == 6 && palette.items.size() == 6);
	std::vector<MissionPaletteGroup> groups;
	for (const MissionPaletteSection &section : palette.sections) groups.push_back(section.group);
	TEST_EXPECT(groups == std::vector<MissionPaletteGroup>({ MissionPaletteGroup::People, MissionPaletteGroup::Objects,
			MissionPaletteGroup::Buildings, MissionPaletteGroup::Markers }));
	const auto names = [&](const MissionPaletteSection &section) {
		std::vector<std::string> out;
		for (const size_t index : section.items) out.push_back(palette.items[index].name);
		return out;
	};
	if (palette.sections.size() == 4) {
		TEST_EXPECT(names(palette.sections[1]) == std::vector<std::string>({ "Drop Crate", "Drop Pump", "Drop Scaled Pump" }));
		const MissionPaletteItem &crate = palette.items[palette.sections[1].items[0]];
		TEST_EXPECT(crate.item == 106190 && crate.model == "models/crate.3di" && crate.pool == MissionKind::Item &&
				crate.type == 6 && !crate.recent);
		const MissionPaletteItem &rifleman = palette.items[palette.sections[0].items[0]];
		TEST_EXPECT(rifleman.name == "Drop Rifleman" && rifleman.pool == MissionKind::Organic);
		const MissionPaletteItem &marker = palette.items[palette.sections[3].items[0]];
		TEST_EXPECT(marker.item == 100001 && marker.pool == MissionKind::Marker && marker.model.empty());
	}
	// A search: by name, case aside; by the model's file.
	TEST_EXPECT(mission_palette(*graph, "PUMP", {}).items.size() == 2);
	TEST_EXPECT(mission_palette(*graph, "crate.3di", {}).items.size() == 1);
	TEST_EXPECT(mission_palette(*graph, "nothing like it", {}).items.empty());
	// The recently placed first, an id no catalog defines left out.
	palette = mission_palette(*graph, "", { 106101, 999999 });
	TEST_EXPECT(!palette.sections.empty() && palette.sections.front().group == MissionPaletteGroup::Recent &&
			palette.sections.front().items.size() == 1 && palette.items[palette.sections.front().items[0]].item == 106101 &&
			palette.items[palette.sections.front().items[0]].recent && palette.sections.size() == 5);

	// The wire: the palette op, a page of the rows in the groups' order.
	std::string error;
	JsonValue answer = ask(rig, R"({"op": "palette", "limit": 2})", error);
	TEST_EXPECT(error.empty() && answer.get_number("count", 0) == 6.0 && answer.get_number("matching", 0) == 6.0);
	const JsonValue *rows = answer.get("items");
	TEST_EXPECT(rows && rows->array.size() == 2 && rows->array[0].get_string("name", "") == "Drop Rifleman" &&
			rows->array[0].get_string("group", "") == "people" && rows->array[0].get_string("pool", "") == "organics" &&
			rows->array[1].get_string("model", "") == "models/crate.3di" && answer.get_number("next_offset", 0) == 2.0);
	const JsonValue *wire_groups = answer.get("groups");
	TEST_EXPECT(wire_groups && wire_groups->array.size() == 4 && wire_groups->array[1].get_string("words", "") == "Objects" &&
			wire_groups->array[1].get_number("count", 0) == 3.0);
	answer = ask(rig, R"({"op": "palette", "text": "armory"})", error);
	TEST_EXPECT(error.empty() && answer.get_number("matching", 0) == 1.0);
	ask(rig, R"({"op": "palette", "x": 3})", error);
	TEST_EXPECT(error.find("op palette takes no \"x\"") != std::string::npos);
	// A served drop of an item: first among the recently placed at once (first in the palette), kept
	// with the preferences by the next poll, outside the request; the same item again writes nothing.
	ViewportDrop drop;
	drop.reference = "item";
	drop.name = "106101";
	drop.x = float(rig.context().width) * 0.5f;
	drop.y = float(rig.context().height) * 0.5f;
	drop.kind = ViewportKind::Mission;
	const size_t saves = rig.preferences.saves();
	rig.session.handle(request::edit_in_viewport(kMission, drop));
	TEST_EXPECT(rig.session.outcome().done() && rig.session.view().project.recent_items == std::vector<int64_t>({ 106101 }) &&
			rig.preferences.saves() == saves);
	rig.session.poll();
	TEST_EXPECT(rig.preferences.saves() == saves + 1 &&
			rig.preferences.preferences().recent_items.at("jo") == std::vector<int64_t>({ 106101 }) &&
			rig.preferences.preferences().recent_items.size() == 1); // kept under the project's game
	rig.session.handle(request::edit_in_viewport(kMission, drop));
	rig.session.poll();
	TEST_EXPECT(rig.session.outcome().done() && rig.preferences.saves() == saves + 1);
	// A store that cannot keep them fails no placement: the drop done, the placement made, a note in
	// Output; the list shown all the same.
	rig.preferences.fail = true;
	drop.name = "106102";
	rig.session.handle(request::edit_in_viewport(kMission, drop));
	TEST_EXPECT(rig.session.outcome().done() && rig.session.view().project.recent_items.front() == 106102);
	rig.session.poll();
	bool noted = false;
	for (const std::string &line : rig.session.view().activity.output)
		noted = noted || line.find("could not be kept") != std::string::npos;
	TEST_EXPECT(noted && rig.session.view().activity.status.find("settings") == std::string::npos);
	rig.preferences.fail = false;
	rig.session.handle(request::undo(kMission));
	rig.session.handle(request::undo(kMission));
	rig.session.handle(request::undo(kMission));
	drop.name = "106101";
	rig.session.handle(request::edit_in_viewport(kMission, drop));
	rig.session.poll();
	TEST_EXPECT(rig.preferences.preferences().recent_items.at("jo") == std::vector<int64_t>({ 106101, 106102 }));
	answer = ask(rig, R"({"op": "palette", "limit": 1})", error);
	rows = answer.get("items");
	TEST_EXPECT(rows && rows->array.size() == 1 && rows->array[0].get_string("group", "") == "recent" &&
			rows->array[0].get_bool("recent", false) && rows->array[0].get_number("item", 0) == 106101.0);
	// A refused drop is not noted, and says why on the status line.
	drop.name = "999999";
	rig.session.handle(request::edit_in_viewport(kMission, drop));
	TEST_EXPECT(!rig.session.outcome().done() && rig.session.view().project.recent_items.size() == 2 &&
			rig.session.view().activity.status.find("999999") != std::string::npos);
	// The tool is the viewport's options' (the toolbar's and the wire's alike): Place with its item,
	// which the envelope's body says in words; a tool, an item or a path of the wrong shape refused.
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "mission", "options": {"tool": "place", "item": 106101}})"));
	TEST_EXPECT(rig.session.outcome().done() && rig.viewport()->options().tool == MissionTool::Place &&
			rig.viewport()->options().item == 106101);
	answer = ask(rig, R"({"op": "state", "limit": 1})", error);
	const JsonValue *options = answer.get("options");
	const JsonValue *body = answer.get("body");
	TEST_EXPECT(options && options->get_string("tool", "") == "place" && options->get_number("item", 0) == 106101.0);
	TEST_EXPECT(body && body->get_string("hint", "").find("Place Drop Armory: click the ground") == 0);
	for (const char *refused : { R"({"kind": "mission", "options": {"tool": "paint"}})",
				 R"({"kind": "mission", "options": {"item": -1}})", R"({"kind": "mission", "options": {"path": 124}})" }) {
		rig.session.handle(request::set_viewport(kMission, refused));
		TEST_EXPECT(!rig.session.outcome().done() && rig.viewport()->options().tool == MissionTool::Place);
	}
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "mission", "options": {"tool": "path", "path": 1}})"));
	answer = ask(rig, R"({"op": "state", "limit": 1})", error);
	body = answer.get("body");
	TEST_EXPECT(body && body->get_string("hint", "").find("Path 1: click the ground") == 0);
	std::printf("test_palette passed\n");
	return 0;
}

// What placing writes (S15): a drop snapped to the grid, its height the ground's there; a path's next
// stop (a marker of the item the path's stops use, at the point, and a stop naming it after the
// path's last: one undo step, the new marker selected), refused for a command path; an area over a
// box on the ground, refused with no extent.
static int test_placing() {
	Rig rig("opennova_editor_mission_viewport_placing");
	TEST_EXPECT(rig.open(true, true));
	const MissionViewport *viewport = rig.viewport();
	rig.session.viewports().set_devices(&rig.devices.cache);
	FakeDevice *device = rig.device();
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->ground = [](double x, double) { return 4.0 + x / 50.0; };
	const ViewportContext context = rig.context();
	const auto &document = static_cast<const MissionDocument &>(*rig.document());
	std::string error;
	editor_test::Gathered gathered;
	// Snapped to 5 m: x and y on the grid, z the ground's there (the armory's ground point its origin).
	ViewportDrop drop;
	drop.reference = "item";
	drop.name = "106101";
	drop.x = float(context.width) * 0.5f + 7.0f;
	drop.y = float(context.height) * 0.5f + 3.0f;
	drop.snap = 5.0f;
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1);
	TEST_EXPECT(gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 5);
	const auto on_grid = [](double value) {
		return near(std::fmod(std::fabs(value), 5.0), 0.0, 1e-9) || near(std::fmod(std::fabs(value), 5.0), 5.0, 1e-9);
	};
	if (gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 5) {
		const std::vector<Edit> &edits = gathered.requests[0].edits;
		const double x = std::get<double>(edits[1].value), y = std::get<double>(edits[2].value);
		TEST_EXPECT(on_grid(x) && on_grid(y));
		TEST_EXPECT(near(std::get<double>(edits[3].value), 4.0 + x / 50.0, 1e-9));
	}
	// A model whose ground point is off its axis (the crate's, -0.5 east, -0.25 north, 0.75 up): the
	// stored origin on the grid, the point a move snaps, its height the ground's under its ground point
	// less the anchor's height. (Snapping the ground point instead left the origin off the grid by the
	// anchor, and a first drag jumped it.)
	TEST_EXPECT(mint_anchored_crate(rig));
	gathered.requests.clear();
	drop.name = "106190";
	TEST_EXPECT(rig.viewport()->drop(rig.context(), drop, gathered, error) && gathered.requests.size() == 1 &&
			gathered.requests[0].edits.size() == 5);
	if (gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 5) {
		const std::vector<Edit> &edits = gathered.requests[0].edits;
		const double x = std::get<double>(edits[1].value), y = std::get<double>(edits[2].value);
		TEST_EXPECT(on_grid(x) && on_grid(y));
		TEST_EXPECT(near(std::get<double>(edits[3].value), 4.0 + (x + kCrateAnchor[0]) / 50.0 - kCrateAnchor[2], 1e-9));
	}
	// A path's next stop: path 1's markers are of item 100001, the marker the stop names the new one.
	const size_t markers = document.rows_of(MissionKind::Marker).size();
	TEST_EXPECT(mission_stop_item(viewport->scene(), 1) == 100001);
	gathered.requests.clear();
	drop = ViewportDrop();
	drop.reference = "path";
	drop.name = "1";
	drop.x = float(context.width) * 0.5f;
	drop.y = float(context.height) * 0.5f;
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1);
	const std::string before = rig.document()->serialize().text;
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	const auto &after = static_cast<const MissionDocument &>(*rig.document());
	TEST_EXPECT(after.rows_of(MissionKind::Marker).size() == markers + 1);
	const MissionPathMark *path = nullptr;
	for (const MissionPathMark &each : viewport->scene().paths())
		if (each.index == 1) path = &each;
	TEST_EXPECT(path && path->stops.size() == 5 && path->stops.back() == after.rows_of(MissionKind::Marker).back()->id);
	const MissionEntityMark *marker = path ? viewport->scene().entity(path->stops.back()) : nullptr;
	TEST_EXPECT(marker && marker->item == 100001 && near(marker->z, 4.0 + marker->x / 50.0, 1e-3));
	// The new marker is selected (what the batch made).
	TEST_EXPECT(marker && rig.session.view().documents.selection.primary.row == marker->row);
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == before);
	rig.pump();
	// Refused: a command path.
	drop.name = "124";
	TEST_EXPECT(!viewport->drop(context, drop, gathered, error) && error.find("command") != std::string::npos);
	// The planner's refusals, nothing planned: path 0, no marker item, an item of another pool, a path
	// holding its 32 stops (a path the mission has not, below).
	{
		const auto &mission = static_cast<const MissionDocument &>(*rig.document());
		const double where[3] = { 0.0, 0.0, 0.0 };
		std::vector<Edit> planned;
		TEST_EXPECT(!mission_stop_edits(mission, 0, 100001, MissionKind::Marker, where, 0, planned, error) &&
				error.find("path 0 is none") != std::string::npos && planned.empty());
		TEST_EXPECT(!mission_stop_edits(mission, 1, 0, MissionKind::Marker, where, 0, planned, error) &&
				error.find("Pick the marker") != std::string::npos);
		TEST_EXPECT(!mission_stop_edits(mission, 1, 106101, MissionKind::Building, where, 0, planned, error) &&
				error.find("among the buildings") != std::string::npos);
		const Node *path_row = nullptr;
		for (const Node *each : mission.rows_of(MissionKind::WaypointPath))
			if (static_cast<const PathRow &>(*each).native.number == 1) path_row = each;
		TEST_EXPECT(path_row != nullptr);
		if (path_row) {
			const size_t held = static_cast<const PathRow &>(*path_row).native.record.waypoint_numbers.size();
			std::vector<Edit> fill;
			for (size_t i = held; i < opennova::mission::kMaxWaypointPathMarkers; ++i) {
				Edit stop;
				stop.operation = EditOperation::Add;
				stop.address = NodeAddress{ path_row->id, node_kind(MissionKind::Stop), 0 };
				stop.field = "marker";
				stop.value = int64_t(0);
				fill.push_back(std::move(stop));
			}
			rig.session.handle(request::edit_record(kMission, fill));
			TEST_EXPECT(rig.session.outcome().done());
			TEST_EXPECT(!mission_stop_edits(static_cast<const MissionDocument &>(*rig.document()), 1, 100001, MissionKind::Marker,
								 where, 0, planned, error) &&
					error.find("holds its 32 stops") != std::string::npos);
			rig.session.handle(request::undo(kMission));
			TEST_EXPECT(rig.session.outcome().done());
		}
	}
	// No path's stops left (each path's stops removed), and none placed recently: the drop asks for a
	// marker placed first.
	{
		const auto &mission = static_cast<const MissionDocument &>(*rig.document());
		std::vector<Edit> removes;
		for (const Node *each : mission.rows_of(MissionKind::WaypointPath))
			mission.walk_records(*each, [&](const NodeAddress &record, const Document::Placement &) {
				if (record.kind == node_kind(MissionKind::Stop)) {
					Edit remove;
					remove.operation = EditOperation::Remove;
					remove.address = record;
					removes.push_back(std::move(remove));
				}
				return true;
			});
		TEST_EXPECT(!removes.empty());
		rig.session.handle(request::edit_record(kMission, removes));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		drop.name = "1";
		gathered.requests.clear();
		TEST_EXPECT(!rig.viewport()->drop(rig.context(), drop, gathered, error) &&
				error.find("No stop of the mission names a marker yet") != std::string::npos && gathered.requests.empty());
		rig.session.handle(request::undo(kMission));
		rig.pump();
	}
	// An area over a box on the ground; none with no extent.
	gathered.requests.clear();
	drop = ViewportDrop();
	drop.reference = "area";
	drop.box = true;
	drop.x = float(context.width) * 0.4f;
	drop.y = float(context.height) * 0.4f;
	drop.x2 = float(context.width) * 0.6f;
	drop.y2 = float(context.height) * 0.6f;
	drop.snap = 1.0f;
	const size_t areas = viewport->scene().areas().size();
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	rig.pump();
	TEST_EXPECT(viewport->scene().areas().size() == areas + 1);
	if (viewport->scene().areas().size() == areas + 1) {
		const MissionAreaMark &area = viewport->scene().areas().back();
		TEST_EXPECT(area.max[0] > area.min[0] && area.max[1] > area.min[1] && near(area.min[0], std::round(area.min[0]), 1e-6));
	}
	drop.x2 = drop.x;
	drop.y2 = drop.y;
	gathered.requests.clear();
	TEST_EXPECT(!viewport->drop(context, drop, gathered, error) && error.find("width and depth") != std::string::npos);
	rig.session.viewports().set_devices(nullptr);
	std::printf("test_placing passed\n");
	return 0;
}

// What tweaking writes (S15): a duplicate of the selection moved by a way (one batch: the copies
// made and moved, the primary's copy the primary, one undo step); select_same (every entity of the
// selected entity's item, the primary kept); paste at a point (the clipboard's copied entities, their
// middle where the point meets the ground, one batch, one undo step); a framing of everything on a
// mission spread wide looking at its densest place; and a command's members its planner does not read
// refused.
static int test_tweaking_commands() {
	Rig rig("opennova_editor_mission_viewport_tweaking");
	TEST_EXPECT(rig.open(true, true));
	const MissionViewport *viewport = rig.viewport();
	const auto &document = static_cast<const MissionDocument &>(*rig.document());
	const std::vector<const Node *> items = document.rows_of(MissionKind::Item);
	TEST_EXPECT(items.size() == 3);
	if (items.size() != 3) return 1;
	const NodeAddress a{ items[0]->id, items[0]->kind, 0 }, b{ items[1]->id, items[1]->kind, 0 };
	const double ax = viewport->scene().entity(a.row)->x, bx = viewport->scene().entity(b.row)->x;
	rig.session.handle(request::select_record(kMission, a, SelectMode::Replace, { b, a }));
	const std::string before = rig.document()->serialize().text;
	// Duplicate, 3 m east.
	ViewportCommand duplicate;
	duplicate.name = "duplicate";
	duplicate.kind = ViewportKind::Mission;
	duplicate.by = { 3.0, 0.0 };
	rig.session.handle(request::edit_in_viewport(kMission, duplicate));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const std::vector<const Node *> now = static_cast<const MissionDocument &>(*rig.document()).rows_of(MissionKind::Item);
	TEST_EXPECT(now.size() == 5);
	const Selection &selection = rig.session.view().documents.selection;
	TEST_EXPECT(selection.records.size() == 2 && !selection.holds(a) && !selection.holds(b));
	const MissionEntityMark *copy = viewport->scene().entity(selection.primary.row);
	TEST_EXPECT(copy && near(copy->x, ax + 3.0, 1e-4));
	for (const NodeAddress &record : selection.records)
		if (const MissionEntityMark *each = viewport->scene().entity(record.row))
			TEST_EXPECT(near(each->x, ax + 3.0, 1e-4) || near(each->x, bx + 3.0, 1e-4));
	// The two copies made in one batch: each a fresh SSN, neither an original's nor the other's.
	{
		const Document &now_document = *rig.document();
		const auto ssn_of = [&](const NodeAddress &record) {
			Value value;
			return now_document.get(record, "id", value) && std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value)
																							  : int64_t(-1);
		};
		std::vector<int64_t> ssns;
		for (const NodeAddress &record : selection.records) ssns.push_back(ssn_of(record));
		TEST_EXPECT(ssns.size() == 2 && ssns[0] >= 0 && ssns[1] >= 0 && ssns[0] != ssns[1] && ssns[0] != ssn_of(a) &&
				ssns[0] != ssn_of(b) && ssns[1] != ssn_of(a) && ssns[1] != ssn_of(b));
	}
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == before);
	rig.pump();
	// The primary named twice is copied once; a copy past what the file holds (40 km east) is refused,
	// nothing planned.
	{
		editor_test::Gathered planned;
		std::string why;
		ViewportCommand twice = duplicate;
		twice.ids = { a.row, a.row };
		rig.session.handle(request::select_record(kMission, a));
		TEST_EXPECT(viewport->command_of(rig.context(), twice, planned, why) && planned.requests.size() == 1);
		size_t copies = 0;
		for (const Edit &edit : planned.requests.empty() ? std::vector<Edit>() : planned.requests[0].edits)
			copies += edit.operation == EditOperation::Duplicate ? 1 : 0;
		TEST_EXPECT(copies == 1);
		planned.requests.clear();
		ViewportCommand far = duplicate;
		far.by = { 40000.0, 0.0 };
		TEST_EXPECT(!viewport->command_of(rig.context(), far, planned, why) && why.find("32,768 m") != std::string::npos &&
				planned.requests.empty());
	}
	// select_same: the three pumps share item 106100.
	rig.session.handle(request::select_record(kMission, a));
	editor_test::Gathered gathered;
	std::string error;
	TEST_EXPECT(viewport->command(rig.context(), "select_same", {}, gathered, error) && gathered.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests) && selection.records.size() == 3 && selection.primary == a);
	// Paste here: two copied, pasted with their middle at the picture's middle (the plane through the
	// camera's target: no device).
	rig.session.handle(request::select_record(kMission, a, SelectMode::Replace, { b, a }));
	EditorRequest copy_request = request::of(EditorRequestKind::Copy);
	copy_request.path = kMission;
	rig.session.handle(copy_request);
	TEST_EXPECT(rig.session.outcome().done() && !rig.session.view().documents.clipboard.empty());
	ViewportCommand paste;
	paste.name = "paste";
	paste.kind = ViewportKind::Mission;
	paste.has_at = true;
	paste.at_x = float(rig.context().width) * 0.5f;
	paste.at_y = float(rig.context().height) * 0.5f;
	double target[3];
	preview_to_mission(viewport->camera().target, target);
	const std::string unpasted = rig.document()->serialize().text;
	rig.session.handle(request::edit_in_viewport(kMission, paste));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(selection.records.size() == 2);
	double low = 1e9, high = -1e9, south = 1e9, north = -1e9;
	for (const NodeAddress &record : selection.records)
		if (const MissionEntityMark *each = viewport->scene().entity(record.row)) {
			low = std::min(low, each->x);
			high = std::max(high, each->x);
			south = std::min(south, each->y);
			north = std::max(north, each->y);
		}
	TEST_EXPECT(near((low + high) * 0.5, target[0], 0.05) && near((south + north) * 0.5, target[1], 0.05));
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == unpasted);
	// Moved past what the file's positions hold, the clipboard's copies are refused (never wrapped).
	TEST_EXPECT(mission_clip_moved(rig.session.view().documents.clipboard, 40000.0, 0.0).empty() &&
			!mission_clip_moved(rig.session.view().documents.clipboard, 10.0, 0.0).empty());
	// Paste here with Stick over a sloped ground (z = 2 + x / 10): each copy keeps its own height over
	// the ground under it, as a duplicate and a move keep it (not the middle's rise for all of them).
	rig.session.viewports().set_devices(&rig.devices.cache);
	rig.pump();
	FakeDevice *device = rig.device();
	TEST_EXPECT(device != nullptr);
	if (device) {
		device->ground = [](double x, double) { return 2.0 + x / 10.0; };
		const auto clearance = [](const MissionEntityMark &each) { return each.z - (2.0 + each.x / 10.0); };
		std::vector<double> before_clear = { clearance(*viewport->scene().entity(a.row)), clearance(*viewport->scene().entity(b.row)) };
		std::sort(before_clear.begin(), before_clear.end());
		TEST_EXPECT(!near(before_clear[0], before_clear[1], 0.5)); // the test tells one copy's rise from another's
		paste.at_x = float(rig.context().width) * 0.7f;
		rig.session.handle(request::edit_in_viewport(kMission, paste));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		std::vector<double> after_clear;
		for (const NodeAddress &record : selection.records)
			if (const MissionEntityMark *each = viewport->scene().entity(record.row)) after_clear.push_back(clearance(*each));
		std::sort(after_clear.begin(), after_clear.end());
		TEST_EXPECT(after_clear.size() == 2 && near(after_clear[0], before_clear[0], 1e-3) &&
				near(after_clear[1], before_clear[1], 1e-3));
		rig.session.handle(request::undo(kMission));
		TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == unpasted);
	}
	rig.session.viewports().set_devices(nullptr);
	rig.pump();
	// Framing everything on a mission spread wide (one pump 6 km east): its densest place, not the
	// middle of its box, which is empty ground 3 km out.
	rig.session.handle(request::select_record(kMission, NodeAddress()));
	rig.session.handle(request::edit_record(kMission, set_of(a, "x", 6000.0)));
	rig.pump();
	gathered.requests.clear();
	TEST_EXPECT(viewport->command(rig.context(), "frame", {}, gathered, error) && editor_test::serve(rig.session, gathered.requests));
	double framed_at[3];
	preview_to_mission(viewport->camera().target, framed_at);
	TEST_EXPECT(framed_at[0] < 1000.0);
	rig.session.handle(request::undo(kMission));
	rig.pump();
	// Refused: members a command does not read, a paste with no point.
	gathered.requests.clear();
	ViewportCommand frame;
	frame.name = "frame";
	frame.by = { 1.0 };
	TEST_EXPECT(!viewport->command_of(rig.context(), frame, gathered, error) && error.find("takes no \"by\"") != std::string::npos);
	paste.has_at = false;
	TEST_EXPECT(!viewport->command_of(rig.context(), paste, gathered, error) && error.find("paste takes at") != std::string::npos);
	std::printf("test_tweaking_commands passed\n");
	return 0;
}

// The polish's measure of the labels: the canvas's overlay (MissionCanvas::shapes, the labels' layout in
// it) with the labels option on over `path`, on a 1600 x 900 picture framed on everything (the densest
// place), the 24 nearest entities selected, each frame in the Shell's order (the view follows, takes the
// pointer and draws the overlay; then the session serves what it raised and the viewport follows): 120
// idle frames (the pointer still over nothing, nothing moving: the labels laid out and each title worded
// at the first alone), then 120 drag frames (the primary's mark dragged a pixel a frame: no title worded
// again, the layout made at most once a frame). Prints the mean and the slowest frame of each,
// milliseconds; everything undone.
static int measure_labels(Rig &rig, const std::string &path) {
	rig.session.handle(request::open_document(path));
	const DocumentBase *open = rig.session.document_for(path);
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	const std::string full = open->path();
	const std::string before = records_of(*open)->serialize().text;
	rig.session.handle(request::set_viewport(path,
			R"({"kind": "mission", "device": {"width": 1600, "height": 900}, "options": {"marks": {"labels": true}}})"));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	ViewportCommand frame;
	frame.name = "frame";
	frame.kind = ViewportKind::Mission;
	rig.session.handle(request::edit_in_viewport(path, frame));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const MissionViewport *viewport =
			static_cast<const MissionViewport *>(rig.session.viewports().find(full, ViewportKind::Mission));
	TEST_EXPECT(viewport != nullptr);
	if (!viewport) return 1;
	const ViewportContext first = viewport_context(rig.session.view(), *viewport);
	const int width = first.width, height = first.height;
	const std::vector<MissionMark> marks = viewport->marks(width, height, first.device);
	std::vector<size_t> nearest;
	size_t shown = 0;
	for (size_t i = 0; i < marks.size(); ++i) {
		shown += marks[i].shown ? 1 : 0;
		if (marks[i].shown && marks[i].entity >= 0) nearest.push_back(i);
	}
	std::sort(nearest.begin(), nearest.end(), [&](size_t a, size_t b) { return marks[a].depth < marks[b].depth; });
	if (nearest.size() > 24) nearest.resize(24);
	TEST_EXPECT(!nearest.empty());
	if (nearest.empty()) return 1;
	std::vector<NodeAddress> records;
	for (const size_t i : nearest) records.push_back(marks[i].record);
	rig.session.handle(request::select_record(path, records.front(), SelectMode::Replace, records));
	MissionCanvas canvas;
	editor_test::Gathered out;
	const auto input_at = [&](float x, float y) {
		CanvasInput in;
		in.width = width;
		in.height = height;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y };
		in.hovered = true;
		return in;
	};
	size_t labels = 0;
	// One frame in the Shell's order: the view's frame start, the pointer and the overlay (timed); then
	// the session serves what the canvas raised, and the viewport follows the document.
	int frame_index = 0;
	const auto frame_of = [&](const CanvasInput &in, double &total, double &slowest, int &slowest_at) {
		const ViewportContext context = viewport_context(rig.session.view(), *viewport);
		canvas.follow(*viewport, context, out);
		canvas.input(context, in, out);
		const auto started = std::chrono::steady_clock::now();
		const OverlayList list = canvas.shapes(context, in);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		editor_test::serve(rig.session, out.requests);
		out.requests.clear();
		rig.pump();
		total += ms;
		if (ms > slowest) slowest_at = frame_index;
		slowest = std::max(slowest, ms);
		++frame_index;
		labels = 0;
		for (const OverlayShape &shape : list.shapes) labels += shape.kind == OverlayKind::Text ? 1 : 0;
	};
	constexpr int kFrames = 120;
	double idle = 0.0, idle_slowest = 0.0, drag = 0.0, drag_slowest = 0.0;
	int idle_slowest_at = -1, drag_slowest_at = -1, ignored_at = -1;
	// Idle: the labels laid out and every title worded by the first frame, never again.
	frame_of(input_at(2.0f, 2.0f), idle, idle_slowest, idle_slowest_at);
	const size_t layouts = canvas.label_layout().made(), titles = canvas.titles().made(), drops = canvas.titles().dropped();
	TEST_EXPECT(labels > 0 && layouts > 0 && titles >= labels);
	for (int i = 1; i < kFrames; ++i) frame_of(input_at(2.0f, 2.0f), idle, idle_slowest, idle_slowest_at);
	TEST_EXPECT(canvas.label_layout().made() == layouts && canvas.titles().made() == titles && canvas.titles().dropped() == drops);
	const size_t idle_labels = labels;
	const MissionMark &primary = marks[nearest.front()];
	CanvasInput press = input_at(primary.x, primary.y);
	press.pressed = press.down = true;
	double ignored = 0.0, ignored_slowest = 0.0;
	frame_of(press, ignored, ignored_slowest, ignored_at);
	// The drag: every sample a revision of the gesture's, no title worded again, the layout made at most
	// once a frame (the dragged marks move).
	const uint64_t revision = rig.session.document_for(path)->revision();
	const size_t drag_titles = canvas.titles().made(), drag_drops = canvas.titles().dropped(),
	             drag_layouts = canvas.label_layout().made();
	frame_index = 1;
	for (int i = 1; i <= kFrames; ++i) {
		CanvasInput moved = input_at(primary.x + float(i), primary.y);
		moved.down = true;
		moved.delta = CanvasPoint{ 1.0f, 0.0f };
		frame_of(moved, drag, drag_slowest, drag_slowest_at);
	}
	TEST_EXPECT(rig.session.document_for(path)->revision() > revision);
	TEST_EXPECT(canvas.titles().dropped() == drag_drops && canvas.titles().made() == drag_titles);
	TEST_EXPECT(canvas.label_layout().made() <= drag_layouts + size_t(kFrames));
	frame_of(input_at(primary.x + float(kFrames), primary.y), ignored, ignored_slowest, ignored_at);
	std::printf("labels: %s with the labels on, %zu marks shown, %zu labels drawn idle and %zu dragging 24 selected: idle "
	            "%.3f ms a frame (slowest %.3f, frame %d), drag %.3f ms a frame (slowest %.3f, drag frame %d), %d frames each\n",
	            path.c_str(), shown, idle_labels, labels, idle / kFrames, idle_slowest, idle_slowest_at, drag / kFrames,
	            drag_slowest, drag_slowest_at, kFrames);
	while (records_of(*rig.session.document_for(path))->dirty()) {
		rig.session.handle(request::undo(path));
		if (!rig.session.outcome().done()) break;
	}
	TEST_EXPECT(records_of(*rig.session.document_for(path))->serialize().text == before);
	rig.session.handle(request::close_document(path));
	rig.pump();
	return 0;
}

// The retail leg (S14 V11; OPENNOVA_JO_DIR, base and each expansion through the VFS): every shipped
// mission written into one project (a mission's own file, as an import writes it; its terrain, its
// environment and its models are the device's, and the fake devices read none), then each opened in
// its viewport: ready, the scene's pools as many as the document's (each entity pool, the areas);
// every mark shown on the first framing hit at its pixel answers the front-most record there; the
// first item framed and moved by a drag of 64 pixels is one edit, and its undo gives the document's
// bytes back. Prints the largest mission's counts.
static int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission in its viewport)");
	Rig rig("opennova_editor_mission_viewport_retail");
	TEST_EXPECT(rig.open(false));
	const std::string project = rig.session.view().project.root;
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	std::vector<std::string> paths;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			const std::string path = "missions/" + (expansion.empty() ? std::string("base") : expansion) + "/" +
					std::filesystem::path(file.logical_name).filename().string();
			TEST_EXPECT(editor_test::write_bytes(project + "/" + path, bytes));
			paths.push_back(path);
		}
	}
	if (paths.empty()) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	const auto started = std::chrono::steady_clock::now();
	size_t opened = 0, hits = 0, moved = 0;
	std::string largest;
	size_t largest_entities = 0;
	std::string largest_counts;
	for (const std::string &path : paths) {
		rig.session.handle(request::open_document(path));
		TEST_EXPECT(rig.session.outcome().done());
		const DocumentBase *open = rig.session.document_for(path);
		TEST_EXPECT(open != nullptr);
		if (!open) continue;
		const std::string full = open->path();
		rig.pump();
		const MissionViewport *viewport =
				static_cast<const MissionViewport *>(rig.session.viewports().find(full, ViewportKind::Mission));
		TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready);
		const Document *document = records_of(*open);
		if (!viewport || !document) continue;
		++opened;
		const MissionDocument &mission = static_cast<const MissionDocument &>(*document);
		const MissionScene &scene = viewport->scene();
		// The scene's pools, the document's.
		const MissionKind pools[] = { MissionKind::Item, MissionKind::Building, MissionKind::Marker, MissionKind::Organic };
		const MissionPool scene_pools[] = { MissionPool::Item, MissionPool::Building, MissionPool::Marker, MissionPool::Organic };
		size_t entities = 0;
		for (size_t i = 0; i < 4; ++i) {
			const size_t rows = mission.rows_of(pools[i]).size();
			TEST_EXPECT(scene.count(scene_pools[i]) == rows);
			entities += rows;
		}
		TEST_EXPECT(scene.areas().size() == mission.rows_of(MissionKind::Area).size());
		if (entities > largest_entities) {
			largest_entities = entities;
			largest = path;
			char line[256];
			std::snprintf(line, sizeof(line), "%zu items, %zu buildings, %zu markers, %zu organics, %zu areas, %zu paths drawn",
					scene.count(MissionPool::Item), scene.count(MissionPool::Building), scene.count(MissionPool::Marker),
					scene.count(MissionPool::Organic), scene.areas().size(), scene.paths().size());
			largest_counts = line;
		}
		// Each mark the first framing shows, hit at its pixel: its own record, or the front-most one a
		// shown mark within the pick slop of that pixel stands for, its id the row the mark names.
		const ViewportContext context = viewport_context(rig.session.view(), *viewport);
		const std::vector<MissionMark> marks = viewport->marks(context.width, context.height, context.device);
		for (size_t i = 0; i < marks.size(); ++i) {
			const MissionMark &mark = marks[i];
			if (!mark.shown) continue;
			const ViewportHit hit = viewport->hit(context, mark.x, mark.y);
			TEST_EXPECT(hit.current && hit.index >= 0 && size_t(hit.index) < marks.size() && hit.id != 0);
			if (hit.index < 0 || size_t(hit.index) >= marks.size()) continue;
			const MissionMark &found = marks[size_t(hit.index)];
			TEST_EXPECT(hit.id == found.record.row && found.shown);
			TEST_EXPECT(size_t(hit.index) == i ||
					(std::fabs(found.x - mark.x) <= kMissionPickSlop && std::fabs(found.y - mark.y) <= kMissionPickSlop &&
							found.depth <= mark.depth + 1e-3f));
			++hits;
		}
		// The first item framed, then moved 64 pixels east on the picture: one edit, whose undo gives
		// the bytes back.
		const std::vector<const Node *> items = mission.rows_of(MissionKind::Item);
		if (!items.empty()) {
			const NodeId item = items.front()->id;
			ViewportCommand frame;
			frame.name = "frame";
			frame.ids = { item };
			frame.kind = ViewportKind::Mission;
			rig.session.handle(request::edit_in_viewport(path, frame));
			TEST_EXPECT(rig.session.outcome().done());
			rig.pump();
			const std::string before = records_of(*rig.session.document_for(path))->serialize().text;
			const MissionEntityMark held = *viewport->scene().entity(item);
			ViewportDrag drag;
			drag.id = item;
			drag.handle = "move";
			drag.x = 64.0f;
			drag.kind = ViewportKind::Mission;
			rig.session.handle(request::edit_in_viewport(path, drag));
			TEST_EXPECT(rig.session.outcome().done());
			rig.pump();
			const Document *after = records_of(*rig.session.document_for(path));
			TEST_EXPECT(after->dirty() && after->serialize().text != before);
			// It went along the ground (x or y moved, on the plane through it: no device, its height
			// stands).
			const MissionEntityMark *went = viewport->scene().entity(item);
			TEST_EXPECT(went && (went->x != held.x || went->y != held.y) && went->z == held.z);
			rig.session.handle(request::undo(path));
			TEST_EXPECT(rig.session.outcome().done());
			rig.pump();
			const Document *undone = records_of(*rig.session.document_for(path));
			TEST_EXPECT(!undone->dirty() && undone->serialize().text == before);
			++moved;
		}
		rig.session.handle(request::close_document(path));
		TEST_EXPECT(rig.session.outcome().done() && rig.session.document_for(path) == nullptr);
		rig.pump();
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::printf("retail: %zu missions in their viewports (%zu marks hit, %zu first items moved and undone) in %.1f s; the "
	            "largest, %s: %zu entities (%s)\n",
	            opened, hits, moved, seconds, largest.c_str(), largest_entities, largest_counts.c_str());
	TEST_EXPECT(opened == paths.size() && opened > 0 && hits > 0 && moved > 0);
	TEST_EXPECT(measure_labels(rig, largest) == 0);
	std::printf("test_retail passed\n");
	return 0;
}

// Files by name, for the fog reach's read.
struct NamedFiles : opennova::FileSource {
	std::vector<std::pair<std::string, std::string>> files;
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		for (const auto &[file, text] : files)
			if (file == name) {
				out.assign(text.begin(), text.end());
				return true;
			}
		return false;
	}
	uint64_t stamp(const std::string &name) const override {
		std::vector<uint8_t> bytes;
		return read(name, bytes) ? 1 : 0;
	}
};

// The demo round's bug 12: CP10 at 11:00, its fog ending 325 m off, framed from 400 m was the fog's
// colour alone. The settled fog level (held within 1000, as the mission's start holds it), the reach a
// framing stands within (half the fog's end, under the header's override; none with no .env), and a
// framing of the rig's mission whose .env fogs at 60 m standing within 30 m.
static int test_fog_reach() {
	TEST_EXPECT(near(mission_settled_fog_level(1500.0f), 1000.0) && near(mission_settled_fog_level(325.0f), 325.0));
	NamedFiles files;
	MissionSceneHeader header;
	header.environment = "fogged";
	TEST_EXPECT(mission_fog_reach(files, header) == 0.0f);
	files.files.push_back({ "fogged.env", "fog_level 600\nfog_type 1\n" });
	TEST_EXPECT(near(mission_fog_reach(files, header), 300.0));
	files.files[0].second = "fog_level 1500\nfog_type 1\n";
	TEST_EXPECT(near(mission_fog_reach(files, header), 500.0));
	header.attrib_flags = 0x2;
	header.fog_override = 325;
	TEST_EXPECT(near(mission_fog_reach(files, header), 162.5));
	header.environment.clear();
	TEST_EXPECT(mission_fog_reach(files, header) == 0.0f);

	Rig plain("opennova_editor_mission_viewport_fog_plain");
	TEST_EXPECT(plain.open());
	const std::string environment = plain.viewport()->scene().header().environment;
	TEST_EXPECT(!environment.empty());
	const float wide = plain.viewport()->camera().distance;
	TEST_EXPECT(wide > 30.0f);
	Rig rig("opennova_editor_mission_viewport_fog");
	TEST_EXPECT(rig.open(false));
	const std::string root = rig.session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/" + environment + ".env", "fog_level 60\nfog_type 1\n"));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.session.handle(request::open_document(kMission));
	TEST_EXPECT(rig.session.outcome().done());
	rig.path = rig.session.document_for(kMission)->path();
	rig.devices.sync(rig.session);
	TEST_EXPECT(rig.viewport()->camera().distance <= 30.0f + 1e-3f);
	std::printf("test_fog_reach passed\n");
	return 0;
}

// --- DI-07: the ground under the pointer, in the game's words ---------------------------------------------

// Files by name (any case), each write moving its stamp, as a project's files move theirs.
struct GroundFiles final : opennova::FileSource {
	std::map<std::string, std::vector<uint8_t>> files;
	std::map<std::string, uint64_t> stamps;
	uint64_t serial = 0;
	static std::string key(const std::string &name) {
		std::string out = name;
		for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files[key(name)] = std::move(bytes);
		stamps[key(name)] = ++serial;
	}
	void put_text(const std::string &name, const std::string &text) { put(name, std::vector<uint8_t>(text.begin(), text.end())); }
	void drop(const std::string &name) {
		files.erase(key(name));
		stamps.erase(key(name));
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files.find(key(name));
		if (found == files.end()) return false;
		out = found->second;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = stamps.find(key(name));
		return found == stamps.end() ? 0 : found->second;
	}
};

// A char map of 256 over Tmap's island (fixtures/terrain/tmap: the four quadrants in the middle of its 8 x 8
// grid from (-4, -4)): class c painted in a 32-texel block whose corner is texel (16 + 32 (c % 5),
// 16 + 32 (c / 5)), dirt (1) elsewhere; the legend its palette.
constexpr int kGroundMapSide = 256;
std::vector<uint8_t> ground_charmap() {
	opennova::IndexedImage8 map;
	map.width = kGroundMapSide;
	map.height = kGroundMapSide;
	map.indices.assign(size_t(kGroundMapSide) * kGroundMapSide, 1);
	for (int c = 0; c < opennova::kCharmapLegendCount; ++c)
		for (int z = 0; z < 32; ++z)
			for (int x = 0; x < 32; ++x)
				map.indices[size_t(16 + 32 * (c / 5) + z) * kGroundMapSide + size_t(16 + 32 * (c % 5) + x)] = uint8_t(c);
	for (int i = 0; i < opennova::kCharmapLegendCount; ++i) {
		map.palette[i][0] = opennova::kCharmapLegend[i].r;
		map.palette[i][1] = opennova::kCharmapLegend[i].g;
		map.palette[i][2] = opennova::kCharmapLegend[i].b;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::encode_pcx_indexed(map, bytes, error)) std::fprintf(stderr, "the char map: %s\n", error.c_str());
	return bytes;
}

// Where class c's block has its middle, mission metres: its texel (32 + 32 (c % 5), 32 + 32 (c / 5)), a
// texel 4 units of the 1024-unit atlas whose texel (512 + x, 512 - y) mission (x, y) is on the island
// (terrain-re.md, the island layout; Tmap's grid is the same), at the texel's middle.
void block_middle(int c, double &x, double &y) {
	const int tx = 32 + 32 * (c % 5), tz = 32 + 32 * (c / 5);
	x = 4.0 * tx - 512.0 + 2.0;
	y = 512.0 - 4.0 * tz - 2.0;
}

std::vector<uint8_t> tile_at(double x, double y, uint8_t index) {
	opennova::TilFile til;
	opennova::TilOverlayEntry entry;
	// The tile's 16-unit square from (x, y), its z stored negated as the game reads it.
	entry.x_fixed = opennova::bms::to_fixed_16_16(x);
	entry.z_fixed = -opennova::bms::to_fixed_16_16(y);
	entry.tile_index = index;
	til.entries.push_back(entry);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::save_til(til, bytes, error)) std::fprintf(stderr, "the .til: %s\n", error.c_str());
	return bytes;
}

// The ground's facts over a minted terrain (DI-07): Tmap's .trn and .cpt with a char map painting every
// class at a known place, read through the runtime's own load; each class at its block's middle with its
// footstep slots (snow's for 3) and its effects row (class + 4) and the snow block's line and wire form;
// under the water plane (the .trn's 21 half units) the water's slot and row; off the island the ocean's 7;
// the follow reading nothing again while no stamp moves; a placed tile deciding over the char map (TSD_NULL
// with no .tsd, the tile set's .tsd class with one, the mission's tile set naming another .tsd); the water's
// ladder (the terrain's over the environment's, the mission's override over both, the environment's where
// the terrain has none); no char map (1 everywhere); no terrain; a body on an entity.
static int test_ground_facts() {
	using namespace opennova;
	auto files = std::make_shared<GroundFiles>();
	files->put("Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
	files->put("Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
	files->put("Tmap_m.pcx", ground_charmap());
	MissionSceneHeader header;
	header.terrain = "Tmap";
	MissionGround ground;
	ground.follow(files, 1, header, "pad");
	TEST_EXPECT(ground.terrain() && ground.error().empty() && ground.surface_map() == "Tmap_m.pcx" && ground.reads() == 1);
	// Tmap's water: its .trn's 21 half units (no environment, no override).
	TEST_EXPECT(ground.water() && near(ground.water_height(), 10.5));
	for (int c = 0; c < kCharmapLegendCount; ++c) {
		double x = 0.0, y = 0.0;
		block_middle(c, x, y);
		const MissionGroundFacts f = ground.terrain_at(x, y, 20.0);
		TEST_EXPECT(f.on == MissionGroundOn::Terrain && f.surface == c && f.from == MissionSurfaceFrom::Map);
		TEST_EXPECT(!f.under_water && f.impact_row == c + 4);
		TEST_EXPECT(f.footstep[0] == (c == 3 ? audio::kSlotFootLSnow : audio::kSlotFootLGround) &&
				f.footstep[1] == (c == 3 ? audio::kSlotFootRSnow : audio::kSlotFootRGround));
	}
	double sx = 0.0, sy = 0.0;
	block_middle(3, sx, sy);
	const MissionGroundFacts snow = ground.terrain_at(sx, sy, 20.0);
	TEST_EXPECT(mission_ground_line(snow) ==
			"Snow (surface 3, TSD_SNOW): footsteps play SSLFootSnow and SSRFootSnow; a round plays its ammo's snow row (7).");
	const JsonValue json = mission_ground_to_json(snow);
	TEST_EXPECT(json.get_string("on", "") == "terrain" && json.get_number("surface", -1) == 3 &&
			json.get_string("name", "") == "TSD_SNOW" && json.get_string("words", "") == "Snow" &&
			json.get_string("colour", "") == "#CCEFF4" && json.get_string("from", "") == "map" &&
			json.get_bool("water", false) && near(json.get_number("water_height", 0), 10.5) && !json.get_bool("under_water", true));
	const JsonValue *footstep = json.get("footstep");
	const JsonValue *names = footstep ? footstep->get("names") : nullptr;
	TEST_EXPECT(names && names->array.size() == 2 && names->array[0].string == "SSLFootSnow" &&
			names->array[1].string == "SSRFootSnow" && footstep->get_string("words", "") == "snow");
	const JsonValue *row = json.get("impact_row");
	TEST_EXPECT(row && row->get_number("row", 0) == 7 && row->get_string("tag", "") == "snow" && row->get_string("words", "") == "Snow");
	// Under the water plane: the water's one slot, a round from above meeting the water first.
	const MissionGroundFacts wet = ground.terrain_at(sx, sy, 4.0);
	TEST_EXPECT(wet.under_water && wet.footstep[0] == audio::kSlotFootWater && wet.footstep[1] == audio::kSlotFootWater &&
			wet.impact_row == world::kWaterImpactEffectTag && wet.surface == 3);
	TEST_EXPECT(mission_ground_line(wet).find("6.5 m under the water: footsteps play SSFootWater;") == 0 &&
			mission_ground_line(wet).find("water row (11)") != std::string::npos);
	// Off the island: a sector the grid leaves empty reads the ocean, 7.
	const MissionGroundFacts sea = ground.terrain_at(1500.0, 0.0, 20.0);
	TEST_EXPECT(sea.surface == 7 && sea.from == MissionSurfaceFrom::Ocean && sea.impact_row == 11);
	TEST_EXPECT(mission_ground_line(sea).find("Underwater (surface 7, TSD_UNDERWATER: a sector the terrain leaves empty") == 0);
	// Nothing moved: nothing read again, the files' generation moving or not.
	ground.follow(files, 1, header, "pad");
	ground.follow(files, 2, header, "pad");
	TEST_EXPECT(ground.reads() == 1);

	// A placed tile over the snow block's middle: the tile set has no .tsd, so the game's memset table's
	// TSD_NULL (D-SND-15), the char map's snow under it.
	files->put("pad.til", tile_at(sx - 2.0, sy - 2.0, 5));
	ground.follow(files, 3, header, "pad");
	TEST_EXPECT(ground.reads() == 2 && ground.tiles() == 1);
	MissionGroundFacts tiled = ground.terrain_at(sx, sy, 20.0);
	TEST_EXPECT(tiled.surface == 0 && tiled.from == MissionSurfaceFrom::Tile && tiled.tile == 5 && tiled.map_surface == 3 &&
			tiled.impact_row == 4 && tiled.footstep[0] == audio::kSlotFootLGround);
	TEST_EXPECT(mission_ground_line(tiled).find("Null (surface 0, TSD_NULL) from placed tile 5, over the char map's Snow (3)") == 0);
	// Past its square: the char map again.
	const MissionGroundFacts beside = ground.terrain_at(sx + 15.0, sy, 20.0);
	TEST_EXPECT(beside.surface == 3 && beside.from == MissionSurfaceFrom::Map);
	// The tile set's .tsd, beside the strip Tmap's .trn names (mnml_t.tga): the tile's class.
	files->put_text("mnml_t.tsd", "INDEX_5 TSD_WOOD\r\n");
	ground.follow(files, 4, header, "pad");
	tiled = ground.terrain_at(sx, sy, 20.0);
	TEST_EXPECT(ground.reads() == 3 && tiled.surface == 13 && tiled.from == MissionSurfaceFrom::Tile && tiled.impact_row == 17);
	// The mission's tile set names another strip, whose .tsd the project lacks: TSD_NULL again.
	header.tile_set = "other_t";
	ground.follow(files, 4, header, "pad");
	TEST_EXPECT(ground.reads() == 4 && ground.terrain_at(sx, sy, 20.0).surface == 0);
	header.tile_set.clear();
	files->drop("pad.til");

	// The water's ladder: the terrain's over the environment's, the mission's override over both (its half
	// units), the environment's where the terrain has none.
	files->put_text("pad.env", "water_height 30\r\n");
	header.environment = "pad";
	ground.follow(files, 5, header, "pad");
	TEST_EXPECT(near(ground.water_height(), 10.5));
	header.attrib_flags = 0x1;
	header.water_override = 40;
	ground.follow(files, 5, header, "pad");
	TEST_EXPECT(near(ground.water_height(), 20.0));
	header.attrib_flags = 0;
	header.water_override = 0;
	{
		const std::vector<uint8_t> trn = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
		std::string text(trn.begin(), trn.end());
		const size_t at = text.find("water_height");
		TEST_EXPECT(at != std::string::npos);
		if (at == std::string::npos) return 1;
		text.replace(at, text.find('\n', at) - at, "water_height 0");
		files->put_text("Tmap.trn", text);
	}
	ground.follow(files, 6, header, "pad");
	TEST_EXPECT(near(ground.water_height(), 15.0));
	header.environment.clear();
	ground.follow(files, 6, header, "pad");
	TEST_EXPECT(!ground.water() && !ground.terrain_at(sx, sy, -5.0).under_water);

	// No char map: the game reads 1 everywhere.
	files->drop("Tmap_m.pcx");
	ground.follow(files, 7, header, "pad");
	const MissionGroundFacts plain = ground.terrain_at(sx, sy, 20.0);
	TEST_EXPECT(ground.terrain() && ground.surface_map().empty() && plain.surface == 1 && plain.from == MissionSurfaceFrom::NoMap &&
			plain.impact_row == 5);
	TEST_EXPECT(mission_ground_line(plain).find("Dirt (surface 1, TSD_DIRT: the terrain has no char map, so 1 everywhere)") == 0);
	// No terrain: no class to name.
	header.terrain = "absent";
	ground.follow(files, 7, header, "pad");
	const MissionGroundFacts none = ground.terrain_at(sx, sy, 20.0);
	TEST_EXPECT(!ground.terrain() && !ground.error().empty() && none.surface == -1 && none.from == MissionSurfaceFrom::NoTerrain &&
			none.impact_row == -1 && mission_ground_line(none).find("The terrain was not read") == 0);
	// A body standing on an entity: the object's slots; the material it strikes decides a round's row.
	const MissionGroundFacts on = ground.record_at(7, "Wooden crate", 1.0, 2.0, 20.0);
	TEST_EXPECT(on.on == MissionGroundOn::Record && on.footstep[0] == audio::kSlotFootLObject &&
			on.footstep[1] == audio::kSlotFootRObject && on.impact_row == -1);
	TEST_EXPECT(mission_ground_line(on) == "On Wooden crate: footsteps play SSLFootOBJ and SSRFootOBJ (a body standing on an "
										   "entity); a round plays the row of the material it strikes.");
	const JsonValue on_json = mission_ground_to_json(on);
	TEST_EXPECT(on_json.get_string("on", "") == "record" && on_json.get_number("row", 0) == 7 &&
			on_json.get_string("record", "") == "Wooden crate" && on_json.get("impact_row") && on_json.get("impact_row")->is_null());
	TEST_EXPECT(mission_ground_to_json(MissionGroundFacts()).get_string("on", "") == "nothing");
	std::printf("test_ground_facts passed\n");
	return 0;
}

// The viewport's hit carries the ground (DI-07) over the project's own files: Tmap's .trn and .cpt and the
// minted char map in the project, a device whose ray meets the terrain over the snow block (the class, the
// slots and the row in the hit's `ground`), an entity's surface (a body on it), nothing (the sky), and no
// device (nothing to say); the canvas's ground under a hovered pointer, asked of the device once while the
// pointer stands, nothing while it is off the picture.
static int test_ground_hit() {
	Rig rig("opennova_editor_mission_viewport_ground_facts");
	TEST_EXPECT(rig.open());
	const std::string root = rig.session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn"))));
	TEST_EXPECT(editor_test::write_bytes(root + "/Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt"))));
	TEST_EXPECT(editor_test::write_bytes(root + "/Tmap_m.pcx", ground_charmap()));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.pump();
	const MissionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport->scene().header().terrain == "Tmap");
	std::string error;
	// No device to say: nothing under the point.
	JsonValue hit = ask(rig, R"({"op": "hit", "x": 40, "y": 30})", error);
	TEST_EXPECT(error.empty() && hit.get("ground") && hit.get("ground")->get_string("on", "") == "nothing");
	rig.session.viewports().set_devices(&rig.devices.cache);
	rig.pump();
	FakeDevice *device = rig.device();
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	double sx = 0.0, sy = 0.0;
	block_middle(3, sx, sy);
	device->ground = [](double, double) { return 20.0; };
	device->ray = [sx, sy](const double *, const double *) {
		ViewportRayHit met;
		met.met = ViewportRayHit::Met::Surface;
		met.point[0] = sx;
		met.point[1] = sy;
		met.point[2] = 20.25;
		return met;
	};
	hit = ask(rig, R"({"op": "hit", "x": 40, "y": 30})", error);
	const JsonValue *ground = hit.get("ground");
	TEST_EXPECT(error.empty() && ground && ground->get_string("on", "") == "terrain" && ground->get_number("surface", -1) == 3);
	if (!ground) return 1;
	const JsonValue *at = ground->get("at");
	// The point's height the ground's there.
	TEST_EXPECT(at && at->array.size() == 3 && near(at->array[0].number, sx) && near(at->array[2].number, 20.0));
	const JsonValue *names = ground->get("footstep") ? ground->get("footstep")->get("names") : nullptr;
	TEST_EXPECT(names && names->array.size() == 2 && names->array[0].string == "SSLFootSnow");
	TEST_EXPECT(ground->get("impact_row") && ground->get("impact_row")->get_number("row", 0) == 7 &&
			ground->get_string("line", "").find("Snow (surface 3") == 0);
	// An entity's surface: a body standing on it.
	const NodeAddress item = first_of(*rig.document(), MissionKind::Item);
	device->ray = [item](const double *, const double *) {
		ViewportRayHit met;
		met.met = ViewportRayHit::Met::Record;
		met.row = item.row;
		met.point[2] = 25.0;
		return met;
	};
	hit = ask(rig, R"({"op": "hit", "x": 40, "y": 30})", error);
	ground = hit.get("ground");
	TEST_EXPECT(ground && ground->get_string("on", "") == "record" && ground->get_number("row", 0) == double(item.row) &&
			!ground->get_string("record", "").empty() && ground->get("impact_row") && ground->get("impact_row")->is_null());
	// The sky.
	device->ray = [](const double *, const double *) {
		ViewportRayHit met;
		met.met = ViewportRayHit::Met::Nothing;
		return met;
	};
	hit = ask(rig, R"({"op": "hit", "x": 40, "y": 30})", error);
	TEST_EXPECT(hit.get("ground") && hit.get("ground")->get_string("on", "") == "nothing");

	// The canvas: the ground under a hovered pointer, asked once while the pointer stands still.
	device->ray = [sx, sy](const double *, const double *) {
		ViewportRayHit met;
		met.met = ViewportRayHit::Met::Surface;
		met.point[0] = sx;
		met.point[1] = sy;
		return met;
	};
	const ViewportContext context = rig.context();
	std::unique_ptr<CanvasHalf> half = viewport->make_canvas();
	auto *canvas = static_cast<MissionCanvas *>(half.get());
	editor_test::Gathered gathered;
	canvas->follow(*viewport, context, gathered);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	in.hovered = true;
	in.mouse = CanvasPoint{ 40.0f, 30.0f };
	TEST_EXPECT(canvas->ground(context, in).surface == 3);
	const int rays = device->rays;
	TEST_EXPECT(canvas->ground(context, in).surface == 3 && device->rays == rays);
	in.mouse = CanvasPoint{ 41.0f, 30.0f };
	TEST_EXPECT(canvas->ground(context, in).surface == 3 && device->rays == rays + 1);
	in.hovered = false;
	TEST_EXPECT(canvas->ground(context, in).on == MissionGroundOn::Nothing);
	rig.session.viewports().set_devices(nullptr);
	std::printf("test_ground_hit passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_ground_facts() == 0);
	TEST_EXPECT(test_ground_hit() == 0);
	TEST_EXPECT(test_kind_row() == 0);
	TEST_EXPECT(test_status_and_follow() == 0);
	TEST_EXPECT(test_change_sets() == 0);
	TEST_EXPECT(test_rebuild_held_for_a_gesture() == 0);
	TEST_EXPECT(test_files() == 0);
	TEST_EXPECT(test_camera_frame() == 0);
	TEST_EXPECT(test_fog_reach() == 0);
	TEST_EXPECT(test_hit_and_box() == 0);
	TEST_EXPECT(test_sphere_picking() == 0);
	TEST_EXPECT(test_envelope() == 0);
	TEST_EXPECT(test_drop() == 0);
	TEST_EXPECT(test_ground_command() == 0);
	TEST_EXPECT(test_palette() == 0);
	TEST_EXPECT(test_placing() == 0);
	TEST_EXPECT(test_tweaking_commands() == 0);
	TEST_EXPECT(test_retail() == 0);
	std::printf("editor_mission_viewport: all tests passed\n");
	return 0;
}
