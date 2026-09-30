// The model preview's portable half (editor/preview/model_preview_*, ADR 0046 S10p2): the
// honest status when there is nothing to show; the open model shown as it would save; an
// edit of what is drawn builds again, a user point's edit builds nothing (the overlays
// alone show it); a texture the build read that changes builds again; the options (a
// level, a held register) are an Update; the camera projects its target to the middle
// and frames the model; Auto walks the model's levels as the camera backs away; the JSON,
// and what the editor MCP sets through it (the options, the camera, a marker's drag).

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
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_json.h>
#include <editor/preview/model_preview_state.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
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

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

JsonValue json(const SessionView &view, const ModelPreviewModel &model) {
	return model_preview_to_json(model_preview_snapshot(view, model, true));
}

void set(ProjectSession &session, const std::string &path, const NodeAddress &address, const char *field, Value value) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, path);
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	session.handle(request);
}

bool near(double a, double b, double tolerance = 0.01) { return std::fabs(a - b) <= tolerance; }

bool strutil_iequals(const std::string &a, const std::string &b) { return opennova::strutil::iequals(a, b); }

} // namespace

static int test_status_and_builds() {
	editor_test::TempProjectDir dir("opennova_editor_model_preview");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	ModelPreviewModel model;
	const SessionView &view = session.view();
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Keep);
	TEST_EXPECT(json(view, model).get_string("status", "") == "no_project");
	TEST_EXPECT(model_preview_to_json(model_preview_snapshot(view, model, false)).get_string("status", "") == "no_device");

	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Model Preview Test"));
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	model.follow(view);
	TEST_EXPECT(json(view, model).get_string("status", "") == "no_model");
	TEST_EXPECT(json(view, model).get_string("message", "") ==
	            "Open a model, a clip or an animation table to preview it.");

	// The open model, as it would save.
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && view.documents.previews.model.path == "models/armory.3di");
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Rebuild);
	TEST_EXPECT(model.builds() == 1 && model.model() && model.follow(view) == ModelPreviewAction::Keep);
	JsonValue shown = json(view, model);
	TEST_EXPECT(shown.get_string("status", "") == "ready" && shown.get_bool("current", false));
	TEST_EXPECT(shown.get_string("path", "") == "models/armory.3di");
	const JsonValue *points = shown.get("overlays");
	TEST_EXPECT(points && points->array.size() == model.model()->user_point_count + model.model()->light_count);
	TEST_EXPECT(!points->array.empty() && points->array[0].get_string("kind", "") == "user_point");
	const JsonValue *registers = shown.get("registers");
	TEST_EXPECT(registers && registers->array.size() == model.model()->ctrl.count);

	// A user point moved: the overlays show it, nothing is built.
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->collections[3][0]};
	const double x_before = points->array[0].get("position")->array[0].number;
	const double z_before = points->array[0].get("position")->array[2].number;
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, document->path(), point, "position.x", std::get<double>(value) + 1.0);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Update && model.builds() == 1);
	shown = json(view, model);
	TEST_EXPECT(shown.get_bool("current", false));
	const JsonValue &moved = shown.get("overlays")->array[0];
	TEST_EXPECT(!near(moved.get("position")->array[0].number, x_before) ||
	            !near(moved.get("position")->array[2].number, z_before));

	// A light's colour is drawn: it builds again.
	const NodeAddress light{row->id, node_kind(ModelKind::Light), document->model_row()->collections[2][0]};
	set(session, document->path(), light, "start.r", int64_t(12));
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Rebuild && model.builds() == 2);

	// The options apply without a build.
	ModelPreviewOptions options;
	options.lod = 0;
	options.ctrl["HEAT"] = 5;
	model.set_options(options);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Update && model.builds() == 2);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Keep && model.lod() == 0);
	shown = json(view, model);
	TEST_EXPECT(shown.get("options")->get("lod")->number == 0.0);
	TEST_EXPECT(shown.get("options")->get("ctrl")->get("HEAT")->number == 5.0);

	// A texture the build read (or looked for) changes: it builds again.
	auto files = std::make_shared<StampedFiles>(view.findings.assets);
	std::vector<uint8_t> bytes;
	TEST_EXPECT(!files->read("preview_skin.tga", bytes) && files->read_names().size() == 1);
	model.built(files);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Keep);
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/textures/preview_skin.tga"), std::vector<uint8_t>(18, 0)));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Rebuild && model.builds() == 3);

	// Saved and closed: nothing to show.
	session.handle(make_request(EditorRequestKind::SaveAll));
	session.handle(make_request(EditorRequestKind::CloseDocument, "models/armory.3di"));
	TEST_EXPECT(view.documents.previews.model.path.empty());
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Clear);
	TEST_EXPECT(json(view, model).get_string("status", "") == "no_model" && !model.model());
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

