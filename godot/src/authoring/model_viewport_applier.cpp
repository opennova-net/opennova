#include "authoring/model_viewport_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
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

const opennova::editor::ModelViewport &model_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::ModelViewport &>(model);
}

} // namespace

ModelViewportApplier::ModelViewportApplier(SubViewport &viewport) {
	viewport.set_msaa_3d(Viewport::MSAA_4X);
	Node3D *root = memnew(Node3D);
	viewport.add_child(root);
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
}

void ModelViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport, const opennova::editor::SessionView &view) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	if (!model.model() || !view.findings.assets) {
		clear();
		return;
	}
	// The textures read through the project's files, each name and stamp noted (a flipbook frame
	// loads when first drawn: the files stay noting), so a changed one builds again.
	auto files = std::make_shared<opennova::editor::StampedFiles>(view.findings.assets);
	files_ = files;
	data_.instantiate();
	data_->open_from_model(model.model(), opennova::to_gd(model.path()), std::make_shared<opennova::TextureFiles>(files));
	object_->set_object_data(data_);
	applied_ctrl_.clear();
	applied_lod_ = -1;
	applied_skeleton_ = UINT64_MAX;
	apply_registers_(viewport);
}

void ModelViewportApplier::update(const opennova::editor::ViewportModel &model) {
	apply_registers_(model);
}

void ModelViewportApplier::clear() {
	object_->set_object_data(Ref<ObjectData>());
	data_.unref();
	files_.reset();
	applied_ctrl_.clear();
	applied_lod_ = -1;
}

// The CTRL registers the options hold (a register let go reads 0 again).
void ModelViewportApplier::apply_registers_(const opennova::editor::ViewportModel &viewport) {
	const std::map<std::string, int64_t> &held = model_of(viewport).options().ctrl;
	object_->begin_ctrl_update();
	for (const auto &entry : applied_ctrl_) {
		if (held.find(entry.first) == held.end()) object_->clear_ctrl_value(opennova::to_gd(entry.first));
	}
	for (const auto &entry : held) {
		object_->set_ctrl_value(opennova::to_gd(entry.first), entry.second);
	}
	object_->end_ctrl_update();
	applied_ctrl_ = held;
}

void ModelViewportApplier::place_camera_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	if (model.status() != opennova::editor::ViewportStatus::Ready || data_.is_null()) return;
	const int lod = model.lod();
	if (lod >= 0 && lod != applied_lod_) {
		object_->set_active_lod(lod);
		applied_lod_ = lod;
	}
}

void ModelViewportApplier::bind_rig_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	applied_skeleton_ = model.skeleton_serial();
	if (!model.skeleton() || data_.is_null()) {
		object_->set_skeletal_anim(Ref<SkeletalAnim>());
		return;
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	skeletal->set_rig(model.skeleton());
	object_->set_skeletal_anim(skeletal);
}

void ModelViewportApplier::play_clip_(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	if (model.skeleton() && !model.clip_key().empty() && data_.is_valid()) {
		object_->play_body_clip_at(opennova::to_gd(model.clip_key()), int(clock.ticks()), model.clip_variant());
	}
}

void ModelViewportApplier::step(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	if (applied_skeleton_ != model_of(viewport).skeleton_serial()) bind_rig_(viewport);
	play_clip_(viewport, clock);
	place_camera_(viewport);
	if (files_) report.files = files_->stamps();
}

void ModelViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	play_clip_(viewport, clock);
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(int64_t(clock.ms()), ++frame_);
}

} // namespace godot
