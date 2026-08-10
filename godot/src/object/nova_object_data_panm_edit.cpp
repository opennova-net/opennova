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
// key<->control mapping, the labels, and get_panm_mode_options().
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

bool panm_style_is_wave_lookup(uint8_t control) {
	return control >= THREEDI_PANM_STYLE_WAVE_LOOKUP_FIRST &&
			control <= THREEDI_PANM_STYLE_WAVE_LOOKUP_LAST;
}

String part_anim_part_label(int part_index) {
	return vformat("Part %02d", part_index);
}

Dictionary part_anim_part_ref(const ThreediLod &lod, int part_index) {
	Dictionary result;
	result["index"] = part_index;
	result["label"] = part_anim_part_label(part_index);
	result["valid"] = part_index >= 0 && static_cast<size_t>(part_index) < lod.render_object_count;
	return result;
}

String panm_mode_key_for_control(uint8_t control) {
	for (const PanmModeEntry &entry : kPanmModes) {
		if (entry.control == control) {
			return String(entry.key);
		}
	}
	if (panm_style_is_wave_lookup(control)) {
		return "unsupported";
	}
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	if (info == nullptr || info->name == nullptr) {
		return "unsupported";
	}
	String key = String(info->name).to_lower();
	return key.replace("set_wave_", "").replace("add_wave_", "add_");
}

String panm_mode_label_for_control(uint8_t control) {
	for (const PanmModeEntry &entry : kPanmModes) {
		if (entry.control == control) {
			return String(entry.label);
		}
	}
	if (panm_style_is_wave_lookup(control)) {
		return vformat("Wave lookup (raw code %d; not authorable)", control);
	}
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	return (info != nullptr && info->name != nullptr) ? String(info->name).capitalize() : String("Unsupported");
}

bool panm_control_uses_register(uint8_t control) {
	return threedi_panm_control_uses_register(control) != 0;
}

