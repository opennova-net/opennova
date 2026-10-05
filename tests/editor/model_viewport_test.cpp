// The model's viewport (editor/preview/model_viewport, ADR 0046 S10p2, S13 V5): the honest reason
// when there is nothing to show; the open model shown as it would save; an edit of what is drawn
// builds again, a user point's edit builds nothing (the overlays alone show it); a texture the
// device read that changes builds again; the options (a level, a held register) are an Update; the
// camera projects its target to the middle and frames the model; Auto walks the model's levels as
// the camera backs away; the envelope, and what a SetViewport sets (the options, the clock, the
// camera) and the MCP's drag of a marker. What the document cannot say (S13 V8: Unknown) builds only
// where the model draws otherwise; a change set naming nothing is Keep.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/animation_uses.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_o3d_read.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/world/infantry.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/anim_test_support.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

void set(ProjectSession &session, const std::string &path, const NodeAddress &address, const char *field, Value value) {
	EditorRequest request = request::edit_record(path, Edit());
	request.edits[0].address = address;
	request.edits[0].field = field;
	request.edits[0].value = std::move(value);
	session.handle(request);
}

bool near(double a, double b, double tolerance = 0.01) { return std::fabs(a - b) <= tolerance; }

bool strutil_iequals(const std::string &a, const std::string &b) { return opennova::strutil::iequals(a, b); }

// The model the Preview follows, its viewport and its device: the Shell's pump, then what the
// device took, and the envelope.
struct Rig {
	ProjectSession &session;
	editor_test::FakeDevices devices;
	const SessionView &view() const { return session.view(); }
	const std::string &path() const { return view().documents.previews[ViewportKind::Model].path; }
	ViewportAction pump() {
		devices.sync(session);
		return devices.last(path(), ViewportKind::Model);
	}
	const ModelViewport *viewport() {
		return static_cast<const ModelViewport *>(session.viewports().find(path(), ViewportKind::Model));
	}
	// Its envelope; none kept, the kind's over no document (editor_test::empty_viewport_json).
	JsonValue json() {
		return viewport() ? viewport_to_json(view(), *viewport(), JsonPage())
						  : editor_test::empty_viewport_json(view(), ViewportKind::Model);
	}
	const PreviewClock &clock() { return session.viewports().clock(); }
	ViewportContext context(float snap = 0.0f) { return viewport_context(session.view(), *viewport(), snap); }
	// A SetViewport of the followed model's viewport.
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path(), change));
		return session.outcome().done();
	}
	uint64_t builds() { return viewport() ? viewport()->builds() : 0; }
};

JsonValue overlay(const JsonValue &shown, const char *kind, int index) {
	for (const JsonValue &row : shown.get("items")->array)
		if (row.get_string("kind", "") == kind && int(row.get("index")->number) == index) return row;
	return JsonValue();
}

// The marker of a kind and index, copied (the list is often a temporary).
std::optional<ModelOverlay> find_overlay(const std::vector<ModelOverlay> &overlays, ModelOverlayKind kind, int index) {
	for (const ModelOverlay &overlay : overlays)
		if (overlay.kind == kind && overlay.index == index) return overlay;
	return std::nullopt;
}

void apply(ProjectSession &session, const std::string &path, std::vector<Edit> edits) {
	EditorRequest request = request::edit_record(path, std::move(edits));
	session.handle(request);
	session.handle(request::end_edit(path));
}

} // namespace

static int test_status_and_builds() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	const SessionView &view = session.view();
	TEST_EXPECT(rig.pump() == ViewportAction::Keep);
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_project" && rig.json().get_string("units", "") == "pixels");

	session.handle(request::new_project(dir.file("project"), "Model Viewport Test"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	rig.pump();
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_model");
	TEST_EXPECT(rig.json().get_string("message", "") == "Open a model, a clip or an animation table to preview it.");

	// The open model, as it would save.
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && rig.path() == "models/armory.3di");
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	// What the device read as it built (a texture it looked for), reported.
	editor_test::FakeDevice *device = rig.devices.held(rig.path(), ViewportKind::Model);
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->reads = {"preview_skin.tga"};
	const ModelViewport *model = rig.viewport();
	TEST_EXPECT(rig.builds() == 1 && model->model() && rig.pump() == ViewportAction::Keep);
	const uint64_t reads = model->reads();
	JsonValue shown = rig.json();
	TEST_EXPECT(shown.get_string("status", "") == "ready" && shown.get_bool("current", false));
	TEST_EXPECT(shown.get_string("path", "") == "models/armory.3di" && shown.get_number("builds", 0) == 1.0);
	const JsonValue *points = shown.get("items");
	TEST_EXPECT(points && points->array.size() == model->model()->user_point_count + model->model()->light_count);
	TEST_EXPECT(!points->array.empty() && points->array[0].get_string("kind", "") == "user_point");
	const JsonValue *registers = shown.get("body")->get("registers");
	TEST_EXPECT(registers && registers->array.size() == model->model()->ctrl.count);
	// Nothing moved (ChangeClass None): nothing read again.
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && model->reads() == reads);

	// A user point moved: the overlays show it, nothing is built, and nothing is read (S13 V8: the
	// change set names the model row alone, alike but for its user points, so the held model is
	// patched with them).
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->ids.lists[3][0].id};
	const double x_before = points->array[0].get("position")->array[0].number;
	const double z_before = points->array[0].get("position")->array[2].number;
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, document->path(), point, "position.x", std::get<double>(value) + 1.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Update && rig.builds() == 1 && model->reads() == reads &&
			model->patches() == 1);
	shown = rig.json();
	TEST_EXPECT(shown.get_bool("current", false));
	const JsonValue &moved = shown.get("items")->array[0];
	TEST_EXPECT(!near(moved.get("position")->array[0].number, x_before) ||
	            !near(moved.get("position")->array[2].number, z_before));

	// A light's colour is drawn: it builds again.
	const NodeAddress light{row->id, node_kind(ModelKind::Light), document->model_row()->ids.lists[2][0].id};
	set(session, document->path(), light, "start.r", int64_t(12));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && rig.builds() == 2);

	// The options apply without a build or a read (a SetViewport: the document did not change).
	const uint64_t before_options = model->reads();
	TEST_EXPECT(rig.set(R"({"kind": "model", "options": {"lod": 0, "ctrl": {"HEAT": 5}}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Update && rig.builds() == 2 && model->reads() == before_options);
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && model->lod() == 0);
	shown = rig.json();
	TEST_EXPECT(shown.get("options")->get("lod")->number == 0.0);
	TEST_EXPECT(shown.get("options")->get("ctrl")->get("HEAT")->number == 5.0);

	// A texture the device read (or looked for) changes: it builds again.
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/textures/preview_skin.tga"), std::vector<uint8_t>(18, 0)));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && rig.builds() == 3);
	// Changed again in the same follow as a user point's edit (which alone is an Update): it builds.
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/textures/preview_skin.tga"), std::vector<uint8_t>(36, 0)));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, document->path(), point, "position.x", std::get<double>(value) + 1.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && rig.builds() == 4);

	// Saved and closed: the viewport goes with its document, and its device.
	session.handle(request::save_all());
	session.handle(request::close_document("models/armory.3di"));
	TEST_EXPECT(view.documents.previews[ViewportKind::Model].path.empty());
	rig.pump();
	TEST_EXPECT(session.viewports().size() == 0 && rig.devices.cache.size() == 0);
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_model");
	return 0;
}

// S13 V8 (the review): what the document cannot say (ChangeClass::Unknown: here a state an edit after
// an undo discarded) reads the model again and builds only where it draws otherwise, its user points
// aside: a light's colour another builds; a user point's place alone is an Update. A change set that
// names nothing patches nothing and is Keep.
static int test_unknown_and_empty_changes() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_unknown");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	session.handle(request::new_project(dir.file("project"), "Unknown Changes"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && rig.pump() == ViewportAction::Rebuild);
	if (!document) return 1;
	const ModelViewport *model = rig.viewport();
	const ModelRow *row = document->model_row();
	const NodeAddress light{row->id, node_kind(ModelKind::Light), row->ids.lists[2][0].id};
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->ids.lists[3][0].id};
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	const double x = std::get<double>(value);

	// A light's colour followed; undone and another set: the state followed is gone (Unknown), the
	// model drawn otherwise, built.
	set(session, document->path(), light, "start.r", int64_t(12));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	uint64_t builds = rig.builds(), reads = model->reads();
	session.handle(request::undo(document->path()));
	set(session, document->path(), light, "start.r", int64_t(40));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && model->followed_change() == ChangeClass::Unknown);
	TEST_EXPECT(rig.builds() == builds + 1 && model->reads() == reads + 1);
	// A user point's place followed; undone and another set: Unknown, drawn alike, an Update.
	set(session, document->path(), point, "position.x", x + 1.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Update);
	builds = rig.builds();
	reads = model->reads();
	session.handle(request::undo(document->path()));
	set(session, document->path(), point, "position.x", x + 2.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Update && model->followed_change() == ChangeClass::Unknown);
	TEST_EXPECT(rig.builds() == builds && model->reads() == reads + 1);

	// A viewport of its own over the document, followed by hand: a change set naming nothing is Keep,
	// nothing patched or read.
	ModelViewport alone(document->path());
	PreviewClock clock;
	alone.attach();
	TEST_EXPECT(alone.follow(ViewportInput{ rig.view(), clock, document, ChangeClass::Loaded }, clock) ==
			ViewportAction::Rebuild);
	TEST_EXPECT(alone.take_action() == ViewportAction::Rebuild);
	const ChangeSet nothing = RowChanges();
	const uint64_t alone_reads = alone.reads();
	TEST_EXPECT(alone.follow(ViewportInput{ rig.view(), clock, document, ChangeClass::Changed, &nothing }, clock) ==
			ViewportAction::Keep);
	TEST_EXPECT(alone.reads() == alone_reads && alone.patches() == 0 && alone.take_action() == ViewportAction::Keep);
	std::printf("test_unknown_and_empty_changes passed\n");
	return 0;
}

