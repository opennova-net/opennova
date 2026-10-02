// The mission viewport (editor/preview/mission_viewport, ADR 0046 S14) over a real session, a project
// holding the minted mission (fixtures/bms/synth_logic.bms) and fake devices: its kind's row (Main,
// the rows as they stand, held for a gesture, a canvas; the type's main and default kind); what it
// shows (no project, no mission, ready) and what its follow comes to (a Rebuild on open, Keep on a
// pump with nothing changed); what each change set comes to (an entity's x an Update with builds()
// standing, an event's field Keep with rows_read() standing, an Add a Rebuild, the header's terrain
// a Rebuild, a reload a Rebuild); a Rebuild held under an open gesture and issued at its end; a file
// the device read moving its stamp a Rebuild, and the files it misses as notes; its camera (the
// wire's yaw a compass heading against presentation_forward_from_angles, the first framing moving
// the Viewports concern once, a SetViewport of its camera and options, refused members named); its
// hits and boxes (each entity at its projected pixel, the front-most, a kind's marks off and the
// mark range dropping marks, a box's records); and its envelope (the body's counts, a page of items,
// the notes). S14 V10: a drop's item facts and its one batch (on the plane, over a device's ground
// with the model's anchor baked in), its refusals; the ground command. S14 V11, the retail leg
// (--retail, OPENNOVA_JO_DIR): every shipped mission in a project, each opened in its viewport.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <base/io/bam.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_viewport.h>
#include <formats/threedi/threedi_3di3.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
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
								   "begin \"Drop Crate\"\nid 106190\ntype object\ngraphic crate\nend\n";

// A session over a project holding the minted mission, the mission open and its viewport followed
// once through the fake devices.
struct Rig {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
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
	TEST_EXPECT(edits.size() == 4 && edits[0].operation == EditOperation::Add &&
			edits[0].address.kind == node_kind(MissionKind::Building) && edits[0].field == "item" &&
			std::get<int64_t>(edits[0].value) == 106101);
	double target[3];
	preview_to_mission(viewport->camera().target, target);
	for (size_t i = 1; i < edits.size() && edits.size() == 4; ++i) {
		TEST_EXPECT(edits[i].address.row == batch_made(0) && edits[i].operation == EditOperation::Set);
		TEST_EXPECT(near(std::get<double>(edits[i].value), target[i - 1], 1e-2));
	}
	TEST_EXPECT(edits.size() == 4 && edits[1].field == "x" && edits[2].field == "y" && edits[3].field == "z");
	const std::string before = rig.document()->serialize().text;
	TEST_EXPECT(editor_test::serve(rig.session, gathered.requests));
	TEST_EXPECT(static_cast<const MissionDocument &>(*rig.document()).rows_of(MissionKind::Building).size() == buildings + 1);
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.document()->serialize().text == before);
	// A model file of one item drops that item.
	drop.reference.clear();
	drop.name.clear();
	drop.file = "armory.3di";
	gathered.requests.clear();
	TEST_EXPECT(viewport->drop(context, drop, gathered, error) && gathered.requests.size() == 1 &&
			gathered.requests[0].edits.size() == 4 && std::get<int64_t>(gathered.requests[0].edits[0].value) == 106101);
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
	if (gathered.requests.size() == 1 && gathered.requests[0].edits.size() == 4) {
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
	std::printf("test_retail passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_kind_row() == 0);
	TEST_EXPECT(test_status_and_follow() == 0);
	TEST_EXPECT(test_change_sets() == 0);
	TEST_EXPECT(test_rebuild_held_for_a_gesture() == 0);
	TEST_EXPECT(test_files() == 0);
	TEST_EXPECT(test_camera_frame() == 0);
	TEST_EXPECT(test_hit_and_box() == 0);
	TEST_EXPECT(test_envelope() == 0);
	TEST_EXPECT(test_drop() == 0);
	TEST_EXPECT(test_ground_command() == 0);
	TEST_EXPECT(test_retail() == 0);
	std::printf("editor_mission_viewport: all tests passed\n");
	return 0;
}
