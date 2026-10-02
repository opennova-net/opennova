#include <editor/preview/mission_viewport.h>

#include <algorithm>
#include <cmath>
#include <variant>

#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/model/document.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_source.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The mission document at a viewport's path, as records (null: none open there).
const Document *document_of(const ViewportInput &input) {
	return input.document ? records_of(*input.document) : nullptr;
}

// The selection while `document` is the active one (null otherwise).
const Selection *selection_of(const ViewportInput &input, const Document &document) {
	const DocumentsView &documents = input.view.documents;
	return documents.active == document.path() ? &documents.selection : nullptr;
}

// A turn of `to` from `from`, degrees in -180..180.
double turned(double from, double to) {
	double by = std::fmod(to - from, 360.0);
	if (by > 180.0) by -= 360.0;
	if (by < -180.0) by += 360.0;
	return by;
}

JsonValue mission_point(const MissionMark &mark) {
	double at[3];
	preview_to_mission(mark.at, at);
	JsonValue out = JsonValue::make_array();
	for (const double value : at) out.push(json_number(value));
	return out;
}

// The scan's entry of a file a drop names: by its logical name, else by its project-relative path
// (what a Files row carries).
const AssetEntry *dropped_file(const SessionView &view, const std::string &file) {
	if (!view.project.scan) return nullptr;
	if (const AssetEntry *entry = view.project.scan->find(file)) return entry;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (strutil::iequals(entry.relative_path, file)) return &entry;
	return nullptr;
}

Edit set_of(const NodeAddress &record, const char *field, Value value) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

} // namespace

const char *mission_view_status_token(MissionViewStatus status) {
	switch (status) {
	case MissionViewStatus::NoProject: return "no_project";
	case MissionViewStatus::NoMission: return "no_mission";
	case MissionViewStatus::Ready: return "ready";
	}
	return "no_project";
}

std::string mission_view_status_message(MissionViewStatus status) {
	switch (status) {
	case MissionViewStatus::NoProject: return "Open a project to see its missions.";
	case MissionViewStatus::NoMission: return "Open a mission to see it.";
	case MissionViewStatus::Ready: return std::string();
	}
	return std::string();
}

std::string mission_camera_change(const OrbitCamera &camera) {
	// What a SetViewport takes: the camera's own members, not the read-only eye and fov.
	JsonValue view = mission_camera_to_json(camera);
	JsonValue taken = JsonValue::make_object();
	for (const char *member : { "target", "yaw", "pitch", "distance" })
		if (const JsonValue *value = view.get(member)) taken.set(member, *value);
	return viewport_change(ViewportKind::Mission, "camera", std::move(taken));
}

// --- MissionViewport -----------------------------------------------------------------------------

MissionViewport::MissionViewport(std::string path) :
		ViewportModel(ViewportKind::Mission, std::move(path), ViewportState{ 1024, 768 }) {
	// A mission is seen from afar: the scene's far plane is the device's (its fog's); the marks'
	// projection reads none.
	camera_.yaw = 0.0f;
	camera_.pitch = kMissionFramePitch;
	camera_.distance = 120.0f;
	camera_.near_plane = 0.5f;
}

std::unique_ptr<ViewportModel> MissionViewport::make(const std::string &path) {
	return std::make_unique<MissionViewport>(path);
}

ViewportStatus MissionViewport::status() const {
	return reason_ == MissionViewStatus::Ready ? ViewportStatus::Ready : ViewportStatus::Empty;
}

std::unique_ptr<CanvasHalf> MissionViewport::make_canvas() const {
	return std::make_unique<MissionCanvas>();
}

std::vector<MissionMark> MissionViewport::marks(int width, int height, const ViewportDevice *device) const {
	if (reason_ != MissionViewStatus::Ready) return {};
	return mission_marks(scene_, options_, camera_, width, height, device);
}

bool MissionViewport::pressed(const NodeAddress &record, MissionPressed &out) const {
	out = MissionPressed();
	out.record = record;
	if (const MissionEntityMark *entity = scene_.entity(record.row)) {
		out.x = entity->x;
		out.y = entity->y;
		out.z = entity->z;
		out.yaw = entity->yaw;
		return true;
	}
	if (const MissionAreaMark *area = scene_.area(record.row)) {
		out.area = true;
		for (int axis = 0; axis < 3; ++axis) {
			out.min[axis] = area->min[axis];
			out.max[axis] = area->max[axis];
		}
		out.x = (area->min[0] + area->max[0]) * 0.5;
		out.y = (area->min[1] + area->max[1]) * 0.5;
		out.z = area->min[2];
		return true;
	}
	return false;
}