static int test_camera() {
	OrbitCamera camera;
	camera.target = PreviewVec3{1.0f, 2.0f, 3.0f};
	camera.distance = 10.0f;
	float x = 0.0f, y = 0.0f, depth = 0.0f;
	TEST_EXPECT(camera.project(camera.target, 800, 600, x, y, &depth));
	TEST_EXPECT(near(x, 400.0) && near(y, 300.0) && near(depth, 10.0, 0.001));
	// Right of the target on the screen's right, above it higher up.
	PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	const PreviewVec3 beside{camera.target.x + right.x, camera.target.y + right.y, camera.target.z + right.z};
	TEST_EXPECT(camera.project(beside, 800, 600, x, y) && x > 400.0f && near(y, 300.0));
	TEST_EXPECT(near(x - 400.0f, OrbitCamera::focal_pixels(800) / 10.0f));
	const PreviewVec3 above{camera.target.x + up.x, camera.target.y + up.y, camera.target.z + up.z};
	TEST_EXPECT(camera.project(above, 800, 600, x, y) && near(x, 400.0) && y < 300.0f);
	// Behind the eye: not on the device.
	const PreviewVec3 eye = camera.eye();
	const PreviewVec3 behind{eye.x + back.x, eye.y + back.y, eye.z + back.z};
	TEST_EXPECT(!camera.project(behind, 800, 600, x, y));
	// The game's horizontal field of view across the width.
	TEST_EXPECT(near(OrbitCamera::focal_pixels(800), 400.0 / std::tan(40.0 * 3.14159265358979 / 180.0), 0.01));
	// Framed: the sphere's edge is inside the device.
	camera.frame(PreviewVec3{0.0f, 1.0f, 0.0f}, 2.0f, 800, 600);
	TEST_EXPECT(camera.project(PreviewVec3{0.0f, 3.0f, 0.0f} , 800, 600, x, y) && y > 0.0f && y < 300.0f);
	// Panning slides the target in the view plane; dolly scales the distance.
	const float distance = camera.distance;
	camera.dolly(2.0f);
	TEST_EXPECT(near(camera.distance, distance * 2.0f, 0.001));
	const PreviewVec3 before = camera.target;
	camera.pan(100.0f, 0.0f, 800);
	TEST_EXPECT(!near(camera.target.x, before.x, 1e-4) || !near(camera.target.z, before.z, 1e-4));
	TEST_EXPECT(near(camera.target.y, before.y, 1e-4));
	return 0;
}

// The overlays ride the posed model: a user point on a part that turns (house_lod0_sine_rotx
// rocks its only part about x) moves with the preview clock and holds while it is paused; a hit at
// a marker's pixel picks it and names its record, the record maps back to the marker; the pivots
// show when asked.
static int test_overlays() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_overlays");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	session.handle(request::new_project(dir.file("project"), "Overlay Test"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/house.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/house.3di"));
	TEST_EXPECT(document && rig.pump() == ViewportAction::Rebuild);

	// The ground point moved off the axis the part turns about (the model's vertical): the
	// turning part carries it.
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->ids.lists[3][0].id};
	set(session, document->path(), point, "position.x", 2.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Update);
	const JsonValue at_zero = overlay(rig.json(), "user_point", 0);
	TEST_EXPECT(at_zero.get("position"));
	session.advance(0.25);
	TEST_EXPECT(rig.clock().ms() == 250);
	const JsonValue later = overlay(rig.json(), "user_point", 0);
	TEST_EXPECT(!near(later.get("position")->array[0].number, at_zero.get("position")->array[0].number, 1e-3) ||
	            !near(later.get("position")->array[2].number, at_zero.get("position")->array[2].number, 1e-3));
	TEST_EXPECT(near(later.get("position")->array[1].number, at_zero.get("position")->array[1].number, 1e-4));
	TEST_EXPECT(rig.set(R"({"clock": {"playing": false}})"));
	session.advance(0.25);
	TEST_EXPECT(rig.clock().ms() == 250 && !rig.json().get("clock")->get_bool("playing", true));
	TEST_EXPECT(rig.set(R"({"clock": {"time_ms": 0}})"));
	TEST_EXPECT(near(overlay(rig.json(), "user_point", 0).get("position")->array[0].number,
	                 at_zero.get("position")->array[0].number, 1e-4));

	// A hit at the marker's pixel picks it, and it names the record.
	const JsonValue shown = rig.json();
	const JsonValue marker = overlay(shown, "user_point", 0);
	TEST_EXPECT(marker.get("screen") && marker.get("screen")->array.size() == 2);
	const float sx = float(marker.get("screen")->array[0].number), sy = float(marker.get("screen")->array[1].number);
	const ViewportHit hit = rig.viewport()->hit(rig.context(), sx + 2.0f, sy - 2.0f);
	TEST_EXPECT(hit.kind == "user_point" && hit.index == 0 && hit.current);
	TEST_EXPECT(hit.id == point.child && NodeId(marker.get("id")->number) == point.child);
	TEST_EXPECT(rig.viewport()->hit(rig.context(), sx + 40.0f, sy + 40.0f).index == -1);
	ModelOverlayKind kind;
	int index = -1;
	TEST_EXPECT(model_overlay_of(*document, point, kind, index) && kind == ModelOverlayKind::UserPoint && index == 0);

	// The pivots, when asked: one per part of the drawn level, each naming its PANM row.
	TEST_EXPECT(rig.set(R"({"options": {"overlays": {"pivots": true, "user_points": false, "lights": false}}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Update);
	const std::vector<ModelOverlay> marks = rig.viewport()->overlays(rig.clock());
	TEST_EXPECT(marks.size() == rig.viewport()->model()->lods[0].render_object_count);
	for (const ModelOverlay &mark : marks) {
		TEST_EXPECT(mark.kind == ModelOverlayKind::Pivot);
		const NodeAddress record = model_overlay_record(*document, mark, rig.viewport()->lod());
		TEST_EXPECT(record.kind == node_kind(ModelKind::PartAnimation) && record.child != 0);
		TEST_EXPECT(model_overlay_of(*document, record, kind, index) && kind == ModelOverlayKind::Pivot && index == mark.index);
	}
	return 0;
}

// A drag of a marker writes its record: the place lands where the drag put it, through the
// turned part's pose (house_lod0_sine_rotx at a quarter second), snapped when asked; the
// axis turns to the drag; one gesture is one undo step; a pivot, and an omni light's axis,
// have no handle; a pixel on the view plane is the point the drag means; the MCP's drag.
static int test_handles() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_handles");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	session.handle(request::new_project(dir.file("project"), "Handle Test"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/house.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/house.3di"));
	TEST_EXPECT(document && rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(rig.set(R"({"clock": {"time_ms": 250}})"));
	const ModelViewport *model = rig.viewport();
	int32_t bus[96];
	model_preview_ctrl_bus(model->options().ctrl, bus);

	// The place: where the drag puts it, the part's pose undone and applied again.
	std::optional<ModelOverlay> point = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && point->part == 0);
	const PreviewVec3 to{point->at.x + 0.5f, point->at.y + 0.3f, point->at.z - 0.2f};
	std::vector<Edit> edits;
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(model_handle_edits(*document, *model->model(), *point, model->lod(), rig.clock().ms(), bus,
	                               ModelHandle::Place, to, 0.0f, gesture, edits));
	TEST_EXPECT(edits.size() == 3 && edits[0].field == "position.x" && edits[0].gesture == gesture);
	const uint64_t before = document->revision();
	apply(session, document->path(), edits);
	TEST_EXPECT(document->revision() != before && rig.pump() == ViewportAction::Update);
	point = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && near(point->at.x, to.x, 1e-3) && near(point->at.y, to.y, 1e-3) && near(point->at.z, to.z, 1e-3));
	TEST_EXPECT(rig.builds() == 1);
	// To where it is: nothing to write (S13 V7: a field the record would hold as it holds it, its
	// 16.16 word within half a step, gets no edit).
	TEST_EXPECT(point && model_handle_edits(*document, *model->model(), *point, model->lod(), rig.clock().ms(), bus,
	                                        ModelHandle::Place, point->at, 0.0f, next_edit_gesture(), edits) &&
	            edits.empty());

	// Snapped: every place field on the grid.
	TEST_EXPECT(model_handle_edits(*document, *model->model(), *point, model->lod(), rig.clock().ms(), bus,
	                               ModelHandle::Place, PreviewVec3{0.37f, 1.13f, -0.61f}, 0.25f, next_edit_gesture(), edits));
	for (const Edit &edit : edits) {
		const double value = std::get<double>(edit.value);
		TEST_EXPECT(near(value / 0.25, std::round(value / 0.25), 1e-9));
	}

	// The axis: turned toward the drag.
	const PreviewVec3 aim{point->at.x, point->at.y + 1.0f, point->at.z + 1.0f};
	TEST_EXPECT(model_handle_edits(*document, *model->model(), *point, model->lod(), rig.clock().ms(), bus,
	                               ModelHandle::Axis, aim, 0.0f, next_edit_gesture(), edits));
	// The axis's fields the turn changes (S13 V7: one it leaves as the record holds it gets no edit).
	TEST_EXPECT(!edits.empty() && edits.size() <= 3);
	for (const Edit &edit : edits) TEST_EXPECT(edit.field.rfind("direction.", 0) == 0);
	apply(session, document->path(), edits);
	rig.pump();
	point = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	const float inv = 1.0f / std::sqrt(2.0f);
	TEST_EXPECT(point && point->has_direction && near(point->direction.x, 0.0, 1e-3) &&
	            near(point->direction.y, inv, 1e-3) && near(point->direction.z, inv, 1e-3));

	// One drag, one undo step: undo takes the axis back, then the place.
	session.handle(request::undo(document->path()));
	session.handle(request::undo(document->path()));
	rig.pump();
	point = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && !near(point->at.x, to.x, 1e-3));

	// No handle: a pivot (geometry), an omni light's axis.
	TEST_EXPECT(rig.set(R"({"options": {"overlays": {"pivots": true}}})"));
	rig.pump();
	const std::vector<ModelOverlay> marks = model->overlays(rig.clock());
	std::optional<ModelOverlay> pivot = find_overlay(marks, ModelOverlayKind::Pivot, 0);
	TEST_EXPECT(pivot && !model_handle_edits(*document, *model->model(), *pivot, model->lod(), rig.clock().ms(), bus,
	                                         ModelHandle::Place, to, 0.0f, 1, edits));
	for (const ModelOverlay &mark : marks)
		if (mark.kind == ModelOverlayKind::Light && !mark.has_direction)
			TEST_EXPECT(!model_handle_edits(*document, *model->model(), mark, model->lod(), rig.clock().ms(), bus,
			                                ModelHandle::Axis, to, 0.0f, 1, edits));

	// The pixel a marker is drawn at, on the plane through it, is the marker's point.
	const int width = model->size().width, height = model->size().height;
	std::optional<ModelOverlay> light = find_overlay(marks, ModelOverlayKind::Light, 0);
	if (light) {
		float x = 0.0f, y = 0.0f;
		PreviewVec3 back;
		TEST_EXPECT(model->camera().project(light->at, width, height, x, y));
		TEST_EXPECT(model->camera().on_view_plane(x, y, width, height, light->at, back));
		TEST_EXPECT(near(back.x, light->at.x, 1e-3) && near(back.y, light->at.y, 1e-3) && near(back.z, light->at.z, 1e-3));
		TEST_EXPECT(model->handle_edits(*document, *light, ModelHandle::Place, x, y, width, height, 0.0f,
				next_edit_gesture(), rig.clock(), edits));
		apply(session, document->path(), edits);
		rig.pump();
		std::optional<ModelOverlay> still = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::Light, 0);
		TEST_EXPECT(still && near(still->at.x, light->at.x, 1e-3) && near(still->at.y, light->at.y, 1e-3) &&
		            near(still->at.z, light->at.z, 1e-3));
	}

	// The editor MCP's drag (ModelViewport::drag): the user point's record to a picture pixel, the
	// canvas's plan, one undo step; refused for a record no marker is, and while the picture is of
	// another revision.
	rig.pump();
	point = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	const NodeAddress record = model->record_of(rig.context().input, *point);
	TEST_EXPECT(record.child != 0);
	float px = 0.0f, py = 0.0f;
	TEST_EXPECT(model->camera().project(point->at, width, height, px, py));
	const uint64_t before_drag = document->revision();
	ViewportDrag drag;
	drag.id = 999999;
	drag.handle = "place";
	drag.by = false;
	drag.x = px + 20.0f;
	drag.y = py;
	editor_test::Gathered planned;
	std::string error;
	TEST_EXPECT(!model->drag(rig.context(), drag, planned, error) && planned.requests.empty());
	drag.id = record.child;
	TEST_EXPECT(model->drag(rig.context(), drag, planned, error) && planned.requests.size() == 2);
	TEST_EXPECT(editor_test::serve(session, planned.requests) && document->revision() != before_drag);
	editor_test::Gathered stale;
	drag.x = px;
	TEST_EXPECT(!model->drag(rig.context(), drag, stale, error) && stale.requests.empty());
	rig.pump();
	std::optional<ModelOverlay> dragged = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(dragged && !near(dragged->at.x, point->at.x, 1e-3));
	session.handle(request::undo(document->path()));
	rig.pump();
	dragged = find_overlay(model->overlays(rig.clock()), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(dragged && near(dragged->at.x, point->at.x, 1e-3) && near(dragged->at.y, point->at.y, 1e-3));

	// An operation holding the documents (S13 A3): the drag plans nothing and says why, the model
	// as it was.
	const uint64_t held = document->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	editor_test::Gathered holding;
	drag.x = px + 20.0f;
	TEST_EXPECT(!model->drag(rig.context(), drag, holding, error) && holding.requests.empty() &&
			error.find("takes no edit") != std::string::npos);
	TEST_EXPECT(document->revision() == held);
	session.run_operations();
	return 0;
}

