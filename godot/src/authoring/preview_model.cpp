#include "authoring/preview_model.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <iterator>

#include <formats/threedi/threedi_3di3.h>

#include "util/string_convert.h"
#include "util/texture_files.h"

namespace godot {

namespace {

using opennova::threedi::THREEDI_TEX_SLOT_DETAIL;
using opennova::threedi::THREEDI_TEX_SLOT_DIFFUSE;
using opennova::threedi::THREEDI_TEX_SLOT_NORMAL;
using opennova::threedi::THREEDI_TEX_SLOT_NORMAL_B;

// True when the material row has a texture row of `slot` (or, for the normal stage, of the second normal slot it
// falls back to).
bool has_stage(const opennova::threedi::ThreediMaterial &material, int slot) {
	const uint32_t rows = std::min<uint32_t>(material.texture_count, uint32_t(std::size(material.textures)));
	for (uint32_t i = 0; i < rows; ++i) {
		const int row = int(material.textures[i].slot);
		if (row == slot || (slot == THREEDI_TEX_SLOT_NORMAL && row == THREEDI_TEX_SLOT_NORMAL_B)) return true;
	}
	return false;
}

} // namespace

PreviewModel::PreviewModel(Node3D &parent) {
	object_ = memnew(ObjectModel);
	// Every level kept, the level the owner picks applied as it changes (the build makes every level's meshes).
	object_->set_authored_lod_enabled(true);
	object_->set_presenter_driven_lod(true);
	clock_.instantiate();
	object_->set_panm_clock(clock_);
	parent.add_child(object_);
}

void PreviewModel::plan_(Build &build) {
	const opennova::threedi::Threedi3di3 &model = build.data->native_model();
	// The textures the scene's materials bind, each stage's as ObjectModel::create_material loads it and each
	// flipbook frame as collect_anim_frames does: decoded here, the scene's loads hit the texture files' cache.
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
}

void PreviewModel::begin(const opennova::assets::Model &model, const std::string &path,
		std::shared_ptr<const opennova::editor::StampedFiles> files, std::shared_ptr<const opennova::anim::SkeletalClips> rig) {
	build_.reset();
	if (!model || !files) {
		clear();
		return;
	}
	auto build = std::make_unique<Build>();
	build->files = files;
	build->data.instantiate();
	build->data->open_from_model(model, opennova::to_gd(path), std::make_shared<opennova::TextureFiles>(files));
	if (rig) {
		build->skeletal.instantiate();
		build->skeletal->set_rig(rig);
		build->bone_count = build->skeletal->get_bone_count();
	}
	plan_(*build);
	build_ = std::move(build);
}

bool PreviewModel::step() {
	if (!build_) return false;
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
		DEV_ASSERT(build.data->has_document());
		assemble_(build);
		break;
	}
	if (build.next < build.units.size()) return false;
	build_.reset();
	return true;
}

opennova::editor::OperationProgress PreviewModel::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->next;
	progress.total = build_->units.size();
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
		case Unit::Kind::Texture:
		case Unit::Kind::Frame: progress.label = "textures"; break;
		case Unit::Kind::Meshes: progress.label = "meshes"; break;
		case Unit::Kind::Scene: progress.label = "scene"; break;
		}
	}
	return progress;
}

void PreviewModel::assemble_(Build &build) {
	// The scene let go and the rig bound with no data held (nothing built either time), then the data swapped in:
	// the scene built once, with the rig's skeleton and every level, from the caches the units filled.
	if (build.skeletal.is_valid() || object_->get_skeletal_anim().is_valid()) {
		object_->set_object_data(Ref<ObjectData>());
		object_->set_skeletal_anim(build.skeletal);
	}
	object_->set_object_data(build.data);
	data_ = build.data;
	files_ = build.files;
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

void PreviewModel::set_lift(float metres) {
	object_->set_transform(Transform3D(Basis(), Vector3(0.0f, metres, 0.0f)));
}

void PreviewModel::tick(int64_t ms) {
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(ms, ++frame_);
}

opennova::editor::FileStamps PreviewModel::stamps() const {
	if (build_ && build_->files) return build_->files->stamps();
	return files_ ? files_->stamps() : opennova::editor::FileStamps();
}

} // namespace godot
