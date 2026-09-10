#include "model/model_authoring_manifest.h"

using namespace godot;
using namespace opennova::threedi;

namespace {

String resource_array_hint(const char *p_class) {
	return String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":" + p_class;
}

} // namespace

void ModelPartAnimationRow::_bind_methods() {
#define MODEL_PANM_INT_BIND(m_name)                                                                         \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelPartAnimationRow::set_##m_name);         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelPartAnimationRow::get_##m_name);                  \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_PANM_INT_FIELDS(MODEL_PANM_INT_BIND)
#undef MODEL_PANM_INT_BIND
#define MODEL_PANM_TRACK_BIND(m_name)                                                                       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelPartAnimationRow::set_##m_name);         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelPartAnimationRow::get_##m_name);                  \
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_PANM_TRACK_FIELDS(MODEL_PANM_TRACK_BIND)
#undef MODEL_PANM_TRACK_BIND
}

PackedInt32Array ModelPartAnimationRow::track_words(const ThreediTransform &p_track) {
	PackedInt32Array out;
	out.push_back(p_track.control);
	out.push_back(p_track.control_param);
	out.push_back(p_track.rate);
	out.push_back(p_track.start);
	out.push_back(p_track.end);
	return out;
}

bool ModelPartAnimationRow::words_track(const PackedInt32Array &p_words, ThreediTransform &r_track) {
	r_track = ThreediTransform{};
	if (p_words.is_empty()) {
		return true;
	}
	if (p_words.size() != 5) {
		return false;
	}
	r_track.control = static_cast<uint8_t>(p_words[0]);
	r_track.control_param = static_cast<uint8_t>(p_words[1]);
	r_track.rate = static_cast<int16_t>(p_words[2]);
	r_track.start = static_cast<int16_t>(p_words[3]);
	r_track.end = static_cast<int16_t>(p_words[4]);
	return true;
}

void ModelPartAnimationRow::assign(const ThreediPartAnimation &p_row) {
	flags_ = static_cast<int>(p_row.flags);
	parent_subobject_ = p_row.parent_subobject;
	subobject_index_ = p_row.subobject_index;
	matrix_index_ = p_row.matrix_index;
	matrix_offset_ = p_row.matrix_offset;
	bind_matrix_index_ = p_row.bind_matrix_index;
	rotation_x_ = track_words(p_row.rotation_x);
	rotation_y_ = track_words(p_row.rotation_y);
	rotation_z_ = track_words(p_row.rotation_z);
	scale_x_ = track_words(p_row.scale_x);
	scale_y_ = track_words(p_row.scale_y);
	scale_z_ = track_words(p_row.scale_z);
	translation_ = track_words(p_row.translation);
}

bool ModelPartAnimationRow::write(ThreediPartAnimation &r_row) const {
	r_row = ThreediPartAnimation{};
	r_row.flags = static_cast<uint32_t>(flags_);
	r_row.parent_subobject = static_cast<uint8_t>(parent_subobject_);
	r_row.subobject_index = static_cast<uint8_t>(subobject_index_);
	r_row.matrix_index = static_cast<uint8_t>(matrix_index_);
	r_row.matrix_offset = static_cast<uint8_t>(matrix_offset_);
	r_row.bind_matrix_index = bind_matrix_index_;
	return words_track(rotation_x_, r_row.rotation_x) && words_track(rotation_y_, r_row.rotation_y) &&
			words_track(rotation_z_, r_row.rotation_z) && words_track(scale_x_, r_row.scale_x) &&
			words_track(scale_y_, r_row.scale_y) && words_track(scale_z_, r_row.scale_z) &&
			words_track(translation_, r_row.translation);
}

void ModelLodSpec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_threshold", "value"), &ModelLodSpec::set_threshold);
	ClassDB::bind_method(D_METHOD("get_threshold"), &ModelLodSpec::get_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "threshold"), "set_threshold", "get_threshold");
	ClassDB::bind_method(D_METHOD("set_model_type", "value"), &ModelLodSpec::set_model_type);
	ClassDB::bind_method(D_METHOD("get_model_type"), &ModelLodSpec::get_model_type);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "model_type"), "set_model_type", "get_model_type");
	ClassDB::bind_method(D_METHOD("set_part_animations", "value"), &ModelLodSpec::set_part_animations);
	ClassDB::bind_method(D_METHOD("get_part_animations"), &ModelLodSpec::get_part_animations);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "part_animations", PROPERTY_HINT_ARRAY_TYPE,
						 resource_array_hint("ModelPartAnimationRow")),
			"set_part_animations", "get_part_animations");
}

void ModelTextureSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_path", "value"), &ModelTextureSource::set_source_path);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ModelTextureSource::get_source_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path", PROPERTY_HINT_FILE, "*.png"), "set_source_path",
			"get_source_path");
	ClassDB::bind_method(D_METHOD("set_output_name", "value"), &ModelTextureSource::set_output_name);
	ClassDB::bind_method(D_METHOD("get_output_name"), &ModelTextureSource::get_output_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output_name"), "set_output_name", "get_output_name");
	ClassDB::bind_method(D_METHOD("set_with_alpha", "value"), &ModelTextureSource::set_with_alpha);
	ClassDB::bind_method(D_METHOD("get_with_alpha"), &ModelTextureSource::get_with_alpha);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "with_alpha"), "set_with_alpha", "get_with_alpha");
}

void ModelAuthoringManifest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_model_name", "value"), &ModelAuthoringManifest::set_model_name);
	ClassDB::bind_method(D_METHOD("get_model_name"), &ModelAuthoringManifest::get_model_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "model_name"), "set_model_name", "get_model_name");
	ClassDB::bind_method(D_METHOD("set_version", "value"), &ModelAuthoringManifest::set_version);
	ClassDB::bind_method(D_METHOD("get_version"), &ModelAuthoringManifest::get_version);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "version"), "set_version", "get_version");
	ClassDB::bind_method(D_METHOD("set_skinned", "value"), &ModelAuthoringManifest::set_skinned);
	ClassDB::bind_method(D_METHOD("get_skinned"), &ModelAuthoringManifest::get_skinned);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "skinned"), "set_skinned", "get_skinned");
	ClassDB::bind_method(D_METHOD("set_scene", "value"), &ModelAuthoringManifest::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"), &ModelAuthoringManifest::get_scene);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_scene",
			"get_scene");
	ClassDB::bind_method(D_METHOD("set_output_directory", "value"), &ModelAuthoringManifest::set_output_directory);
	ClassDB::bind_method(D_METHOD("get_output_directory"), &ModelAuthoringManifest::get_output_directory);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output_directory"), "set_output_directory", "get_output_directory");
	ClassDB::bind_method(D_METHOD("set_control_registers", "value"), &ModelAuthoringManifest::set_control_registers);
	ClassDB::bind_method(D_METHOD("get_control_registers"), &ModelAuthoringManifest::get_control_registers);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "control_registers"), "set_control_registers",
			"get_control_registers");
	ClassDB::bind_method(D_METHOD("set_matrix_count", "value"), &ModelAuthoringManifest::set_matrix_count);
	ClassDB::bind_method(D_METHOD("get_matrix_count"), &ModelAuthoringManifest::get_matrix_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "matrix_count"), "set_matrix_count", "get_matrix_count");
	ClassDB::bind_method(D_METHOD("set_materials", "value"), &ModelAuthoringManifest::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &ModelAuthoringManifest::get_materials);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE,
						 resource_array_hint("ModelMaterialSpec")),
			"set_materials", "get_materials");
	ClassDB::bind_method(D_METHOD("set_lods", "value"), &ModelAuthoringManifest::set_lods);
	ClassDB::bind_method(D_METHOD("get_lods"), &ModelAuthoringManifest::get_lods);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lods", PROPERTY_HINT_ARRAY_TYPE, resource_array_hint("ModelLodSpec")),
			"set_lods", "get_lods");
	ClassDB::bind_method(D_METHOD("set_texture_sources", "value"), &ModelAuthoringManifest::set_texture_sources);
	ClassDB::bind_method(D_METHOD("get_texture_sources"), &ModelAuthoringManifest::get_texture_sources);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "texture_sources", PROPERTY_HINT_ARRAY_TYPE,
						 resource_array_hint("ModelTextureSource")),
			"set_texture_sources", "get_texture_sources");
	ClassDB::bind_method(D_METHOD("set_expected_bone_rows", "value"), &ModelAuthoringManifest::set_expected_bone_rows);
	ClassDB::bind_method(D_METHOD("get_expected_bone_rows"), &ModelAuthoringManifest::get_expected_bone_rows);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "expected_bone_rows"), "set_expected_bone_rows",
			"get_expected_bone_rows");
	ClassDB::bind_method(D_METHOD("find_material", "material_name"), &ModelAuthoringManifest::find_material);
	ClassDB::bind_method(D_METHOD("find_material_index", "material_name"),
			&ModelAuthoringManifest::find_material_index);
}

int ModelAuthoringManifest::find_material_index(const String &p_material_name) const {
	for (int i = 0; i < materials_.size(); ++i) {
		const Ref<ModelMaterialSpec> spec = materials_[i];
		if (spec.is_valid() && spec->get_material_name() == p_material_name) {
			return i;
		}
	}
	return -1;
}

Ref<ModelMaterialSpec> ModelAuthoringManifest::find_material(const String &p_material_name) const {
	const int index = find_material_index(p_material_name);
	return index < 0 ? Ref<ModelMaterialSpec>() : Ref<ModelMaterialSpec>(materials_[index]);
}