// What a SetViewport sets (the model viewport's apply): each member over what the viewport holds,
// a number's fraction dropped, the pitch kept short of straight up or down; a member unknown, of
// another type or out of range refuses the whole change and changes nothing.
static int test_options_from_json() {
	const auto parse = [](const char *text) {
		JsonValue json;
		std::string error;
		opennova::io::json_parse(text, json, error);
		return json;
	};
	ModelViewport model("models/a.3di");
	PreviewClock clock;
	std::string error;
	TEST_EXPECT(model.apply(
	        parse(R"({"kind": "model", "options": {"lod": 2, "ctrl": {"GEAR": 3, "FLAP": 0}, "overlays": {"pivots": true},)"
	              R"( "rig_model": "rig.3di"}, "clock": {"playing": false, "time_ms": 250.7, "ticks": 8}})"),
	        clock, error));
	const ModelViewportOptions &options = model.options();
	TEST_EXPECT(options.lod == 2 && options.ctrl.size() == 1 && options.ctrl.at("GEAR") == 3 && !clock.playing());
	TEST_EXPECT(options.overlays.pivots && options.rig_model == "rig.3di" && clock.ms() == 250 && clock.ticks() == 8);
	TEST_EXPECT(model.apply(parse(R"({"options": {"lod": "auto"}})"), clock, error) && model.options().lod == -1);
	const ModelViewportOptions held = model.options();
	for (const char *bad : {R"({"options": {"lod": 256}})", R"({"options": {"lod": "near"}})",
	                        R"({"options": {"ctrl": {"GEAR": "x"}}})", R"({"options": {"ctrl": 3}})",
	                        R"({"options": {"overlays": {"lamps": true}}})", R"({"clock": {"time_ms": -1}})",
	                        R"({"clock": {"time_ms": "9"}})", R"({"clock": {"ticks": -1}})",
	                        R"({"clock": {"playing": 1}})", R"({"options": {"bogus": 1}})",
	                        R"({"clock": {"playing": true}, "options": {"lod": -2}})", R"({"kind": "menu"})"}) {
		error.clear();
		TEST_EXPECT(!model.apply(parse(bad), clock, error) && !error.empty());
		TEST_EXPECT(model.options() == held && clock.ms() == 250 && !clock.playing());
	}
	// The clock's last millisecond, and any later time taken as it; never wrapped round to an early
	// one.
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 4294967294}})"), clock, error) && clock.ms() == 4294967294u);
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 4294967295}})"), clock, error) && clock.ms() == 4294967295u);
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 250}})"), clock, error) && clock.ms() == 250);
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 4294967296, "playing": true}})"), clock, error) &&
	            clock.ms() == 4294967295u && clock.playing());
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 1e15}})"), clock, error) && clock.ms() == 4294967295u);
	TEST_EXPECT(model.apply(parse(R"({"clock": {"time_ms": 250, "playing": false, "rate": 2}})"), clock, error) &&
	            clock.ms() == 250 && !clock.playing() && clock.rate() == 2.0);

	TEST_EXPECT(model.apply(
	        parse(R"({"camera": {"yaw": 1.25, "pitch": 3.0, "distance": 12, "target": [1, 2, 3]},)"
	              R"( "device": {"width": 640, "height": 480.9}})"),
	        clock, error));
	const OrbitCamera &camera = model.camera();
	TEST_EXPECT(near(camera.yaw, 1.25, 1e-6) && near(camera.pitch, kOrbitPitchLimit, 1e-6) &&
	            near(camera.distance, 12.0, 1e-6));
	TEST_EXPECT(near(camera.target.x, 1.0, 1e-6) && near(camera.target.y, 2.0, 1e-6) && near(camera.target.z, 3.0, 1e-6));
	TEST_EXPECT(model.state().width == 640 && model.state().height == 480);
	TEST_EXPECT(model.apply(parse(R"({"camera": {"pitch": -9}})"), clock, error) &&
	            near(model.camera().pitch, -kOrbitPitchLimit, 1e-6));
	for (const char *bad : {R"({"camera": {"distance": 0}})", R"({"camera": {"target": [1, 2]}})",
	                        R"({"camera": {"target": [1, 2, "3"]}})", R"({"device": {"width": 0}})",
	                        R"({"device": {"height": 8193}})", R"({"camera": {"frame": 1}})", R"({"camera": {"zoom": 2}})",
	                        R"({"camera": {"yaw": 0.5, "distance": -1}})"}) {
		error.clear();
		TEST_EXPECT(!model.apply(parse(bad), clock, error));
		TEST_EXPECT(near(model.camera().yaw, 1.25, 1e-6) && near(model.camera().distance, 12.0, 1e-6) &&
		            model.state().width == 640);
	}
	std::printf("test_options_from_json passed\n");
	return 0;
}

