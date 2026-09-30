#include <editor/preview/model_preview_state.h>

#include <algorithm>
#include <cmath>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

const char *model_preview_status_token(ModelPreviewStatus status) {
	switch (status) {
	case ModelPreviewStatus::NoProject: return "no_project";
	case ModelPreviewStatus::NoModel: return "no_model";
	case ModelPreviewStatus::NoDevice: return "no_device";
	case ModelPreviewStatus::Unserializable: return "unserializable";
	case ModelPreviewStatus::Unreadable: return "unreadable";
	case ModelPreviewStatus::NoRig: return "no_rig";
	case ModelPreviewStatus::Ready: return "ready";
	}
	return "no_project";
}

std::string model_preview_status_message(ModelPreviewStatus status, const std::string &detail) {
	switch (status) {
	case ModelPreviewStatus::NoProject: return "Open a project to preview its models.";
	case ModelPreviewStatus::NoModel: return "Open a model, a clip or an animation table to preview it.";
	case ModelPreviewStatus::NoDevice: return "No preview renderer is attached.";
	case ModelPreviewStatus::Unserializable:
		return "The game could not read this file as it stands" + (detail.empty() ? std::string(".") : ": " + detail);
	case ModelPreviewStatus::Unreadable:
		return detail.empty() ? "The model this writes does not read back." : detail + " does not read as a model.";
	case ModelPreviewStatus::NoRig:
		return "No item pairs " + (detail.empty() ? std::string("this animation") : detail) +
		       " with a model: choose the model it plays on.";
	case ModelPreviewStatus::Ready: return std::string();
	}
	return std::string();
}

bool StampedFiles::read(const std::string &name, std::vector<uint8_t> &out) const {
	const bool found = files_ && files_->read(name, out);
	note_(name, files_ ? files_->stamp(name) : 0);
	return found;
}

uint64_t StampedFiles::stamp(const std::string &name) const {
	const uint64_t stamp = files_ ? files_->stamp(name) : 0;
	note_(name, stamp);
	return stamp;
}

void StampedFiles::note_(const std::string &name, uint64_t stamp) const {
	for (const FileStamp &seen : read_)
		if (strutil::iequals(seen.name, name)) return;
	read_.push_back({name, stamp});
}

bool StampedFiles::moved(const FileSource &files) const {
	for (const FileStamp &seen : read_)
		if (files.stamp(seen.name) != seen.stamp) return true;
	return false;
}

namespace {

// What the device draws, the user points aside: a hash of the model written without them
// (a user point's edit shows in the overlays alone, so it builds nothing).
uint64_t drawn_key(const threedi::Threedi3di3 &model) {
	threedi::Threedi3di3 drawn = model;
	drawn.user_points = nullptr;
	drawn.user_point_count = 0;
	std::vector<uint8_t> bytes;
	if (threedi::threedi_3di3_write_memory(&drawn, bytes) != 0) return 0;
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
}

} // namespace

void ModelPreviewModel::set_options(const ModelPreviewOptions &options) {
	if (options == options_) return;
	options_ = options;
	options_moved_ = true;
}

void ModelPreviewModel::set_device_size(int width, int height) {
	width_ = std::max(width, 1);
	height_ = std::max(height, 1);
}

void ModelPreviewModel::frame() {
	if (!model_) return;
	PreviewVec3 center;
	float radius = 1.0f;
	model_preview_sphere(*model_, center, radius);
	camera_.frame(center, radius, width_, height_);
}

int ModelPreviewModel::auto_lod(int32_t *projected_q16) const {
	if (projected_q16) *projected_q16 = 0;
	return model_ ? model_preview_auto_lod(*model_, camera_, width_, projected_q16) : -1;
}

void ModelPreviewModel::advance(double seconds) {
	if (!options_.playing || !(seconds > 0.0)) return;
	clock_carry_ += seconds * 1000.0;
	const double whole = std::floor(clock_carry_);
	clock_carry_ -= whole;
	clock_ms_ += static_cast<uint32_t>(whole); // wraps as the game's millisecond clock does
	tick_carry_ += seconds * io::kTickHz;
	const double ticks = std::floor(tick_carry_);
	tick_carry_ -= ticks;
	if (ticks_ < INT32_MAX - int32_t(ticks)) ticks_ += int32_t(ticks);
}

