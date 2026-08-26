// ObjectData — PANM authoring: the editor's part-animation entries and
// per-channel/track editing over the IR part-animation blocks.
#include "object/nova_object_data_internal.h"

#include <threedi/threedi_panm.h> // the byte-lane flag helpers
#include <threedi/threedi_panm_runtime.h>

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdint>
#include <cstdlib>

using namespace novaobj;

namespace {

// The PANM track units live engine-side (threedi/threedi_panm.h carries the
// [orig] witnesses); these are the double-precision views the editor math uses.
const double kPanmRotationUnit = 360.0 / (double)THREEDI_PANM_ROTATION_COUNTS_PER_TURN;
const double kPanmValueUnit = 1.0 / (double)THREEDI_PANM_VALUE_ONE;

// The semantic modes the part-animation inspector authors, keyed by the
// engine style byte (threedi_panm.h ThreediPanmStyle). One table drives the
// key<->control mapping and the labels.
struct PanmModeEntry {
	uint8_t control;
	const char *key;
	const char *label;
};

constexpr PanmModeEntry kPanmModes[] = {
	{ THREEDI_PANM_STYLE_NONE, "none", "None" },
	{ THREEDI_PANM_STYLE_SLIDE, "slide", "Slide" },
	{ THREEDI_PANM_STYLE_SLIDE_INVERSE, "slide_inverse", "Slide inverse" },
	{ THREEDI_PANM_STYLE_SET, "set", "Set" },
	{ THREEDI_PANM_STYLE_ROTATE_CW, "rotate_cw", "Rotate clockwise" },
	{ THREEDI_PANM_STYLE_ROTATE_CCW, "rotate_ccw", "Rotate counter-clockwise" },
	{ THREEDI_PANM_STYLE_SINE_WAVE, "sine_wave", "Sine wave" },
	{ THREEDI_PANM_STYLE_SAW_WAVE, "saw_wave", "Saw wave" },
	{ THREEDI_PANM_STYLE_INVERSE_SAW_WAVE, "inverse_saw_wave", "Inverse saw wave" },
	{ THREEDI_PANM_STYLE_CONTROL_REGISTER, "control_register", "Control register" },
};

bool panm_control_uses_register(uint8_t control) {
	return threedi_panm_control_uses_register(control) != 0;
}

bool panm_control_for_mode(const String &p_mode, uint8_t &out_control) {
	const String mode = p_mode.to_lower();
	for (const PanmModeEntry &entry : kPanmModes) {
		if (mode == entry.key) {
			out_control = entry.control;
			return true;
		}
	}
	return false;
}

int16_t panm_clamp_i16_from_double(double value) {
	if (!std::isfinite(value)) {
		return 0;
	}
	return clamp_to_i16(static_cast<int>(std::lround(value)));
}

uint32_t panm_pack_flags_from_parts(uint8_t scale_type, uint8_t rotation_type, uint8_t rotation_reversed, uint8_t translate_type) {
	return static_cast<uint32_t>(scale_type) |
			(static_cast<uint32_t>(rotation_type) << 8) |
			(static_cast<uint32_t>(rotation_reversed) << 16) |
			(static_cast<uint32_t>(translate_type) << 24);
}

uint8_t panm_translate_type_for_axis(const String &p_axis) {
	const String axis = p_axis.to_lower();
	if (axis == "y") {
		return THREEDI_TRANS_Y;
	}
	if (axis == "z") {
		return THREEDI_TRANS_Z;
	}
	if (axis == "none") {
		return THREEDI_TRANS_NONE;
	}
	return THREEDI_TRANS_X;
}

ThreediTransform *semantic_part_anim_track(ThreediPartAnimation &anim, const String &p_channel, const String &p_axis) {
	const String channel = p_channel.to_lower();
	const String axis = p_axis.to_lower();
	if (channel == "rotation") {
		if (axis == "y") {
			return &anim.rotation_y;
		}
		if (axis == "z") {
			return &anim.rotation_z;
		}
		return &anim.rotation_x;
	}
	if (channel == "scale") {
		if (axis == "y") {
			return &anim.scale_y;
		}
		if (axis == "z") {
			return &anim.scale_z;
		}
		return &anim.scale_x;
	}
	if (channel == "translation") {
		return &anim.translation;
	}
	return nullptr;
}

void ensure_part_anim_channel_flag(ThreediPartAnimation &anim, const String &p_channel, const String &p_axis) {
	uint8_t scale_type = threedi_panm_scale_type(anim.flags);
	uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
	uint8_t rotation_reversed = threedi_panm_rotation_reversed(anim.flags) ? 1 : 0;
	uint8_t translate_type = threedi_panm_translate_type(anim.flags);
	const String channel = p_channel.to_lower();
	if (channel == "rotation") {
		rotation_type = 2;
	} else if (channel == "scale") {
		const String axis = p_axis.to_lower();
		scale_type = (axis == "y" || axis == "z" || axis == "per_axis") ? 2 : 1;
	} else if (channel == "translation") {
		translate_type = panm_translate_type_for_axis(p_axis);
		if (translate_type == THREEDI_TRANS_NONE) {
			translate_type = THREEDI_TRANS_X;
		}
	}
	anim.flags = panm_pack_flags_from_parts(scale_type, rotation_type, rotation_reversed, translate_type);
}

Dictionary transform_to_dict(const ThreediTransform &track, const Threedi3di3 &model) {
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

const ThreediTransform *part_anim_track_for_name(const ThreediPartAnimation &anim, const String &p_track) {
	const String track = p_track.to_lower();
	if (track == "rotation_x" || track == "yaw") {
		return &anim.rotation_x;
	}
	if (track == "rotation_y" || track == "pitch") {
		return &anim.rotation_y;
	}
	if (track == "rotation_z" || track == "roll") {
		return &anim.rotation_z;
	}
	if (track == "scale_x" || track == "scale") {
		return &anim.scale_x;
	}
	if (track == "scale_y") {
		return &anim.scale_y;
	}
	if (track == "scale_z") {
		return &anim.scale_z;
	}
	if (track == "translation" || track == "trans_x" || track == "trans_y" || track == "trans_z") {
		return &anim.translation;
	}
	return nullptr;
}

ThreediTransform *part_anim_track_for_name(ThreediPartAnimation &anim, const String &p_track) {
	const String track = p_track.to_lower();
	if (track == "rotation_x" || track == "yaw") {
		return &anim.rotation_x;
	}
	if (track == "rotation_y" || track == "pitch") {
		return &anim.rotation_y;
	}
	if (track == "rotation_z" || track == "roll") {
		return &anim.rotation_z;
	}
	if (track == "scale_x" || track == "scale") {
		return &anim.scale_x;
	}
	if (track == "scale_y") {
		return &anim.scale_y;
	}
	if (track == "scale_z") {
		return &anim.scale_z;
	}
	if (track == "translation" || track == "trans_x" || track == "trans_y" || track == "trans_z") {
		return &anim.translation;
	}
	return nullptr;
}

} // namespace

int ObjectData::get_part_anim_count(int p_lod_index) const {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return 0;
	}
	return static_cast<int>(source_model.lods[p_lod_index].part_animation_count);
}