bool MissionViewport::handle_at(const MissionMark &mark, MissionHandle handle, PreviewVec3 &out) const {
	if (mark.area >= 0) {
		if (handle == MissionHandle::Move) {
			out = mark.at;
			return true;
		}
		if (!mission_handle_is_edge(handle)) return false;
		// At the anchor's height: the ground under its middle, or its z_min (the mark's own).
		double anchor[3];
		preview_to_mission(mark.at, anchor);
		out = mission_area_edge_middle(scene_.areas()[size_t(mark.area)], handle, anchor[2]);
		return true;
	}
	if (mark.entity < 0) return false;
	const MissionEntityMark &entity = scene_.entities()[size_t(mark.entity)];
	switch (handle) {
	case MissionHandle::Move: out = entity.at; return true;
	case MissionHandle::Height: out = mission_height_handle(entity, handle_reach()); return true;
	case MissionHandle::Yaw: out = mission_yaw_handle(entity, handle_reach()); return true;
	default: return false;
	}
}

OrbitCamera MissionViewport::framed(const std::vector<MissionMark> &marks, const std::vector<int> &of, int width,
		int height) const {
	OrbitCamera camera = camera_;
	// The box of the anchors, its middle and half its diagonal.
	bool any = false;
	PreviewVec3 low, high;
	const auto take = [&](const MissionMark &mark) {
		if (!any) low = high = mark.at;
		any = true;
		low = PreviewVec3{ std::min(low.x, mark.at.x), std::min(low.y, mark.at.y), std::min(low.z, mark.at.z) };
		high = PreviewVec3{ std::max(high.x, mark.at.x), std::max(high.y, mark.at.y), std::max(high.z, mark.at.z) };
	};
	if (of.empty()) {
		for (const MissionMark &mark : marks) take(mark);
	} else {
		for (const int index : of)
			if (index >= 0 && size_t(index) < marks.size()) take(marks[size_t(index)]);
	}
	if (!any) return camera;
	const PreviewVec3 center{ (low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f };
	const float dx = high.x - low.x, dy = high.y - low.y, dz = high.z - low.z;
	const float radius = std::max(kMissionFrameRadius, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
	camera.frame(center, radius, width, height);
	camera.distance = std::min(camera.distance, kMissionFrameDistance);
	return camera;
}

ViewportAction MissionViewport::stop_(MissionViewStatus reason) {
	reason_ = reason;
	detail_.clear();
	scene_.clear();
	missing_.clear();
	ground_ = false;
	shown_none();
	return picture_.stop();
}

ViewportAction MissionViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const SessionView &view = input.view;
	gesture_open_ = view.documents.gesture_in(path()).open();
	if (!view.project.open || !view.findings.assets) return stop_(MissionViewStatus::NoProject);
	const Document *document = document_of(input);
	const std::unique_ptr<MissionSceneSource> source = document ? mission_scene_source(*document) : nullptr;
	if (!source) return stop_(MissionViewStatus::NoMission);
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const PreviewFollow::Key key;
	reason_ = MissionViewStatus::Ready;

	// What the document changed, in the picture's terms: a change set patches the scene by its rows;
	// anything else (followed the first time, read again, a state its history no longer holds) reads
	// it whole.
	const auto *rows = input.change == ChangeClass::Changed && input.changes ? std::get_if<RowChanges>(input.changes)
																		   : nullptr;
	const bool anew = !picture_.shows() || (input.change != ChangeClass::None && !rows);
	if (anew) {
		scene_.read(*source);
		picture_.show(key, generation);
		shown(*document);
		if (!framed_) {
			// The one change its follow derives: the camera on the entities, looking north and down.
			framed_ = true;
			const ViewportState picture = size();
			camera_.yaw = 0.0f;
			camera_.pitch = kMissionFramePitch;
			camera_ = framed(mission_marks(scene_, MissionViewportOptions(), camera_, picture.width, picture.height, nullptr),
					{}, picture.width, picture.height);
			state_moved();
		}
		options_moved_ = false;
		return picture_.built(FileStamps());
	}
	MissionSceneDelta delta;
	if (rows) {
		delta = scene_.patch(*rows, *source);
		shown(*document);
	}
	// A file the device read moved: the picture made again from the files, over the scene as it is.
	if (picture_.follow(key, false, files, generation) == PreviewFollow::Found::Files) {
		options_moved_ = false;
		return picture_.built(FileStamps());
	}
	if (delta.header || delta.reshaped) {
		options_moved_ = false;
		return ViewportAction::Rebuild;
	}
	if (!delta.moved && !options_moved_) return ViewportAction::Keep;
	options_moved_ = false;
	return ViewportAction::Update;
}

bool MissionViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool MissionViewport::check_(const io::JsonValue &json, std::string &error) const {
	MissionViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !mission_options_from_json(*member, options, error))
		return false;
	OrbitCamera camera = camera_;
	if (const JsonValue *member = json.get("camera"); member && !mission_camera_from_json(*member, camera, error))
		return false;
	return true;
}

void MissionViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	MissionViewportOptions options = options_;
	if (const JsonValue *member = json.get("options");
			member && mission_options_from_json(*member, options, error) && options != options_) {
		options_ = options;
		options_moved_ = true;
	}
	if (const JsonValue *member = json.get("camera")) mission_camera_from_json(*member, camera_, error);
}

void MissionViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	missing_ = report.missing;
	ground_ = report.surface;
}

int MissionViewport::mark_of_(const std::vector<MissionMark> &marks, NodeId id) {
	for (size_t i = 0; i < marks.size(); ++i)
		if (marks[i].record.row == id) return int(i);
	return -1;
}

MissionCanvasFrame MissionViewport::canvas_frame(const ViewportContext &context) const {
	MissionCanvasFrame frame;
	const ViewportInput &input = context.input;
	frame.viewport = this;
	frame.snap = context.snap;
	frame.turn_snap = context.snap > 0.0f ? kMissionTurnSnap : 0.0f;
	frame.editable = context.editable();
	frame.device = context.device;
	frame.document = reason_ == MissionViewStatus::Ready ? document_of(input) : nullptr;
	frame.current = frame.document && current(input);
	frame.marks = marks(context.width, context.height, context.device);
	const Selection *selection = frame.current ? selection_of(input, *frame.document) : nullptr;
	if (!selection) return frame;
	frame.records = selection->records;
	for (const NodeAddress &record : selection->records) {
		const int mark = mark_of_(frame.marks, record.row);
		if (mark < 0) continue;
		if (record == selection->primary) frame.primary = mark;
		else frame.selected.push_back(mark);
	}
	return frame;
}

ViewportHit MissionViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	const Document *document = document_of(context.input);
	if (reason_ != MissionViewStatus::Ready || !document) return out;
	const std::vector<MissionMark> shown = marks(context.width, context.height, context.device);
	out.index = pick_mission_mark(shown, x, y);
	if (out.index < 0) return out;
	const MissionMark &mark = shown[size_t(out.index)];
	out.id = out.current ? mark.record.row : 0;
	out.name = document->record_title(mark.record);
	out.kind = mark.kind;
	return out;
}

std::vector<ViewportHit> MissionViewport::box(const ViewportContext &context, float x0, float y0, float x1,
		float y1) const {
	std::vector<ViewportHit> out;
	const Document *document = document_of(context.input);
	if (reason_ != MissionViewStatus::Ready || !document || !current(context.input)) return out;
	const std::vector<MissionMark> shown = marks(context.width, context.height, context.device);
	for (const NodeAddress &record : mission_box_records(shown, CanvasPoint{ x0, y0 }, CanvasPoint{ x1, y1 })) {
		const int index = mark_of_(shown, record.row);
		if (index < 0) continue;
		ViewportHit hit;
		hit.current = true;
		hit.index = index;
		hit.id = record.row;
		hit.name = document->record_title(record);
		hit.kind = shown[size_t(index)].kind;
		out.push_back(std::move(hit));
	}
	return out;
}

const Document *MissionViewport::planned_(const ViewportContext &context, std::string &error) const {
	const Document *document = document_of(context.input);
	if (reason_ != MissionViewStatus::Ready || !document || !current(context.input)) {
		const std::string why = message();
		error = "The viewport shows no picture of the mission as it is now" + (why.empty() ? std::string(".") : ": " + why);
		return nullptr;
	}
	return document;
}

