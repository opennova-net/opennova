// ObjectData: read-only inspection of authored .3di PANM records.
#include "object/object_data_internal.h"
#include "object/model_inspection_records.h"

#include <formats/threedi/threedi_panm.h>

using namespace novaobj;

namespace {

Ref<PartAnimTrack> track_record(
		const ThreediTransform &track, const Threedi3di3 &model) {
	Ref<PartAnimTrack> out;
	out.instantiate();
	out->set_control(static_cast<int>(track.control));
	out->set_control_param(static_cast<int>(track.control_param));
	out->set_reg_name(control_register_name_for(model, track.control_param));
	out->set_rate(static_cast<int>(track.rate));
	out->set_start(static_cast<int>(track.start));
	out->set_end(static_cast<int>(track.end));
	return out;
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

Ref<PartAnimInfo> ObjectData::get_part_anim_info(
		int p_lod_index, int p_anim_index) const {
	if (!has_source_model || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return Ref<PartAnimInfo>();
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 ||
			static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return Ref<PartAnimInfo>();
	}
	const ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	Ref<PartAnimInfo> info;
	info.instantiate();
	info->set_index(p_anim_index);
	info->set_transform_as(static_cast<int>(anim.subobject_index));
	info->set_parent_subobject(static_cast<int>(anim.parent_subobject));
	info->set_flags(static_cast<int64_t>(anim.flags));
	info->set_scale_type(static_cast<int>(threedi_panm_scale_type(anim.flags)));
	info->set_rotation_type(static_cast<int>(threedi_panm_rotation_type(anim.flags)));
	info->set_rotation_reversed(threedi_panm_rotation_reversed(anim.flags) != 0);
	info->set_translate_type(static_cast<int>(threedi_panm_translate_type(anim.flags)));
	info->set_rotation_x(track_record(anim.rotation_x, source_model));
	info->set_rotation_y(track_record(anim.rotation_y, source_model));
	info->set_rotation_z(track_record(anim.rotation_z, source_model));
	info->set_scale_x(track_record(anim.scale_x, source_model));
	info->set_scale_y(track_record(anim.scale_y, source_model));
	info->set_scale_z(track_record(anim.scale_z, source_model));
	info->set_translation(track_record(anim.translation, source_model));
	return info;
}
