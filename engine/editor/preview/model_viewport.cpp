#include <editor/preview/model_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <variant>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/animation_uses.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_panm_pose.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using io::json_whole_in;

JsonValue vec3(const PreviewVec3 &v) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(v.x));
	out.push(json_number(v.y));
	out.push(json_number(v.z));
	return out;
}

// The hash of a model as it draws, its user points aside: its bytes written without them (a user
// point's edit shows in the overlays alone, so it builds nothing); false when it does not write.
// Taken only where the document cannot say what changed (ChangeClass::Unknown, S13 V8): a change set
// says it without writing anything.
bool drawn_hash(const threedi::Threedi3di3 &model, uint64_t &out) {
	threedi::Threedi3di3 drawn = model;
	drawn.user_points = nullptr;
	drawn.user_point_count = 0;
	std::vector<uint8_t> written;
	if (threedi::threedi_3di3_write_memory(&drawn, written) != 0) return false;
	out = io::fnv1a64_bytes(io::kFnv1a64Offset, written.data(), written.size());
	return true;
}

// The model row of a model document, as the document holds it (null: none).
std::shared_ptr<const Node> model_row_of(const ModelDocument &document) {
	for (const auto &row : document.rows())
		if (row && row->kind == node_kind(ModelKind::Model)) return row;
	return nullptr;
}

// `read` with `points` for its user points (S13 V8): its other tables and the geometry the model it
// was read as, kept alive with it.
assets::Model with_user_points(const assets::Model &read, const std::vector<threedi::ThreediUserPoint> &points) {
	struct Patched {
		assets::Model read;
		threedi::Threedi3di3 model;
		std::vector<threedi::ThreediUserPoint> points;
	};
	auto patched = std::make_shared<Patched>();
	patched->read = read;
	patched->model = *read; // shallow: every table but the user points is the read model's
	patched->points = points;
	patched->model.user_points = patched->points.empty() ? nullptr : patched->points.data();
	patched->model.user_point_count = patched->points.size();
	return assets::Model(patched, &patched->model);
}

bool same_rig(const PreviewRig &a, const PreviewRig &b) {
	return a.model == b.model && a.table == b.table && a.clip == b.clip && a.source == b.source;
}

std::string file_of(const std::string &path) {
	return path.substr(path.find_last_of("/\\") + 1);
}

// A colour on the wire: "RRGGBB".
std::string rgb_hex(uint32_t rgb) {
	char color[8];
	std::snprintf(color, sizeof(color), "%06X", rgb & 0xFFFFFFu);
	return color;
}

// The options an `options` member sets over `held`: {lod ("auto" or a level 0..255), ctrl
// ({register: number}, the registers held, a 0 not held), overlays ({user_points, lights, pivots}
// booleans), rig_model (a model's file name, "" the paired one)}, each optional, a number's fraction
// dropped. False, with why, for another member or a value out of range or of another type.
bool read_options(const JsonValue &json, ModelViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "\"options\" is an object.";
		return false;
	}
	ModelViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		int64_t number = 0;
		if (key == "lod") {
			if (value.is_string() && value.string == "auto") {
				options.lod = -1;
			} else {
				if (!json_whole_in(value, 0.0, 255.0, number)) {
					error = "options.lod is \"auto\" or a level from 0 to 255.";
					return false;
				}
				options.lod = int(number);
			}
		} else if (key == "ctrl") {
			if (!value.is_object()) {
				error = "options.ctrl is an object of register -> number.";
				return false;
			}
			options.ctrl.clear();
			for (const io::JsonMember &register_held : value.object) {
				if (!json_whole_in(register_held.value, -9007199254740992.0, 9007199254740992.0, number)) {
					error = "options.ctrl." + register_held.key + " is a whole number.";
					return false;
				}
				if (number != 0) options.ctrl[register_held.key] = number;
			}
		} else if (key == "overlays") {
			// The markers, and the collision layers by their tokens (preview/model_collision).
			std::string tokens = "user_points, lights, pivots";
			for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount); ++l)
				tokens += std::string(", ") + model_collision_layer(ModelCollisionLayer(l)).token;
			if (!value.is_object()) {
				error = "options.overlays is an object {" + tokens + "}.";
				return false;
			}
			for (const io::JsonMember &mark : value.object) {
				if (!mark.value.is_bool()) {
					error = "options.overlays." + mark.key + " is true or false.";
					return false;
				}
				bool layer = false;
				for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount) && !layer; ++l)
					if (mark.key == model_collision_layer(ModelCollisionLayer(l)).token) {
						model_collision_layer_set(options.overlays, ModelCollisionLayer(l), mark.value.boolean);
						layer = true;
					}
				if (layer) continue;
				if (mark.key == "user_points") options.overlays.user_points = mark.value.boolean;
				else if (mark.key == "lights") options.overlays.lights = mark.value.boolean;
				else if (mark.key == "pivots") options.overlays.pivots = mark.value.boolean;
				else {
					error = "Unknown overlay \"" + mark.key + "\" (" + tokens + ").";
					return false;
				}
			}
		} else if (key == "rig_model") {
			if (!value.is_string()) {
				error = "options.rig_model is a model's file name (\"\" the paired one).";
				return false;
			}
			options.rig_model = value.string;
		} else if (key == "repeat" || key == "bones") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			(key == "repeat" ? options.repeat : options.bones) = value.boolean;
		} else {
			error = "Unknown model option \"" + key + "\" (it takes lod, ctrl, overlays, rig_model, repeat, bones).";
			return false;
		}
	}
	held = options;
	return true;
}

// The camera a `camera` member sets over `held`: {yaw, pitch, distance (> 0), target [x, y, z],
// frame (look at the whole model again)}, each optional, the pitch kept within kOrbitPitchLimit.
bool read_camera(const JsonValue &json, OrbitCamera &held, bool &frame, std::string &error) {
	if (!json.is_object()) {
		error = "\"camera\" is an object.";
		return false;
	}
	OrbitCamera camera = held;
	frame = false;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "yaw" || key == "pitch" || key == "distance") {
			if (!value.is_number() || !std::isfinite(value.number)) {
				error = "camera." + key + " is a number.";
				return false;
			}
			const float f = float(value.number);
			if (key == "distance" && !(f > 0.0f)) {
				error = "camera.distance is more than 0.";
				return false;
			}
			(key == "yaw" ? camera.yaw : key == "pitch" ? camera.pitch : camera.distance) = f;
		} else if (key == "target") {
			bool numbers = value.is_array() && value.array.size() == 3;
			for (size_t i = 0; numbers && i < 3; ++i) numbers = value.array[i].is_number();
			if (!numbers) {
				error = "camera.target is [x, y, z].";
				return false;
			}
			camera.target = PreviewVec3{ float(value.array[0].number), float(value.array[1].number),
				float(value.array[2].number) };
		} else if (key == "frame") {
			if (!value.is_bool()) {
				error = "camera.frame is true or false.";
				return false;
			}
			frame = value.boolean;
		} else {
			error = "Unknown camera member \"" + key + "\" (it takes yaw, pitch, distance, target, frame).";
			return false;
		}
	}
	camera.pitch = std::clamp(camera.pitch, -kOrbitPitchLimit, kOrbitPitchLimit);
	held = camera;
	return true;
}

} // namespace