bool MissionViewport::handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x,
		float &y, std::string &error) const {
	MissionHandle held = MissionHandle::Move;
	if (!mission_handle_from_token(handle.c_str(), held)) {
		error = "Unknown handle \"" + handle + "\" (move, height, yaw, x_min, x_max, y_min, y_max).";
		return false;
	}
	if (!planned_(context, error)) return false;
	const std::vector<MissionMark> shown = marks(context.width, context.height, context.device);
	const int index = mark_of_(shown, id);
	if (index < 0) {
		error = "Record " + std::to_string(id) + " is no entity or area the viewport shows.";
		return false;
	}
	PreviewVec3 at;
	if (!handle_at(shown[size_t(index)], held, at)) {
		error = shown[size_t(index)].area >= 0 ? "An area has no " + handle + " handle (move, x_min, x_max, y_min, y_max)."
											   : "An entity has no " + handle + " handle (move, height, yaw).";
		return false;
	}
	if (!camera_.project(at, context.width, context.height, x, y)) {
		error = "The record's handle is not on the picture: drag it to a point of the picture (to).";
		return false;
	}
	return true;
}

std::vector<MissionPressed> MissionViewport::taken_(const ViewportContext &context, const Document &document,
		const NodeAddress &record, MissionHandle handle, size_t &grabbed) const {
	std::vector<MissionPressed> out;
	grabbed = 0;
	MissionPressed own;
	if (!pressed(record, own)) return out;
	const Selection *selection = selection_of(context.input, document);
	// An edge is its area's alone; a record not selected moves alone.
	if (mission_handle_is_edge(handle) || !selection || !selection->holds(record)) {
		out.push_back(own);
		return out;
	}
	for (const NodeAddress &each : selection->records) {
		MissionPressed held;
		if (!pressed(each, held) || (held.area && handle != MissionHandle::Move)) continue;
		if (each == record) grabbed = out.size();
		out.push_back(held);
	}
	return out;
}

bool MissionViewport::drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
		std::string &error) const {
	float hx = 0.0f, hy = 0.0f;
	// The handle where the picture shows it now: what a `by` goes from, and what refuses a handle the
	// record has not. A drag to a point takes a handle that is off the picture.
	std::string off;
	const bool on_picture = handle_point(context, drag.id, drag.handle, hx, hy, off);
	MissionHandle handle = MissionHandle::Move;
	if (!on_picture && (drag.by || off.find("not on the picture") == std::string::npos)) {
		error = off;
		return false;
	}
	mission_handle_from_token(drag.handle.c_str(), handle);
	const Document *document = planned_(context, error);
	if (!document) return false;
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	if (!(drag.snap >= 0.0f)) {
		error = "The snap is 0 or more.";
		return false;
	}
	// A step that moves nothing plans no batch; the gesture its sample names ends with it all the same.
	if (drag.by && drag.x == 0.0f && drag.y == 0.0f) {
		if (drag.end && drag.gesture) out.request(request::end_edit(document->path()));
		return true;
	}
	const NodeAddress record = document->address_of(drag.id);
	size_t grabbed = 0;
	const std::vector<MissionPressed> taken = taken_(context, *document, record, handle, grabbed);
	if (taken.empty()) {
		error = "Record " + std::to_string(drag.id) + " is no entity or area the viewport shows.";
		return false;
	}
	const MissionPressed &held = taken[grabbed];
	const float x = drag.by ? hx + drag.x : drag.x, y = drag.by ? hy + drag.y : drag.y;
	const uint64_t gesture = drag.gesture ? drag.gesture : next_edit_gesture();
	std::vector<Edit> edits;
	bool planned = false;
	switch (handle) {
	case MissionHandle::Move: {
		// On the ground: the anchor goes as far as the ground point under the pointer went from the one
		// under the handle (an entity standing on the ground goes to the point itself).
		double to[3], from[3];
		if (!mission_ground_point(context, camera_, x, y, held.z, to)) {
			error = "The point is not over the ground.";
			return false;
		}
		double target[2] = { to[0], to[1] };
		if (on_picture && mission_ground_point(context, camera_, hx, hy, held.z, from)) {
			target[0] = held.x + (to[0] - from[0]);
			target[1] = held.y + (to[1] - from[1]);
		}
		planned = mission_move_edits(*document, taken, grabbed, target, drag.snap, options_.stick, context.device,
				gesture, edits);
		break;
	}
	case MissionHandle::Height: {
		// In the plane through the handle that faces the eye: the handle's height there.
		const PreviewVec3 through = mission_scene_point(held.x, held.y, held.z + double(handle_reach()));
		PreviewVec3 at;
		if (!camera_.on_view_plane(x, y, context.width, context.height, through, at)) {
			error = "The point is not over the record's height.";
			return false;
		}
		planned = mission_height_edits(*document, taken, grabbed, double(at.y) - double(through.y), drag.snap, gesture, edits);
		break;
	}
	case MissionHandle::Yaw: {
		// The heading from the anchor to the point under the pointer at the anchor's height.
		double at[3];
		if (!mission_camera_on_height(camera_, x, y, context.width, context.height, held.z, at) ||
				(at[0] == held.x && at[1] == held.y)) {
			error = "The point is not beside the record.";
			return false;
		}
		const double heading = std::atan2(at[0] - held.x, at[1] - held.y) / io::kRadiansPerDegree;
		planned = mission_yaw_edits(*document, taken, grabbed, turned(double(held.yaw), heading), drag.snap, gesture, edits);
		break;
	}
	default: {
		double to[3];
		if (!mission_ground_point(context, camera_, x, y, held.z, to)) {
			error = "The point is not over the ground.";
			return false;
		}
		const bool east = handle == MissionHandle::XMin || handle == MissionHandle::XMax;
		planned = mission_area_edge_edits(*document, held, handle, to[east ? 0 : 1], drag.snap, gesture, edits);
		break;
	}
	}
	if (!planned) {
		error = "The record has no such handle.";
		return false;
	}
	const bool wrote = !edits.empty();
	if (wrote) out.request(request::edit_record(document->path(), std::move(edits)));
	if (drag.end && (wrote || drag.gesture)) out.request(request::end_edit(document->path()));
	return true;
}

