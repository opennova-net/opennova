#include "model/clip_set_source.h"

using namespace godot;

namespace {

String resource_array_hint(const char *p_class) {
	return String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":" +
			String(p_class);
}

} // namespace

void ClipSpec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_animation", "value"), &ClipSpec::set_animation);
	ClassDB::bind_method(D_METHOD("get_animation"), &ClipSpec::get_animation);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "animation"), "set_animation", "get_animation");
	ClassDB::bind_method(D_METHOD("set_clip_name", "value"), &ClipSpec::set_clip_name);
	ClassDB::bind_method(D_METHOD("get_clip_name"), &ClipSpec::get_clip_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "clip_name"), "set_clip_name", "get_clip_name");
	ClassDB::bind_method(D_METHOD("set_loop", "value"), &ClipSpec::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &ClipSpec::get_loop);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");
	ClassDB::bind_method(D_METHOD("set_hold_frames", "value"), &ClipSpec::set_hold_frames);
	ClassDB::bind_method(D_METHOD("get_hold_frames"), &ClipSpec::get_hold_frames);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hold_frames", PROPERTY_HINT_RANGE, "1,600,1"), "set_hold_frames",
			"get_hold_frames");
	ClassDB::bind_method(D_METHOD("set_forward_speed", "value"), &ClipSpec::set_forward_speed);
	ClassDB::bind_method(D_METHOD("get_forward_speed"), &ClipSpec::get_forward_speed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forward_speed", PROPERTY_HINT_NONE, "suffix:m/s"),
			"set_forward_speed", "get_forward_speed");
	ClassDB::bind_method(D_METHOD("set_lateral_speed", "value"), &ClipSpec::set_lateral_speed);
	ClassDB::bind_method(D_METHOD("get_lateral_speed"), &ClipSpec::get_lateral_speed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lateral_speed", PROPERTY_HINT_NONE, "suffix:m/s"),
			"set_lateral_speed", "get_lateral_speed");
	ClassDB::bind_method(D_METHOD("set_vertical_speed", "value"), &ClipSpec::set_vertical_speed);
	ClassDB::bind_method(D_METHOD("get_vertical_speed"), &ClipSpec::get_vertical_speed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertical_speed", PROPERTY_HINT_NONE, "suffix:m/s"),
			"set_vertical_speed", "get_vertical_speed");
	ClassDB::bind_method(D_METHOD("set_triggers", "value"), &ClipSpec::set_triggers);
	ClassDB::bind_method(D_METHOD("get_triggers"), &ClipSpec::get_triggers);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "triggers"), "set_triggers", "get_triggers");
	ClassDB::bind_method(D_METHOD("set_capsule_bottom", "value"), &ClipSpec::set_capsule_bottom);
	ClassDB::bind_method(D_METHOD("get_capsule_bottom"), &ClipSpec::get_capsule_bottom);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "capsule_bottom", PROPERTY_HINT_NONE, "suffix:m"),
			"set_capsule_bottom", "get_capsule_bottom");
	ClassDB::bind_method(D_METHOD("set_capsule_top", "value"), &ClipSpec::set_capsule_top);
	ClassDB::bind_method(D_METHOD("get_capsule_top"), &ClipSpec::get_capsule_top);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "capsule_top", PROPERTY_HINT_NONE, "suffix:m"), "set_capsule_top",
			"get_capsule_top");
}

void AnimSetRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_key", "value"), &AnimSetRow::set_key);
	ClassDB::bind_method(D_METHOD("get_key"), &AnimSetRow::get_key);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "key"), "set_key", "get_key");
	ClassDB::bind_method(D_METHOD("set_variants", "value"), &AnimSetRow::set_variants);
	ClassDB::bind_method(D_METHOD("get_variants"), &AnimSetRow::get_variants);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "variants"), "set_variants", "get_variants");
}

void ClipSetSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_adm_name", "value"), &ClipSetSource::set_adm_name);
	ClassDB::bind_method(D_METHOD("get_adm_name"), &ClipSetSource::get_adm_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "adm_name"), "set_adm_name", "get_adm_name");
	ClassDB::bind_method(D_METHOD("set_fps", "value"), &ClipSetSource::set_fps);
	ClassDB::bind_method(D_METHOD("get_fps"), &ClipSetSource::get_fps);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fps", PROPERTY_HINT_RANGE, "1,120,1"), "set_fps", "get_fps");
	ClassDB::bind_method(D_METHOD("set_scene", "value"), &ClipSetSource::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"), &ClipSetSource::get_scene);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_scene",
			"get_scene");
	ClassDB::bind_method(D_METHOD("set_ground_bone", "value"), &ClipSetSource::set_ground_bone);
	ClassDB::bind_method(D_METHOD("get_ground_bone"), &ClipSetSource::get_ground_bone);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_bone"), "set_ground_bone", "get_ground_bone");
	ClassDB::bind_method(D_METHOD("set_clips", "value"), &ClipSetSource::set_clips);
	ClassDB::bind_method(D_METHOD("get_clips"), &ClipSetSource::get_clips);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "clips", PROPERTY_HINT_ARRAY_TYPE, resource_array_hint("ClipSpec")),
			"set_clips", "get_clips");
	ClassDB::bind_method(D_METHOD("set_rows", "value"), &ClipSetSource::set_rows);
	ClassDB::bind_method(D_METHOD("get_rows"), &ClipSetSource::get_rows);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "rows", PROPERTY_HINT_ARRAY_TYPE, resource_array_hint("AnimSetRow")),
			"set_rows", "get_rows");
	ClassDB::bind_method(D_METHOD("set_output_directory", "value"), &ClipSetSource::set_output_directory);
	ClassDB::bind_method(D_METHOD("get_output_directory"), &ClipSetSource::get_output_directory);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output_directory", PROPERTY_HINT_DIR), "set_output_directory",
			"get_output_directory");
	ClassDB::bind_method(D_METHOD("find_clip", "clip_name"), &ClipSetSource::find_clip);
}

Ref<ClipSpec> ClipSetSource::find_clip(const String &p_clip_name) const {
	for (int i = 0; i < clips_.size(); ++i) {
		const Ref<ClipSpec> clip = clips_[i];
		if (clip.is_valid() && clip->get_clip_name().to_lower() == p_clip_name.to_lower()) {
			return clip;
		}
	}
	return Ref<ClipSpec>();
}