const char *model_view_status_token(ModelViewStatus status) {
	switch (status) {
	case ModelViewStatus::NoProject: return "no_project";
	case ModelViewStatus::NoModel: return "no_model";
	case ModelViewStatus::Unserializable: return "unserializable";
	case ModelViewStatus::Unreadable: return "unreadable";
	case ModelViewStatus::NoRig: return "no_rig";
	case ModelViewStatus::Reading: return "reading";
	case ModelViewStatus::Ready: return "ready";
	}
	return "no_project";
}

std::string model_view_status_message(ModelViewStatus status, const std::string &detail) {
	switch (status) {
	case ModelViewStatus::NoProject: return "Open a project to preview its models.";
	case ModelViewStatus::NoModel: return "Open a model, a clip or an animation table to preview it.";
	case ModelViewStatus::Unserializable:
		return "The game could not read this file as it stands" + (detail.empty() ? std::string(".") : ": " + detail);
	case ModelViewStatus::Unreadable:
		return detail.empty() ? "The model this writes does not read back." : detail + " does not read as a model.";
	case ModelViewStatus::NoRig:
		return "No item pairs " + (detail.empty() ? std::string("this animation") : detail) +
		       " with a model: choose the model it plays on.";
	case ModelViewStatus::Reading:
		return "Reading the project's references: the model " +
		       (detail.empty() ? std::string("this animation") : detail) +
		       " plays on shows when they are read, or choose it.";
	case ModelViewStatus::Ready: return std::string();
	}
	return std::string();
}

std::string model_camera_change(const OrbitCamera &camera) {
	JsonValue view = JsonValue::make_object();
	view.set("target", vec3(camera.target));
	view.set("yaw", json_number(camera.yaw));
	view.set("pitch", json_number(camera.pitch));
	view.set("distance", json_number(camera.distance));
	return viewport_change(ViewportKind::Model, "camera", std::move(view));
}

io::JsonValue model_options_to_json(const ModelViewportOptions &held) {
	JsonValue options = JsonValue::make_object();
	options.set("lod", held.lod < 0 ? json_string("auto") : json_number(held.lod));
	JsonValue registers_held = JsonValue::make_object();
	for (const auto &entry : held.ctrl) registers_held.set(entry.first, json_number(double(entry.second)));
	options.set("ctrl", std::move(registers_held));
	JsonValue marks = JsonValue::make_object();
	marks.set("user_points", JsonValue::make_bool(held.overlays.user_points));
	marks.set("lights", JsonValue::make_bool(held.overlays.lights));
	marks.set("pivots", JsonValue::make_bool(held.overlays.pivots));
	for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount); ++l)
		marks.set(model_collision_layer(ModelCollisionLayer(l)).token,
		          JsonValue::make_bool(model_collision_layer_on(held.overlays, ModelCollisionLayer(l))));
	options.set("overlays", std::move(marks));
	options.set("rig_model", json_string(held.rig_model));
	options.set("repeat", JsonValue::make_bool(held.repeat));
	options.set("bones", JsonValue::make_bool(held.bones));
	return options;
}

// --- ModelViewport -------------------------------------------------------------------------------

ModelViewport::ModelViewport(std::string path) :
		ViewportModel(ViewportKind::Model, std::move(path), ViewportState{ 800, 600 }) {}

std::unique_ptr<ViewportModel> ModelViewport::make(const std::string &path) {
	return std::make_unique<ModelViewport>(path);
}

ViewportStatus ModelViewport::status() const {
	switch (reason_) {
	case ModelViewStatus::Ready: return ViewportStatus::Ready;
	case ModelViewStatus::Unserializable:
	case ModelViewStatus::Unreadable: return ViewportStatus::Failed;
	default: return ViewportStatus::Empty;
	}
}

std::string ModelViewport::caption() const {
	return animating_ && !rig_.model.empty() ? " on " + rig_.model : std::string();
}

std::unique_ptr<CanvasHalf> ModelViewport::make_canvas() const {
	return std::make_unique<ModelCanvas>();
}

int ModelViewport::auto_lod(int32_t *projected_q16) const {
	if (projected_q16) *projected_q16 = 0;
	return model_ ? model_preview_auto_lod(*model_, camera_, size().width, projected_q16) : -1;
}

int ModelViewport::lod() const {
	if (!model_ || model_->lod_count == 0) return -1;
	if (options_.lod >= 0) return std::min(options_.lod, static_cast<int>(model_->lod_count) - 1);
	return auto_lod();
}

std::vector<ModelOverlay> ModelViewport::overlays(const PreviewClock &clock) const {
	if (!model_) return {};
	int32_t bus[96];
	model_preview_ctrl_bus(options_.ctrl, bus);
	std::vector<ModelOverlay> out = model_overlays(*model_, lod(), clock.ms(), bus, options_.overlays);
	// A clip playing (S17): a user point on a bone rides it as the skin carries the mesh there, as
	// the game's attachment resolve poses a point on its bone [orig: Entity_GetAttachmentWorldPosition
	// @ 0x4B2670, as world::EntityPoseProvider ports it].
	if (!animating_ || !skeleton_ || clip_key_.empty() || !options_.overlays.user_points) return out;
	const std::vector<PreviewJoint> posed = joints(clock);
	for (ModelOverlay &overlay : out) {
		if (overlay.kind != ModelOverlayKind::UserPoint || overlay.index < 0 ||
				size_t(overlay.index) >= model_->user_point_count)
			continue;
		const int bone = model_->user_points[overlay.index].subobject_index;
		if (bone < 0 || size_t(bone) >= posed.size()) continue;
		overlay.at = preview_joint_carry(posed[size_t(bone)], overlay.at);
		if (overlay.has_direction) {
			const PreviewVec3 axis = preview_joint_carry(posed[size_t(bone)], overlay.direction, true);
			const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
			if (length > 0.0f) overlay.direction = PreviewVec3{axis.x / length, axis.y / length, axis.z / length};
		}
		overlay.part = bone;
	}
	return out;
}

ModelCollisionShapesPtr ModelViewport::collision(const PreviewClock &clock, ModelCollisionPick also) const {
	static const ModelCollisionShapesPtr none = std::make_shared<const std::vector<ModelCollisionShape>>();
	if (!model_ || animating_) return none;
	const int level = lod();
	// The clock matters only while a part the shapes ride animates (LOD 0's for the collision, the drawn
	// level's for the part spheres).
	const bool live = threedi::threedi_panm_lod_has_live(*model_, 0) ||
	                  (options_.overlays.part_spheres && level >= 0 && threedi::threedi_panm_lod_has_live(*model_, level));
	const uint32_t time_ms = live ? clock.ms() : 0;
	CollisionCache &cache = collision_cache_;
	if (cache.shapes && cache.model == model_.get() && cache.lod == level && cache.overlays == options_.overlays &&
	    cache.ctrl == options_.ctrl && cache.also == also && cache.time_ms == time_ms)
		return cache.shapes;
	int32_t bus[96];
	model_preview_ctrl_bus(options_.ctrl, bus);
	cache.shapes = std::make_shared<const std::vector<ModelCollisionShape>>(
			model_collision_shapes(model_, level, time_ms, bus, options_.overlays, also));
	cache.model = model_.get();
	cache.lod = level;
	cache.overlays = options_.overlays;
	cache.ctrl = options_.ctrl;
	cache.also = also;
	cache.time_ms = time_ms;
	++cache.builds;
	return cache.shapes;
}