bool MissionViewport::drop(const ViewportContext &context, const ViewportDrop &drop, CanvasRequests &out,
		std::string &error) const {
	const Document *document = planned_(context, error);
	if (!document) return false;
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	const SessionView &view = context.input.view;
	// The item: named, else the one item whose graphic the dropped model is.
	int64_t item = 0;
	if (!drop.reference.empty()) {
		const std::optional<int> id = strutil::parse_int(drop.name);
		if (drop.reference != "item" || !id) {
			error = "A mission viewport takes an item by its id (reference \"item\") or a model file.";
			return false;
		}
		item = *id;
	} else {
		const AssetEntry *entry = dropped_file(view, drop.file);
		if (!entry) {
			error = "The project has no file \"" + drop.file + "\".";
			return false;
		}
		if (entry->kind != AssetKind::Model) {
			error = entry->logical_name + " is no model: a mission viewport takes a model file or an item.";
			return false;
		}
		const std::vector<int64_t> items = mission_items_of_model(view, entry->relative_path);
		if (items.empty()) {
			error = "No item of the project draws " + entry->logical_name + ": add one to an item catalog, or drop an item.";
			return false;
		}
		if (items.size() > 1) {
			error = "Several items draw " + entry->logical_name + ":";
			for (size_t i = 0; i < items.size(); ++i) {
				MissionItemFacts facts;
				std::string ignored;
				mission_item_facts(view, items[i], facts, ignored);
				error += std::string(i ? ", " : " ") + (facts.name.empty() ? std::string() : facts.name + " ") + "(" +
						std::to_string(items[i]) + ")";
			}
			error += ". Drop one of them by its item.";
			return false;
		}
		item = items.front();
	}
	MissionItemFacts facts;
	if (!mission_item_facts(view, item, facts, error)) return false;
	// Where the point meets the ground (the device's terrain, else the plane through the camera's
	// target); on the terrain, the model's ground anchor baked in (the stored position is the ground
	// point less the anchor: docs/world/world-wac-ai-re.md section 12).
	double target[3], at[3];
	preview_to_mission(camera_.target, target);
	bool on_terrain = false;
	if (!mission_ground_point(context, camera_, drop.x, drop.y, target[2], at, &on_terrain)) {
		error = "The point is not over the ground.";
		return false;
	}
	if (on_terrain)
		for (int i = 0; i < 3; ++i) at[i] -= facts.anchor[i];
	// One batch: the entity of the item added to the pool its TYPE puts it in, then placed.
	const NodeKind kind = node_kind(facts.pool);
	std::vector<Edit> edits;
	Edit add;
	add.operation = EditOperation::Add;
	add.address = NodeAddress{ 0, kind, 0 };
	add.field = "item";
	add.value = item;
	edits.push_back(std::move(add));
	const NodeAddress made{ batch_made(0), kind, 0 };
	edits.push_back(set_of(made, "x", at[0]));
	edits.push_back(set_of(made, "y", at[1]));
	edits.push_back(set_of(made, "z", at[2]));
	out.request(request::edit_record(document->path(), std::move(edits)));
	return true;
}

