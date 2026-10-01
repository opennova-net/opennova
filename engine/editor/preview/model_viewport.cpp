#include <editor/preview/model_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

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

// What the device draws, the user points aside: a hash of the model written without them
// (a user point's edit shows in the overlays alone, so it builds nothing).
uint64_t drawn_key(const threedi::Threedi3di3 &model) {
	threedi::Threedi3di3 drawn = model;
	drawn.user_points = nullptr;
	drawn.user_point_count = 0;
	std::vector<uint8_t> bytes;
	if (threedi::threedi_3di3_write_memory(&drawn, bytes) != 0) return 1;
	// Never 0: 0 is no scene.
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size()) | 1u;
}

bool same_rig(const PreviewRig &a, const PreviewRig &b) {
	return a.model == b.model && a.table == b.table && a.clip == b.clip && a.source == b.source;
}

std::string file_of(const std::string &path) {
	return path.substr(path.find_last_of("/\\") + 1);
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
			if (!value.is_object()) {
				error = "options.overlays is an object {user_points, lights, pivots}.";
				return false;
			}
			for (const io::JsonMember &mark : value.object) {
				if (!mark.value.is_bool()) {
					error = "options.overlays." + mark.key + " is true or false.";
					return false;
				}
				if (mark.key == "user_points") options.overlays.user_points = mark.value.boolean;
				else if (mark.key == "lights") options.overlays.lights = mark.value.boolean;
				else if (mark.key == "pivots") options.overlays.pivots = mark.value.boolean;
				else {
					error = "Unknown overlay \"" + mark.key + "\" (user_points, lights, pivots).";
					return false;
				}
			}
		} else if (key == "rig_model") {
			if (!value.is_string()) {
				error = "options.rig_model is a model's file name (\"\" the paired one).";
				return false;
			}
			options.rig_model = value.string;
		} else {
			error = "Unknown model option \"" + key + "\" (it takes lod, ctrl, overlays, rig_model).";
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
	options.set("overlays", std::move(marks));
	options.set("rig_model", json_string(held.rig_model));
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
	return model_ ? model_preview_auto_lod(*model_, camera_, state_.width, projected_q16) : -1;
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
	return model_overlays(*model_, lod(), clock.ms(), bus, options_.overlays);
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
	camera_ = framed(state_.width, state_.height);
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

double ModelViewport::clip_frame(const PreviewClock &clock) const {
	const anim::SkeletalClips::LoadedClip *clip =
			skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.playback().frame_at(clock.ticks()) : 0.0;
}

ViewportAction ModelViewport::stop_(ModelViewStatus reason, const std::string &detail, bool failed) {
	reason_ = reason;
	detail_ = detail;
	model_.reset();
	drawn_key_ = 0;
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
	switch (picture_.follow(key, input.change != ChangeClass::None, files, generation)) {
	case PreviewFollow::Found::Same: {
		if (!options_moved_ || picture_.is_failed()) return ViewportAction::Keep;
		options_moved_ = false;
		return ViewportAction::Update;
	}
	case PreviewFollow::Found::Files:
		// A texture the device read moved: the scene is built again over the model read before.
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
	const uint64_t key_drawn = drawn_key(*model);
	const bool rebuild = key_drawn != drawn_key_;
	model_ = std::move(model);
	drawn_key_ = key_drawn;
	reason_ = ModelViewStatus::Ready;
	detail_.clear();
	shown(document);
	if (framed_ != path()) {
		framed_ = path();
		frame_();
	}
	options_moved_ = false;
	// A drawn model the same as before, the user points aside: the overlays alone show the change.
	if (!rebuild && !textures) return ViewportAction::Update;
	return picture_.built(FileStamps());
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
		drawn_key_ = 0;
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
		return stop_(ModelViewStatus::NoRig, file, false);
	}
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const bool files_moved = generation != generation_;
	generation_ = generation;
	bool rebuild = false;
	const bool model_moved = !model_ || rig.model != model_file_ || model_read_.moved(files);
	// A rig model that does not read stays that way until its file changes.
	if (model_failed_ && !model_moved && rig.model == model_file_) return ViewportAction::Keep;
	if (model_moved || model_failed_) {
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
		drawn_key_ = 0;
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
	// The clip the selection plays (the selection is the active document's).
	std::string key = clip_key_;
	int variant = clip_variant_;
	if (view.documents.active == document.path() &&
			(!skeleton_ || !preview_clip_of(document, view.documents.selection.primary, rig_, *skeleton_, key, variant)))
		key.clear();
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
	}
	const bool options = options_moved_;
	options_moved_ = false;
	if (rebuild || drawn_key_ == 0) {
		// The scene of the rig's model, built: any key but 0 says the device has one.
		drawn_key_ = 1;
		picture_.show(PreviewFollow::Key(), generation);
		return picture_.built(FileStamps());
	}
	return rig_moved || options || document_moved ? ViewportAction::Update : ViewportAction::Keep;
}

bool ModelViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool ModelViewport::check_(const io::JsonValue &json, std::string &error) const {
	ModelViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, options, error))
		return false;
	OrbitCamera camera = camera_;
	bool frame = false;
	if (const JsonValue *member = json.get("camera"); member && !read_camera(*member, camera, frame, error))
		return false;
	return true;
}

