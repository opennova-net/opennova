// The model's viewport (editor/preview/model_viewport, ADR 0046 S10p2, S13 V5): the honest reason
// when there is nothing to show; the open model shown as it would save; an edit of what is drawn
// builds again, a user point's edit builds nothing (the overlays alone show it); a texture the
// device read that changes builds again; the options (a level, a held register) are an Update; the
// camera projects its target to the middle and frames the model; Auto walks the model's levels as
// the camera backs away; the envelope, and what a SetViewport sets (the options, the clock, the
// camera) and the MCP's drag of a marker.

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
#include <editor/graph/reference_queries.h>
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
	JsonValue json() { return viewport_to_json(view(), viewport(), ViewportKind::Model, JsonPage()); }
	const PreviewClock &clock() { return session.viewports().clock(); }
	ViewportContext context(float snap = 0.0f) { return editor_test::viewport_context(session, *viewport(), snap); }
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
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
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

	// A user point moved: the overlays show it, nothing is built.
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->collections[3][0]};
	const double x_before = points->array[0].get("position")->array[0].number;
	const double z_before = points->array[0].get("position")->array[2].number;
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, document->path(), point, "position.x", std::get<double>(value) + 1.0);
	TEST_EXPECT(rig.pump() == ViewportAction::Update && rig.builds() == 1 && model->reads() == reads + 1);
	shown = rig.json();
	TEST_EXPECT(shown.get_bool("current", false));
	const JsonValue &moved = shown.get("items")->array[0];
	TEST_EXPECT(!near(moved.get("position")->array[0].number, x_before) ||
	            !near(moved.get("position")->array[2].number, z_before));

	// A light's colour is drawn: it builds again.
	const NodeAddress light{row->id, node_kind(ModelKind::Light), document->model_row()->collections[2][0]};
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
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && rig.builds() == 3);
	// Changed again in the same follow as a user point's edit (which alone is an Update): it builds.
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/textures/preview_skin.tga"), std::vector<uint8_t>(36, 0)));
	session.handle(request::rescan());
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
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(request::rescan());
	session.handle(request::open_document("models/house.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/house.3di"));
	TEST_EXPECT(document && rig.pump() == ViewportAction::Rebuild);

	// The ground point moved off the axis the part turns about (the model's vertical): the
	// turning part carries it.
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->collections[3][0]};
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
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(request::rescan());
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
	TEST_EXPECT(edits.size() == 3 && edits[0].field == "direction.x");
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
	const int width = model->state().width, height = model->state().height;
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
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                        "/fixtures/threedi/o3d/skinned.o3d")));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
	session.handle(import);
	TEST_EXPECT(view.project.scan->find("skinned.3di") && view.project.scan->find("SKIN.adm") && view.project.scan->find("walk.bad"));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def",
	                                    "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\n"
	                                    "anim_def skin\nend\n"));
	session.handle(request::rescan());

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

	// The walk row selected: it plays from tick 0, the clock in game ticks.
	NodeAddress walk;
	TEST_EXPECT(find_definition(AssetGraph(), *table, "anim_walk_forward", walk));
	EditorRequest select = request::select_record(table->path(), walk);
	session.handle(select);
	rig.pump();
	TEST_EXPECT(model->clip_key() == "anim_walk_forward" && model->clip_variant() == 0 && rig.clock().ticks() == 0);
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
	rig.pump();
	TEST_EXPECT(rig.clock().ticks() == clip_model->tick_of_frame(3) && !rig.clock().playing());
	session.advance(0.5);
	TEST_EXPECT(rig.clock().ticks() == clip_model->tick_of_frame(3));

	// No item pairs a table: no rig, until a model is chosen.
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def", "begin \"Nothing\"\nid 100201\ntype building\nend\n"));
	session.handle(request::rescan());
	session.handle(request::open_document("anims/SKIN.adm"));
	rig.pump();
	TEST_EXPECT(model->view_status() == ModelViewStatus::NoRig);
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_rig");
	TEST_EXPECT(rig.set(R"({"options": {"rig_model": "skinned.3di"}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && model->view_status() == ModelViewStatus::Ready);
	TEST_EXPECT(model->rig().source == "chosen" && model->skeleton());
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
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                        "/fixtures/threedi/o3d/skinned.o3d")));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	TEST_EXPECT(editor_test::write_text(source + "/step.o3a", kStepClips));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}, {source + "/step.o3a", {}}};
	session.handle(import);
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
	select.address = {walk.row, clip_kind, tokens[1]};
	session.handle(select);
	rig.pump();
	TEST_EXPECT(model->clip_key().empty() && model->clip_file().empty() && model->clip_events().empty());

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
	TEST_EXPECT(test_overlays() == 0);
	TEST_EXPECT(test_handles() == 0);
	TEST_EXPECT(test_animation() == 0);
	TEST_EXPECT(test_runtime_clips() == 0);
	TEST_EXPECT(test_camera() == 0);
	TEST_EXPECT(test_options_from_json() == 0);
	TEST_EXPECT(test_auto_lod() == 0);
	std::printf("editor_model_viewport: all tests passed\n");
	return 0;
}