// A clip or a table plays on its rig's model: the table on the model an item pairs with it
// (its graphic beside its anim_def), the clip on the table that names it; the rig loads
// through the game's loader over the model's bone table; the selected row plays, the preview
// clock's ticks run it, and its trigger events sit on the ticks the clip's own clock first
// samples their frames; a table no item pairs has no rig until a model is chosen.
static int test_animation() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_animation");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Animation Test"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	// The skinned fixture with two user points at one place off the bones' axis: on the child bone
	// (part 1) and on the root (part 0).
	std::vector<uint8_t> skinned = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                  "/fixtures/threedi/o3d/skinned.o3d");
	const std::string tip = "userpoint tip 0 0.25 1 0 0 1 1\nuserpoint hip 0 0.25 1 0 0 1 0\n";
	skinned.insert(skinned.end(), tip.begin(), tip.end());
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d", skinned));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("skinned.3di") && view.project.scan->find("SKIN.adm") && view.project.scan->find("walk.bad"));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def",
	                                    "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\n"
	                                    "anim_def skin\nend\n"));
	session.handle(request::rescan());
	session.run_operations();

	// The table plays on the item's graphic.
	session.handle(request::open_document("anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	TEST_EXPECT(table && rig.path() == "anims/SKIN.adm");
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	JsonValue shown = rig.json();
	TEST_EXPECT(shown.get_string("status", "") == "ready" && shown.get_string("path", "") == "anims/SKIN.adm");
	const JsonValue *animation = shown.get("body")->get("animation");
	TEST_EXPECT(animation && strutil_iequals(animation->get_string("model", ""), "skinned.3di"));
	TEST_EXPECT(animation->get_string("source", "").find("Skinned Thing") != std::string::npos);
	const ModelViewport *model = rig.viewport();
	TEST_EXPECT(animation->get_bool("rig", false) && model->skeleton() && model->skeleton()->bone_count() == 3);
	TEST_EXPECT(model->caption() == " on " + model->rig().model);

	// The walk row selected: it plays from tick 0, the clock in game ticks. The clock sought is one of
	// the follow's derived changes: the view's Viewports concern moves at the pump (the clock running
	// as frames pass never moves it).
	NodeAddress walk;
	TEST_EXPECT(find_definition(AssetGraph(), *table, "anim_walk_forward", walk));
	uint64_t moved = view.revisions.of(ViewConcern::Viewports);
	session.advance(0.5);
	TEST_EXPECT(rig.clock().ticks() > 0 && view.revisions.of(ViewConcern::Viewports) == moved);
	EditorRequest select = request::select_record(table->path(), walk);
	session.handle(select);
	TEST_EXPECT(view.revisions.of(ViewConcern::Viewports) == moved);
	rig.pump();
	TEST_EXPECT(model->clip_key() == "anim_walk_forward" && model->clip_variant() == 0 && rig.clock().ticks() == 0);
	TEST_EXPECT(view.revisions.of(ViewConcern::Viewports) > moved);
	TEST_EXPECT(strutil_iequals(model->clip_file(), "walk"));
	session.advance(1.0);
	TEST_EXPECT(rig.clock().ticks() == 62);
	// Its triggers (frames 1 and 3; the last row repeats the one before, as retail's
	// exporter writes it) on the ticks the clip's clock first reaches them.
	const std::vector<PreviewClipEvent> &events = model->clip_events();
	TEST_EXPECT(events.size() >= 2 && events[0].frame == 1 && events[0].trigger == 1u && events[1].frame == 3 &&
	            events[1].trigger == 2u && events[0].tick > 0 && events[1].tick > events[0].tick);
	TEST_EXPECT(model->clip_loops() && model->clip_length_ticks() > events[1].tick);

	// The clip plays on the table that names it, as the row that names it (a viewport of its own).
	session.handle(request::open_document("anims/walk.bad"));
	TEST_EXPECT(rig.path() == "anims/walk.bad");
	rig.pump();
	const ModelViewport *clip_model = rig.viewport();
	TEST_EXPECT(clip_model && clip_model != model);
	TEST_EXPECT(clip_model->view_status() == ModelViewStatus::Ready && strutil_iequals(clip_model->rig().table, "SKIN.adm"));
	TEST_EXPECT(clip_model->clip_key() == "anim_walk_forward" && clip_model->skeleton());
	// An event selected in the clip seeks the clock to it and holds it there.
	Document *clip = session.document_for("anims/walk.bad");
	TEST_EXPECT(clip && !clip->rows().empty());
	const Node &clip_row = *clip->rows().front();
	EditorRequest pick = request::select_record(clip->path(),
			{ clip_row.id, node_kind(AnimationKind::Event), clip_row.collections[1][3] });
	session.handle(pick);
	session.advance(0.5);
	moved = view.revisions.of(ViewConcern::Viewports);
	rig.pump();
	TEST_EXPECT(rig.clock().ticks() == clip_model->tick_of_frame(3) && !rig.clock().playing());
	TEST_EXPECT(view.revisions.of(ViewConcern::Viewports) > moved);
	session.advance(0.5);
	TEST_EXPECT(rig.clock().ticks() == clip_model->tick_of_frame(3));

	// No item pairs a table: no rig, until a model is chosen.
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def", "begin \"Nothing\"\nid 100201\ntype building\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("anims/SKIN.adm"));
	rig.pump();
	TEST_EXPECT(model->view_status() == ModelViewStatus::NoRig);
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_rig");
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && model->view_status() == ModelViewStatus::Ready);
	TEST_EXPECT(model->rig().source == "chosen" && model->skeleton());

	// A rig model that does not read: the picture dropped, the model read once and not again each
	// pump (the failure latch), until its file changes; another model chosen, read and shown.
	TEST_EXPECT(editor_test::write_text(view.project.root + "/models/garbage.3di", "not a model"));
	session.handle(request::rescan());
	session.run_operations();
	const uint64_t rig_reads = model->rig_model_reads();
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "garbage.3di"}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Clear && model->view_status() == ModelViewStatus::Unreadable);
	TEST_EXPECT(model->rig_model_reads() == rig_reads + 1);
	for (int i = 0; i < 4; ++i) TEST_EXPECT(rig.pump() == ViewportAction::Keep);
	TEST_EXPECT(model->rig_model_reads() == rig_reads + 1 && model->view_status() == ModelViewStatus::Unreadable);
	TEST_EXPECT(editor_test::write_text(view.project.root + "/models/garbage.3di", "still not a model, a longer one"));
	session.handle(request::rescan());
	session.run_operations();
	rig.pump();
	rig.pump();
	TEST_EXPECT(model->rig_model_reads() == rig_reads + 2 && model->view_status() == ModelViewStatus::Unreadable);
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && model->view_status() == ModelViewStatus::Ready);
	TEST_EXPECT(model->rig_model_reads() == rig_reads + 3);
	return 0;
}

// A one-shot clip set over the skinned fixture's bones: `step` runs 3 frames at 100 fps,
// fast enough that its last frame comes up only on the tick the clip stops.
constexpr const char *kStepClips = R"(o3a 1
adm STEP.adm
row anim_reset "stand"
row anim_idle "step"
clip stand
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip step
fps 100
flags 0x0
frames 3
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0.3826834 0.9238795
 k 0 0 0.7071068 0.7071068
 k 0 0 0.7071068 0.7071068
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x1 0.9 1.7
event 0 0 0 0x2 0.9 1.7
event 0 0 0 0x2 0.9 1.7
)";