void ModelViewport::apply_(const io::JsonValue &json, PreviewClock &) {
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
	if (!frame.current || input.view.documents.active != frame.document->path()) return frame;
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
	if (out.index < 0) return out;
	const ModelOverlay &hit = marks[size_t(out.index)];
	out.index = hit.index;
	out.id = record_of(context.input, hit).child;
	out.name = hit.name;
	out.kind = model_overlay_kind_token(hit.kind);
	return out;
}

bool ModelViewport::drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
		std::string &error) const {
	ModelHandle handle = ModelHandle::Place;
	if (!model_handle_from_token(drag.handle.c_str(), handle)) {
		error = "Unknown handle \"" + drag.handle + "\" (place, axis).";
		return false;
	}
	const auto *document = dynamic_cast<const ModelDocument *>(
			context.input.document ? records_of(*context.input.document) : nullptr);
	if (!document || !current(context.input)) {
		error = "The viewport does not show the model as it is now.";
		return false;
	}
	if (!context.editable()) {
		error = "The session takes no edit now (the model is blocked, or an operation holds the documents).";
		return false;
	}
	if (!(drag.snap >= 0.0f)) {
		error = "The snap is 0 or more.";
		return false;
	}
	ModelOverlayKind kind;
	int index = -1;
	if (!model_overlay_of(*document, document->address_of(drag.id), kind, index)) {
		error = "Record " + std::to_string(drag.id) + " is no marker the viewport shows.";
		return false;
	}
	for (const ModelOverlay &overlay : overlays(context.input.clock)) {
		if (overlay.kind != kind || overlay.index != index) continue;
		std::vector<Edit> edits;
		if (!handle_edits(*document, overlay, handle, drag.x, drag.y, context.width, context.height, drag.snap,
					next_edit_gesture(), context.input.clock, edits)) {
			error = "The marker has no such handle (a pivot is geometry; an omni light has no axis).";
			return false;
		}
		out.request(request::edit_record(document->path(), std::move(edits)));
		out.request(request::end_edit(document->path()));
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
	// The marker of the first record named, else the whole model.
	const auto *document = dynamic_cast<const ModelDocument *>(
			context.input.document ? records_of(*context.input.document) : nullptr);
	ModelOverlayKind kind;
	int index = -1;
	if (document && !ids.empty() && model_overlay_of(*document, document->address_of(ids.front()), kind, index))
		for (const ModelOverlay &overlay : overlays(context.input.clock))
			if (overlay.kind == kind && overlay.index == index) {
				out.request(request::set_viewport(path(),
						model_camera_change(framed_on(overlay, context.width, context.height))));
				return true;
			}
	out.request(request::set_viewport(path(), model_camera_change(framed(context.width, context.height))));
	return true;
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
	if (!animating_) {
		body.set("animation", JsonValue::make_null());
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
	animation.set("ticks", json_number(input.clock.ticks()));
	animation.set("frame", json_number(clip_frame(input.clock)));
	animation.set("length_ticks", json_number(clip_length_ticks()));
	animation.set("loops", JsonValue::make_bool(clip_loops()));
	JsonValue events = JsonValue::make_array();
	for (const PreviewClipEvent &event : clip_events_) {
		JsonValue row = JsonValue::make_object();
		row.set("frame", json_number(event.frame));
		row.set("tick", json_number(event.tick));
		row.set("trigger", json_number(double(event.trigger)));
		events.push(std::move(row));
	}
	animation.set("events", std::move(events));
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
		if (camera_.project(overlay.at, state_.width, state_.height, x, y)) {
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
			char color[8];
			std::snprintf(color, sizeof(color), "%06X", overlay.color & 0xFFFFFFu);
			row.set("color", json_string(color));
		}
		items.push(std::move(row));
	}
	return items;
}

} // namespace opennova::editor