JsonValue overlay(const JsonValue &shown, const char *kind, int index) {
	for (const JsonValue &row : shown.get("overlays")->array)
		if (row.get_string("kind", "") == kind && int(row.get("index")->number) == index) return row;
	return JsonValue();
}

// The overlays ride the posed model: a user point on a part that turns (house_lod0_sine_rotx
// rocks its only part about x) moves with the clock and holds while paused; a click on a
// marker's pixel picks it and names its record, the record maps back to the marker; the
// pivots show when asked.
static int test_overlays() {
	editor_test::TempProjectDir dir("opennova_editor_model_preview_overlays");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	ModelPreviewModel model;
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Overlay Test"));
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/house.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/house.3di"));
	TEST_EXPECT(document && model.follow(view) == ModelPreviewAction::Rebuild);

	// The ground point moved off the axis the part turns about (the model's vertical): the
	// turning part carries it.
	const ModelRow *row = document->model_row();
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->collections[3][0]};
	set(session, document->path(), point, "position.x", 2.0);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Update);
	const JsonValue at_zero = overlay(json(view, model), "user_point", 0);
	TEST_EXPECT(at_zero.get("position"));
	model.advance(0.25);
	TEST_EXPECT(model.clock_ms() == 250);
	const JsonValue later = overlay(json(view, model), "user_point", 0);
	TEST_EXPECT(!near(later.get("position")->array[0].number, at_zero.get("position")->array[0].number, 1e-3) ||
	            !near(later.get("position")->array[2].number, at_zero.get("position")->array[2].number, 1e-3));
	TEST_EXPECT(near(later.get("position")->array[1].number, at_zero.get("position")->array[1].number, 1e-4));
	ModelPreviewOptions paused = model.options();
	paused.playing = false;
	model.set_options(paused);
	model.follow(view);
	model.advance(0.25);
	TEST_EXPECT(model.clock_ms() == 250 && !json(view, model).get("clock")->get_bool("playing", true));
	model.seek(0);
	TEST_EXPECT(near(overlay(json(view, model), "user_point", 0).get("position")->array[0].number,
	                 at_zero.get("position")->array[0].number, 1e-4));

	// A click at the marker's pixel picks it, and it names the record.
	const JsonValue shown = json(view, model);
	const JsonValue marker = overlay(shown, "user_point", 0);
	TEST_EXPECT(marker.get("screen") && marker.get("screen")->array.size() == 2);
	const float sx = float(marker.get("screen")->array[0].number), sy = float(marker.get("screen")->array[1].number);
	const JsonValue hit = model_preview_hit_to_json(model_preview_snapshot(view, model, true), sx + 2.0f, sy - 2.0f);
	TEST_EXPECT(hit.get_string("kind", "") == "user_point" && hit.get("index")->number == 0.0);
	TEST_EXPECT(NodeId(hit.get("id")->number) == point.child && NodeId(marker.get("id")->number) == point.child);
	const JsonValue miss = model_preview_hit_to_json(model_preview_snapshot(view, model, true), sx + 40.0f, sy + 40.0f);
	TEST_EXPECT(miss.get("index")->number == -1.0);
	ModelOverlayKind kind;
	int index = -1;
	TEST_EXPECT(model_overlay_of(*document, point, kind, index) && kind == ModelOverlayKind::UserPoint && index == 0);

	// The pivots, when asked: one per part of the drawn level, each naming its PANM row.
	ModelPreviewOptions pivots = model.options();
	pivots.overlays.pivots = true;
	pivots.overlays.user_points = false;
	pivots.overlays.lights = false;
	model.set_options(pivots);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Update);
	const std::vector<ModelOverlay> marks = model.overlays();
	TEST_EXPECT(marks.size() == model.model()->lods[0].render_object_count);
	for (const ModelOverlay &mark : marks) {
		TEST_EXPECT(mark.kind == ModelOverlayKind::Pivot);
		const NodeAddress record = model_overlay_record(*document, mark, model.lod());
		TEST_EXPECT(record.kind == node_kind(ModelKind::PartAnimation) && record.child != 0);
		TEST_EXPECT(model_overlay_of(*document, record, kind, index) && kind == ModelOverlayKind::Pivot && index == mark.index);
	}
	return 0;
}