void ModelPreviewModel::seek_ticks(int32_t ticks) {
	ticks_ = std::max(ticks, 0);
	tick_carry_ = 0.0;
}

int32_t ModelPreviewModel::clip_length_ticks() const {
	const anim::SkeletalClips::LoadedClip *clip =
	    skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.playback().length_ticks() : -1;
}

bool ModelPreviewModel::clip_loops() const {
	const anim::SkeletalClips::LoadedClip *clip =
	    skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip && clip->clip.loops();
}

int32_t ModelPreviewModel::tick_of_frame(int frame) const {
	const anim::SkeletalClips::LoadedClip *clip =
	    skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	if (!clip || frame < 0) return -1;
	const std::vector<int32_t> ticks = clip->clip.playback().first_ticks();
	return size_t(frame) < ticks.size() ? ticks[size_t(frame)] : -1;
}

double ModelPreviewModel::clip_frame() const {
	const anim::SkeletalClips::LoadedClip *clip =
	    skeleton_ && !clip_key_.empty() ? skeleton_->find_clip_variant(clip_key_, clip_variant_) : nullptr;
	return clip ? clip->clip.playback().frame_at(ticks_) : 0.0;
}

std::vector<ModelOverlay> ModelPreviewModel::overlays() const {
	if (!model_) return {};
	int32_t bus[96];
	model_preview_ctrl_bus(options_.ctrl, bus);
	return model_overlays(*model_, lod(), clock_ms_, bus, options_.overlays);
}

PreviewVec3 ModelPreviewModel::axis_tip(const ModelOverlay &overlay) const {
	const float length = axis_length();
	return PreviewVec3{overlay.at.x + overlay.direction.x * length, overlay.at.y + overlay.direction.y * length,
	                   overlay.at.z + overlay.direction.z * length};
}

bool ModelPreviewModel::handle_edits(const ModelDocument &document, const ModelOverlay &overlay, ModelHandle handle,
                                     float x, float y, float snap, uint64_t gesture, std::vector<Edit> &out) const {
	out.clear();
	if (!model_) return false;
	const PreviewVec3 through = handle == ModelHandle::Axis ? axis_tip(overlay) : overlay.at;
	PreviewVec3 to;
	if (!camera_.on_view_plane(x, y, width_, height_, through, to)) return false;
	int32_t bus[96];
	model_preview_ctrl_bus(options_.ctrl, bus);
	return model_handle_edits(document, *model_, overlay, lod(), clock_ms_, bus, handle, to, snap, gesture, out);
}

int ModelPreviewModel::lod() const {
	if (!model_ || model_->lod_count == 0) return -1;
	if (options_.lod >= 0) return std::min(options_.lod, static_cast<int>(model_->lod_count) - 1);
	return auto_lod();
}

ModelPreviewAction ModelPreviewModel::stop_(ModelPreviewStatus status, const std::string &detail) {
	status_ = status;
	detail_ = detail;
	model_.reset();
	built_files_.reset();
	drawn_key_ = 0;
	const bool was = device_built_;
	device_built_ = false;
	return was ? ModelPreviewAction::Clear : ModelPreviewAction::Keep;
}

void ModelPreviewModel::reset_animation_() {
	animating_ = false;
	rig_ = PreviewRig();
	model_file_.clear();
	model_stamp_ = 0;
	if (skeleton_) ++skeleton_serial_;
	skeleton_.reset();
	rig_read_.reset();
	clip_key_.clear();
	clip_file_.clear();
	clip_variant_ = 0;
	clip_events_.clear();
}

ModelPreviewAction ModelPreviewModel::follow(const SessionView &view) {
	if (!view.project.open || !view.findings.assets) {
		shown_identity_ = 0;
		reset_animation_();
		return stop_(ModelPreviewStatus::NoProject, std::string());
	}
	const Document *document = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == view.documents.previews.model.path) document = open.get();
	if (const auto *model = dynamic_cast<const ModelDocument *>(document)) return follow_model_(view, *model);
	if (document && (is_animation_kind(document->kind()) || is_animation_map_kind(document->kind())))
		return follow_animation_(view, *document);
	shown_identity_ = 0;
	reset_animation_();
	return stop_(ModelPreviewStatus::NoModel, std::string());
}

