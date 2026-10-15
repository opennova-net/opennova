#include "authoring/preview_model.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <utility>

#include "object/avatar_database.h"
#include "player/player_viewmodel_rig.h"
#include "util/string_convert.h"
#include "util/texture_files.h"

namespace godot {

PreviewModel::PreviewModel(Node3D &parent) {
	object_ = memnew(ObjectModel);
	// Every level kept, the level the owner picks applied as it changes (the build makes every level's meshes).
	object_->set_authored_lod_enabled(true);
	object_->set_presenter_driven_lod(true);
	clock_.instantiate();
	object_->set_panm_clock(clock_);
	parent.add_child(object_);
}

// --- PreviewModel ------------------------------------------------------------------------------------------

void PreviewModel::begin(const opennova::assets::Model &model, const std::string &path,
		std::shared_ptr<const opennova::StampedFiles> files, std::shared_ptr<const opennova::anim::SkeletalClips> rig) {
	build_.reset();
	if (!model || !files) {
		clear();
		return;
	}
	auto build = std::make_unique<Build>();
	build->files = files;
	Ref<ObjectData> data;
	data.instantiate();
	data->open_from_model(model, opennova::to_gd(path), std::make_shared<opennova::TextureFiles>(files));
	int bones = 0;
	if (rig) {
		build->skeletal.instantiate();
		build->skeletal->set_rig(rig);
		bones = build->skeletal->get_bone_count();
	}
	build->data = std::make_unique<ObjectDataBuild>(data, build->skeletal.is_valid(), bones);
	build_ = std::move(build);
}

bool PreviewModel::step() {
	if (!build_) return false;
	Build &build = *build_;
	if (!build.data->finished()) {
		build.data->step();
		return false;
	}
	// The scene: built only over a model, which open_from_model always holds.
	DEV_ASSERT(build.data->data()->has_document());
	assemble_(build);
	build_.reset();
	return true;
}

opennova::editor::OperationProgress PreviewModel::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->data->done();
	progress.total = build_->data->total() + 1;
	progress.label = build_->data->finished() ? "scene" : build_->data->label();
	return progress;
}

void PreviewModel::assemble_(Build &build) {
	// The scene let go and the rig bound with no data held (nothing built either time), then the data swapped in:
	// the scene built once, with the rig's skeleton and every level, from the caches the units filled.
	if (build.skeletal.is_valid() || object_->get_skeletal_anim().is_valid()) {
		object_->set_object_data(Ref<ObjectData>());
		object_->set_skeletal_anim(build.skeletal);
	}
	object_->set_object_data(build.data->data());
	data_ = build.data->data();
	files_ = build.files;
	if (arms_) {
		object_->set_graphic_name(data_->get_source_path());
		AvatarDatabase::apply_part_camo(object_, Vector3i(arms_camo_[0], arms_camo_[1], arms_camo_[2]),
				PlayerViewmodelRig::kCtrlOwnerFpArmsCamo);
	}
	applied_ctrl_.clear();
	applied_lod_ = -1;
}

void PreviewModel::clear() {
	build_.reset();
	object_->set_object_data(Ref<ObjectData>());
	object_->set_skeletal_anim(Ref<SkeletalAnim>());
	data_.unref();
	files_.reset();
	applied_ctrl_.clear();
	applied_lod_ = -1;
}

void PreviewModel::set_registers(const std::map<std::string, int64_t> &held) {
	if (held == applied_ctrl_ || data_.is_null()) return;
	object_->begin_ctrl_update();
	for (const auto &entry : applied_ctrl_)
		if (held.find(entry.first) == held.end()) object_->clear_ctrl_value(opennova::to_gd(entry.first));
	for (const auto &entry : held) object_->set_ctrl_value(opennova::to_gd(entry.first), entry.second);
	object_->end_ctrl_update();
	applied_ctrl_ = held;
}

void PreviewModel::set_hidden_sections(uint32_t mask) {
	// The model keeps its mask across the scenes it builds (each part made visible by it).
	if (mask == applied_hidden_) return;
	object_->set_destroyed_section_mask(int64_t(mask));
	applied_hidden_ = mask;
}

void PreviewModel::set_level(int lod) {
	if (data_.is_null() || lod < 0 || lod == applied_lod_) return;
	object_->set_active_lod(lod);
	applied_lod_ = lod;
}

void PreviewModel::pose_body(const opennova::world::InfantryBodyPose &body) {
	if (data_.is_null() || object_->get_skeletal_anim().is_null()) return;
	// The body channel as the game's presenter dispatches a person's row (EntityPresenter's body leg over the
	// PF_ANIM_* fields, the mission device's pose of a placed person).
	const String key = opennova::to_gd(opennova::world::infantry_anim_key(body.state));
	if (body.blending && body.source_state >= 0 && body.weight < 1.0f)
		object_->play_body_blend_at(opennova::to_gd(opennova::world::infantry_anim_key(body.source_state)),
				body.source_phase, key, body.phase, body.weight, body.source_variant, body.variant);
	else
		object_->play_body_clip_at(key, body.phase, body.variant, body.parked);
}

void PreviewModel::play_clip(const std::string &key, int variant, int ticks) {
	if (data_.is_null() || object_->get_skeletal_anim().is_null() || key.empty()) return;
	object_->play_body_clip_variant_at_tick(opennova::to_gd(key), variant, ticks);
}

void PreviewModel::play_blend(const std::string &key, int ticks, const std::string &blend_key, int blend_ticks,
		float weight, int variant, int blend_variant) {
	if (data_.is_null() || object_->get_skeletal_anim().is_null() || key.empty()) return;
	object_->play_body_blend_at(opennova::to_gd(key), ticks, opennova::to_gd(blend_key), blend_ticks, weight, variant,
			blend_variant);
}

void PreviewModel::set_arms(int camo0, int camo1, int camo2) {
	if (!arms_) {
		arms_ = true;
		object_->set_authored_lod_enabled(false);
		object_->set_presenter_driven_lod(false);
		object_->set_avatar_part(ObjectModel::AVATAR_PART_ARMS);
	}
	arms_camo_[0] = camo0;
	arms_camo_[1] = camo1;
	arms_camo_[2] = camo2;
}

void PreviewModel::set_lift(float metres) {
	object_->set_transform(Transform3D(Basis(), Vector3(0.0f, metres, 0.0f)));
}

void PreviewModel::tick(int64_t ms) {
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(ms, ++frame_);
}

opennova::FileStamps PreviewModel::stamps() const {
	if (build_ && build_->files) return build_->files->stamps();
	return files_ ? files_->stamps() : opennova::FileStamps();
}

} // namespace godot