void apply(ProjectSession &session, const std::string &path, std::vector<Edit> edits) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, path);
	request.edits = std::move(edits);
	session.handle(request);
	session.handle(make_request(EditorRequestKind::EndEdit, path));
}

// The marker of a kind and index, copied (the list is often a temporary).
std::optional<ModelOverlay> find_overlay(const std::vector<ModelOverlay> &overlays, ModelOverlayKind kind, int index) {
	for (const ModelOverlay &overlay : overlays)
		if (overlay.kind == kind && overlay.index == index) return overlay;
	return std::nullopt;
}

// A drag of a marker writes its record: the place lands where the drag put it, through the
// turned part's pose (house_lod0_sine_rotx at a quarter second), snapped when asked; the
// axis turns to the drag; one gesture is one undo step; a pivot, and an omni light's axis,
// have no handle; a pixel on the view plane is the point the drag means.
static int test_handles() {
	editor_test::TempProjectDir dir("opennova_editor_model_preview_handles");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	ModelPreviewModel model;
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Handle Test"));
	TEST_EXPECT(editor_test::write_bytes(dir.file("project/models/house.3di"),
	                                     test_io::read_file(synth("house_lod0_sine_rotx.3di"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/house.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/house.3di"));
	TEST_EXPECT(document && model.follow(view) == ModelPreviewAction::Rebuild);
	model.seek(250);
	int32_t bus[96];
	model_preview_ctrl_bus(model.options().ctrl, bus);

	// The place: where the drag puts it, the part's pose undone and applied again.
	std::optional<ModelOverlay> point = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && point->part == 0);
	const PreviewVec3 to{point->at.x + 0.5f, point->at.y + 0.3f, point->at.z - 0.2f};
	std::vector<Edit> edits;
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(model_handle_edits(*document, *model.model(), *point, model.lod(), model.clock_ms(), bus,
	                               ModelHandle::Place, to, 0.0f, gesture, edits));
	TEST_EXPECT(edits.size() == 3 && edits[0].field == "position.x" && edits[0].gesture == gesture);
	const uint64_t before = document->revision();
	apply(session, document->path(), edits);
	TEST_EXPECT(document->revision() != before && model.follow(view) == ModelPreviewAction::Update);
	point = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && near(point->at.x, to.x, 1e-3) && near(point->at.y, to.y, 1e-3) && near(point->at.z, to.z, 1e-3));
	TEST_EXPECT(model.builds() == 1);

	// Snapped: every place field on the grid.
	TEST_EXPECT(model_handle_edits(*document, *model.model(), *point, model.lod(), model.clock_ms(), bus,
	                               ModelHandle::Place, PreviewVec3{0.37f, 1.13f, -0.61f}, 0.25f, next_edit_gesture(), edits));
	for (const Edit &edit : edits) {
		const double value = std::get<double>(edit.value);
		TEST_EXPECT(near(value / 0.25, std::round(value / 0.25), 1e-9));
	}

	// The axis: turned toward the drag.
	const PreviewVec3 aim{point->at.x, point->at.y + 1.0f, point->at.z + 1.0f};
	TEST_EXPECT(model_handle_edits(*document, *model.model(), *point, model.lod(), model.clock_ms(), bus,
	                               ModelHandle::Axis, aim, 0.0f, next_edit_gesture(), edits));
	TEST_EXPECT(edits.size() == 3 && edits[0].field == "direction.x");
	apply(session, document->path(), edits);
	model.follow(view);
	point = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	const float inv = 1.0f / std::sqrt(2.0f);
	TEST_EXPECT(point && point->has_direction && near(point->direction.x, 0.0, 1e-3) &&
	            near(point->direction.y, inv, 1e-3) && near(point->direction.z, inv, 1e-3));

	// One drag, one undo step: undo takes the axis back, then the place.
	session.handle(make_request(EditorRequestKind::Undo, document->path()));
	session.handle(make_request(EditorRequestKind::Undo, document->path()));
	model.follow(view);
	point = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(point && !near(point->at.x, to.x, 1e-3));

	// No handle: a pivot (geometry), an omni light's axis.
	ModelPreviewOptions all = model.options();
	all.overlays.pivots = true;
	model.set_options(all);
	model.follow(view);
	const std::vector<ModelOverlay> marks = model.overlays();
	std::optional<ModelOverlay> pivot = find_overlay(marks, ModelOverlayKind::Pivot, 0);
	TEST_EXPECT(pivot && !model_handle_edits(*document, *model.model(), *pivot, model.lod(), model.clock_ms(), bus,
	                                         ModelHandle::Place, to, 0.0f, 1, edits));
	for (const ModelOverlay &mark : marks)
		if (mark.kind == ModelOverlayKind::Light && !mark.has_direction)
			TEST_EXPECT(!model_handle_edits(*document, *model.model(), mark, model.lod(), model.clock_ms(), bus,
			                                ModelHandle::Axis, to, 0.0f, 1, edits));

	// The pixel a marker is drawn at, on the plane through it, is the marker's point.
	std::optional<ModelOverlay> light = find_overlay(marks, ModelOverlayKind::Light, 0);
	if (light) {
		float x = 0.0f, y = 0.0f;
		PreviewVec3 back;
		TEST_EXPECT(model.camera().project(light->at, model.device_width(), model.device_height(), x, y));
		TEST_EXPECT(model.camera().on_view_plane(x, y, model.device_width(), model.device_height(), light->at, back));
		TEST_EXPECT(near(back.x, light->at.x, 1e-3) && near(back.y, light->at.y, 1e-3) && near(back.z, light->at.z, 1e-3));
		TEST_EXPECT(model.handle_edits(*document, *light, ModelHandle::Place, x, y, 0.0f, next_edit_gesture(), edits));
		apply(session, document->path(), edits);
		model.follow(view);
		std::optional<ModelOverlay> still = find_overlay(model.overlays(), ModelOverlayKind::Light, 0);
		TEST_EXPECT(still && near(still->at.x, light->at.x, 1e-3) && near(still->at.y, light->at.y, 1e-3) &&
		            near(still->at.z, light->at.z, 1e-3));
	}

	// The editor MCP's drag (model_preview_drag): the user point's record to a device pixel,
	// the pane's plan, one undo step; refused while the picture is of another revision and
	// for a record no marker is.
	model.follow(view);
	point = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	const NodeAddress record = model_preview_record(model_preview_snapshot(view, model, true), *point);
	TEST_EXPECT(record.child != 0);
	float px = 0.0f, py = 0.0f;
	TEST_EXPECT(model.camera().project(point->at, model.device_width(), model.device_height(), px, py));
	const uint64_t before_drag = document->revision();
	TEST_EXPECT(!model_preview_drag(session, model_preview_snapshot(view, model, true), 999999, ModelHandle::Place,
	                                px + 20.0f, py, 0.0f));
	TEST_EXPECT(model_preview_drag(session, model_preview_snapshot(view, model, true), record.child, ModelHandle::Place,
	                               px + 20.0f, py, 0.0f));
	TEST_EXPECT(document->revision() != before_drag);
	TEST_EXPECT(!model_preview_drag(session, model_preview_snapshot(view, model, true), record.child, ModelHandle::Place,
	                                px, py, 0.0f));
	model.follow(view);
	std::optional<ModelOverlay> dragged = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(dragged && !near(dragged->at.x, point->at.x, 1e-3));
	session.handle(make_request(EditorRequestKind::Undo, document->path()));
	model.follow(view);
	dragged = find_overlay(model.overlays(), ModelOverlayKind::UserPoint, 0);
	TEST_EXPECT(dragged && near(dragged->at.x, point->at.x, 1e-3) && near(dragged->at.y, point->at.y, 1e-3));

	// Refused by the session, an operation holding the documents (S13 A2): the drag answers false
	// (it had answered true whatever the session made of it), the model as it was.
	const uint64_t held = document->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(!model_preview_drag(session, model_preview_snapshot(view, model, true), record.child, ModelHandle::Place,
	                                px + 20.0f, py, 0.0f));
	TEST_EXPECT(document->revision() == held);
	session.run_operations();
	return 0;
}

// What the editor MCP sets (model_preview_options_from_json, model_preview_camera_from_json):
// each member over what the preview holds, a number's fraction dropped, the pitch kept short
// of straight up or down; a member unknown, of another type or out of range refuses the
// whole object and changes nothing.
static int test_options_from_json() {
	const auto parse = [](const char *text) {
		JsonValue json;
		std::string error;
		opennova::io::json_parse(text, json, error);
		return json;
	};
	ModelPreviewModel model;
	TEST_EXPECT(model_preview_options_from_json(
	        parse(R"({"lod": 2, "ctrl": {"GEAR": 3, "FLAP": 0}, "playing": false, "overlays": {"pivots": true},)"
	              R"( "rig_model": "rig.3di", "time_ms": 250.7, "clip_ticks": 8})"),
	        model));
	const ModelPreviewOptions &options = model.options();
	TEST_EXPECT(options.lod == 2 && options.ctrl.size() == 1 && options.ctrl.at("GEAR") == 3 && !options.playing);
	TEST_EXPECT(options.overlays.pivots && options.rig_model == "rig.3di" && model.clock_ms() == 250);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"lod": "auto"})"), model) && model.options().lod == -1);
	const ModelPreviewOptions held = model.options();
	for (const char *bad : {R"({"lod": 256})", R"({"lod": "near"})", R"({"ctrl": {"GEAR": "x"}})", R"({"ctrl": 3})",
	                        R"({"overlays": {"lamps": true}})", R"({"time_ms": -1})", R"({"time_ms": "9"})",
	                        R"({"clip_ticks": -1})", R"({"playing": 1})", R"({"bogus": 1})",
	                        R"({"playing": true, "lod": -2})"}) {
		TEST_EXPECT(!model_preview_options_from_json(parse(bad), model));
		TEST_EXPECT(model.options() == held && model.clock_ms() == 250);
	}
	// The clock's last millisecond, and any later time taken as it (the rest of the request
	// with it); never wrapped round to an early one.
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 4294967294})"), model) &&
	            model.clock_ms() == 4294967294u);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 4294967295})"), model) &&
	            model.clock_ms() == 4294967295u);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 250})"), model) && model.clock_ms() == 250);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 4294967296, "playing": true})"), model) &&
	            model.clock_ms() == 4294967295u && model.options().playing);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 1e15})"), model) && model.clock_ms() == 4294967295u);
	TEST_EXPECT(model_preview_options_from_json(parse(R"({"time_ms": 250, "playing": false})"), model) &&
	            model.clock_ms() == 250 && !model.options().playing);

	TEST_EXPECT(model_preview_camera_from_json(
	        parse(R"({"yaw": 1.25, "pitch": 3.0, "distance": 12, "target": [1, 2, 3], "width": 640, "height": 480.9})"),
	        model));
	const OrbitCamera &camera = model.camera();
	TEST_EXPECT(near(camera.yaw, 1.25, 1e-6) && near(camera.pitch, kOrbitPitchLimit, 1e-6) &&
	            near(camera.distance, 12.0, 1e-6));
	TEST_EXPECT(near(camera.target.x, 1.0, 1e-6) && near(camera.target.y, 2.0, 1e-6) && near(camera.target.z, 3.0, 1e-6));
	TEST_EXPECT(model.device_width() == 640 && model.device_height() == 480);
	TEST_EXPECT(model_preview_camera_from_json(parse(R"({"pitch": -9})"), model) &&
	            near(model.camera().pitch, -kOrbitPitchLimit, 1e-6));
	for (const char *bad : {R"({"distance": 0})", R"({"target": [1, 2]})", R"({"target": [1, 2, "3"]})",
	                        R"({"width": 0})", R"({"height": 8193})", R"({"frame": 1})", R"({"zoom": 2})",
	                        R"({"yaw": 0.5, "distance": -1})"}) {
		TEST_EXPECT(!model_preview_camera_from_json(parse(bad), model));
		TEST_EXPECT(near(model.camera().yaw, 1.25, 1e-6) && near(model.camera().distance, 12.0, 1e-6) &&
		            model.device_width() == 640);
	}
	std::printf("test_options_from_json passed\n");
	return 0;
}