bool panm_control_supported(uint8_t control) {
	if (panm_style_is_wave_lookup(control)) {
		return false;
	}
	return threedi_control_func_info(control) != nullptr;
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

Dictionary panm_axis_to_editor_dict(const ThreediTransform &track, const Threedi3di3 &model, bool is_rotation) {
	Dictionary result;
	const bool supported = panm_control_supported(track.control);
	result["supported"] = supported;
	result["mode"] = panm_mode_key_for_control(track.control);
	result["mode_label"] = panm_mode_label_for_control(track.control);
	result["uses_control_register"] = panm_control_uses_register(track.control);
	result["control_register"] = static_cast<int>(track.control_param);
	result["control_register_label"] = control_register_name_for(model, track.control_param);
	result["speed"] = static_cast<double>(track.rate) * kPanmValueUnit;
	const double unit = is_rotation ? kPanmRotationUnit : kPanmValueUnit;
	result["from_value"] = static_cast<double>(track.start) * unit;
	result["to_value"] = static_cast<double>(track.end) * unit;
	return result;
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

String panm_axis_key_for_translate_type(uint8_t translate_type) {
	switch (translate_type) {
		case THREEDI_TRANS_X: return "x";
		case THREEDI_TRANS_Y: return "y";
		case THREEDI_TRANS_Z: return "z";
		default: return "none";
	}
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

String panm_scale_style_for_type(uint8_t scale_type) {
	if (scale_type == 1) {
		return "uniform";
	}
	if (scale_type == 2) {
		return "per_axis";
	}
	return "none";
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

Array ObjectData::get_part_anim_editor_entries(int p_lod_index) const {
	Array result;
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return result;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediPartAnimation &anim = lod.part_animations[i];
		const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
		const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
		const uint8_t translate_type = threedi_panm_translate_type(anim.flags);
		const bool rotation_enabled = rotation_type == 2;
		const bool scale_enabled = scale_type == 1 || scale_type == 2;
		const bool translation_enabled = translate_type >= THREEDI_TRANS_X && translate_type <= THREEDI_TRANS_Z;

		Dictionary rotation;
		rotation["enabled"] = rotation_enabled;
		rotation["reversed"] = threedi_panm_rotation_reversed(anim.flags) != 0;
		rotation["x"] = panm_axis_to_editor_dict(anim.rotation_x, source_model, true);
		rotation["y"] = panm_axis_to_editor_dict(anim.rotation_y, source_model, true);
		rotation["z"] = panm_axis_to_editor_dict(anim.rotation_z, source_model, true);
		rotation["supported"] = rotation_type == 0 || rotation_type == 2;

		Dictionary scale;
		scale["enabled"] = scale_enabled;
		scale["style"] = panm_scale_style_for_type(scale_type);
		scale["x"] = panm_axis_to_editor_dict(anim.scale_x, source_model, false);
		scale["y"] = panm_axis_to_editor_dict(anim.scale_y, source_model, false);
		scale["z"] = panm_axis_to_editor_dict(anim.scale_z, source_model, false);
		scale["supported"] = scale_type == 0 || scale_type == 1 || scale_type == 2;

		Dictionary translation;
		translation["enabled"] = translation_enabled;
		translation["axis"] = panm_axis_key_for_translate_type(translate_type);
		translation["track"] = panm_axis_to_editor_dict(anim.translation, source_model, false);
		translation["supported"] = translate_type <= THREEDI_TRANS_Z;

		const bool rotation_supported = (rotation_type == 0 || rotation_type == 2) &&
				(!rotation_enabled ||
						(panm_control_supported(anim.rotation_x.control) &&
								panm_control_supported(anim.rotation_y.control) &&
								panm_control_supported(anim.rotation_z.control)));
		const bool scale_supported = (scale_type == 0 || scale_type == 1 || scale_type == 2) &&
				(!scale_enabled ||
						(panm_control_supported(anim.scale_x.control) &&
								panm_control_supported(anim.scale_y.control) &&
								panm_control_supported(anim.scale_z.control)));
		const bool translation_supported = translate_type <= THREEDI_TRANS_Z &&
				(!translation_enabled || panm_control_supported(anim.translation.control));
		const bool supported = rotation_supported && scale_supported && translation_supported;

		Array active_channels;
		if (rotation_enabled) {
			active_channels.push_back("Rotation");
		}
		if (scale_enabled) {
			active_channels.push_back("Scale");
		}
		if (translation_enabled) {
			active_channels.push_back(String("Translate ") + String(translation["axis"]).to_upper());
		}
		String channel_summary = "No channels";
		if (!active_channels.is_empty()) {
			PackedStringArray parts;
			for (int j = 0; j < active_channels.size(); ++j) {
				parts.push_back(String(active_channels[j]));
			}
			channel_summary = String(" + ").join(parts);
		}

		Dictionary entry;
		entry["index"] = static_cast<int64_t>(i);
		entry["target_part"] = static_cast<int>(anim.subobject_index);
		entry["target_part_ref"] = part_anim_part_ref(lod, anim.subobject_index);
		entry["target_part_label"] = part_anim_part_label(anim.subobject_index);
		entry["parent_part"] = static_cast<int>(anim.parent_subobject);
		entry["parent_part_ref"] = part_anim_part_ref(lod, anim.parent_subobject);
		entry["parent_part_label"] = part_anim_part_label(anim.parent_subobject);
		entry["rotation"] = rotation;
		entry["scale"] = scale;
		entry["translation"] = translation;
		entry["supported"] = supported;
		entry["unsupported_reason"] = supported ? String() : String("Unsupported PANM mode");
		entry["summary"] = supported ?
				vformat("%s -> %s | %s", entry["target_part_label"], entry["parent_part_label"], channel_summary) :
				String("Unsupported animation | Not editable");
		result.push_back(entry);
	}
	return result;
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

int ObjectData::duplicate_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return -1;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return -1;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediPartAnimation *next = static_cast<ThreediPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediPartAnimation));
	next[old_count] = lod.part_animations[p_anim_index];
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

bool ObjectData::set_part_anim_target(int p_lod_index, int p_anim_index, int p_part_index, int p_parent_part) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	const int max_part = lod.render_object_count > 0 ? static_cast<int>(lod.render_object_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	const int parent_part = std::clamp(p_parent_part, 0, std::min(max_part, 255));
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.subobject_index = static_cast<uint8_t>(part_index);
	anim.parent_subobject = static_cast<uint8_t>(parent_part);
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

bool ObjectData::set_part_anim_rotation_reversed(int p_lod_index, int p_anim_index, bool p_reversed) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.flags = panm_pack_flags_from_parts(
			threedi_panm_scale_type(anim.flags),
			threedi_panm_rotation_type(anim.flags),
			p_reversed ? 1 : 0,
			threedi_panm_translate_type(anim.flags));
	_notify_object_changed(UPDATE_PANM);
	return true;
}

Array ObjectData::get_part_animations(int p_lod_index) const {
	Array result;
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return result;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediPartAnimation &anim = lod.part_animations[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["flags"] = static_cast<int64_t>(anim.flags);
		item["parent_part"] = anim.parent_subobject;
		item["part_index"] = anim.subobject_index;
		item["matrix_index"] = anim.matrix_index;
		item["bind_matrix_index"] = anim.bind_matrix_index;
		result.push_back(item);
	}
	return result;
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

bool ObjectData::set_part_anim_field(int p_lod_index, int p_anim_index, const String &p_key, const Variant &p_value) {
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return false;
	}
	ThreediLod &lod = source_model.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediPartAnimation &anim = lod.part_animations[p_anim_index];
	const String key = p_key;
	if (key == "transform_as") {
		anim.subobject_index = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "parent_subobject") {
		anim.parent_subobject = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "scale_type" || key == "rotation_type" || key == "translate_type" || key == "rotation_reversed") {
		uint8_t scale_type = threedi_panm_scale_type(anim.flags);
		uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
		uint8_t rotation_reversed = static_cast<uint8_t>(threedi_panm_rotation_reversed(anim.flags) ? 1 : 0);
		uint8_t translate_type = threedi_panm_translate_type(anim.flags);
		if (key == "scale_type") {
			scale_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "rotation_type") {
			rotation_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "translate_type") {
			translate_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "rotation_reversed") {
			rotation_reversed = static_cast<bool>(p_value) ? 1 : 0;
		}
		anim.flags = threedi_panm_pack_flags(scale_type, rotation_type,
				rotation_reversed, translate_type);
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	return false;
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

double ObjectData::panm_rotation_unit_deg() {
	return kPanmRotationUnit;
}

double ObjectData::panm_value_unit() {
	return kPanmValueUnit;
}

Dictionary ObjectData::panm_track_limits() {
	// Tracks are int16 raw counts; the authorable span is the int16 range
	// through the engine units (threedi_panm.h).
	Dictionary out;
	out["rotation_min"] = static_cast<double>(INT16_MIN) * kPanmRotationUnit;
	out["rotation_max"] = static_cast<double>(INT16_MAX) * kPanmRotationUnit;
	out["value_min"] = static_cast<double>(INT16_MIN) * kPanmValueUnit;
	out["value_max"] = static_cast<double>(INT16_MAX) * kPanmValueUnit;
	out["speed_min"] = static_cast<double>(INT16_MIN) * kPanmValueUnit;
	out["speed_max"] = static_cast<double>(INT16_MAX) * kPanmValueUnit;
	return out;
}

Array ObjectData::get_panm_mode_options() {
	Array out;
	for (const PanmModeEntry &entry : kPanmModes) {
		Dictionary d;
		d["mode"] = String(entry.key);
		d["label"] = String(entry.label);
		d["control"] = static_cast<int>(entry.control);
		d["uses_control_register"] = panm_control_uses_register(entry.control);
		out.push_back(d);
	}
	return out;
}

PackedInt32Array ObjectData::get_generator_style_ids() {
	PackedInt32Array out;
	const int count = threedi_generator_style_count();
	out.resize(count);
	for (int i = 0; i < count; ++i) {
		out[i] = threedi_generator_style_code_at(i);
	}
	return out;
}

String ObjectData::generator_style_code_name(int p_style_id) {
	if (p_style_id < 0 || p_style_id > 255) {
		return String();
	}
	const ThreediControlFuncInfo *info =
			threedi_control_func_info(static_cast<uint8_t>(p_style_id));
	return (info != nullptr && info->name != nullptr) ? String(info->name) : String();
}

bool ObjectData::generator_style_reads_control_value(int p_consumer, int p_style_id) {
	if (p_style_id < 0 || p_style_id > 255) {
		return false;
	}
	return threedi_generator_reads_control_value(p_consumer,
				   static_cast<uint8_t>(p_style_id)) != 0;
}

bool ObjectData::generator_style_parameter_is_ctrl_reference(int p_style_id) {
	if (p_style_id < 0 || p_style_id > 255) {
		return false;
	}
	return threedi_panm_parameter_is_ctrl_reference(
				   static_cast<uint8_t>(p_style_id)) != 0;
}

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