bool MissionViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
		CanvasRequests &out, std::string &error) const {
	if (name != "frame" && name != "top" && name != "ground") {
		error = "Unknown mission command \"" + name + "\" (frame, top, ground).";
		return false;
	}
	if (reason_ != MissionViewStatus::Ready) {
		error = "The viewport shows no mission.";
		return false;
	}
	if (name == "ground") {
		// Each named entity (else each selected one) set down on the ground under it: its height the
		// ground's less its model's anchor height (the game's vertical terrain conform: only the
		// height, docs/world/world-wac-ai-re.md section 12), one batch.
		const Document *document = planned_(context, error);
		if (!document) return false;
		if (!context.editable()) {
			error = context.not_editable();
			return false;
		}
		std::vector<NodeId> rows = ids;
		if (rows.empty())
			if (const Selection *selection = selection_of(context.input, *document))
				for (const NodeAddress &record : selection->records)
					if (scene_.entity(record.row)) rows.push_back(record.row);
		if (rows.empty()) {
			error = "No entity to set down: name one, or select one.";
			return false;
		}
		std::vector<Edit> edits;
		for (const NodeId row : rows) {
			const MissionEntityMark *entity = scene_.entity(row);
			if (!entity) {
				error = "Record " + std::to_string(row) + " is no entity of the mission.";
				return false;
			}
			double ground = 0.0;
			if (!context.device || !context.device->ground_at(entity->x, entity->y, ground)) {
				error = "The picture has no ground under record " + std::to_string(row) + " (no terrain built there).";
				return false;
			}
			MissionItemFacts facts;
			std::string ignored;
			mission_item_facts(context.input.view, entity->item, facts, ignored);
			const double z = ground - facts.anchor[2];
			// Where its 16.16 word moves.
			if (bms::to_fixed_16_16(z) != bms::to_fixed_16_16(entity->z))
				edits.push_back(set_of(NodeAddress{ row, entity->kind, 0 }, "z", z));
		}
		if (!edits.empty()) out.request(request::edit_record(document->path(), std::move(edits)));
		return true;
	}
	if (name == "top") {
		OrbitCamera camera = camera_;
		mission_camera_top(camera);
		out.request(request::set_viewport(path(), mission_camera_change(camera)));
		return true;
	}
	// The first record named (one that is no entity or area refused), else the selection, else
	// everything.
	const std::vector<MissionMark> shown = marks(context.width, context.height, context.device);
	std::vector<int> of;
	if (!ids.empty()) {
		const int index = mark_of_(shown, ids.front());
		if (index < 0) {
			error = "Record " + std::to_string(ids.front()) + " is no entity or area the viewport shows.";
			return false;
		}
		of.push_back(index);
	} else if (const Document *document = document_of(context.input)) {
		if (const Selection *selection = selection_of(context.input, *document))
			for (const NodeAddress &record : selection->records)
				if (const int index = mark_of_(shown, record.row); index >= 0) of.push_back(index);
	}
	out.request(request::set_viewport(path(), mission_camera_change(framed(shown, of, context.width, context.height))));
	return true;
}

io::JsonValue MissionViewport::options_json() const {
	return mission_options_to_json(options_);
}

io::JsonValue MissionViewport::camera_json() const {
	return mission_camera_to_json(camera_);
}