// A clip or a table plays on its rig's model: the table on the model an item pairs with it
// (its graphic beside its anim_def), the clip on the table that names it; the rig loads
// through the game's loader over the model's bone table; the selected row plays, the clip
// clock runs in game ticks, and its trigger events sit on the ticks the clip's own clock
// first samples their frames; a table no item pairs has no rig until a model is chosen.
static int test_animation() {
	editor_test::TempProjectDir dir("opennova_editor_model_preview_animation");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	ModelPreviewModel model;
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Animation Test"));
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                        "/fixtures/threedi/o3d/skinned.o3d")));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	EditorRequest import = make_request(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
	session.handle(import);
	TEST_EXPECT(view.project.scan->find("skinned.3di") && view.project.scan->find("SKIN.adm") && view.project.scan->find("walk.bad"));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def",
	                                    "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\n"
	                                    "anim_def skin\nend\n"));
	session.handle(make_request(EditorRequestKind::Rescan));

	// The table plays on the item's graphic.
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	TEST_EXPECT(table && view.documents.previews.model.path == "anims/SKIN.adm");
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Rebuild);
	JsonValue shown = json(view, model);
	TEST_EXPECT(shown.get_string("status", "") == "ready" && shown.get_string("path", "") == "anims/SKIN.adm");
	const JsonValue *animation = shown.get("animation");
	TEST_EXPECT(animation && strutil_iequals(animation->get_string("model", ""), "skinned.3di"));
	TEST_EXPECT(animation->get_string("source", "").find("Skinned Thing") != std::string::npos);
	TEST_EXPECT(animation->get_bool("rig", false) && model.skeleton() && model.skeleton()->bone_count() == 3);

	// The walk row selected: it plays from tick 0, the clock in game ticks.
	NodeAddress walk;
	TEST_EXPECT(table->find("anim_walk_forward", walk));
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, table->path());
	select.edit.address = walk;
	session.handle(select);
	model.follow(view);
	TEST_EXPECT(model.clip_key() == "anim_walk_forward" && model.clip_variant() == 0 && model.clip_ticks() == 0);
	TEST_EXPECT(strutil_iequals(model.clip_file(), "walk"));
	model.advance(1.0);
	TEST_EXPECT(model.clip_ticks() == 62);
	// Its triggers (frames 1 and 3; the last row repeats the one before, as retail's
	// exporter writes it) on the ticks the clip's clock first reaches them.
	const std::vector<PreviewClipEvent> &events = model.clip_events();
	TEST_EXPECT(events.size() >= 2 && events[0].frame == 1 && events[0].trigger == 1u && events[1].frame == 3 &&
	            events[1].trigger == 2u && events[0].tick > 0 && events[1].tick > events[0].tick);
	TEST_EXPECT(model.clip_loops() && model.clip_length_ticks() > events[1].tick);

	// The clip plays on the table that names it, as the row that names it.
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/walk.bad"));
	TEST_EXPECT(view.documents.previews.model.path == "anims/walk.bad");
	model.follow(view);
	TEST_EXPECT(model.status() == ModelPreviewStatus::Ready && strutil_iequals(model.rig().table, "SKIN.adm"));
	TEST_EXPECT(model.clip_key() == "anim_walk_forward" && model.skeleton());
	// An event selected in the clip seeks to it and holds the clock there.
	Document *clip = session.document_for("anims/walk.bad");
	TEST_EXPECT(clip && !clip->rows().empty());
	const Node &clip_row = *clip->rows().front();
	EditorRequest pick = make_request(EditorRequestKind::SelectRecord, clip->path());
	pick.edit.address = {clip_row.id, node_kind(AnimationKind::Event), clip_row.collections[1][3]};
	session.handle(pick);
	model.advance(0.5);
	model.follow(view);
	TEST_EXPECT(model.clip_ticks() == model.tick_of_frame(3) && !model.options().playing);
	model.advance(0.5);
	TEST_EXPECT(model.clip_ticks() == model.tick_of_frame(3));

	// No item pairs a table: no rig, until a model is chosen.
	TEST_EXPECT(editor_test::write_text(view.project.root + "/defs/items.def", "begin \"Nothing\"\nid 100201\ntype building\nend\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/SKIN.adm"));
	model.follow(view);
	TEST_EXPECT(model.status() == ModelPreviewStatus::NoRig);
	TEST_EXPECT(json(view, model).get_string("status", "") == "no_rig");
	ModelPreviewOptions chosen = model.options();
	chosen.rig_model = "skinned.3di";
	model.set_options(chosen);
	TEST_EXPECT(model.follow(view) == ModelPreviewAction::Rebuild && model.status() == ModelPreviewStatus::Ready);
	TEST_EXPECT(model.rig().source == "chosen" && model.skeleton());
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