// The viewport plays what the game's loader registered, from the open table as edited: a
// token whose file does not load registers nothing, so the row's clip after it is the
// slot's next variant, and the missing one plays nothing. A clip's events sit on the ticks
// the game's root motion reads their triggers; a one-shot's stopped tick reads none.
// [orig: AnimMap_ParseConfigLine @0x40cb60, the load gate @0x40cbe7;
//  AnimMap_FindOrLoadBoneFile @0x40c030, no failsafe @0x40c260]
static int test_runtime_clips() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_runtime");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Runtime Clips"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	// The skinned fixture with two user points at one place off the bones' axis: on the child bone
	// (part 1) and on the root (part 0).
	std::vector<uint8_t> skinned = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                  "/fixtures/threedi/o3d/skinned.o3d");
	const std::string tip = "userpoint tip 0 0.25 1 0 0 1 1\nuserpoint hip 0 0.25 1 0 0 1 0\n";
	skinned.insert(skinned.end(), tip.begin(), tip.end());
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d", skinned));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	TEST_EXPECT(editor_test::write_text(source + "/step.o3a", kStepClips));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}, {source + "/step.o3a", {}}};
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("SKIN.adm") && view.project.scan->find("STEP.adm") && view.project.scan->find("step.bad"));

	// The walk row edited to "reset" "missing" "walk", not saved; the table plays on the model
	// chosen for its viewport.
	session.handle(request::open_document("anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	const ModelViewport *model = rig.viewport();
	NodeAddress walk;
	TEST_EXPECT(table && model && find_definition(AssetGraph(), *table, "anim_walk_forward", walk));
	const NodeKind clip_kind = node_kind(AnimationMapKind::Clip);
	EditorRequest select = request::select_record(table->path(), {});
	size_t at = 0;
	for (const char *name : {"reset", "missing"}) {
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {walk.row, clip_kind, 0};
		add.position = at++;
		apply(session, table->path(), {add});
		if (at == 1) {
			// An empty token before walk: the table cannot be written, so the project's files
			// keep the saved one, whose tokens are not the row's; walk selected plays nothing,
			// and the viewport says why.
			TEST_EXPECT(!table->serialize().ok() && table->row(walk.row)->collections[0].size() == 2);
			select.address = {walk.row, clip_kind, table->row(walk.row)->collections[0][1]};
			session.handle(select);
			rig.pump();
			TEST_EXPECT(model->view_status() == ModelViewStatus::Unserializable && !model->skeleton());
			TEST_EXPECT(model->clip_key().empty() && model->clip_file().empty() && model->clip_events().empty());
			TEST_EXPECT(rig.json().get_string("reason", "") == "unserializable");
			// Nor does the empty token, which is the saved table's walk token by index.
			select.address = {walk.row, clip_kind, table->row(walk.row)->collections[0][0]};
			session.handle(select);
			rig.pump();
			TEST_EXPECT(model->view_status() == ModelViewStatus::Unserializable && model->clip_key().empty());
		}
		set(session, table->path(), {walk.row, clip_kind, table->last_added()}, "clip", std::string(name));
	}
	TEST_EXPECT(table->row(walk.row) && table->row(walk.row)->collections[0].size() == 3 && table->serialize().ok());
	const std::vector<NodeId> tokens = table->row(walk.row)->collections[0];
	select.address = {walk.row, clip_kind, tokens[2]};
	session.handle(select);
	rig.pump();
	TEST_EXPECT(model->view_status() == ModelViewStatus::Ready && model->skeleton());
	TEST_EXPECT(model->clip_key() == "anim_walk_forward" && model->clip_variant() == 1 &&
	            strutil_iequals(model->clip_file(), "walk"));
	const auto *served = model->skeleton() ? model->skeleton()->find_clip_variant(model->clip_key(), model->clip_variant())
	                                       : nullptr;
	TEST_EXPECT(served && served->source.file == "walk" && served->source.token == 2 && served->clip.frame_count == 4);
	// The missing token, selected: the game leaves it out, so the slot plays its other clips; the
	// viewport plays the row's first that registered and says why (S17).
	select.address = {walk.row, clip_kind, tokens[1]};
	session.handle(select);
	rig.pump();
	TEST_EXPECT(model->clip_key() == "anim_walk_forward" && model->clip_variant() == 0 &&
	            strutil_iequals(model->clip_file(), "reset"));
	TEST_EXPECT(model->clip_note() ==
	            "missing is not in the project: the game leaves it out of walk forward's clips. walk forward plays reset here.");

	// The one-shot's events against the game's root motion over the same files (its viewport's
	// rig model chosen as well).
	session.handle(request::open_document("anims/step.bad"));
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	rig.pump();
	const ModelViewport *step = rig.viewport();
	TEST_EXPECT(step->view_status() == ModelViewStatus::Ready && strutil_iequals(step->rig().table, "STEP.adm"));
	TEST_EXPECT(step->clip_key() == "anim_idle" && step->clip_variant() == 0 && !step->clip_loops());
	const std::vector<PreviewClipEvent> events = step->clip_events();
	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(view.project.root + "/anims"));
	opennova::assets::AssetStore store{&index};
	opennova::anim::AdmRootMotion motion;
	const int adm = motion.register_adm(&store, "STEP.adm");
	const int idle = opennova::world::anim_state::kIdle;
	TEST_EXPECT(adm >= 0 && opennova::world::infantry_anim_key(idle) == "anim_idle");
	std::vector<PreviewClipEvent> fired;
	for (int32_t tick = 0; tick <= 8; ++tick) {
		uint32_t words[4] = {};
		const int n = motion.scan_triggers(adm, idle, tick - 1, tick, words, 4, 0);
		for (int i = 0; i < n; ++i)
			if (words[i]) fired.push_back({-1, tick, words[i]});
	}
	TEST_EXPECT(events.size() == 1 && events[0].frame == 1 && events[0].tick == 1 && events[0].trigger == 1u);
	TEST_EXPECT(fired.size() == events.size());
	for (size_t i = 0; i < fired.size() && i < events.size(); ++i)
		TEST_EXPECT(fired[i].tick == events[i].tick && fired[i].trigger == events[i].trigger);
	TEST_EXPECT(step->clip_length_ticks() == 2 && step->tick_of_frame(1) == 1 && step->tick_of_frame(2) == -1);
	return 0;
}

// A clip set over the skinned fixture's bones whose `bend` turns the pelvis a quarter about x from
// its frame 1, so the spine and the leg swing off the axis.
constexpr const char *kBendClips = R"(o3a 1
adm BEND.adm
row anim_reset "rest"
row anim_idle "bend"
clip rest
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip bend
fps 30
flags 0x1
frames 2
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0.7071068 0 0 0.7071068
 k 0.7071068 0 0 0.7071068
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
)";

// A clip whose `flip` turns the pelvis half round from frame 1, so what stands above it swings below:
// the model posed away from its rest sphere (as a first-person clip poses its rig: the demo round's bug 4).
constexpr const char *kFlipClips = R"(o3a 1
adm FLIP.adm
row anim_reset "frest"
row anim_idle "flip"
clip frest
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip flip
fps 30
flags 0x1
frames 2
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 1 0 0 0
 k 1 0 0 0
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
)";

struct RecordedRequests final : CanvasRequests {
	std::vector<EditorRequest> raised;
	void request(EditorRequest request) override { raised.push_back(std::move(request)); }
};

