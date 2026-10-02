#include "authoring/model_viewport_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <iterator>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_3di3.h>

#include "env/mission_environment.h"
#include "render/frame_fx.h"
#include "util/string_convert.h"
#include "util/texture_files.h"

namespace godot {

namespace {

using opennova::threedi::THREEDI_TEX_SLOT_DETAIL;
using opennova::threedi::THREEDI_TEX_SLOT_DIFFUSE;
using opennova::threedi::THREEDI_TEX_SLOT_NORMAL;
using opennova::threedi::THREEDI_TEX_SLOT_NORMAL_B;

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const opennova::editor::ModelViewport &model_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::ModelViewport &>(model);
}

// True when the material row has a texture row of `slot` (or, for the normal stage, of the second
// normal slot it falls back to).
bool has_stage(const opennova::threedi::ThreediMaterial &material, int slot) {
	const uint32_t rows = std::min<uint32_t>(material.texture_count, uint32_t(std::size(material.textures)));
	for (uint32_t i = 0; i < rows; ++i) {
		const int row = int(material.textures[i].slot);
		if (row == slot || (slot == THREEDI_TEX_SLOT_NORMAL && row == THREEDI_TEX_SLOT_NORMAL_B)) return true;
	}
	return false;
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
	// Every level kept, the level the viewport picks applied as it changes (the build makes every
	// level's meshes): a level change swaps rows, the scene is not built again.
	object_->set_authored_lod_enabled(true);
	object_->set_presenter_driven_lod(true);
	clock_.instantiate();
	object_->set_panm_clock(clock_);
	root->add_child(object_);
}

void ModelViewportApplier::plan_(Build &build) {
	const opennova::threedi::Threedi3di3 &model = build.data->native_model();
	// The textures the scene's materials bind, each stage's as ObjectModel::create_material loads it
	// and each flipbook frame as collect_anim_frames does: decoded here, the scene's loads hit the
	// texture files' cache.
	for (size_t i = 0; i < model.material_count; ++i) {
		const opennova::threedi::ThreediMaterial &material = model.materials[i];
		for (const int slot : { THREEDI_TEX_SLOT_DIFFUSE, THREEDI_TEX_SLOT_DETAIL, THREEDI_TEX_SLOT_NORMAL }) {
			if (!has_stage(material, slot)) continue;
			Unit unit;
			unit.kind = Unit::Kind::Texture;
			unit.material = int(i);
			unit.slot = slot;
			build.units.push_back(unit);
		}
		const PackedStringArray frames = build.data->get_material_anim_frames(int(i), THREEDI_TEX_SLOT_DIFFUSE);
		for (int frame = 0; frames.size() > 1 && frame < frames.size(); ++frame) {
			Unit unit;
			unit.kind = Unit::Kind::Frame;
			unit.material = int(i);
			unit.frame = frame;
			build.units.push_back(unit);
		}
	}
	// Every level's meshes, as the scene asks them of the data (skinned for the rig it binds).
	for (size_t lod = 0; lod < model.lod_count; ++lod) {
		Unit unit;
		unit.kind = Unit::Kind::Meshes;
		unit.lod = int(lod);
		build.units.push_back(unit);
	}
	Unit scene;
	scene.kind = Unit::Kind::Scene;
	build.units.push_back(scene);
	Unit pose;
	pose.kind = Unit::Kind::Pose;
	build.units.push_back(pose);
}

void ModelViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport, const opennova::editor::SessionView &view,
		const opennova::editor::PreviewClock &) {
	// A build in flight dropped: its data and what it decoded go with it.
	build_.reset();
	const opennova::editor::ModelViewport &model = model_of(viewport);
	if (!model.model() || !view.findings.assets) {
		clear();
		return;
	}
	auto build = std::make_unique<Build>();
	// The textures read through the project's files, each name and stamp noted (a flipbook frame
	// loads when first drawn: the files stay noting), so a changed one builds again.
	auto files = std::make_shared<opennova::editor::StampedFiles>(view.findings.assets);
	build->files = files;
	build->data.instantiate();
	build->data->open_from_model(model.model(), opennova::to_gd(model.path()), std::make_shared<opennova::TextureFiles>(files));
	// The rig an animation's model plays on, bound with the scene: its bones size the skinned meshes.
	build->skeleton_serial = model.skeleton_serial();
	if (model.skeleton()) {
		build->skeletal.instantiate();
		build->skeletal->set_rig(model.skeleton());
		build->bone_count = build->skeletal->get_bone_count();
	}
	plan_(*build);
	build_ = std::move(build);
}

