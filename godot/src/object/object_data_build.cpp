#include "object/object_data_build.h"

#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

#include <formats/threedi/threedi_3di3.h>

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

ObjectDataBuild::ObjectDataBuild(Ref<ObjectData> data, bool skinned, int bone_count) :
		data_(std::move(data)), skinned_(skinned), bone_count_(bone_count) {
	const opennova::threedi::Threedi3di3 &model = data_->native_model();
	for (size_t i = 0; i < model.material_count; ++i) {
		const opennova::threedi::ThreediMaterial &material = model.materials[i];
		for (const int slot : { THREEDI_TEX_SLOT_DIFFUSE, THREEDI_TEX_SLOT_DETAIL, THREEDI_TEX_SLOT_NORMAL }) {
			if (!has_stage(material, slot)) continue;
			Unit unit;
			unit.kind = Unit::Kind::Texture;
			unit.material = int(i);
			unit.slot = slot;
			units_.push_back(unit);
		}
		const PackedStringArray frames = data_->get_material_anim_frames(int(i), THREEDI_TEX_SLOT_DIFFUSE);
		for (int frame = 0; frames.size() > 1 && frame < frames.size(); ++frame) {
			Unit unit;
			unit.kind = Unit::Kind::Frame;
			unit.material = int(i);
			unit.frame = frame;
			units_.push_back(unit);
		}
	}
	for (size_t lod = 0; lod < model.lod_count; ++lod) {
		Unit unit;
		unit.kind = Unit::Kind::Meshes;
		unit.lod = int(lod);
		units_.push_back(unit);
	}
}

void ObjectDataBuild::step() {
	if (finished()) return;
	const Unit unit = units_[next_++];
	switch (unit.kind) {
	case Unit::Kind::Texture: data_->load_material_stage_texture(unit.material, unit.slot); break;
	case Unit::Kind::Frame: data_->load_material_anim_frame(unit.material, THREEDI_TEX_SLOT_DIFFUSE, unit.frame); break;
	case Unit::Kind::Meshes: data_->build_lod_submeshes(unit.lod, skinned_, bone_count_, false); break;
	}
}

const char *ObjectDataBuild::label() const {
	if (finished()) return "";
	return units_[next_].kind == Unit::Kind::Meshes ? "meshes" : "textures";
}

} // namespace godot