std::vector<NodeId> ModelViewport::frame_ids(const ViewportContext &context) const {
	const ViewportInput &input = context.input;
	const auto *document = dynamic_cast<const ModelDocument *>(input.document ? records_of(*input.document) : nullptr);
	const NodeAddress &selected = input.view.documents.selection.primary;
	if (!document || input.view.documents.active != path() || !selected.child) return {};
	ModelOverlayKind kind;
	int index = -1;
	if (model_overlay_of(*document, selected, kind, index)) return {selected.child};
	ModelCollisionPick picked;
	if (!model_collision_of(*document, selected, picked)) return {};
	for (const ModelCollisionShape &shape : *collision(input.clock, picked))
		if (shape.kind == picked.kind && shape.index == picked.index) return {selected.child};
	return {};
}

ModelCollisionPick ModelViewport::selected_collision(const ViewportInput &input) const {
	ModelCollisionPick picked;
	const auto *document = dynamic_cast<const ModelDocument *>(input.document ? records_of(*input.document) : nullptr);
	if (document && current(input) && input.view.documents.active == document->path())
		model_collision_of(*document, input.view.documents.selection.primary, picked);
	return picked;
}

PreviewVec3 ModelViewport::axis_tip(const ModelOverlay &overlay) const {
	const float length = axis_length();
	return PreviewVec3{ overlay.at.x + overlay.direction.x * length,
		overlay.at.y + overlay.direction.y * length, overlay.at.z + overlay.direction.z * length };
}

bool ModelViewport::handle_edits(const ModelDocument &document, const ModelOverlay &overlay,
		ModelHandle handle, float x, float y, int width, int height, float snap, uint64_t gesture,
		const PreviewClock &clock, std::vector<Edit> &out,
		const std::vector<ModelOverlay> *others) const {
	out.clear();
	if (!model_) return false;
	const PreviewVec3 through = handle == ModelHandle::Axis ? axis_tip(overlay) : overlay.at;
	PreviewVec3 to;
	if (!camera_.on_view_plane(x, y, width, height, through, to)) return false;
	int32_t bus[96];
	model_preview_ctrl_bus(options_.ctrl, bus);
	if (!model_handle_edits(document, *model_, overlay, lod(), clock.ms(), bus, handle, to, snap, gesture, out))
		return false;
	if (handle != ModelHandle::Place || !others) return true;
	// The other selected markers' places as far, in the same batch.
	const PreviewVec3 by{ to.x - overlay.at.x, to.y - overlay.at.y, to.z - overlay.at.z };
	for (const ModelOverlay &other : *others) {
		std::vector<Edit> edits;
		const PreviewVec3 moved{ other.at.x + by.x, other.at.y + by.y, other.at.z + by.z };
		if (model_handle_edits(document, *model_, other, lod(), clock.ms(), bus, ModelHandle::Place, moved,
					snap, gesture, edits))
			out.insert(out.end(), std::make_move_iterator(edits.begin()), std::make_move_iterator(edits.end()));
	}
	return true;
}

OrbitCamera ModelViewport::framed(int width, int height) const {
	OrbitCamera camera = camera_;
	if (!model_) return camera;
	PreviewVec3 center;
	float radius = 1.0f;
	model_preview_sphere(*model_, center, radius);
	camera.frame(center, radius, width, height);
	return camera;
}

OrbitCamera ModelViewport::framed_on(const ModelOverlay &overlay, int width, int height) const {
	OrbitCamera camera = camera_;
	if (!model_) return camera;
	PreviewVec3 center;
	float radius = 1.0f;
	model_preview_sphere(*model_, center, radius);
	const bool reach = overlay.kind == ModelOverlayKind::Light && overlay.radius > 0.0f;
	const float around = reach ? overlay.radius : std::max(0.25f, radius * 0.15f);
	camera.frame(overlay.at, around, width, height);
	return camera;
}

void ModelViewport::frame_() {
	camera_ = framed(size().width, size().height);
}

int32_t ModelViewport::clip_length_ticks() const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.playback().length_ticks() : -1;
}

bool ModelViewport::clip_loops() const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip && clip->clip.loops();
}

int32_t ModelViewport::tick_of_frame(int frame) const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	if (!clip || frame < 0) return -1;
	const std::vector<int32_t> ticks = clip->clip.playback().first_ticks();
	return size_t(frame) < ticks.size() ? ticks[size_t(frame)] : -1;
}

uint32_t ModelViewport::clip_frame_count() const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.frame_count : 0u;
}

uint32_t ModelViewport::clip_fps() const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.fps : 0u;
}

int32_t ModelViewport::clip_ticks(const PreviewClock &clock) const {
	const int32_t ticks = clock.ticks();
	const int32_t length = clip_length_ticks();
	if (!options_.repeat || length <= 0 || clip_loops()) return ticks;
	return ticks % (length + kClipRepeatHoldTicks);
}

double ModelViewport::clip_frame(const PreviewClock &clock) const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.playback().frame_at(clip_ticks(clock)) : 0.0;
}

int32_t ModelViewport::tick_of_step(int32_t ticks, int by) const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	if (!clip || by == 0) return ticks;
	const anim::ClipTimeline &timeline = clip->clip.playback();
	const std::vector<int32_t> first = timeline.first_ticks();
	const int32_t length = timeline.length_ticks();
	// The frame shown, within one pass of the clip.
	const int32_t within = length > 0 && clip->clip.loops() ? ticks % length : std::min(ticks, std::max(length, 0));
	const int frame = int(timeline.frame_index_at(within));
	// The next frame along that the clock runs on (a fast clip's clock steps over some).
	for (int f = frame + (by > 0 ? 1 : -1); f >= 0 && size_t(f) < first.size(); f += by > 0 ? 1 : -1)
		if (first[size_t(f)] >= 0) return first[size_t(f)];
	return by > 0 ? within : 0;
}

std::vector<PreviewJoint> ModelViewport::joints(const PreviewClock &clock) const {
	if (!animating_ || !skeleton_ || clip_key_.empty()) return {};
	return preview_posed_joints(*skeleton_, clip_key_, clip_variant_, clip_ticks(clock));
}

ViewportAction ModelViewport::stop_(ModelViewStatus reason, const std::string &detail, bool failed) {
	reason_ = reason;
	detail_ = detail;
	model_.reset();
	read_.reset();
	read_row_.reset();
	drawn_hashed_ = false;
	scene_ = false;
	shown_none();
	return failed ? picture_.failed() : picture_.stop();
}

void ModelViewport::reset_animation_() {
	animating_ = false;
	rig_ = PreviewRig();
	model_file_.clear();
	model_read_.clear();
	model_failed_ = false;
	unwritable_.clear();
	if (skeleton_) ++skeleton_serial_;
	skeleton_.reset();
	rig_read_.clear();
	clip_key_.clear();
	clip_file_.clear();
	clip_variant_ = 0;
	clip_events_.clear();
	clip_note_.clear();
}

