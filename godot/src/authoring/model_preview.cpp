#include "authoring/model_preview.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

#include <editor/assets/project_asset_source.h>
#include <editor/session/view/session_view.h>

#include "env/mission_environment.h"
#include "object/skeletal_anim.h"
#include "render/frame_fx.h"
#include "util/string_convert.h"
#include "util/texture_files.h"

namespace godot {

namespace {

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

} // namespace

ModelPreview::ModelPreview(Node &owner) :
		owner_(owner) {
	viewport_ = memnew(SubViewport);
	// It renders on the frames the window draws it (draw() asks for one update), in a
	// world of its own.
	viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	viewport_->set_use_own_world_3d(true);
	viewport_->set_msaa_3d(Viewport::MSAA_4X);
	viewport_->set_size(Vector2i(model_.device_width(), model_.device_height()));
	Node3D *root = memnew(Node3D);
	viewport_->add_child(root);
	// Retail shaders write gamma-domain values and rely on one terminal display decode per
	// 3D view; the environment with no .env lights like the retail noon.
	root->add_child(memnew(DisplayDecode));
	root->add_child(memnew(MissionEnvironment));
	camera_ = memnew(Camera3D);
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root->add_child(camera_);
	object_ = memnew(ObjectModel);
	clock_.instantiate();
	object_->set_panm_clock(clock_);
	root->add_child(object_);
	owner_.add_child(viewport_);
}

ModelPreview::~ModelPreview() {
	if (viewport_ != nullptr && viewport_->is_inside_tree()) {
		viewport_->queue_free();
	}
}

void ModelPreview::refresh(const opennova::editor::SessionView &view) {
	switch (model_.follow(view)) {
	case opennova::editor::ModelPreviewAction::Clear:
		object_->set_object_data(Ref<ObjectData>());
		data_.unref();
		applied_ctrl_.clear();
		applied_lod_ = -1;
		return;
	case opennova::editor::ModelPreviewAction::Rebuild: {
		// The textures read through the project's files, each name and stamp remembered so
		// a changed one builds again.
		auto files = std::make_shared<opennova::editor::StampedFiles>(view.findings.assets);
		data_.instantiate();
		data_->open_from_model(model_.model(), opennova::to_gd(model_.shown_path()),
				std::make_shared<opennova::TextureFiles>(files));
		object_->set_object_data(data_);
		model_.built(files);
		applied_ctrl_.clear();
		applied_lod_ = -1;
		applied_skeleton_ = UINT64_MAX;
		apply_options_();
		break;
	}
	case opennova::editor::ModelPreviewAction::Update:
		apply_options_();
		break;
	case opennova::editor::ModelPreviewAction::Keep:
		break;
	}
	if (applied_skeleton_ != model_.skeleton_serial()) bind_rig_();
	play_clip_();
	place_camera();
}

void ModelPreview::bind_rig_() {
	applied_skeleton_ = model_.skeleton_serial();
	if (!model_.skeleton() || data_.is_null()) {
		object_->set_skeletal_anim(Ref<SkeletalAnim>());
		return;
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	skeletal->set_rig(model_.skeleton());
	object_->set_skeletal_anim(skeletal);
}

void ModelPreview::play_clip_() {
	if (model_.skeleton() && !model_.clip_key().empty()) {
		object_->play_body_clip_at(opennova::to_gd(model_.clip_key()), int(model_.clip_ticks()), model_.clip_variant());
	}
}

// The CTRL registers the options hold (a register let go reads 0 again), then the camera
// and the level.
void ModelPreview::apply_options_() {
	const std::map<std::string, int64_t> &held = model_.options().ctrl;
	object_->begin_ctrl_update();
	for (const auto &entry : applied_ctrl_) {
		if (held.find(entry.first) == held.end()) object_->clear_ctrl_value(opennova::to_gd(entry.first));
	}
	for (const auto &entry : held) {
		object_->set_ctrl_value(opennova::to_gd(entry.first), entry.second);
	}
	object_->end_ctrl_update();
	applied_ctrl_ = held;
	place_camera();
}

void ModelPreview::place_camera() {
	const opennova::editor::OrbitCamera &camera = model_.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	if (model_.status() != opennova::editor::ModelPreviewStatus::Ready) return;
	const int lod = model_.lod();
	if (lod >= 0 && lod != applied_lod_) {
		object_->set_active_lod(lod);
		applied_lod_ = lod;
	}
}

void ModelPreview::tick(double delta) {
	model_.advance(delta);
	play_clip_();
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(int64_t(model_.clock_ms()), ++frame_);
}

opennova::editor::ModelPreviewSnapshot ModelPreview::snapshot(const opennova::editor::SessionView &view) const {
	return opennova::editor::model_preview_snapshot(view, model_, true);
}

void ModelPreview::draw(int device_width, int device_height) {
	if (viewport_ == nullptr || model_.status() != opennova::editor::ModelPreviewStatus::Ready) {
		return;
	}
	model_.set_device_size(device_width, device_height);
	const Vector2i size(model_.device_width(), model_.device_height());
	if (viewport_->get_size() != size) viewport_->set_size(size);
	place_camera();
	viewport_->set_update_mode(SubViewport::UPDATE_ONCE);
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", viewport_);
	}
}

} // namespace godot