// The clip's preview made a modder's (ADR 0046 S17): a row whose one clip the project lacks plays
// what the game plays in its place, the reset clip, and says so; a map names who plays it and what
// the game does with the selected row, a clip the rows that play it, a model the maps it plays; the
// timeline steps frame by frame and tells frames and seconds; a one-shot repeats after its hold when
// asked; the rig's bones pose with the clip, each named, a point on a bone riding it, and a click on
// a joint in the clip's own document selects its bone; an item is found by its catalog's name.
static int test_clip_preview() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_clip_preview");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Clip Preview"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	// The skinned fixture with two user points at one place off the bones' axis: on the child bone
	// (part 1) and on the root (part 0).
	std::vector<uint8_t> skinned = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                  "/fixtures/threedi/o3d/skinned.o3d");
	const std::string tip = "userpoint tip 0 0.25 1 0 0 1 1\nuserpoint hip 0 0.25 1 0 0 1 0\n";
	skinned.insert(skinned.end(), tip.begin(), tip.end());
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d", skinned));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	TEST_EXPECT(editor_test::write_text(source + "/step.o3a", kStepClips));
	TEST_EXPECT(editor_test::write_text(source + "/bend.o3a", kBendClips));
	TEST_EXPECT(editor_test::write_text(source + "/flip.o3a", kFlipClips));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}, {source + "/step.o3a", {}},
	                  {source + "/bend.o3a", {}}, {source + "/flip.o3a", {}}};
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def",
	                                    "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\n"
	                                    "anim_def skin\nend\n"));
	session.handle(request::rescan());
	session.run_operations();

	// The walk row's one clip renamed to one the project lacks: the game leaves it out, the slot is
	// unauthored, and an unauthored slot plays the reset row's first clip. The viewport plays that
	// and says why; the map names its player and what the game does with the row.
	session.handle(request::open_document("anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	NodeAddress walk;
	TEST_EXPECT(table && find_definition(AssetGraph(), *table, "anim_walk_forward", walk));
	if (!table) return 1;
	const NodeKind clip_kind = node_kind(AnimationMapKind::Clip);
	const NodeId walk_token = table->row(walk.row)->collections[0][0];
	set(session, table->path(), {walk.row, clip_kind, walk_token}, "clip", std::string("nothere.bad"));
	session.handle(request::select_record(table->path(), walk));
	rig.pump();
	const ModelViewport *model = rig.viewport();
	TEST_EXPECT(model && model->view_status() == ModelViewStatus::Ready && model->clip_key() == "anim_reset" &&
	            strutil_iequals(model->clip_file(), "reset"));
	TEST_EXPECT(model->clip_note() == "walk forward has no clip the game loads (none of its files is in the project): "
	                                  "the game plays the reset clip, reset, in its place.");
	JsonValue shown = rig.json();
	const JsonValue *animation = shown.get("body")->get("animation");
	TEST_EXPECT(animation && animation->get_string("note", "") == model->clip_note());
	const JsonValue *players = animation->get("players");
	TEST_EXPECT(players && players->array.size() == 1 && players->array[0].get_string("record", "") == "Skinned Thing" &&
	            strutil_iequals(players->array[0].get_string("model", ""), "skinned.3di") &&
	            !players->array[0].get_bool("first_person", true));
	const JsonValue *notes = animation->get("notes");
	TEST_EXPECT(notes && !notes->array.empty() && notes->array[0].string == "walk forward (slot 1): Walking forward.");
	// A slot the game picks only when the map has its clip (an NPC's attack, S17 review): the note says
	// the game never picks it then and what it does instead, the reset clip shown as what the slot serves.
	set(session, table->path(), walk, "key", std::string("anim_attack"));
	session.handle(request::select_record(table->path(), walk));
	rig.pump();
	TEST_EXPECT(model->clip_key() == "anim_reset" &&
	            model->clip_note() == "attack has no clip the game loads (none of its files is in the project): the game "
	                                  "never picks attack then (the NPC takes another reaction, or closes in). The slot "
	                                  "serves the reset clip, reset, shown here; a mission's forced animation of it plays "
	                                  "that.");
	session.handle(request::undo(table->path()));
	session.handle(request::undo(table->path()));
	session.handle(request::select_record(table->path(), walk));
	rig.pump();
	TEST_EXPECT(model->clip_key() == "anim_walk_forward" && model->clip_note().empty());

	// The timeline's frames and seconds: the walk's four frames at 30, stepped frame by frame from
	// tick to tick the clip's clock first runs on each; its events in words.
	TEST_EXPECT(model->clip_frame_count() == 4 && model->clip_fps() == 30);
	TEST_EXPECT(model->tick_of_step(0, 1) == model->tick_of_frame(1) &&
	            model->tick_of_step(model->tick_of_frame(3), -1) == model->tick_of_frame(2) &&
	            model->tick_of_step(0, -1) == 0);
	shown = rig.json();
	animation = shown.get("body")->get("animation");
	TEST_EXPECT(int(animation->get("frame_count")->number) == 4 && int(animation->get("fps")->number) == 30);
	TEST_EXPECT(near(animation->get("length_seconds")->number, model->clip_length_ticks() / 62.5, 1e-6));
	const JsonValue *events = animation->get("events");
	TEST_EXPECT(events && events->array.size() >= 2 && events->array[0].get_string("words", "") == "left footstep" &&
	            events->array[1].get_string("words", "") == "right footstep");
	// The wire's form of a scrub or a step: a frame, the clock held on the tick that first shows it.
	TEST_EXPECT(rig.set(R"({"frame": 2})") && rig.clock().ticks() == model->tick_of_frame(2) && !rig.clock().playing());
	TEST_EXPECT(!rig.set(R"({"frame": -1})") && rig.clock().ticks() == model->tick_of_frame(2));
	// A frame past the clip's shows its last.
	TEST_EXPECT(model->tick_of_frame_shown(99) == model->tick_of_frame_shown(int(model->clip_frame_count()) - 1));

	// The clip's own document: the rows that play it.
	session.handle(request::open_document("anims/walk.bad"));
	rig.pump();
	shown = rig.json();
	const JsonValue *uses = shown.get("body")->get("animation")->get("uses");
	TEST_EXPECT(uses && uses->array.size() == 1 && uses->array[0].get_string("map", "") == "anims/SKIN.adm" &&
	            uses->array[0].get_string("words", "") == "walk forward");

	// "Reading" only until the first validation has read the project's references (S17 review): an
	// animation no item pairs reads as such, and goes on doing so while a validation runs after an edit.
	session.handle(request::open_document("anims/step.bad"));
	rig.pump();
	TEST_EXPECT(view.activity.validation.read && rig.viewport() &&
	            rig.viewport()->view_status() == ModelViewStatus::NoRig);
	{
		Document *step_clip = session.document_for("anims/step.bad");
		TEST_EXPECT(step_clip && !step_clip->rows().empty());
		if (step_clip && !step_clip->rows().empty()) {
			set(session, step_clip->path(), {step_clip->rows().front()->id, step_clip->rows().front()->kind, 0}, "fps",
			    int64_t(25));
			TEST_EXPECT(view.activity.validation.running && view.activity.validation.read);
			rig.pump();
			TEST_EXPECT(rig.viewport()->view_status() == ModelViewStatus::NoRig);
			session.handle(request::undo(step_clip->path()));
		}
	}
	// A one-shot repeats from its start after its length and the hold; with Repeat off it holds its end.
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	rig.pump();
	const ModelViewport *step = rig.viewport();
	TEST_EXPECT(step && !step->clip_loops() && step->clip_length_ticks() == 2 && step->options().repeat);
	PreviewClock clock;
	clock.seek_ticks(1);
	TEST_EXPECT(step->clip_ticks(clock) == 1);
	clock.seek_ticks(2 + kClipRepeatHoldTicks + 1);
	TEST_EXPECT(step->clip_ticks(clock) == 1);
	TEST_EXPECT(rig.set(R"({"options": {"repeat": false}})"));
	rig.pump();
	TEST_EXPECT(!step->options().repeat && step->clip_ticks(clock) == 2 + kClipRepeatHoldTicks + 1);

	// The bones: the rest pose's on the axis, the bend's frame 1 swinging the spine a quarter turn off
	// it; a point on the spine at rest carried to the spine's joint; the bones on the wire by name.
	session.handle(request::open_document("anims/bend.bad"));
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	rig.pump();
	const ModelViewport *bend = rig.viewport();
	TEST_EXPECT(bend && bend->view_status() == ModelViewStatus::Ready && bend->clip_key() == "anim_idle");
	clock.seek_ticks(0);
	std::vector<PreviewJoint> joints = bend->joints(clock);
	TEST_EXPECT(joints.size() == 3 && joints[0].name == "BN01 Pelvis" && joints[1].name == "BN02 Spine" &&
	            joints[1].parent == 0 && joints[0].parent == -1);
	if (joints.size() != 3) return 1;
	// Signs pinned (S17 review: a mirrored rig passes no test): the spine's joint up at rest, and the
	// quarter turn of the pelvis swinging it to -x in the preview's frame.
	TEST_EXPECT(near(joints[1].at.x, 0.0) && near(joints[1].at.y, 1.0) && near(joints[1].at.z, 0.0));
	const PreviewVec3 spine_rest = joints[1].at;
	// The user points through the viewport's own overlays (the path the picture and the wire take).
	const auto point_at = [&](const PreviewClock &at, int index) {
		const std::optional<ModelOverlay> point = find_overlay(bend->overlays(at), ModelOverlayKind::UserPoint, index);
		return point ? point->at : PreviewVec3{-99.0f, -99.0f, -99.0f};
	};
	const PreviewVec3 tip_rest = point_at(clock, 0), hip_rest = point_at(clock, 1);
	TEST_EXPECT(near(tip_rest.x, 0.25) && near(tip_rest.y, 1.0) && near(tip_rest.z, 0.0) && near(hip_rest.x, 0.25) &&
	            near(hip_rest.y, 1.0) && near(hip_rest.z, 0.0));
	clock.seek_ticks(bend->tick_of_frame(1));
	TEST_EXPECT(bend->tick_of_frame(1) > 0);
	joints = bend->joints(clock);
	TEST_EXPECT(joints.size() == 3 && near(joints[1].at.x, -1.0) && near(joints[1].at.y, 0.0) && near(joints[1].at.z, 0.0));
	// The point on the pelvis turns with it (the quarter turn: (0.25, 1) to (-1, 0.25)); the one on the
	// spine, whose own key holds it unturned, rides its joint (the clip's rotations are each bone's in
	// the model, as the game composes them).
	const PreviewVec3 tip_bent = point_at(clock, 0), hip_bent = point_at(clock, 1);
	TEST_EXPECT(near(hip_bent.x, -1.0) && near(hip_bent.y, 0.25) && near(hip_bent.z, 0.0));
	TEST_EXPECT(near(tip_bent.x, -0.75) && near(tip_bent.y, 0.0) && near(tip_bent.z, 0.0));
	const PreviewVec3 tip_carried = preview_joint_carry(joints[1], tip_rest);
	TEST_EXPECT(near(tip_carried.x, tip_bent.x, 1e-4) && near(tip_carried.y, tip_bent.y, 1e-4) &&
	            near(tip_carried.z, tip_bent.z, 1e-4));
	const PreviewVec3 carried = preview_joint_carry(joints[1], spine_rest);
	TEST_EXPECT(near(carried.x, joints[1].at.x, 1e-4) && near(carried.y, joints[1].at.y, 1e-4) &&
	            near(carried.z, joints[1].at.z, 1e-4));
	// The demo round's bug 4: the camera frames the model as the clip poses it over its frames, never its
	// rest sphere alone (a first-person clip poses its rig away from it): the bend swings the spine off its rest,
	// the framed target follows the posed sphere, and every joint of every frame stands within it.
	{
		PreviewVec3 posed;
		float reach = 0.0f;
		TEST_EXPECT(bend->posed_sphere(posed, reach));
		const OrbitCamera framed = bend->framed(800, 600);
		TEST_EXPECT(near(framed.target.x, posed.x, 1e-4) && near(framed.target.y, posed.y, 1e-4) &&
		            near(framed.target.z, posed.z, 1e-4));
		for (int frame = 0; frame <= int(bend->clip_frame_count()); ++frame) {
			PreviewClock at;
			at.seek_ticks(bend->tick_of_frame_shown(frame));
			for (const PreviewJoint &joint : bend->joints(at)) {
				const float dx = joint.at.x - posed.x, dy = joint.at.y - posed.y, dz = joint.at.z - posed.z;
				TEST_EXPECT(std::sqrt(dx * dx + dy * dy + dz * dz) <= reach);
			}
		}
	}
	TEST_EXPECT(rig.set(R"({"clock": {"playing": false, "ticks": 0}})"));
	shown = rig.json();
	const JsonValue *bones = shown.get("body")->get("animation")->get("bones");
	TEST_EXPECT(bones && bones->array.size() == 3 && bones->array[1].get_string("name", "") == "BN02 Spine");

	// A click on the spine's joint, the clip's own document active, selects its bone; its name on hover.
	Document *bend_clip = session.document_for("anims/bend.bad");
	const ViewportContext context = rig.context();
	ModelCanvasFrame frame = bend->canvas_frame(context);
	TEST_EXPECT(bend_clip && frame.clip_document == bend_clip && frame.joints.size() == 3);
	float jx = 0.0f, jy = 0.0f;
	TEST_EXPECT(bend->camera().project(frame.joints[1].at, context.width, context.height, jx, jy));
	ModelCanvas canvas;
	RecordedRequests out;
	canvas.follow(frame, out);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	in.hovered = true;
	in.mouse = in.screen = CanvasPoint{ jx, jy };
	TEST_EXPECT(model_canvas_under(frame, in) < 0 && model_canvas_bone_under(frame, in) == 1);
	TEST_EXPECT(canvas.hover_tip(frame, -1, 1) == "BN02 Spine (click to select it)");
	in.pressed = in.down = true;
	canvas.input(frame, in, -1, out);
	in.pressed = in.down = false;
	canvas.input(frame, in, -1, out);
	const auto *clip_row = bend_clip ? static_cast<const AnimationDocument *>(bend_clip)->clip() : nullptr;
	TEST_EXPECT(clip_row && out.raised.size() == 1 && out.raised[0].kind == EditorRequestKind::SelectRecord &&
	            out.raised[0].path == "anims/bend.bad" &&
	            out.raised[0].address == NodeAddress({clip_row->id, node_kind(AnimationKind::Bone), clip_row->collections[0][1]}));

	// The demo round's bug 4, in the picture: `flip` swings what stands above the pelvis below it. The model's
	// bounds as each bone carries them (the rest sphere's six extremes, each bone's deform at every frame)
	// all project into a 800 x 600 picture under the posed framing, and some of them out of it under a
	// framing of the rest sphere from the same angles (the framing before the fix).
	{
		session.handle(request::open_document("anims/flip.bad"));
		TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
		rig.pump();
		const ModelViewport *flip = rig.viewport();
		const std::vector<uint8_t> bytes = test_io::read_file(view.project.root + "/models/skinned.3di");
		const opennova::assets::Model rest_model = opennova::assets::parse_model(bytes.data(), bytes.size());
		TEST_EXPECT(flip && flip->view_status() == ModelViewStatus::Ready && rest_model);
		if (flip && rest_model) {
			PreviewVec3 rest;
			float rest_radius = 0.0f;
			model_preview_sphere(*rest_model, rest, rest_radius);
			const OrbitCamera posed = flip->framed(800, 600);
			OrbitCamera old = posed;
			old.frame(rest, rest_radius, 800, 600);
			const auto inside = [](const OrbitCamera &camera, const PreviewVec3 &point) {
				float x = 0.0f, y = 0.0f;
				return camera.project(point, 800, 600, x, y) && x >= 0.0f && x <= 800.0f && y >= 0.0f && y <= 600.0f;
			};
			size_t points = 0, out_posed = 0, out_old = 0;
			for (int frame = 0; frame <= int(flip->clip_frame_count()); ++frame) {
				PreviewClock at;
				at.seek_ticks(flip->tick_of_frame_shown(frame));
				for (const PreviewJoint &joint : flip->joints(at))
					for (const PreviewVec3 &offset : {PreviewVec3{rest_radius, 0, 0}, PreviewVec3{-rest_radius, 0, 0},
					                                  PreviewVec3{0, rest_radius, 0}, PreviewVec3{0, -rest_radius, 0},
					                                  PreviewVec3{0, 0, rest_radius}, PreviewVec3{0, 0, -rest_radius}}) {
						const PreviewVec3 carried = preview_joint_carry(
								joint, PreviewVec3{rest.x + offset.x, rest.y + offset.y, rest.z + offset.z});
						++points;
						out_posed += inside(posed, carried) ? 0 : 1;
						out_old += inside(old, carried) ? 0 : 1;
					}
			}
			TEST_EXPECT(points > 0 && out_posed == 0 && out_old > 0);
			if (out_posed != 0 || out_old == 0)
				std::printf("flip: %zu points, %zu out of the posed framing, %zu out of the rest one\n", points, out_posed,
				            out_old);
		}
	}

	// From the model to its animations: the maps the items pairing it play; an item by its name.
	session.handle(request::open_document("models/skinned.3di"));
	rig.pump();
	shown = rig.json();
	const JsonValue *maps = shown.get("body")->get("animations");
	TEST_EXPECT(maps && maps->array.size() == 1 && maps->array[0].get_string("map", "") == "anims/SKIN.adm" &&
	            maps->array[0].get_string("record", "") == "Skinned Thing");
	// While the first validation reads the references, an animation no item pairs yet says so.
	TEST_EXPECT(std::string(model_view_status_token(ModelViewStatus::Reading)) == "reading" &&
	            model_view_status_message(ModelViewStatus::Reading, "x.adm").rfind("Reading the project's references", 0) == 0);
	const std::vector<std::string> animated = animated_models(*view.findings.graph, *view.project.scan);
	TEST_EXPECT(animated.size() == 1 && strutil_iequals(animated[0], "skinned.3di"));
	bool found = false;
	for (const GraphSearchHit &hit : view.findings.graph->search("skinned th"))
		found = found || (hit.symbol && hit.words == "Skinned Thing" && hit.name == "100200");
	TEST_EXPECT(found);

	// Pairings beyond the graphic (S17 review): an item's graphic_enemy plays its map on that model too;
	// an item naming a map the project lacks plays default.adm there.
	TEST_EXPECT(editor_test::write_text(
	        view.project.root + "/defs/items.def",
	        "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n"
	        "begin \"Bent Enemy\"\nid 100201\ntype building\ngraphicenemy skinned\nanim_def bend\nend\n"
	        "begin \"Lost Map\"\nid 100202\ntype building\ngraphic skinned\nanim_def nowhere\nend\n"));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/anims/default.adm", "anim_reset\t\"reset\"\r\n"));
	session.handle(request::rescan());
	session.run_operations();
	while (view.activity.validation.running) session.poll();
	const std::vector<ModelAnimation> played = model_animations(*view.findings.graph, *view.project.scan, "models/skinned.3di");
	const auto plays = [&](const char *map, const char *record, const char *via) {
		for (const ModelAnimation &one : played)
			if (strutil_iequals(one.map, map) && one.record == record && strutil_iequals(one.via, via)) return true;
		return false;
	};
	TEST_EXPECT(played.size() == 3 && plays("anims/SKIN.adm", "Skinned Thing", "") &&
	            plays("anims/BEND.adm", "Bent Enemy", "as an enemy") &&
	            plays("anims/default.adm", "Lost Map", "in place of nowhere.adm"));
	const PreviewRig paired =
	        resolve_preview_rig(*view.findings.graph, *view.project.scan, "BEND.adm", AssetKind::AnimationMap, std::string());
	TEST_EXPECT(strutil_iequals(paired.model, "skinned.3di") && paired.source.find("Bent Enemy") != std::string::npos);
	const std::vector<MapPlayer> enemies = map_players(*view.findings.graph, *view.project.scan, "anims/BEND.adm");
	TEST_EXPECT(enemies.size() == 1 && strutil_iequals(enemies[0].model, "skinned.3di") && enemies[0].enemy_model.empty());
	return 0;
}

