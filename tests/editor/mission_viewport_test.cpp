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
// the notes).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/bam.h>
#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_viewport.h>
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

std::string fixture(const char *rel) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel;
}

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

	bool open(bool open_mission = true) {
		session.handle(request::new_project(dir.file("project"), "Missions"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &view = session.view();
		if (!editor_test::write_bytes(view.project.root + "/" + kMission, test_io::read_file(fixture("bms/synth_logic.bms"))))
			return false;
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
	const SessionView &view = rig.session.view();
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

int main() {
	TEST_EXPECT(test_kind_row() == 0);
	TEST_EXPECT(test_status_and_follow() == 0);
	TEST_EXPECT(test_change_sets() == 0);
	TEST_EXPECT(test_rebuild_held_for_a_gesture() == 0);
	TEST_EXPECT(test_files() == 0);
	TEST_EXPECT(test_camera_frame() == 0);
	TEST_EXPECT(test_hit_and_box() == 0);
	TEST_EXPECT(test_envelope() == 0);
	std::printf("editor_mission_viewport: all tests passed\n");
	return 0;
}