// The preview plays what the game's loader registered, from the open table as edited: a
// token whose file does not load registers nothing, so the row's clip after it is the
// slot's next variant, and the missing one plays nothing. A clip's events sit on the ticks
// the game's root motion reads their triggers; a one-shot's stopped tick reads none.
// [orig: AnimMap_ParseConfigLine @0x40cb60, the load gate @0x40cbe7;
//  AnimMap_FindOrLoadBoneFile @0x40c030, no failsafe @0x40c260]
static int test_runtime_clips() {
	editor_test::TempProjectDir dir("opennova_editor_model_preview_runtime");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	ModelPreviewModel model;
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Runtime Clips"));
	editor_test::create_missing_files(session);
	const std::string source = dir.file("source");
	TEST_EXPECT(editor_test::write_bytes(source + "/skinned.o3d",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                        "/fixtures/threedi/o3d/skinned.o3d")));
	TEST_EXPECT(editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips));
	TEST_EXPECT(editor_test::write_text(source + "/step.o3a", kStepClips));
	EditorRequest import = make_request(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}, {source + "/step.o3a", {}}};
	session.handle(import);
	TEST_EXPECT(view.project.scan->find("SKIN.adm") && view.project.scan->find("STEP.adm") && view.project.scan->find("step.bad"));
	ModelPreviewOptions chosen = model.options();
	chosen.rig_model = "skinned.3di";
	model.set_options(chosen);

	// The walk row edited to "reset" "missing" "walk", not saved.
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	NodeAddress walk;
	TEST_EXPECT(table && table->find("anim_walk_forward", walk));
	const NodeKind clip_kind = node_kind(AnimationMapKind::Clip);
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, table->path());
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
			// and the preview says why.
			TEST_EXPECT(!table->serialize().ok() && table->row(walk.row)->collections[0].size() == 2);
			select.edit.address = {walk.row, clip_kind, table->row(walk.row)->collections[0][1]};
			session.handle(select);
			model.follow(view);
			TEST_EXPECT(model.status() == ModelPreviewStatus::Unserializable && !model.skeleton());
			TEST_EXPECT(model.clip_key().empty() && model.clip_file().empty() && model.clip_events().empty());
			TEST_EXPECT(json(view, model).get_string("status", "") == "unserializable");
			// Nor does the empty token, which is the saved table's walk token by index.
			select.edit.address = {walk.row, clip_kind, table->row(walk.row)->collections[0][0]};
			session.handle(select);
			model.follow(view);
			TEST_EXPECT(model.status() == ModelPreviewStatus::Unserializable && model.clip_key().empty());
		}
		set(session, table->path(), {walk.row, clip_kind, table->last_added()}, "clip", std::string(name));
	}
	TEST_EXPECT(table->row(walk.row) && table->row(walk.row)->collections[0].size() == 3 && table->serialize().ok());
	const std::vector<NodeId> tokens = table->row(walk.row)->collections[0];
	select.edit.address = {walk.row, clip_kind, tokens[2]};
	session.handle(select);
	model.follow(view);
	TEST_EXPECT(model.status() == ModelPreviewStatus::Ready && model.skeleton());
	TEST_EXPECT(model.clip_key() == "anim_walk_forward" && model.clip_variant() == 1 &&
	            strutil_iequals(model.clip_file(), "walk"));
	const auto *served = model.skeleton() ? model.skeleton()->find_clip_variant(model.clip_key(), model.clip_variant())
	                                      : nullptr;
	TEST_EXPECT(served && served->source.file == "walk" && served->source.token == 2 && served->clip.frame_count == 4);
	select.edit.address = {walk.row, clip_kind, tokens[1]};
	session.handle(select);
	model.follow(view);
	TEST_EXPECT(model.clip_key().empty() && model.clip_file().empty() && model.clip_events().empty());

	// The one-shot's events against the game's root motion over the same files.
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/step.bad"));
	model.follow(view);
	TEST_EXPECT(model.status() == ModelPreviewStatus::Ready && strutil_iequals(model.rig().table, "STEP.adm"));
	TEST_EXPECT(model.clip_key() == "anim_idle" && model.clip_variant() == 0 && !model.clip_loops());
	const std::vector<PreviewClipEvent> events = model.clip_events();
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
	TEST_EXPECT(model.clip_length_ticks() == 2 && model.tick_of_frame(1) == 1 && model.tick_of_frame(2) == -1);
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
	std::printf("editor_model_preview: all tests passed\n");
	return 0;
}
