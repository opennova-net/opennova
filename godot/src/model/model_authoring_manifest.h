#pragma once

#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <formats/threedi/threedi_3di3.h>

#include "model/model_material_spec.h"

namespace godot {

// One PANM row of a LOD, raw: the packed flags, the parent and subject
// subobjects, the matrix fields, and the seven tracks as five-word arrays
// {control, param, rate, start, end}.
class ModelPartAnimationRow : public Resource {
	GDCLASS(ModelPartAnimationRow, Resource)

public:
#define MODEL_PANM_INT_FIELDS(X) \
	X(flags)                     \
	X(parent_subobject)          \
	X(subobject_index)           \
	X(matrix_index)              \
	X(matrix_offset)             \
	X(bind_matrix_index)
#define MODEL_PANM_TRACK_FIELDS(X) \
	X(rotation_x)                  \
	X(rotation_y)                  \
	X(rotation_z)                  \
	X(scale_x)                     \
	X(scale_y)                     \
	X(scale_z)                     \
	X(translation)

private:
#define MODEL_PANM_INT_MEMBER(m_name) int m_name##_ = 0;
	MODEL_PANM_INT_FIELDS(MODEL_PANM_INT_MEMBER)
#undef MODEL_PANM_INT_MEMBER
#define MODEL_PANM_TRACK_MEMBER(m_name) PackedInt32Array m_name##_;
	MODEL_PANM_TRACK_FIELDS(MODEL_PANM_TRACK_MEMBER)
#undef MODEL_PANM_TRACK_MEMBER

	static PackedInt32Array track_words(const opennova::threedi::ThreediTransform &p_track);
	static bool words_track(const PackedInt32Array &p_words, opennova::threedi::ThreediTransform &r_track);

protected:
	static void _bind_methods();

public:
#define MODEL_PANM_INT_ACCESSORS(m_name)                     \
	void set_##m_name(int p_value) { m_name##_ = p_value; }  \
	int get_##m_name() const { return m_name##_; }
	MODEL_PANM_INT_FIELDS(MODEL_PANM_INT_ACCESSORS)
#undef MODEL_PANM_INT_ACCESSORS
#define MODEL_PANM_TRACK_ACCESSORS(m_name)                                       \
	void set_##m_name(const PackedInt32Array &p_value) { m_name##_ = p_value; }  \
	PackedInt32Array get_##m_name() const { return m_name##_; }
	MODEL_PANM_TRACK_FIELDS(MODEL_PANM_TRACK_ACCESSORS)
#undef MODEL_PANM_TRACK_ACCESSORS

	void assign(const opennova::threedi::ThreediPartAnimation &p_row);
	bool write(opennova::threedi::ThreediPartAnimation &r_row) const;
};

// One RLOD's own words: the switch threshold, the RMDL type tag and the PANM
// rows written for that LOD.
class ModelLodSpec : public Resource {
	GDCLASS(ModelLodSpec, Resource)

	int threshold_ = 0;
	String model_type_ = "gnrc";
	TypedArray<ModelPartAnimationRow> part_animations_;

protected:
	static void _bind_methods();

public:
	void set_threshold(int p_value) { threshold_ = p_value; }
	int get_threshold() const { return threshold_; }
	void set_model_type(const String &p_value) { model_type_ = p_value; }
	String get_model_type() const { return model_type_; }
	void set_part_animations(const TypedArray<ModelPartAnimationRow> &p_value) { part_animations_ = p_value; }
	TypedArray<ModelPartAnimationRow> get_part_animations() const { return part_animations_; }
};

// A source image and the artifact it becomes: a PNG under the authoring tree
// exported as `<output_name>` (a .tga, stem at most 12 characters so the
// name fits a MTRL texture slot) beside the model.
class ModelTextureSource : public Resource {
	GDCLASS(ModelTextureSource, Resource)

	String source_path_;
	String output_name_;
	bool with_alpha_ = false;

protected:
	static void _bind_methods();

public:
	void set_source_path(const String &p_value) { source_path_ = p_value; }
	String get_source_path() const { return source_path_; }
	void set_output_name(const String &p_value) { output_name_ = p_value; }
	String get_output_name() const { return output_name_; }
	// 32 bpp with the image's alpha (the cursor, a normal-from-alpha map),
	// else 24 bpp.
	void set_with_alpha(bool p_value) { with_alpha_ = p_value; }
	bool get_with_alpha() const { return with_alpha_; }
};

// The typed record that maps an authoring scene to its exported model: the
// GHDR words (name, skinned, version), the CTRL registers, the material
// rows keyed by surface material name, the per-LOD words, the texture
// sources and, for a rig, the bone-row count the file must carry. Everything
// the scene's nodes cannot spell as ordinary scene data lives here, never in
// importer-private metadata (ADR 0038 decision 5).
class ModelAuthoringManifest : public Resource {
	GDCLASS(ModelAuthoringManifest, Resource)

	String model_name_;
	int version_ = 259;
	bool skinned_ = false;
	Ref<PackedScene> scene_;
	String output_directory_ = "res://../assets";
	PackedStringArray control_registers_;
	int matrix_count_ = 1;
	TypedArray<ModelMaterialSpec> materials_;
	TypedArray<ModelLodSpec> lods_;
	TypedArray<ModelTextureSource> texture_sources_;
	int expected_bone_rows_ = 0;

protected:
	static void _bind_methods();

public:
	void set_model_name(const String &p_value) { model_name_ = p_value; }
	String get_model_name() const { return model_name_; }
	void set_version(int p_value) { version_ = p_value; }
	int get_version() const { return version_; }
	void set_skinned(bool p_value) { skinned_ = p_value; }
	bool get_skinned() const { return skinned_; }
	void set_scene(const Ref<PackedScene> &p_value) { scene_ = p_value; }
	Ref<PackedScene> get_scene() const { return scene_; }
	void set_output_directory(const String &p_value) { output_directory_ = p_value; }
	String get_output_directory() const { return output_directory_; }
	void set_control_registers(const PackedStringArray &p_value) { control_registers_ = p_value; }
	PackedStringArray get_control_registers() const { return control_registers_; }
	void set_matrix_count(int p_value) { matrix_count_ = p_value; }
	int get_matrix_count() const { return matrix_count_; }
	void set_materials(const TypedArray<ModelMaterialSpec> &p_value) { materials_ = p_value; }
	TypedArray<ModelMaterialSpec> get_materials() const { return materials_; }
	void set_lods(const TypedArray<ModelLodSpec> &p_value) { lods_ = p_value; }
	TypedArray<ModelLodSpec> get_lods() const { return lods_; }
	void set_texture_sources(const TypedArray<ModelTextureSource> &p_value) { texture_sources_ = p_value; }
	TypedArray<ModelTextureSource> get_texture_sources() const { return texture_sources_; }
	void set_expected_bone_rows(int p_value) { expected_bone_rows_ = p_value; }
	int get_expected_bone_rows() const { return expected_bone_rows_; }

	// The row whose material_name matches, or null.
	Ref<ModelMaterialSpec> find_material(const String &p_material_name) const;
	int find_material_index(const String &p_material_name) const;
};

} // namespace godot