io::JsonValue MissionViewport::body_json(const ViewportInput &) const {
	JsonValue body = JsonValue::make_object();
	const MissionSceneHeader &header = scene_.header();
	body.set("terrain", json_string(header.terrain));
	body.set("environment", json_string(header.environment));
	JsonValue counts = JsonValue::make_object();
	counts.set("items", json_number(double(scene_.count(MissionPool::Item))));
	counts.set("buildings", json_number(double(scene_.count(MissionPool::Building))));
	counts.set("markers", json_number(double(scene_.count(MissionPool::Marker))));
	counts.set("organics", json_number(double(scene_.count(MissionPool::Organic))));
	counts.set("areas", json_number(double(scene_.areas().size())));
	counts.set("paths", json_number(double(scene_.paths().size())));
	body.set("counts", std::move(counts));
	body.set("ground", JsonValue::make_bool(ground_));
	body.set("missing", json_number(double(missing_.size())));
	return body;
}

io::JsonValue MissionViewport::items_json(const ViewportInput &input) const {
	JsonValue items = JsonValue::make_array();
	const Document *document = document_of(input);
	if (reason_ != MissionViewStatus::Ready || !document || !current(input)) return items;
	const ViewportContext context = viewport_context(input.view, *this);
	const std::vector<MissionMark> shown = marks(context.width, context.height, context.device);
	const Selection *selection = selection_of(input, *document);
	for (size_t index = 0; index < shown.size(); ++index) {
		const MissionMark &mark = shown[index];
		JsonValue item = JsonValue::make_object();
		item.set("index", json_number(double(index)));
		item.set("id", json_number(double(mark.record.row)));
		item.set("kind", json_string(mark.kind));
		item.set("name", json_string(document->record_title(mark.record)));
		item.set("at", mission_point(mark));
		if (mark.entity >= 0) {
			const MissionEntityMark &entity = scene_.entities()[size_t(mark.entity)];
			item.set("item", json_number(double(entity.item)));
			item.set("yaw", json_number(entity.yaw));
			item.set("team", json_number(entity.team));
		} else {
			const MissionAreaMark &area = scene_.areas()[size_t(mark.area)];
			item.set("zone", json_number(area.zone));
			JsonValue low = JsonValue::make_array(), high = JsonValue::make_array();
			for (int axis = 0; axis < 3; ++axis) {
				low.push(json_number(area.min[axis]));
				high.push(json_number(area.max[axis]));
			}
			item.set("min", std::move(low));
			item.set("max", std::move(high));
		}
		// Where the picture shows it (only while it is in front of the eye).
		if (mark.depth > 0.0f) {
			JsonValue screen = JsonValue::make_array();
			screen.push(json_number(mark.x));
			screen.push(json_number(mark.y));
			item.set("screen", std::move(screen));
			item.set("depth", json_number(mark.depth));
		}
		item.set("shown", JsonValue::make_bool(mark.shown));
		item.set("selected", JsonValue::make_bool(selection && selection->holds(mark.record)));
		items.push(std::move(item));
	}
	return items;
}

io::JsonValue MissionViewport::notes_json(const ViewportInput &input) const {
	JsonValue notes = JsonValue::make_array();
	const Document *document = document_of(input);
	if (reason_ != MissionViewStatus::Ready || !document || !current(input)) return notes;
	// A file the picture asked the project for and did not find: an Import in Problems mends it.
	for (const std::string &name : missing_) {
		JsonValue note = JsonValue::make_object();
		note.set("code", json_string("file.missing"));
		note.set("name", json_string(name));
		note.set("message", json_string("The project has no " + name + ": import it to see it."));
		notes.push(std::move(note));
	}
	// A path's stop that names no marker: its line skips it.
	for (const MissionPathMark &path : scene_.paths()) {
		for (size_t stop = 0; stop < path.stops.size(); ++stop) {
			if (path.stops[stop] && scene_.entity(path.stops[stop])) continue;
			JsonValue note = JsonValue::make_object();
			note.set("code", json_string("path.stop"));
			note.set("id", json_number(double(path.row)));
			note.set("name", json_string(document->record_title(NodeAddress{ path.row, path.kind, 0 })));
			note.set("stop", json_number(double(stop)));
			note.set("message", json_string("Stop " + std::to_string(stop + 1) + " names no marker."));
			notes.push(std::move(note));
		}
	}
	return notes;
}

} // namespace opennova::editor