ViewportAction ModelViewport::follow_(const ViewportInput &input, PreviewClock &clock) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) {
		reset_animation_();
		return stop_(ModelViewStatus::NoProject, std::string(), false);
	}
	const Document *document = input.document ? records_of(*input.document) : nullptr;
	if (const auto *model = dynamic_cast<const ModelDocument *>(document)) return follow_model_(input, *model);
	if (document && (is_animation_kind(document->kind()) || is_animation_map_kind(document->kind())))
		return follow_animation_(input, *document, clock);
	reset_animation_();
	return stop_(ModelViewStatus::NoModel, std::string(), false);
}

ViewportAction ModelViewport::follow_model_(const ViewportInput &input, const ModelDocument &document) {
	if (animating_) {
		// From an animation to a model document: the rig's model is not this one.
		reset_animation_();
		stop_(ModelViewStatus::NoModel, std::string(), false);
	}
	const FileSource &files = *input.view.findings.assets;
	const uint64_t generation = input.view.findings.assets->generation();
	const PreviewFollow::Key key;
	// A change only the overlays show: the held model patched, the scene standing (S13 V8).
	const bool overlays = input.change == ChangeClass::Changed && overlays_alone_(input, document);
	switch (picture_.follow(key, input.change != ChangeClass::None && !overlays, files, generation)) {
	case PreviewFollow::Found::Same: {
		// A change set naming nothing patches nothing: Keep, as for no change.
		const bool patched = overlays && patch_user_points_(document);
		if (overlays) shown(document);
		if (!patched && (!options_moved_ || picture_.is_failed())) return ViewportAction::Keep;
		options_moved_ = false;
		return ViewportAction::Update;
	}
	case PreviewFollow::Found::Files:
		// A texture the device read moved: the scene is built again over the model read before.
		if (overlays) {
			patch_user_points_(document);
			shown(document);
		}
		options_moved_ = false;
		return picture_.built(FileStamps());
	case PreviewFollow::Found::Anew: break;
	}
	// A texture the device read that moved in the same follow as the document builds the scene again.
	const bool textures = picture_.files_moved(files, generation);
	picture_.show(key, generation);
	++reads_;
	const SerializeResult written = document.serialize();
	if (!written.ok()) return stop_(ModelViewStatus::Unserializable, written.issues.front().message, true);
	assets::Model model =
			assets::parse_model(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size());
	if (!model) return stop_(ModelViewStatus::Unreadable, std::string(), true);
	// A change set that reached here names something the scene draws; what the document cannot say
	// builds again only when the drawn model moved, the user points aside: the model read now written
	// once, its hash against the held model's.
	uint64_t drawn = 0;
	const bool hashed = input.change == ChangeClass::Unknown && drawn_hash(*model, drawn);
	const bool rebuild =
			!hashed || !scene_ || !model_ || textures || !held_drawn_hash_() || drawn != drawn_hash_;
	read_ = model;
	model_ = std::move(model);
	read_row_ = model_row_of(document);
	drawn_hash_ = drawn;
	drawn_hashed_ = hashed;
	reason_ = ModelViewStatus::Ready;
	detail_.clear();
	shown(document);
	if (framed_ != path()) {
		framed_ = path();
		frame_();
		state_moved();
	}
	options_moved_ = false;
	// A drawn model the same as before, the user points aside: the overlays alone show the change.
	if (!rebuild) return ViewportAction::Update;
	scene_ = true;
	return picture_.built(FileStamps());
}

bool ModelViewport::overlays_alone_(const ViewportInput &input, const ModelDocument &document) const {
	const auto *rows = input.changes ? std::get_if<RowChanges>(input.changes) : nullptr;
	if (!rows || rows->reshapes() || rows->file_state || !model_ || !read_ || !read_row_ || picture_.is_failed())
		return false;
	const std::shared_ptr<const Node> now = model_row_of(document);
	if (!now || rows->changed.size() > 1 || (rows->changed.size() == 1 && rows->changed.front() != now->id))
		return false;
	return now == read_row_ ||
			alike_but_user_points(static_cast<const ModelRow &>(*read_row_), static_cast<const ModelRow &>(*now));
}

bool ModelViewport::patch_user_points_(const ModelDocument &document) {
	const std::shared_ptr<const Node> now = model_row_of(document);
	if (!now || now == read_row_) return false;
	read_row_ = now;
	model_ = with_user_points(read_, static_cast<const ModelRow &>(*now).user_points);
	++patches_;
	return true;
}

bool ModelViewport::held_drawn_hash_() {
	if (!drawn_hashed_ && read_) drawn_hashed_ = drawn_hash(*read_, drawn_hash_);
	return drawn_hashed_;
}

