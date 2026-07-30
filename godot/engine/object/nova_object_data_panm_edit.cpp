// NovaObjectData — PANM authoring: the editor's part-animation entries and
// per-channel/track editing over the IR part-animation blocks.
#include "object/nova_object_data_internal.h"

#include <threedi/threedi_panm.h> // the byte-lane flag helpers
#include <threedi/threedi_panm_runtime.h>

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>

using namespace novaobj;

namespace {

constexpr double kPanmRotationUnit = 360.0 / 16384.0;
constexpr double kPanmValueUnit = 1.0 / 256.0;

String part_anim_part_label(int part_index) {
	return vformat("Part %02d", part_index);
}

Dictionary part_anim_part_ref(const ThreediIRLod &lod, int part_index) {
	Dictionary result;
	result["index"] = part_index;
	result["label"] = part_anim_part_label(part_index);
	result["valid"] = part_index >= 0 && static_cast<size_t>(part_index) < lod.part_count;
	return result;
}

String panm_mode_key_for_control(uint8_t control) {
	switch (control) {
		case 0: return "none";
		case 16: return "slide";
		case 17: return "slide_inverse";
		case 24: return "set";
		case 32: return "rotate_cw";
		case 33: return "rotate_ccw";
		case 50: return "sine_wave";
		case 52: return "saw_wave";
		case 53: return "inverse_saw_wave";
		case 113: return "control_register";
		default:
			break;
	}
	if (control >= 114 && control <= 117) {
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
	switch (control) {
		case 0: return "None";
		case 16: return "Slide";
		case 17: return "Slide inverse";
		case 24: return "Set";
		case 32: return "Rotate clockwise";
		case 33: return "Rotate counter-clockwise";
		case 50: return "Sine wave";
		case 52: return "Saw wave";
		case 53: return "Inverse saw wave";
		case 113: return "Control register";
		default:
			break;
	}
	if (control >= 114 && control <= 117) {
		return vformat("Wave lookup (raw code %d; not authorable)", control);
	}
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	return (info != nullptr && info->name != nullptr) ? String(info->name).capitalize() : String("Unsupported");
}

bool panm_control_uses_register(uint8_t control) {
	return threedi_panm_control_uses_register(control) != 0;
}

bool panm_control_supported(uint8_t control) {
	if (control >= 114 && control <= 117) {
		return false;
	}
	return threedi_control_func_info(control) != nullptr;
}

bool panm_control_for_mode(const String &p_mode, uint8_t &out_control) {
	const String mode = p_mode.to_lower();
	if (mode == "none") {
		out_control = 0;
		return true;
	}
	if (mode == "slide") {
		out_control = 16;
		return true;
	}
	if (mode == "slide_inverse") {
		out_control = 17;
		return true;
	}
	if (mode == "set") {
		out_control = 24;
		return true;
	}
	if (mode == "rotate_cw") {
		out_control = 32;
		return true;
	}
	if (mode == "rotate_ccw") {
		out_control = 33;
		return true;
	}
	if (mode == "sine_wave") {
		out_control = 50;
		return true;
	}
	if (mode == "saw_wave") {
		out_control = 52;
		return true;
	}
	if (mode == "inverse_saw_wave") {
		out_control = 53;
		return true;
	}
	if (mode == "control_register") {
		out_control = 113;
		return true;
	}
	return false;
}

Dictionary panm_axis_to_editor_dict(const ThreediIRTransform &track, const ThreediModelIR &ir, bool is_rotation) {
	Dictionary result;
	const bool supported = panm_control_supported(track.control);
	result["supported"] = supported;
	result["mode"] = panm_mode_key_for_control(track.control);
	result["mode_label"] = panm_mode_label_for_control(track.control);
	result["uses_control_register"] = panm_control_uses_register(track.control);
	result["control_register"] = static_cast<int>(track.control_param);
	result["control_register_label"] = control_register_name_for(ir, track.control_param);
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

ThreediIRTransform *semantic_part_anim_track(ThreediIRPartAnimation &anim, const String &p_channel, const String &p_axis) {
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

void ensure_part_anim_channel_flag(ThreediIRPartAnimation &anim, const String &p_channel, const String &p_axis) {
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

Dictionary transform_to_dict(const ThreediIRTransform &track, const ThreediModelIR &ir) {
	Dictionary result;
	result["control"] = static_cast<int>(track.control);
	result["function"] = static_cast<int>(track.control);
	result["control_param"] = static_cast<int>(track.control_param);
	result["reg"] = static_cast<int>(track.control_param);
	result["reg_name"] = control_register_name_for(ir, track.control_param);
	result["rate"] = static_cast<int>(track.rate);
	result["start"] = static_cast<int>(track.start);
	result["end"] = static_cast<int>(track.end);
	return result;
}

const ThreediIRTransform *part_anim_track_for_name(const ThreediIRPartAnimation &anim, const String &p_track) {
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

ThreediIRTransform *part_anim_track_for_name(ThreediIRPartAnimation &anim, const String &p_track) {
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

int NovaObjectData::get_part_anim_count(int p_lod_index) const {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return 0;
	}
	return static_cast<int>(ir.lods[p_lod_index].part_animation_count);
}

Array NovaObjectData::get_part_anim_editor_entries(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediIRPartAnimation &anim = lod.part_animations[i];
		const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
		const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
		const uint8_t translate_type = threedi_panm_translate_type(anim.flags);
		const bool rotation_enabled = rotation_type == 2;
		const bool scale_enabled = scale_type == 1 || scale_type == 2;
		const bool translation_enabled = translate_type >= THREEDI_TRANS_X && translate_type <= THREEDI_TRANS_Z;

		Dictionary rotation;
		rotation["enabled"] = rotation_enabled;
		rotation["reversed"] = threedi_panm_rotation_reversed(anim.flags) != 0;
		rotation["x"] = panm_axis_to_editor_dict(anim.rotation_x, ir, true);
		rotation["y"] = panm_axis_to_editor_dict(anim.rotation_y, ir, true);
		rotation["z"] = panm_axis_to_editor_dict(anim.rotation_z, ir, true);
		rotation["supported"] = rotation_type == 0 || rotation_type == 2;

		Dictionary scale;
		scale["enabled"] = scale_enabled;
		scale["style"] = panm_scale_style_for_type(scale_type);
		scale["x"] = panm_axis_to_editor_dict(anim.scale_x, ir, false);
		scale["y"] = panm_axis_to_editor_dict(anim.scale_y, ir, false);
		scale["z"] = panm_axis_to_editor_dict(anim.scale_z, ir, false);
		scale["supported"] = scale_type == 0 || scale_type == 1 || scale_type == 2;

		Dictionary translation;
		translation["enabled"] = translation_enabled;
		translation["axis"] = panm_axis_key_for_translate_type(translate_type);
		translation["track"] = panm_axis_to_editor_dict(anim.translation, ir, false);
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
		entry["target_part"] = static_cast<int>(anim.part_index);
		entry["target_part_ref"] = part_anim_part_ref(lod, anim.part_index);
		entry["target_part_label"] = part_anim_part_label(anim.part_index);
		entry["parent_part"] = static_cast<int>(anim.parent_part);
		entry["parent_part_ref"] = part_anim_part_ref(lod, anim.parent_part);
		entry["parent_part_label"] = part_anim_part_label(anim.parent_part);
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

int NovaObjectData::add_part_anim(int p_lod_index, int p_part_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return -1;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediIRPartAnimation *next = static_cast<ThreediIRPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	if (lod.part_animations != nullptr && old_count > 0) {
		std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediIRPartAnimation));
	}
	const int max_part = lod.part_count > 0 ? static_cast<int>(lod.part_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	ThreediIRPartAnimation &anim = next[old_count];
	anim.part_index = static_cast<uint8_t>(part_index);
	anim.parent_part = 0;
	anim.matrix_index = static_cast<uint8_t>(part_index);
	anim.matrix_offset = 0;
	anim.bind_matrix_index = part_index;
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return static_cast<int>(old_count);
}

int NovaObjectData::duplicate_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return -1;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return -1;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediIRPartAnimation *next = static_cast<ThreediIRPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediIRPartAnimation));
	next[old_count] = lod.part_animations[p_anim_index];
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return static_cast<int>(old_count);
}

bool NovaObjectData::delete_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return false;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count - 1;
	ThreediIRPartAnimation *next = nullptr;
	if (new_count > 0) {
		next = static_cast<ThreediIRPartAnimation *>(
				std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
		if (next == nullptr) {
			return false;
		}
		const size_t remove_index = static_cast<size_t>(p_anim_index);
		if (remove_index > 0) {
			std::memcpy(next, lod.part_animations, remove_index * sizeof(ThreediIRPartAnimation));
		}
		if (remove_index + 1 < old_count) {
			std::memcpy(next + remove_index,
					lod.part_animations + remove_index + 1,
					(old_count - remove_index - 1) * sizeof(ThreediIRPartAnimation));
		}
	}
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_target(int p_lod_index, int p_anim_index, int p_part_index, int p_parent_part) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	const int max_part = lod.part_count > 0 ? static_cast<int>(lod.part_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	const int parent_part = std::clamp(p_parent_part, 0, std::min(max_part, 255));
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.part_index = static_cast<uint8_t>(part_index);
	anim.parent_part = static_cast<uint8_t>(parent_part);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_channel_enabled(int p_lod_index, int p_anim_index, const String &p_channel, bool p_enabled) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
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

bool NovaObjectData::set_part_anim_channel_mode(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, const String &p_mode, int p_control_register) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	uint8_t control = 0;
	if (!panm_control_for_mode(p_mode, control)) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
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

bool NovaObjectData::set_part_anim_channel_values(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, double p_from_value, double p_to_value, double p_speed) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
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

bool NovaObjectData::set_part_anim_rotation_reversed(int p_lod_index, int p_anim_index, bool p_reversed) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.flags = panm_pack_flags_from_parts(
			threedi_panm_scale_type(anim.flags),
			threedi_panm_rotation_type(anim.flags),
			p_reversed ? 1 : 0,
			threedi_panm_translate_type(anim.flags));
	_notify_object_changed(UPDATE_PANM);
	return true;
}

Array NovaObjectData::get_part_animations(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediIRPartAnimation &anim = lod.part_animations[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["flags"] = static_cast<int64_t>(anim.flags);
		item["parent_part"] = anim.parent_part;
		item["part_index"] = anim.part_index;
		item["matrix_index"] = anim.matrix_index;
		item["bind_matrix_index"] = anim.bind_matrix_index;
		result.push_back(item);
	}
	return result;
}

Dictionary NovaObjectData::get_part_anim_info(int p_lod_index, int p_anim_index) const {
	Dictionary info;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return info;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return info;
	}
	const ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	info["index"] = p_anim_index;
	info["transform_as"] = static_cast<int>(anim.part_index);
	info["parent_subobject"] = static_cast<int>(anim.parent_part);
	info["flags"] = static_cast<int64_t>(anim.flags);
	info["scale_type"] = static_cast<int>(threedi_panm_scale_type(anim.flags));
	info["rotation_type"] = static_cast<int>(threedi_panm_rotation_type(anim.flags));
	info["rotation_reversed"] = threedi_panm_rotation_reversed(anim.flags) != 0;
	info["translate_type"] = static_cast<int>(threedi_panm_translate_type(anim.flags));
	info["rotation_x"] = transform_to_dict(anim.rotation_x, ir);
	info["rotation_y"] = transform_to_dict(anim.rotation_y, ir);
	info["rotation_z"] = transform_to_dict(anim.rotation_z, ir);
	info["scale_x"] = transform_to_dict(anim.scale_x, ir);
	info["scale_y"] = transform_to_dict(anim.scale_y, ir);
	info["scale_z"] = transform_to_dict(anim.scale_z, ir);
	info["translation"] = transform_to_dict(anim.translation, ir);
	return info;
}

bool NovaObjectData::set_part_anim_field(int p_lod_index, int p_anim_index, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	const String key = p_key;
	if (key == "transform_as") {
		anim.part_index = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "parent_subobject") {
		anim.parent_part = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
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

bool NovaObjectData::set_part_anim_track_field(int p_lod_index, int p_anim_index, const String &p_track, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = part_anim_track_for_name(anim, p_track);
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
		if (!resolve_control_register_index(ir, String(p_value), reg) || reg < 0 || reg > 255) {
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

Error NovaObjectData::set_part_animation_flags(int p_lod_index, int p_anim_index, int p_flags) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return ERR_INVALID_PARAMETER;
	}
	lod.part_animations[p_anim_index].flags = static_cast<uint32_t>(p_flags);
	_notify_object_changed(UPDATE_PANM);
	return OK;
}