int ObjectData::add_part_anim(int p_lod_index, int p_part_index) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return -1;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediPartAnimation *next = static_cast<ThreediPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	if (lod.part_animations != nullptr && old_count > 0) {
		std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediPartAnimation));
	}
	const int max_part = lod.render_object_count > 0 ? static_cast<int>(lod.render_object_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	ThreediPartAnimation &anim = next[old_count];
	anim.subobject_index = static_cast<uint8_t>(part_index);
	anim.parent_subobject = 0;
	anim.matrix_index = static_cast<uint8_t>(part_index);
	anim.matrix_offset = 0;
	anim.bind_matrix_index = part_index;
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return static_cast<int>(old_count);
}

bool ObjectData::delete_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return false;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count - 1;
	ThreediPartAnimation *next = nullptr;
	if (new_count > 0) {
		next = static_cast<ThreediPartAnimation *>(
				std::calloc(new_count, sizeof(ThreediPartAnimation)));
		if (next == nullptr) {
			return false;
		}
		const size_t remove_index = static_cast<size_t>(p_anim_index);
		if (remove_index > 0) {
			std::memcpy(next, lod.part_animations, remove_index * sizeof(ThreediPartAnimation));
		}
		if (remove_index + 1 < old_count) {
			std::memcpy(next + remove_index,
					lod.part_animations + remove_index + 1,
					(old_count - remove_index - 1) * sizeof(ThreediPartAnimation));
		}
	}
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool ObjectData::set_part_anim_channel_enabled(int p_lod_index, int p_anim_index, const String &p_channel, bool p_enabled) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	uint8_t scale_type = threedi_panm_scale_type(anim.flags);
	uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
	uint8_t rotation_reversed = threedi_panm_rotation_reversed(anim.flags) ? 1 : 0;
	uint8_t translate_type = threedi_panm_translate_type(anim.flags);
	const String channel = p_channel.to_lower();
	if (channel == "rotation") {
		rotation_type = p_enabled ? 2 : 0;
	} else if (channel == "scale") {
		scale_type = p_enabled ? (scale_type == 2 ? 2 : 1) : 0;
	} else if (channel == "translation") {
		translate_type = p_enabled ? (translate_type == THREEDI_TRANS_NONE || translate_type > THREEDI_TRANS_Z ? THREEDI_TRANS_X : translate_type) : THREEDI_TRANS_NONE;
	} else {
		return false;
	}
	anim.flags = panm_pack_flags_from_parts(scale_type, rotation_type, rotation_reversed, translate_type);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool ObjectData::set_part_anim_channel_mode(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, const String &p_mode, int p_control_register) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	uint8_t control = 0;
	if (!panm_control_for_mode(p_mode, control)) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
	if (track == nullptr) {
		return false;
	}
	ensure_part_anim_channel_flag(anim, p_channel, p_axis);
	track->control = control;
	if (panm_control_uses_register(control)) {
		track->control_param = static_cast<uint8_t>(std::clamp(p_control_register, 0, 255));
	} else {
		track->control_param = 0;
	}
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool ObjectData::set_part_anim_channel_values(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, double p_from_value, double p_to_value, double p_speed) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
	if (track == nullptr) {
		return false;
	}
	ensure_part_anim_channel_flag(anim, p_channel, p_axis);
	const bool is_rotation = p_channel.to_lower() == "rotation";
	const double unit = is_rotation ? kPanmRotationUnit : kPanmValueUnit;
	track->start = panm_clamp_i16_from_double(p_from_value / unit);
	track->end = panm_clamp_i16_from_double(p_to_value / unit);
	track->rate = panm_clamp_i16_from_double(p_speed / kPanmValueUnit);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