// A clip or a table plays on its rig's model (as the project's files hold it): the model is read
// again when its stamp moves, the rig when the rig or a file it read moves, and the clip follows the
// selection.
ViewportAction ModelViewport::follow_animation_(const ViewportInput &input, const Document &document,
		PreviewClock &clock) {
	const SessionView &view = input.view;
	if (!animating_) {
		// From a model document: its drawn model is not the rig's.
		model_.reset();
		read_.reset();
		read_row_.reset();
		drawn_hashed_ = false;
		scene_ = false;
		picture_.stop();
	}
	animating_ = true;
	const bool document_moved = input.change != ChangeClass::None;
	// A clip or a table that cannot be written as it stands: the project's files keep the saved one
	// (ProjectAssetSource), whose rows and tokens are not the ones the selection names, so nothing
	// plays until it can be written.
	if (document_moved) {
		++reads_;
		const SerializeResult written = document.serialize();
		unwritable_ = written.ok() ? std::string() : written.issues.front().message;
	}
	if (!unwritable_.empty()) {
		if (skeleton_) ++skeleton_serial_;
		skeleton_.reset();
		rig_read_.clear();
		clip_key_.clear();
		clip_file_.clear();
		clip_variant_ = 0;
		clip_events_.clear();
		clip_note_.clear();
		return stop_(ModelViewStatus::Unserializable, unwritable_, true);
	}
	const std::string file = file_of(document.path());
	PreviewRig rig = view.findings.graph
			? resolve_preview_rig(*view.findings.graph, *view.project.scan, file, document.kind(), options_.rig_model)
			: PreviewRig();
	if (rig.model.empty()) {
		rig_ = rig;
		model_file_.clear();
		model_read_.clear();
		if (skeleton_) ++skeleton_serial_;
		skeleton_.reset();
		clip_key_.clear();
		clip_events_.clear();
		clip_note_.clear();
		// Until the first validation has read the project's references, the pairing item may not be read
		// yet: said so, not that none pairs it (a validation after an edit reads with the graph built).
		return stop_(!view.activity.validation.read ? ModelViewStatus::Reading : ModelViewStatus::NoRig, file, false);
	}
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const bool files_moved = generation != generation_;
	generation_ = generation;
	bool rebuild = false;
	// Another model, its file changed, or none read yet (one that failed is not read again: it stays
	// failed until its file changes, the failure latch).
	const bool model_moved = rig.model != model_file_ || model_read_.moved(files) || (!model_ && !model_failed_);
	if (model_failed_ && !model_moved) return ViewportAction::Keep;
	if (model_moved) {
		++rig_model_reads_;
		std::vector<uint8_t> bytes;
		FileStamps read;
		read.note(rig.model, files.stamp(rig.model));
		assets::Model model = files.read(rig.model, bytes) ? assets::parse_model(bytes.data(), bytes.size())
		                                                  : assets::Model();
		model_file_ = rig.model;
		model_read_ = read;
		if (!model) {
			rig_ = rig;
			model_failed_ = true;
			return stop_(ModelViewStatus::Unreadable, rig.model, true);
		}
		model_failed_ = false;
		model_ = std::move(model);
		scene_ = false;
		rebuild = true;
	} else if (files_moved && picture_.files().moved(files)) {
		rebuild = true; // a texture the device read
	}
	auto stamped = std::make_shared<StampedFiles>(view.findings.assets);
	const PreviewRigFiles rig_files(stamped);
	const bool rig_moved = rebuild || !same_rig(rig, rig_) || (files_moved && rig_read_.moved(files));
	if (rig_moved) {
		skeleton_ = load_preview_rig(rig, *model_, rig_files);
		++skeleton_serial_;
	}
	rig_ = rig;
	// The clip the selection plays (the selection is the active document's), or what the game plays
	// in its place, with why.
	std::string key = clip_key_;
	int variant = clip_variant_;
	if (view.documents.active == document.path()) {
		PreviewClipChoice choice;
		if (skeleton_) choice = preview_clip_choice(document, view.documents.selection.primary, rig_, *skeleton_);
		key = choice.key;
		variant = choice.variant;
		clip_note_ = choice.note;
	}
	if (key != clip_key_ || variant != clip_variant_ || rig_moved || document_moved) {
		if (key != clip_key_ || variant != clip_variant_) clock.seek_ticks(0);
		clip_key_ = key;
		clip_variant_ = variant;
		const anim::SkeletalClips::ClipSource *source =
				skeleton_ && !key.empty() ? skeleton_->find_clip_source(key, variant) : nullptr;
		clip_file_ = source ? source->file : std::string();
		clip_events_.clear();
		const auto clip = clip_file_.empty() ? nullptr : rig_files.bone_animation(clip_file_);
		if (clip) clip_events_ = preview_clip_events(*skeleton_, clip_key_, clip_variant_, *clip);
	}
	if (rig_moved) rig_read_ = stamped->stamps();
	// An event selected in the clip seeks the clock to the tick the clip first samples it, and
	// holds it there (as a scrub does).
	const auto *clip_document = dynamic_cast<const AnimationDocument *>(&document);
	const NodeAddress &primary = view.documents.selection.primary;
	if (clip_document && view.documents.active == document.path() &&
			primary.kind == node_kind(AnimationKind::Event) && primary.child && primary.child != sought_event_) {
		sought_event_ = primary.child;
		if (const Node *row = clip_document->row(primary.row)) {
			const std::vector<NodeId> &events = row->collections[1];
			const auto found = std::find(events.begin(), events.end(), primary.child);
			const int32_t tick = found == events.end() ? -1 : tick_of_frame(int(found - events.begin()));
			if (tick >= 0) {
				clock.seek_ticks(tick);
				clock.set_playing(false);
			}
		}
	}
	reason_ = ModelViewStatus::Ready;
	detail_ = skeleton_ ? std::string() : "The rig does not load: " + (rig_.table.empty() ? rig_.clip : rig_.table) + ".";
	shown(document);
	if (framed_ != model_file_) {
		framed_ = model_file_;
		frame_();
		state_moved();
	}
	const bool options = options_moved_;
	options_moved_ = false;
	if (rebuild || !scene_) {
		// The scene of the rig's model, built.
		scene_ = true;
		picture_.show(PreviewFollow::Key(), generation);
		return picture_.built(FileStamps());
	}
	return rig_moved || options || document_moved ? ViewportAction::Update : ViewportAction::Keep;
}

bool ModelViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera" || member == "frame";
}

int32_t ModelViewport::tick_of_frame_shown(int frame) const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	if (!clip || frame < 0) return -1;
	const std::vector<int32_t> first = clip->clip.playback().first_ticks();
	for (size_t f = size_t(frame); f < first.size(); ++f)
		if (first[f] >= 0) return first[f];
	// Past the last frame the clock runs on: a one-shot holds its end, a loop its last frame shown.
	for (size_t f = std::min(size_t(frame), first.size()); f-- > 0;)
		if (first[f] >= 0) return clip->clip.loops() ? first[f] : clip->clip.playback().length_ticks();
	return -1;
}

bool ModelViewport::check_(const io::JsonValue &json, std::string &error) const {
	ModelViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, options, error))
		return false;
	OrbitCamera camera = camera_;
	bool frame = false;
	if (const JsonValue *member = json.get("camera"); member && !read_camera(*member, camera, frame, error))
		return false;
	// A clip's frame (S17): the clock held on the first tick the clip shows it, the wire's form of a
	// scrub or a step of the timeline.
	if (const JsonValue *member = json.get("frame")) {
		int64_t number = 0;
		if (!json_whole_in(*member, 0.0, 1000000.0, number)) {
			error = "frame is a clip's frame, a whole number from 0.";
			return false;
		}
		if (clip_key_.empty() || tick_of_frame_shown(int(number)) < 0) {
			error = "frame: no clip plays in this viewport.";
			return false;
		}
	}
	return true;
}

void ModelViewport::apply_(const io::JsonValue &json, PreviewClock &clock) {
	if (const JsonValue *member = json.get("frame")) {
		int64_t number = 0;
		if (json_whole_in(*member, 0.0, 1000000.0, number)) {
			clock.seek_ticks(tick_of_frame_shown(int(number)));
			clock.set_playing(false);
		}
	}
	std::string error;
	ModelViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && read_options(*member, options, error) &&
			options != options_) {
		options_ = options;
		options_moved_ = true;
	}
	bool frame = false;
	if (const JsonValue *member = json.get("camera")) read_camera(*member, camera_, frame, error);
	if (frame) frame_();
}

void ModelViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
}

NodeAddress ModelViewport::record_of(const ViewportInput &input, const ModelOverlay &overlay) const {
	const auto *document = dynamic_cast<const ModelDocument *>(input.document ? records_of(*input.document) : nullptr);
	return document && current(input) ? model_overlay_record(*document, overlay, lod()) : NodeAddress();
}

ModelCanvasFrame ModelViewport::canvas_frame(const ViewportContext &context) const {
	ModelCanvasFrame frame;
	const ViewportInput &input = context.input;
	frame.model = this;
	frame.clock = &input.clock;
	frame.snap = context.snap;
	frame.editable = context.editable();
	frame.document = dynamic_cast<const ModelDocument *>(input.document ? records_of(*input.document) : nullptr);
	frame.current = frame.document && current(input);
	frame.overlays = overlays(input.clock);
	// A clip playing (S17): its bones, and the clip's document while it is the active one (a joint's
	// bone is its bone record of that index), its selected bone ringed.
	if (animating_ && options_.bones) {
		frame.joints = joints(input.clock);
		const auto *clip = dynamic_cast<const AnimationDocument *>(input.document ? records_of(*input.document) : nullptr);
		if (clip && input.view.documents.active == clip->path() && clip->clip()) {
			frame.clip_document = clip;
			const NodeAddress &primary = input.view.documents.selection.primary;
			const std::vector<NodeId> &bones = clip->clip()->collections[0];
			const auto found = primary.kind == node_kind(AnimationKind::Bone)
			                           ? std::find(bones.begin(), bones.end(), primary.child)
			                           : bones.end();
			if (found != bones.end()) frame.selected_bone = int(found - bones.begin());
		}
	}
	// The collision shown (S17), and the selected record's shape whatever its layer.
	const bool active = frame.current && input.view.documents.active == frame.document->path();
	const ModelCollisionPick picked = selected_collision(input);
	frame.collision = collision(input.clock, picked);
	for (size_t i = 0; picked.valid() && i < frame.collision->size(); ++i)
		if ((*frame.collision)[i].kind == picked.kind && (*frame.collision)[i].index == picked.index)
			frame.selected_collision = int(i);
	if (!active) return frame;
	const Selection &selection = input.view.documents.selection;
	model_overlay_of(*frame.document, selection.primary, frame.selected_kind, frame.selected);
	// The other selected records' markers (a place's drag moves them as far).
	for (const NodeAddress &record : selection.records) {
		ModelOverlayKind kind = ModelOverlayKind::UserPoint;
		int index = -1;
		if (record == selection.primary || !model_overlay_of(*frame.document, record, kind, index)) continue;
		for (const ModelOverlay &overlay : frame.overlays)
			if (overlay.kind == kind && overlay.index == index) frame.others.push_back(overlay);
	}
	return frame;
}

