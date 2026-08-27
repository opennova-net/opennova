// ObjectData: read-only inspection of authored .3di PANM records.
#include "object/object_data_internal.h"

#include <formats/threedi/threedi_panm.h>

using namespace novaobj;

namespace {

Dictionary transform_to_dict(
		const ThreediTransform &track, const Threedi3di3 &model) {
	Dictionary result;
	result["control"] = static_cast<int>(track.control);
	result["function"] = static_cast<int>(track.control);
	result["control_param"] = static_cast<int>(track.control_param);
	result["reg"] = static_cast<int>(track.control_param);
	result["reg_name"] = control_register_name_for(model, track.control_param);
	result["rate"] = static_cast<int>(track.rate);
	result["start"] = static_cast<int>(track.start);
	result["end"] = static_cast<int>(track.end);
	return result;
}

} // namespace

int ObjectData::get_part_anim_count(int p_lod_index) const {
	if (!has_source_model || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return 0;
	}
	return static_cast<int>(
			source_model.lods[p_lod_index].part_animation_count);
}

Dictionary ObjectData::get_part_anim_info(
		int p_lod_index, int p_anim_index) const {
	Dictionary info;
	if (!has_source_model || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return info;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 ||
			static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return info;
	}
	const ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	info["index"] = p_anim_index;
	info["transform_as"] = static_cast<int>(anim.subobject_index);
	info["parent_subobject"] = static_cast<int>(anim.parent_subobject);
	info["flags"] = static_cast<int64_t>(anim.flags);
	info["scale_type"] = static_cast<int>(
			threedi_panm_scale_type(anim.flags));
	info["rotation_type"] = static_cast<int>(
			threedi_panm_rotation_type(anim.flags));
	info["rotation_reversed"] =
			threedi_panm_rotation_reversed(anim.flags) != 0;
	info["translate_type"] = static_cast<int>(
			threedi_panm_translate_type(anim.flags));
	info["rotation_x"] = transform_to_dict(anim.rotation_x, source_model);
	info["rotation_y"] = transform_to_dict(anim.rotation_y, source_model);
	info["rotation_z"] = transform_to_dict(anim.rotation_z, source_model);
	info["scale_x"] = transform_to_dict(anim.scale_x, source_model);
	info["scale_y"] = transform_to_dict(anim.scale_y, source_model);
	info["scale_z"] = transform_to_dict(anim.scale_z, source_model);
	info["translation"] = transform_to_dict(anim.translation, source_model);
	return info;
}
