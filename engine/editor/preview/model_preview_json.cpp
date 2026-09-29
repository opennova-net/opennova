#include <editor/preview/model_preview_json.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <editor/documents/model_document.h>
#include <editor/model/edit.h>
#include <editor/session/project_session.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

JsonValue num(double value) { return JsonValue::make_number(value); }
JsonValue str(const std::string &value) { return JsonValue::make_string(value); }

JsonValue vec3(const PreviewVec3 &v) {
	JsonValue out = JsonValue::make_array();
	out.push(num(v.x));
	out.push(num(v.y));
	out.push(num(v.z));
	return out;
}

std::string fixed_name(const char *name, size_t size) {
	size_t length = 0;
	while (length < size && name[length]) ++length;
	return std::string(name, length);
}

bool ready(const ModelPreviewSnapshot &snapshot) {
	return snapshot.status == ModelPreviewStatus::Ready && snapshot.model && snapshot.model->model();
}

// True when the device shows the document as it is now.
bool current(const ModelPreviewSnapshot &snapshot) {
	return ready(snapshot) && snapshot.document && snapshot.model->shown_path() == snapshot.document->path() &&
	       snapshot.model->shown_revision() == snapshot.document->revision();
}

// A JSON number as a whole number in [lo, hi], its fraction dropped; false for anything else.
bool whole_in(const JsonValue &json, double lo, double hi, int64_t &out) {
	if (!json.is_number()) return false;
	const double whole = std::trunc(json.number);
	if (!(whole >= lo && whole <= hi)) return false;
	out = int64_t(whole);
	return true;
}

} // namespace

ModelPreviewSnapshot model_preview_snapshot(const SessionView &view, const ModelPreviewModel &model, bool device) {
	ModelPreviewSnapshot snapshot;
	snapshot.status = device ? model.status() : ModelPreviewStatus::NoDevice;
	snapshot.model = &model;
	for (const auto &open : view.documents)
		if (open && open->path() == view.model_preview.path) snapshot.document = dynamic_cast<const ModelDocument *>(open.get());
	return snapshot;
}

NodeAddress model_preview_record(const ModelPreviewSnapshot &snapshot, const ModelOverlay &overlay) {
	return current(snapshot) ? model_overlay_record(*snapshot.document, overlay, snapshot.model->lod()) : NodeAddress();
}