ViewportHit ModelViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	if (status() != ViewportStatus::Ready || !model_) return out;
	const std::vector<ModelOverlay> marks = overlays(context.input.clock);
	out.index = pick_model_overlay(marks, camera_, context.width, context.height, x, y);
	if (out.index < 0) {
		// No marker there: the collision shape the pixel is on (S17), the picture's own (the selected
		// record's shape among them whatever its layer).
		const ModelCollisionShapesPtr shapes = collision(context.input.clock, selected_collision(context.input));
		const int at = pick_model_collision(*shapes, camera_, context.width, context.height, x, y);
		if (at < 0) return out;
		const ModelCollisionShape &shape = (*shapes)[size_t(at)];
		const auto *document = dynamic_cast<const ModelDocument *>(
				context.input.document ? records_of(*context.input.document) : nullptr);
		out.index = shape.index;
		out.id = document && out.current ? model_collision_record(*document, shape).child : 0;
		out.name = shape.name;
		out.kind = model_collision_kind_token(shape.kind);
		return out;
	}
	const ModelOverlay &hit = marks[size_t(out.index)];
	out.index = hit.index;
	out.id = record_of(context.input, hit).child;
	out.name = hit.name;
	out.kind = model_overlay_kind_token(hit.kind);
	return out;
}

bool ModelViewport::dragged_marker_(const ViewportContext &context, NodeId id, const std::string &token,
		ModelHandle &handle, ModelOverlay &marker, std::string &error) const {
	if (!model_handle_from_token(token.c_str(), handle)) {
		error = "Unknown handle \"" + token + "\" (place, axis).";
		return false;
	}
	const auto *document = dynamic_cast<const ModelDocument *>(
			context.input.document ? records_of(*context.input.document) : nullptr);
	if (!document || !current(context.input)) {
		const std::string why = message();
		error = "The viewport shows no picture of the model as it is now" + (why.empty() ? std::string(".") : ": " + why);
		return false;
	}
	ModelOverlayKind kind;
	int index = -1;
	if (model_overlay_of(*document, document->address_of(id), kind, index))
		for (const ModelOverlay &overlay : overlays(context.input.clock))
			if (overlay.kind == kind && overlay.index == index) {
				marker = overlay;
				return true;
			}
	error = "Record " + std::to_string(id) + " is no marker the viewport shows.";
	return false;
}

bool ModelViewport::handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x,
		float &y, std::string &error) const {
	ModelHandle held = ModelHandle::Place;
	ModelOverlay marker;
	if (!dragged_marker_(context, id, handle, held, marker, error)) return false;
	const PreviewVec3 through = held == ModelHandle::Axis ? axis_tip(marker) : marker.at;
	if (!camera_.project(through, context.width, context.height, x, y)) {
		error = "The marker's handle is not on the picture: drag it to a point of the picture (to).";
		return false;
	}
	return true;
}

bool ModelViewport::drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
		std::string &error) const {
	ModelHandle handle = ModelHandle::Place;
	ModelOverlay marker;
	if (!dragged_marker_(context, drag.id, drag.handle, handle, marker, error)) return false;
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	if (!(drag.snap >= 0.0f)) {
		error = "The snap is 0 or more.";
		return false;
	}
	const auto *document = static_cast<const ModelDocument *>(records_of(*context.input.document));
	// A step that moves nothing plans no batch (the picture's point and back would move the marker
	// by what the round trip loses); the gesture its sample names ends with it all the same.
	if (drag.by && drag.x == 0.0f && drag.y == 0.0f) {
		if (drag.end && drag.gesture) out.request(request::end_edit(document->path()));
		return true;
	}
	const NodeAddress dragged = document->address_of(drag.id);
	ModelOverlayKind kind = marker.kind;
	const int index = marker.index;
	const std::vector<ModelOverlay> marks = overlays(context.input.clock);
	// A place's drag of a selected marker moves the other selected markers as far, as the canvas's
	// drag of the primary's does (canvas_frame's others).
	std::vector<ModelOverlay> others;
	const DocumentsView &documents = context.input.view.documents;
	if (handle == ModelHandle::Place && documents.active == document->path() && documents.selection.holds(dragged)) {
		for (const NodeAddress &record : documents.selection.records) {
			ModelOverlayKind other_kind = ModelOverlayKind::UserPoint;
			int other_index = -1;
			if (record == dragged || !model_overlay_of(*document, record, other_kind, other_index)) continue;
			for (const ModelOverlay &overlay : marks)
				if (overlay.kind == other_kind && overlay.index == other_index) others.push_back(overlay);
		}
	}
	for (const ModelOverlay &overlay : marks) {
		if (overlay.kind != kind || overlay.index != index) continue;
		// By (dx, dy) pixels: to the pixel the picture shows the handle on now, moved as far.
		float x = drag.x, y = drag.y;
		if (drag.by) {
			const PreviewVec3 through = handle == ModelHandle::Axis ? axis_tip(overlay) : overlay.at;
			float px = 0.0f, py = 0.0f;
			if (!camera_.project(through, context.width, context.height, px, py)) {
				error = "The marker's handle is not on the picture: drag it to a point of the picture (to).";
				return false;
			}
			x = px + drag.x;
			y = py + drag.y;
		}
		std::vector<Edit> edits;
		if (!handle_edits(*document, overlay, handle, x, y, context.width, context.height, drag.snap,
					drag.gesture ? drag.gesture : next_edit_gesture(), context.input.clock, edits, &others)) {
			error = "The marker has no such handle (a pivot is geometry; an omni light has no axis).";
			return false;
		}
		// The batch, then the gesture's end where there is one to end (as the menu's drag).
		const bool planned = !edits.empty();
		if (planned) out.request(request::edit_record(document->path(), std::move(edits)));
		if (drag.end && (planned || drag.gesture)) out.request(request::end_edit(document->path()));
		return true;
	}
	error = "Record " + std::to_string(drag.id) + " is no marker the viewport shows.";
	return false;
}