ModelPreviewAction ModelPreviewModel::follow_model_(const SessionView &view, const ModelDocument &model_document) {
	const ModelDocument *document = &model_document;
	if (animating_) {
		// From an animation to a model document: the rig's model is not this one.
		reset_animation_();
		shown_identity_ = 0;
		device_built_ = false;
	}
	if (shown_identity_ == document->identity() && shown_revision_ == document->revision()) {
		// A model the game could not read stays that way until it changes.
		if (failed_) return ModelPreviewAction::Keep;
		const uint64_t generation = view.findings.assets->generation();
		if (generation != generation_) {
			generation_ = generation;
			if (built_files_ && built_files_->moved(*view.findings.assets)) {
				++builds_;
				built_files_.reset();
				options_moved_ = false;
				return ModelPreviewAction::Rebuild;
			}
		}
		if (!options_moved_) return ModelPreviewAction::Keep;
		options_moved_ = false;
		return ModelPreviewAction::Update;
	}
	shown_identity_ = document->identity();
	shown_revision_ = document->revision();
	shown_path_ = document->path();
	generation_ = view.findings.assets->generation();
	const SerializeResult written = document->serialize();
	if (!written.ok()) {
		failed_ = true;
		return stop_(ModelPreviewStatus::Unserializable, written.issues.front().message);
	}
	assets::Model model =
	    assets::parse_model(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size());
	if (!model) {
		failed_ = true;
		return stop_(ModelPreviewStatus::Unreadable, std::string());
	}
	failed_ = false;
	const uint64_t key = drawn_key(*model);
	const bool rebuild = !device_built_ || key != drawn_key_;
	model_ = std::move(model);
	drawn_key_ = key;
	status_ = ModelPreviewStatus::Ready;
	detail_.clear();
	if (framed_path_ != shown_path_) {
		framed_path_ = shown_path_;
		frame();
	}
	options_moved_ = false;
	if (!rebuild) return ModelPreviewAction::Update;
	device_built_ = true;
	++builds_;
	built_files_.reset();
	return ModelPreviewAction::Rebuild;
}