io::JsonValue model_preview_to_json(const ModelPreviewSnapshot &snapshot) {
	JsonValue out = JsonValue::make_object();
	const ModelPreviewModel &model = *snapshot.model;
	out.set("status", str(model_preview_status_token(snapshot.status)));
	out.set("message", str(model_preview_status_message(snapshot.status, model.detail())));
	out.set("detail", str(model.detail()));
	// The model document, or the clip or table an animation preview follows.
	out.set("path", str(snapshot.document ? snapshot.document->path() : model.animating() ? model.shown_path() : std::string()));
	out.set("revision", num(snapshot.document ? double(snapshot.document->revision()) : 0.0));
	out.set("shown_revision", num(double(model.shown_revision())));
	out.set("current", JsonValue::make_bool(current(snapshot)));
	out.set("builds", num(double(model.builds())));
	JsonValue device = JsonValue::make_object();
	device.set("width", num(model.device_width()));
	device.set("height", num(model.device_height()));
	out.set("device", device);
	const ModelPreviewOptions &held = model.options();
	JsonValue options = JsonValue::make_object();
	options.set("lod", held.lod < 0 ? str("auto") : num(held.lod));
	JsonValue registers_held = JsonValue::make_object();
	for (const auto &entry : held.ctrl) registers_held.set(entry.first, num(double(entry.second)));
	options.set("ctrl", registers_held);
	options.set("playing", JsonValue::make_bool(held.playing));
	JsonValue marks = JsonValue::make_object();
	marks.set("user_points", JsonValue::make_bool(held.overlays.user_points));
	marks.set("lights", JsonValue::make_bool(held.overlays.lights));
	marks.set("pivots", JsonValue::make_bool(held.overlays.pivots));
	options.set("overlays", marks);
	out.set("options", options);
	JsonValue clock = JsonValue::make_object();
	clock.set("time_ms", num(double(model.clock_ms())));
	clock.set("playing", JsonValue::make_bool(held.playing));
	out.set("clock", clock);
	const OrbitCamera &camera = model.camera();
	JsonValue view = JsonValue::make_object();
	view.set("target", vec3(camera.target));
	view.set("yaw", num(camera.yaw));
	view.set("pitch", num(camera.pitch));
	view.set("distance", num(camera.distance));
	view.set("fov", num(OrbitCamera::fov_horizontal_degrees()));
	out.set("camera", view);
	JsonValue lod = JsonValue::make_object();
	JsonValue registers = JsonValue::make_array();
	JsonValue overlays = JsonValue::make_array();
	if (ready(snapshot)) {
		const threedi::Threedi3di3 &shown = *model.model();
		int32_t projected = 0;
		lod.set("shown", num(model.lod()));
		lod.set("auto", num(model.auto_lod(&projected)));
		lod.set("count", num(double(shown.lod_count)));
		lod.set("projected_px", num(projected / 65536.0));
		JsonValue thresholds = JsonValue::make_array();
		for (size_t i = 0; i < shown.lod_count; ++i) thresholds.push(num(shown.lods[i].lod_threshold));
		lod.set("thresholds", thresholds);
		PreviewVec3 center;
		float radius = 0.0f;
		model_preview_sphere(shown, center, radius);
		JsonValue sphere = JsonValue::make_object();
		sphere.set("center", vec3(center));
		sphere.set("radius", num(radius));
		out.set("sphere", sphere);
		for (uint32_t i = 0; i < shown.ctrl.count; ++i) {
			const std::string name = fixed_name(shown.ctrl.registers[i].name, sizeof(shown.ctrl.registers[i].name));
			JsonValue row = JsonValue::make_object();
			row.set("name", str(name));
			const auto value = held.ctrl.find(name);
			row.set("value", num(value == held.ctrl.end() ? 0.0 : double(value->second)));
			registers.push(row);
		}
		for (const ModelOverlay &overlay : model.overlays()) {
			JsonValue row = JsonValue::make_object();
			row.set("kind", str(model_overlay_kind_token(overlay.kind)));
			row.set("index", num(overlay.index));
			row.set("id", num(double(model_preview_record(snapshot, overlay).child)));
			row.set("name", str(overlay.name));
			row.set("part", num(overlay.part));
			row.set("position", vec3(overlay.at));
			float x = 0.0f, y = 0.0f;
			if (camera.project(overlay.at, model.device_width(), model.device_height(), x, y)) {
				JsonValue screen = JsonValue::make_array();
				screen.push(num(x));
				screen.push(num(y));
				row.set("screen", screen);
			} else {
				row.set("screen", JsonValue());
			}
			if (overlay.has_direction) row.set("direction", vec3(overlay.direction));
			if (overlay.kind == ModelOverlayKind::Light) {
				row.set("radius", num(overlay.radius));
				row.set("cone", num(overlay.cone));
				char color[8];
				std::snprintf(color, sizeof(color), "%06X", overlay.color & 0xFFFFFFu);
				row.set("color", str(color));
			}
			overlays.push(row);
		}
	}
	out.set("lod", lod);
	out.set("registers", registers);
	out.set("overlays", overlays);
	options.set("rig_model", str(held.rig_model));
	out.set("options", options);
	if (!model.animating()) {
		out.set("animation", JsonValue());
		return out;
	}
	const PreviewRig &rig = model.rig();
	JsonValue animation = JsonValue::make_object();
	animation.set("table", str(rig.table));
	animation.set("clip", str(rig.clip));
	animation.set("model", str(rig.model));
	animation.set("source", str(rig.source));
	animation.set("rig", JsonValue::make_bool(model.skeleton() != nullptr));
	animation.set("key", str(model.clip_key()));
	animation.set("variant", num(model.clip_variant()));
	animation.set("file", str(model.clip_file()));
	animation.set("ticks", num(model.clip_ticks()));
	animation.set("frame", num(model.clip_frame()));
	animation.set("length_ticks", num(model.clip_length_ticks()));
	animation.set("loops", JsonValue::make_bool(model.clip_loops()));
	JsonValue events = JsonValue::make_array();
	for (const PreviewClipEvent &event : model.clip_events()) {
		JsonValue row = JsonValue::make_object();
		row.set("frame", num(event.frame));
		row.set("tick", num(event.tick));
		row.set("trigger", num(double(event.trigger)));
		events.push(row);
	}
	animation.set("events", events);
	out.set("animation", animation);
	return out;
}

io::JsonValue model_preview_hit_to_json(const ModelPreviewSnapshot &snapshot, float x, float y) {
	JsonValue out = JsonValue::make_object();
	int index = -1;
	std::vector<ModelOverlay> overlays;
	if (ready(snapshot)) {
		const ModelPreviewModel &model = *snapshot.model;
		overlays = model.overlays();
		index = pick_model_overlay(overlays, model.camera(), model.device_width(), model.device_height(), x, y);
	}
	const ModelOverlay *hit = index >= 0 ? &overlays[size_t(index)] : nullptr;
	out.set("kind", str(hit ? model_overlay_kind_token(hit->kind) : ""));
	out.set("index", num(hit ? hit->index : -1));
	out.set("id", num(hit ? double(model_preview_record(snapshot, *hit).child) : 0.0));
	out.set("name", str(hit ? hit->name : std::string()));
	out.set("current", JsonValue::make_bool(current(snapshot)));
	return out;
}