Dictionary ObjectData::get_part_anim_info(int p_lod_index, int p_anim_index) const {
	Dictionary info;
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return info;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return info;
	}
	const ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	info["index"] = p_anim_index;
	info["transform_as"] = static_cast<int>(anim.subobject_index);
	info["parent_subobject"] = static_cast<int>(anim.parent_subobject);
	info["flags"] = static_cast<int64_t>(anim.flags);
	info["scale_type"] = static_cast<int>(threedi_panm_scale_type(anim.flags));
	info["rotation_type"] = static_cast<int>(threedi_panm_rotation_type(anim.flags));
	info["rotation_reversed"] = threedi_panm_rotation_reversed(anim.flags) != 0;
	info["translate_type"] = static_cast<int>(threedi_panm_translate_type(anim.flags));
	info["rotation_x"] = transform_to_dict(anim.rotation_x, source_model);
	info["rotation_y"] = transform_to_dict(anim.rotation_y, source_model);
	info["rotation_z"] = transform_to_dict(anim.rotation_z, source_model);
	info["scale_x"] = transform_to_dict(anim.scale_x, source_model);
	info["scale_y"] = transform_to_dict(anim.scale_y, source_model);
	info["scale_z"] = transform_to_dict(anim.scale_z, source_model);
	info["translation"] = transform_to_dict(anim.translation, source_model);
	return info;
}

bool ObjectData::set_part_anim_track_field(int p_lod_index, int p_anim_index, const String &p_track, const String &p_key, const Variant &p_value) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediTransform *track = part_anim_track_for_name(anim, p_track);
	if (track == nullptr) {
		return false;
	}
	const String key = p_key.to_lower();
	if (key == "control" || key == "function") {
		track->control = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "control_param" || key == "reg") {
		track->control_param = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "reg_name") {
		int32_t reg = -1;
		if (!resolve_control_register_index(source_model, String(p_value), reg) || reg < 0 || reg > 255) {
			return false;
		}
		track->control_param = static_cast<uint8_t>(reg);
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "rate") {
		track->rate = clamp_to_i16(static_cast<int>(p_value));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "start") {
		track->start = clamp_to_i16(static_cast<int>(p_value));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "end") {
		track->end = clamp_to_i16(static_cast<int>(p_value));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	return false;
}

// --- Witnessed threedi catalog/unit re-exports (statics) --------------------

Error ObjectData::set_part_animation_flags(int p_lod_index, int p_anim_index, int p_flags) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return ERR_INVALID_PARAMETER;
	}
	lod.part_animations[p_anim_index].flags = static_cast<uint32_t>(p_flags);
	_notify_object_changed(UPDATE_PANM);
	return OK;
}