bool ModelViewport::command(const ViewportContext &context, const std::string &name,
		const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const {
	if (name != "frame") {
		error = "Unknown model command \"" + name + "\" (frame).";
		return false;
	}
	if (!model_) {
		error = "The viewport shows no model.";
		return false;
	}
	// The marker of the first record named (one that is no marker refused, as a menu's command refuses
	// a record that is no window), else the whole model.
	if (ids.empty()) {
		out.request(request::set_viewport(path(), model_camera_change(framed(context.width, context.height))));
		return true;
	}
	const auto *document = dynamic_cast<const ModelDocument *>(
			context.input.document ? records_of(*context.input.document) : nullptr);
	ModelOverlayKind kind;
	int index = -1;
	if (document && model_overlay_of(*document, document->address_of(ids.front()), kind, index))
		for (const ModelOverlay &overlay : overlays(context.input.clock))
			if (overlay.kind == kind && overlay.index == index) {
				out.request(request::set_viewport(path(),
						model_camera_change(framed_on(overlay, context.width, context.height))));
				return true;
			}
	// A collision record (S17): the camera on its shape, whatever its layer.
	ModelCollisionPick picked;
	if (document && model_collision_of(*document, document->address_of(ids.front()), picked))
		for (const ModelCollisionShape &shape : *collision(context.input.clock, picked))
			if (shape.kind == picked.kind && shape.index == picked.index) {
				PreviewVec3 center;
				float radius = 0.0f;
				model_collision_bounds(shape, center, radius);
				OrbitCamera camera = camera_;
				camera.frame(center, radius, context.width, context.height);
				out.request(request::set_viewport(path(), model_camera_change(camera)));
				return true;
			}
	error = "Record " + std::to_string(ids.front()) + " is no marker or collision shape the viewport shows.";
	return false;
}

io::JsonValue ModelViewport::options_json() const {
	return model_options_to_json(options_);
}

io::JsonValue ModelViewport::camera_json() const {
	JsonValue view = JsonValue::make_object();
	view.set("target", vec3(camera_.target));
	view.set("yaw", json_number(camera_.yaw));
	view.set("pitch", json_number(camera_.pitch));
	view.set("distance", json_number(camera_.distance));
	view.set("fov", json_number(OrbitCamera::fov_horizontal_degrees()));
	return view;
}

io::JsonValue ModelViewport::body_json(const ViewportInput &input) const {
	JsonValue body = JsonValue::make_object();
	JsonValue lod_json = JsonValue::make_object();
	JsonValue registers = JsonValue::make_array();
	if (status() == ViewportStatus::Ready && model_) {
		const threedi::Threedi3di3 &shown = *model_;
		int32_t projected = 0;
		lod_json.set("shown", json_number(lod()));
		lod_json.set("auto", json_number(auto_lod(&projected)));
		lod_json.set("count", json_number(double(shown.lod_count)));
		lod_json.set("projected_px", json_number(projected / 65536.0));
		JsonValue thresholds = JsonValue::make_array();
		for (size_t i = 0; i < shown.lod_count; ++i) thresholds.push(json_number(shown.lods[i].lod_threshold));
		lod_json.set("thresholds", std::move(thresholds));
		PreviewVec3 center;
		float radius = 0.0f;
		model_preview_sphere(shown, center, radius);
		JsonValue sphere = JsonValue::make_object();
		sphere.set("center", vec3(center));
		sphere.set("radius", json_number(radius));
		body.set("sphere", std::move(sphere));
		// The collision layers (S17): each with its words, colour, count and whether it is shown, and the
		// legend of what is drawn.
		JsonValue layers = JsonValue::make_array();
		for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount); ++l) {
			const ModelCollisionLayerRow &layer = model_collision_layer(ModelCollisionLayer(l));
			JsonValue row = JsonValue::make_object();
			row.set("token", json_string(layer.token));
			row.set("label", json_string(layer.label));
			row.set("words", json_string(layer.words));
			row.set("color", json_string(rgb_hex(layer.rgb)));
			row.set("count", json_number(double(model_collision_layer_count(shown, ModelCollisionLayer(l), lod()))));
			row.set("shown", JsonValue::make_bool(model_collision_layer_on(options_.overlays, ModelCollisionLayer(l))));
			layers.push(std::move(row));
		}
		JsonValue legend = JsonValue::make_array();
		for (const ModelCollisionLegendRow &entry : model_collision_legend(*collision(input.clock, selected_collision(input)))) {
			JsonValue row = JsonValue::make_object();
			row.set("color", json_string(rgb_hex(entry.rgb)));
			row.set("words", json_string(entry.words));
			legend.push(std::move(row));
		}
		JsonValue collision_json = JsonValue::make_object();
		collision_json.set("layers", std::move(layers));
		collision_json.set("legend", std::move(legend));
		body.set("collision", std::move(collision_json));
		for (uint32_t i = 0; i < shown.ctrl.count; ++i) {
			const std::string name = strutil::fixed_string(shown.ctrl.registers[i].name, sizeof(shown.ctrl.registers[i].name));
			JsonValue row = JsonValue::make_object();
			row.set("name", json_string(name));
			const auto value = options_.ctrl.find(name);
			row.set("value", json_number(value == options_.ctrl.end() ? 0.0 : double(value->second)));
			registers.push(std::move(row));
		}
	}
	body.set("lod", std::move(lod_json));
	body.set("registers", std::move(registers));
	const AssetGraph *graph = input.view.findings.graph.get();
	const AssetScan *scan = input.view.project.scan.get();
	if (!animating_) {
		body.set("animation", JsonValue::make_null());
		// The maps this model plays (S17): each record pairing it with one (preview_model_fields), and how.
		JsonValue maps = JsonValue::make_array();
		if (graph && scan && input.document)
			for (const ModelAnimation &played : model_animations(*graph, *scan, input.document->path())) {
				JsonValue row = JsonValue::make_object();
				row.set("map", json_string(played.map));
				row.set("record", json_string(played.record));
				row.set("file", json_string(played.file));
				row.set("via", json_string(played.via));
				maps.push(std::move(row));
			}
		body.set("animations", std::move(maps));
		return body;
	}
	JsonValue animation = JsonValue::make_object();
	animation.set("table", json_string(rig_.table));
	animation.set("clip", json_string(rig_.clip));
	animation.set("model", json_string(rig_.model));
	animation.set("source", json_string(rig_.source));
	animation.set("rig", JsonValue::make_bool(skeleton_ != nullptr));
	animation.set("key", json_string(clip_key_));
	animation.set("variant", json_number(clip_variant_));
	animation.set("file", json_string(clip_file_));
	animation.set("note", json_string(clip_note_));
	animation.set("ticks", json_number(input.clock.ticks()));
	// The clip's own clock (S17): the tick it plays (a repeated one-shot's taken again from 0), its
	// frame, and both in seconds as the game's ticks pass.
	const int32_t played = clip_ticks(input.clock);
	const int32_t length = clip_length_ticks();
	animation.set("clip_ticks", json_number(played));
	animation.set("frame", json_number(clip_frame(input.clock)));
	animation.set("frame_count", json_number(clip_frame_count()));
	animation.set("fps", json_number(clip_fps()));
	animation.set("seconds", json_number(played / io::kTickHz));
	animation.set("length_ticks", json_number(length));
	animation.set("length_seconds", json_number(length > 0 ? length / io::kTickHz : 0.0));
	animation.set("loops", JsonValue::make_bool(clip_loops()));
	animation.set("repeat", JsonValue::make_bool(options_.repeat));
	JsonValue events = JsonValue::make_array();
	for (const PreviewClipEvent &event : clip_events_) {
		JsonValue row = JsonValue::make_object();
		row.set("frame", json_number(event.frame));
		row.set("tick", json_number(event.tick));
		row.set("trigger", json_number(double(event.trigger)));
		row.set("words", json_string(animation_trigger_words(event.trigger)));
		events.push(std::move(row));
	}
	animation.set("events", std::move(events));
	// The rig's bones as the clip poses them now: each by its name, its parent, where its joint
	// stands and its pixel on the picture.
	JsonValue bones = JsonValue::make_array();
	for (const PreviewJoint &joint : joints(input.clock)) {
		JsonValue row = JsonValue::make_object();
		row.set("bone", json_number(joint.bone));
		row.set("name", json_string(joint.name));
		row.set("parent", json_number(joint.parent));
		row.set("position", vec3(joint.at));
		float x = 0.0f, y = 0.0f;
		if (camera_.project(joint.at, size().width, size().height, x, y)) {
			JsonValue screen = JsonValue::make_array();
			screen.push(json_number(x));
			screen.push(json_number(y));
			row.set("screen", std::move(screen));
		} else {
			row.set("screen", JsonValue());
		}
		bones.push(std::move(row));
	}
	animation.set("bones", std::move(bones));
	// Who plays it and what for (S17): a map's players (the items and weapons naming it, each item's
	// model) and what the game does with the selected row; a clip's uses (the maps and slots naming it).
	const Document *document = input.document ? records_of(*input.document) : nullptr;
	JsonValue players = JsonValue::make_array();
	JsonValue uses = JsonValue::make_array();
	JsonValue notes = JsonValue::make_array();
	if (const auto *map = dynamic_cast<const AnimationMapDocument *>(document)) {
		if (graph && scan)
			for (const MapPlayer &player : map_players(*graph, *scan, map->path())) {
				JsonValue row = JsonValue::make_object();
				row.set("record", json_string(player.record));
				row.set("file", json_string(player.file));
				row.set("model", json_string(player.model));
				row.set("enemy_model", json_string(player.enemy_model));
				row.set("first_person", JsonValue::make_bool(player.first_person));
				players.push(std::move(row));
			}
		if (input.view.documents.active == map->path())
			for (const std::string &note : map_row_notes(*map, input.view.documents.selection.primary,
			                                             scan ? project_clip_loads(*scan) : ClipLoads()))
				notes.push(json_string(note));
	} else if (document && graph && scan) {
		for (const ClipUse &use : clip_uses(*graph, *scan, document->path())) {
			JsonValue row = JsonValue::make_object();
			row.set("map", json_string(use.map));
			row.set("key", json_string(use.key));
			row.set("words", json_string(use.words));
			uses.push(std::move(row));
		}
	}
	animation.set("players", std::move(players));
	animation.set("uses", std::move(uses));
	animation.set("notes", std::move(notes));
	body.set("animation", std::move(animation));
	return body;
}