bool model_preview_options_from_json(const io::JsonValue &json, ModelPreviewModel &model) {
	if (!json.is_object()) return false;
	ModelPreviewOptions options = model.options();
	int64_t seek_ms = -1, seek_ticks = -1;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		int64_t number = 0;
		if (key == "lod") {
			if (value.is_string() && value.string == "auto") {
				options.lod = -1;
			} else {
				if (!whole_in(value, 0.0, 255.0, number)) return false;
				options.lod = int(number);
			}
		} else if (key == "ctrl") {
			if (!value.is_object()) return false;
			options.ctrl.clear();
			for (const io::JsonMember &held : value.object) {
				if (!whole_in(held.value, -9007199254740992.0, 9007199254740992.0, number)) return false;
				if (number != 0) options.ctrl[held.key] = number;
			}
		} else if (key == "playing") {
			if (!value.is_bool()) return false;
			options.playing = value.boolean;
		} else if (key == "overlays") {
			if (!value.is_object()) return false;
			for (const io::JsonMember &mark : value.object) {
				if (!mark.value.is_bool()) return false;
				if (mark.key == "user_points") options.overlays.user_points = mark.value.boolean;
				else if (mark.key == "lights") options.overlays.lights = mark.value.boolean;
				else if (mark.key == "pivots") options.overlays.pivots = mark.value.boolean;
				else return false;
			}
		} else if (key == "time_ms") {
			// Any time from 0 on, a later one than the clock holds taken as its last (a clip's
			// seek clamps it to the clip anyway).
			if (!value.is_number() || !(value.number >= 0.0)) return false;
			seek_ms = int64_t(std::min(std::trunc(value.number), double(UINT32_MAX)));
		} else if (key == "clip_ticks") {
			if (!whole_in(value, 0.0, double(INT32_MAX), seek_ticks)) return false;
		} else if (key == "rig_model") {
			if (!value.is_string()) return false;
			options.rig_model = value.string;
		} else {
			return false;
		}
	}
	if (seek_ms >= 0) model.seek(uint32_t(seek_ms));
	if (seek_ticks >= 0) model.seek_ticks(int32_t(seek_ticks));
	model.set_options(options);
	return true;
}

bool model_preview_camera_from_json(const io::JsonValue &json, ModelPreviewModel &model) {
	if (!json.is_object()) return false;
	OrbitCamera camera = model.camera();
	int64_t width = model.device_width(), height = model.device_height();
	bool frame = false;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "yaw" || key == "pitch" || key == "distance") {
			if (!value.is_number()) return false;
			const float f = float(value.number);
			if (key == "distance" && !(f > 0.0f)) return false;
			(key == "yaw" ? camera.yaw : key == "pitch" ? camera.pitch : camera.distance) = f;
		} else if (key == "target") {
			if (!value.is_array() || value.array.size() != 3) return false;
			for (const JsonValue &axis : value.array)
				if (!axis.is_number()) return false;
			camera.target = PreviewVec3{float(value.array[0].number), float(value.array[1].number),
			                            float(value.array[2].number)};
		} else if (key == "frame") {
			if (!value.is_bool()) return false;
			frame = value.boolean;
		} else if (key == "width" || key == "height") {
			if (!whole_in(value, 1.0, 8192.0, key == "width" ? width : height)) return false;
		} else {
			return false;
		}
	}
	camera.pitch = std::clamp(camera.pitch, -kOrbitPitchLimit, kOrbitPitchLimit);
	model.camera() = camera;
	model.set_device_size(int(width), int(height));
	if (frame) model.frame();
	return true;
}

bool model_preview_drag(ProjectSession &session, const ModelPreviewSnapshot &snapshot, NodeId record, ModelHandle handle,
                        float x, float y, float snap) {
	if (!current(snapshot) || snapshot.document->blocked() || !(snap >= 0.0f)) return false;
	const ModelDocument &document = *snapshot.document;
	const ModelPreviewModel &model = *snapshot.model;
	ModelOverlayKind kind;
	int index = -1;
	if (!model_overlay_of(document, document.address_of(record), kind, index)) return false;
	for (const ModelOverlay &overlay : model.overlays()) {
		if (overlay.kind != kind || overlay.index != index) continue;
		std::vector<Edit> edits;
		if (!model.handle_edits(document, overlay, handle, x, y, snap, next_edit_gesture(), edits)) return false;
		const std::string path = document.path();
		EditorRequest request = make_request(EditorRequestKind::EditRecord, path);
		request.edits = std::move(edits);
		session.handle(request);
		session.handle(make_request(EditorRequestKind::EndEdit, path));
		return true;
	}
	return false;
}

} // namespace opennova::editor