// The collision shown (S17): the layers by their tokens, each shape an item with its record; the body's
// layers and legend; a hit on a shape names its record; the selected record's shape drawn whatever its
// layer, Frame looking at it; a click on a shape selects its record, its words on hover; the legend drawn.
static int test_collision() {
	editor_test::TempProjectDir dir("opennova_editor_model_viewport_collision");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	session.handle(request::new_project(dir.file("project"), "Collision Test"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && rig.pump() == ViewportAction::Rebuild);
	if (!document) return 1;
	const CollisionRow *collision = document->collision_row();
	TEST_EXPECT(collision && !collision->volumes.empty() && !collision->faces.empty());
	if (!collision || collision->volumes.empty() || collision->faces.empty()) return 1;
	// Off until shown: only the markers are items.
	TEST_EXPECT(rig.viewport()->collision(rig.clock())->empty());
	TEST_EXPECT(!rig.set(R"({"options": {"overlays": {"walls": true}}})"));
	TEST_EXPECT(rig.set(R"({"options": {"overlays": {"volumes": true, "bullet_faces": true, "user_points": false, "lights": false}}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Update);
	JsonValue shown = rig.json();
	TEST_EXPECT(shown.get("options")->get("overlays")->get_bool("volumes", false) &&
	            !shown.get("options")->get("overlays")->get_bool("sections", true));
	size_t faces = 0, volumes = 0;
	JsonValue face_item;
	for (const JsonValue &item : shown.get("items")->array) {
		const std::string kind = item.get_string("kind", "");
		TEST_EXPECT(item.get_number("id", 0) != 0 && !item.get_string("name", "").empty() && item.get("points"));
		if (kind == "face" && !faces++) face_item = item;
		volumes += kind == "volume" ? 1 : 0;
	}
	TEST_EXPECT(faces == collision->faces.size() && volumes == collision->volumes.size());
	const JsonValue *layers = shown.get("body")->get("collision")->get("layers");
	TEST_EXPECT(layers && layers->array.size() == size_t(ModelCollisionLayer::kCount) &&
	            layers->array[0].get_string("token", "") == "bullet_faces" && layers->array[0].get_bool("shown", false) &&
	            layers->array[0].get_number("count", 0) == double(collision->faces.size()));
	TEST_EXPECT(!shown.get("body")->get("collision")->get("legend")->array.empty());

	// A hit on a face's middle names a collision record.
	TEST_EXPECT(face_item.get("screen") && face_item.get("screen")->array.size() == 2);
	const float fx = float(face_item.get("screen")->array[0].number), fy = float(face_item.get("screen")->array[1].number);
	const ViewportHit hit = rig.viewport()->hit(rig.context(), fx, fy);
	TEST_EXPECT((hit.kind == "face" || hit.kind == "volume") && hit.id != 0 && hit.current);

	// The canvas: a click on the shape under the pointer selects its record, its words on hover.
	const ViewportContext context = rig.context();
	ModelCanvasFrame frame = rig.viewport()->canvas_frame(context);
	TEST_EXPECT(model_canvas_collision(frame).size() == faces + volumes && frame.selected_collision == -1);
	ModelCanvas canvas;
	RecordedRequests out;
	canvas.follow(frame, out);
	CanvasInput in;
	in.width = context.width;
	in.height = context.height;
	in.hovered = true;
	in.mouse = in.screen = CanvasPoint{fx, fy};
	const int under = model_canvas_collision_under(frame, in);
	TEST_EXPECT(under >= 0 && model_canvas_under(frame, in) < 0);
	if (under < 0) return 1;
	TEST_EXPECT(canvas.hover_tip(frame, -1, -1, under) == model_canvas_collision(frame)[size_t(under)].name + " (click to select it)");
	in.pressed = in.down = true;
	canvas.input(frame, in, -1, out);
	in.pressed = in.down = false;
	canvas.input(frame, in, -1, out);
	const NodeAddress picked = model_collision_record(*document, model_canvas_collision(frame)[size_t(under)]);
	TEST_EXPECT(out.raised.size() == 1 && out.raised[0].kind == EditorRequestKind::SelectRecord &&
	            out.raised[0].address == picked && NodeId(hit.id) == picked.child);
	// Drawn: lines in the shapes' colours and the legend's words.
	const OverlayList list = canvas.shapes(frame, in, -1, -1, under);
	size_t lines = 0, legend = 0;
	for (const OverlayShape &shape : list.shapes) {
		lines += shape.kind == OverlayKind::Line ? 1 : 0;
		legend += shape.kind == OverlayKind::Text && shape.text == "Bullet faces" ? 1 : 0;
	}
	TEST_EXPECT(lines >= faces * 3 && legend == 1);

	// Made once: another frame of the same state reads the kept shapes (no rebuild), and the canvas
	// looks for what lies under the pointer once for the shapes and the hover together.
	const uint64_t builds = rig.viewport()->collision_builds();
	const ModelCanvasFrame again = rig.viewport()->canvas_frame(rig.context());
	TEST_EXPECT(rig.viewport()->collision_builds() == builds && again.collision == frame.collision);
	ModelCanvas once;
	RecordedRequests followed;
	once.follow(*rig.viewport(), rig.context(), followed);
	once.shapes(rig.context(), in);
	TEST_EXPECT(!once.hover_tip(rig.context(), in).empty() && once.pick_count() == 1);

	// The layers off, a volume selected: its shape alone, highlighted; Frame looks at it.
	TEST_EXPECT(rig.set(R"({"options": {"overlays": {"volumes": false, "bullet_faces": false}}})"));
	rig.pump();
	const NodeAddress volume{collision->id, node_kind(ModelKind::Volume), collision->ids.lists[kCollisionVolumes][0].id};
	session.handle(request::select_record(document->path(), volume));
	frame = rig.viewport()->canvas_frame(rig.context());
	TEST_EXPECT(model_canvas_collision(frame).size() == 1 && frame.selected_collision == 0 &&
	            model_canvas_collision(frame)[0].kind == ModelCollisionKind::Volume && model_canvas_collision(frame)[0].index == 0);
	// The wire as the picture: the selected shape among the items, marked, and a hit on it names it.
	shown = rig.json();
	JsonValue selected_item;
	for (const JsonValue &item : shown.get("items")->array)
		if (item.get_string("kind", "") == "volume") selected_item = item;
	TEST_EXPECT(selected_item.get_bool("selected", false) && NodeId(selected_item.get_number("id", 0)) == volume.child);
	const ViewportHit on_it = rig.viewport()->hit(rig.context(), float(selected_item.get("screen")->array[0].number),
	                                               float(selected_item.get("screen")->array[1].number));
	TEST_EXPECT(on_it.kind == "volume" && on_it.id == volume.child);
	// Frame names a record with a shape; a record with none (a material) leaves the whole model.
	TEST_EXPECT(rig.viewport()->frame_ids(rig.context()) == std::vector<NodeId>{volume.child});
	const ModelRow *model_row = document->model_row();
	session.handle(request::select_record(
			document->path(), {model_row->id, node_kind(ModelKind::Material), model_row->ids.lists[kModelMaterials][0].id}));
	TEST_EXPECT(rig.viewport()->frame_ids(rig.context()).empty());
	session.handle(request::select_record(document->path(), volume));
	const OrbitCamera before = rig.viewport()->camera();
	RecordedRequests framed;
	std::string error;
	TEST_EXPECT(rig.viewport()->command(rig.context(), "frame", {volume.child}, framed, error) && framed.raised.size() == 1);
	for (const EditorRequest &request : framed.raised) session.handle(request);
	PreviewVec3 center;
	float radius = 0.0f;
	model_collision_bounds(model_canvas_collision(frame)[0], center, radius);
	const OrbitCamera &after = rig.viewport()->camera();
	TEST_EXPECT(near(after.target.x, center.x, 1e-3) && near(after.target.y, center.y, 1e-3) &&
	            near(after.target.z, center.z, 1e-3) && (!near(before.distance, after.distance, 1e-4) ||
	                                                     !near(before.target.x, after.target.x, 1e-4)));
	std::printf("test_collision passed\n");
	return 0;
}

// Auto picks a coarser level as the camera backs away, the finest up close, never past
// the last (the two-level scenes the add-on's fixtures mint).
static int test_auto_lod() {
	for (const char *name : {"building.o3d", "spinner.o3d"}) {
		std::ifstream text(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/o3d/" + name);
		std::vector<uint8_t> bytes;
		std::vector<opennova::threedi::SceneFinding> findings;
		TEST_EXPECT(opennova::threedi::threedi_o3d_build(text, opennova::renderer::material_descriptor_tangent_lookup,
		                                                 opennova::renderer::material_texture_dds_only, bytes, findings));
		const opennova::assets::Model parsed = opennova::assets::parse_model(bytes.data(), bytes.size());
		TEST_EXPECT(parsed && parsed->lod_count == 2);
		OrbitCamera camera;
		PreviewVec3 center;
		float radius = 0.0f;
		model_preview_sphere(*parsed, center, radius);
		TEST_EXPECT(radius > 0.0f);
		camera.frame(center, radius, 800, 600);
		camera.distance = radius * 0.5f;
		TEST_EXPECT(model_preview_auto_lod(*parsed, camera, 800) == 0);
		int last = 0;
		int32_t projected = 0, was = 0x7fffffff;
		for (float distance = radius * 0.5f; distance < radius * 4000.0f; distance *= 1.5f) {
			camera.distance = distance;
			const int lod = model_preview_auto_lod(*parsed, camera, 800, &projected);
			TEST_EXPECT(lod >= last && lod <= 1 && projected <= was);
			last = lod;
			was = projected;
		}
		TEST_EXPECT(last == 1);
	}
	return 0;
}

int main() {
	TEST_EXPECT(test_status_and_builds() == 0);
	TEST_EXPECT(test_unknown_and_empty_changes() == 0);
	TEST_EXPECT(test_overlays() == 0);
	TEST_EXPECT(test_handles() == 0);
	TEST_EXPECT(test_animation() == 0);
	TEST_EXPECT(test_runtime_clips() == 0);
	TEST_EXPECT(test_clip_preview() == 0);
	TEST_EXPECT(test_camera() == 0);
	TEST_EXPECT(test_options_from_json() == 0);
	TEST_EXPECT(test_auto_lod() == 0);
	TEST_EXPECT(test_collision() == 0);
	std::printf("editor_model_viewport: all tests passed\n");
	return 0;
}