namespace {

bool same_rig(const PreviewRig &a, const PreviewRig &b) {
	return a.model == b.model && a.table == b.table && a.clip == b.clip && a.source == b.source;
}

std::string file_of(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

} // namespace

// A clip or a table plays on its rig's model (as the project's files hold it): the model is
// read again when its stamp moves, the rig when the rig or a file it read moves, and the
// clip follows the selection.
ModelPreviewAction ModelPreviewModel::follow_animation_(const SessionView &view, const Document &document) {
	if (!animating_) {
		// From a model document: its drawn model is not the rig's.
		shown_identity_ = 0;
		device_built_ = false;
		model_.reset();
		drawn_key_ = 0;
		built_files_.reset();
	}
	animating_ = true;
	const bool document_moved = shown_identity_ != document.identity() || shown_revision_ != document.revision();
	shown_identity_ = document.identity();
	shown_revision_ = document.revision();
	shown_path_ = document.path();
	// A clip or a table that cannot be written as it stands: the project's files keep the
	// saved one (ProjectAssetSource), whose rows and tokens are not the ones the selection
	// names, so nothing plays until it can be written.
	if (document_moved) {
		const SerializeResult written = document.serialize();
		unwritable_ = written.ok() ? std::string() : written.issues.front().message;
	}
	if (!unwritable_.empty()) {
		if (skeleton_) ++skeleton_serial_;
		skeleton_.reset();
		rig_read_.reset();
		clip_key_.clear();
		clip_file_.clear();
		clip_variant_ = 0;
		clip_events_.clear();
		return stop_(ModelPreviewStatus::Unserializable, unwritable_);
	}
	const std::string file = file_of(document.path());
	PreviewRig rig = view.findings.graph ? resolve_preview_rig(*view.findings.graph, *view.project.scan, file, document.kind(), options_.rig_model)
	                            : PreviewRig();
	if (rig.model.empty()) {
		rig_ = rig;
		model_file_.clear();
		if (skeleton_) ++skeleton_serial_;
		skeleton_.reset();
		clip_key_.clear();
		clip_events_.clear();
		return stop_(ModelPreviewStatus::NoRig, file);
	}
	const uint64_t generation = view.findings.assets->generation();
	const bool files_moved = generation != generation_;
	generation_ = generation;
	bool rebuild = false;
	const uint64_t stamp = view.findings.assets->stamp(rig.model);
	// A rig model that does not read stays that way until its file changes.
	if (failed_ && rig.model == model_file_ && stamp == model_stamp_) return ModelPreviewAction::Keep;
	if (!model_ || rig.model != model_file_ || stamp != model_stamp_) {
		std::vector<uint8_t> bytes;
		assets::Model model = view.findings.assets->read(rig.model, bytes) ? assets::parse_model(bytes.data(), bytes.size())
		                                                        : assets::Model();
		if (!model) {
			rig_ = rig;
			model_file_ = rig.model;
			model_stamp_ = stamp;
			failed_ = true;
			return stop_(ModelPreviewStatus::Unreadable, rig.model);
		}
		failed_ = false;
		model_ = std::move(model);
		model_file_ = rig.model;
		model_stamp_ = stamp;
		drawn_key_ = 0;
		rebuild = true;
	} else if (files_moved && built_files_ && built_files_->moved(*view.findings.assets)) {
		rebuild = true;
	}
	auto files = std::make_shared<StampedFiles>(view.findings.assets);
	const PreviewRigFiles rig_files(files);
	const bool rig_moved = rebuild || !same_rig(rig, rig_) || (files_moved && (!rig_read_ || rig_read_->moved(*view.findings.assets)));
	if (rig_moved) {
		skeleton_ = load_preview_rig(rig, *model_, rig_files);
		rig_read_ = files;
		++skeleton_serial_;
	}
	rig_ = rig;
	// The clip the selection plays (the selection is the active document's).
	std::string key = clip_key_;
	int variant = clip_variant_;
	if (view.documents.active == document.path() &&
	    (!skeleton_ || !preview_clip_of(document, view.documents.selection, rig_, *skeleton_, key, variant)))
		key.clear();
	if (key != clip_key_ || variant != clip_variant_ || rig_moved || document_moved) {
		if (key != clip_key_ || variant != clip_variant_) seek_ticks(0);
		clip_key_ = key;
		clip_variant_ = variant;
		const anim::SkeletalClips::ClipSource *source =
		    skeleton_ && !key.empty() ? skeleton_->find_clip_source(key, variant) : nullptr;
		clip_file_ = source ? source->file : std::string();
		clip_events_.clear();
		const auto clip = clip_file_.empty() ? nullptr : rig_files.bone_animation(clip_file_);
		if (clip) clip_events_ = preview_clip_events(*skeleton_, clip_key_, clip_variant_, *clip);
	}
	// An event selected in the clip seeks the clock to the tick the clip first samples it,
	// and holds it there (as a scrub does).
	const auto *clip_document = dynamic_cast<const AnimationDocument *>(&document);
	if (clip_document && view.documents.active == document.path() &&
	    view.documents.selection.kind == node_kind(AnimationKind::Event) && view.documents.selection.child &&
	    view.documents.selection.child != sought_event_) {
		sought_event_ = view.documents.selection.child;
		if (const Node *row = clip_document->row(view.documents.selection.row)) {
			const std::vector<NodeId> &events = row->collections[1];
			const auto found = std::find(events.begin(), events.end(), view.documents.selection.child);
			const int32_t tick = found == events.end() ? -1 : tick_of_frame(int(found - events.begin()));
			if (tick >= 0) {
				seek_ticks(tick);
				options_.playing = false;
			}
		}
	}
	status_ = ModelPreviewStatus::Ready;
	detail_ = skeleton_ ? std::string() : "The rig does not load: " + (rig_.table.empty() ? rig_.clip : rig_.table) + ".";
	if (framed_path_ != model_file_) {
		framed_path_ = model_file_;
		frame();
	}
	const bool options = options_moved_;
	options_moved_ = false;
	if (rebuild || !device_built_) {
		device_built_ = true;
		++builds_;
		built_files_.reset();
		return ModelPreviewAction::Rebuild;
	}
	return rig_moved || options || document_moved ? ModelPreviewAction::Update : ModelPreviewAction::Keep;
}

} // namespace opennova::editor