ApplierStep ModelViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock, std::string &) {
	Build &build = *build_;
	const Unit unit = build.units[build.next++];
	switch (unit.kind) {
	case Unit::Kind::Texture:
		if (build.data->load_material_slot_texture(unit.material, unit.slot).is_null() &&
				unit.slot == THREEDI_TEX_SLOT_NORMAL)
			build.data->load_material_slot_texture(unit.material, THREEDI_TEX_SLOT_NORMAL_B);
		break;
	case Unit::Kind::Frame:
		build.data->load_material_anim_frame(unit.material, THREEDI_TEX_SLOT_DIFFUSE, unit.frame);
		break;
	case Unit::Kind::Meshes:
		build.data->build_lod_submeshes(unit.lod, build.skeletal.is_valid(), build.bone_count, false);
		break;
	case Unit::Kind::Scene:
		// rebuild() builds only over a model, which open_from_model always holds: no scene unit
		// fails (the protocol's Failed waits for a unit that can).
		DEV_ASSERT(build.data->has_document());
		assemble_(build);
		break;
	case Unit::Kind::Pose:
		// The state as it is now, as an Update applies it (one that came while the build ran is
		// folded into this).
		apply_state_(viewport, clock);
		break;
	}
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

opennova::editor::OperationProgress ModelViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->next;
	progress.total = build_->units.size();
	// What the next unit makes.
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
		case Unit::Kind::Texture:
		case Unit::Kind::Frame: progress.label = "textures"; break;
		case Unit::Kind::Meshes: progress.label = "meshes"; break;
		case Unit::Kind::Scene: progress.label = "scene"; break;
		case Unit::Kind::Pose: progress.label = "pose"; break;
		}
	}
	return progress;
}

void ModelViewportApplier::assemble_(Build &build) {
	// The scene let go and the rig bound with no data held (nothing built either time), then the data
	// swapped in: the scene built once, with the rig's skeleton and every level, from the caches the
	// texture and mesh units filled.
	if (build.skeletal.is_valid() || object_->get_skeletal_anim().is_valid()) {
		object_->set_object_data(Ref<ObjectData>());
		object_->set_skeletal_anim(build.skeletal);
	}
	object_->set_object_data(build.data);
	data_ = build.data;
	files_ = build.files;
	applied_ctrl_.clear();
	applied_lod_ = -1;
	applied_skeleton_ = build.skeleton_serial;
}

void ModelViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	apply_state_(model, clock);
}

void ModelViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	if (applied_skeleton_ != model_of(viewport).skeleton_serial()) bind_rig_(viewport);
	apply_registers_(viewport);
	play_clip_(viewport, clock);
	place_camera_(viewport);
}

void ModelViewportApplier::clear() {
	build_.reset();
	object_->set_object_data(Ref<ObjectData>());
	data_.unref();
	files_.reset();
	applied_ctrl_.clear();
	applied_lod_ = -1;
}

// The CTRL registers the options hold (a register let go reads 0 again); nothing when the model holds
// them already (every pump applies the state).
void ModelViewportApplier::apply_registers_(const opennova::editor::ViewportModel &viewport) {
	const std::map<std::string, int64_t> &held = model_of(viewport).options().ctrl;
	if (held == applied_ctrl_) return;
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

void ModelViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	if (build_) {
		// A build runs: the textures its units read so far, and nothing applied over a scene it may be
		// swapping (its pose applies the state as it ends).
		report.files = build_->files->stamps();
		return;
	}
	apply_state_(viewport, clock);
	if (files_) report.files = files_->stamps();
}

void ModelViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	if (!build_) play_clip_(viewport, clock);
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(int64_t(clock.ms()), ++frame_);
}

} // namespace godot