io::JsonValue ModelViewport::items_json(const ViewportInput &input) const {
	JsonValue items = JsonValue::make_array();
	if (status() != ViewportStatus::Ready || !model_) return items;
	for (const ModelOverlay &overlay : overlays(input.clock)) {
		JsonValue row = JsonValue::make_object();
		row.set("kind", json_string(model_overlay_kind_token(overlay.kind)));
		row.set("index", json_number(overlay.index));
		row.set("id", json_number(double(record_of(input, overlay).child)));
		row.set("name", json_string(overlay.name));
		row.set("part", json_number(overlay.part));
		row.set("position", vec3(overlay.at));
		float x = 0.0f, y = 0.0f;
		if (camera_.project(overlay.at, size().width, size().height, x, y)) {
			JsonValue screen = JsonValue::make_array();
			screen.push(json_number(x));
			screen.push(json_number(y));
			row.set("screen", std::move(screen));
		} else {
			row.set("screen", JsonValue());
		}
		if (overlay.has_direction) row.set("direction", vec3(overlay.direction));
		if (overlay.kind == ModelOverlayKind::Light) {
			row.set("radius", json_number(overlay.radius));
			row.set("cone", json_number(overlay.cone));
			row.set("color", json_string(rgb_hex(overlay.color)));
		}
		items.push(std::move(row));
	}
	// The collision shapes the options show (S17): each with its record (0 for a bound the game derives),
	// its words, its colour, and its sphere or its corners in the preview's space, a handle's data.
	const auto *document = dynamic_cast<const ModelDocument *>(input.document ? records_of(*input.document) : nullptr);
	const bool now = document && current(input);
	const ModelCollisionPick picked = selected_collision(input);
	for (const ModelCollisionShape &shape : *collision(input.clock, picked)) {
		JsonValue row = JsonValue::make_object();
		row.set("kind", json_string(model_collision_kind_token(shape.kind)));
		// The selected record's shape, listed whatever its layer as the picture draws it.
		row.set("selected", JsonValue::make_bool(picked.valid() && picked.kind == shape.kind && picked.index == shape.index));
		if (shape.kind == ModelCollisionKind::Section) {
			row.set("person", JsonValue::make_bool(shape.person));
			row.set("breaks", JsonValue::make_bool(shape.breaks));
		}
		row.set("index", json_number(shape.index));
		row.set("id", json_number(now ? double(model_collision_record(*document, shape).child) : 0.0));
		row.set("name", json_string(shape.name));
		row.set("legend", json_string(shape.legend));
		row.set("section", json_number(shape.section));
		row.set("color", json_string(rgb_hex(shape.rgb)));
		if (shape.sphere) {
			row.set("center", vec3(shape.center));
			row.set("radius", json_number(shape.radius));
		}
		// Its corners once each (a face's three, a solid's), in the order first drawn.
		JsonValue points = JsonValue::make_array();
		std::vector<PreviewVec3> seen;
		for (const PreviewVec3 &p : shape.edges.empty() ? shape.triangles : shape.edges) {
			bool again = false;
			for (const PreviewVec3 &q : seen)
				again = again || (std::fabs(p.x - q.x) < 1e-5f && std::fabs(p.y - q.y) < 1e-5f && std::fabs(p.z - q.z) < 1e-5f);
			if (again) continue;
			seen.push_back(p);
			points.push(vec3(p));
		}
		row.set("points", std::move(points));
		// A pixel on it (where a hit takes it, nothing nearer in the way): a sphere's centre, else the
		// middle of its first triangle, else the middle of its bounds.
		PreviewVec3 center;
		float radius = 0.0f;
		model_collision_bounds(shape, center, radius);
		if (shape.sphere) center = shape.center;
		else if (shape.triangles.size() >= 3)
			center = PreviewVec3{(shape.triangles[0].x + shape.triangles[1].x + shape.triangles[2].x) / 3.0f,
			                     (shape.triangles[0].y + shape.triangles[1].y + shape.triangles[2].y) / 3.0f,
			                     (shape.triangles[0].z + shape.triangles[1].z + shape.triangles[2].z) / 3.0f};
		float x = 0.0f, y = 0.0f;
		if (camera_.project(center, size().width, size().height, x, y)) {
			JsonValue screen = JsonValue::make_array();
			screen.push(json_number(x));
			screen.push(json_number(y));
			row.set("screen", std::move(screen));
		} else {
			row.set("screen", JsonValue());
		}
		items.push(std::move(row));
	}
	return items;
}

} // namespace opennova::editor
