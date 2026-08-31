#pragma once

// Godot adapter for engine/runtime/renderer object pipeline descriptors.
// Selects a finite checked-in Shader resource whose static include graph
// embodies the descriptor; no runtime shader source is generated.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <unordered_map>

namespace godot {

class ObjectShaderCache : public Object {
	GDCLASS(ObjectShaderCache, Object)

public:
	static ObjectShaderCache *get_singleton();
	// Extension objects must not outlive module deinit: a leaked singleton
	// reaches ObjectDB::cleanup() after the library's class info is gone and
	// the late teardown walk lands in freed memory (the packaging boot-smoke
	// 0xC0000005). Called from uninitialize_opennova_module (SCENE level).
	static void destroy_singleton();

	ObjectShaderCache();
	~ObjectShaderCache();

	Ref<Shader> get_shader_for_key(int32_t key);
	void configure_material_for_key(const Ref<ShaderMaterial> &material, int32_t key);

	int32_t classify(const String &shader_tag,
			int32_t material_flags,
			int32_t emissive_type,
			int32_t is_glass_flag,
			int32_t alpha_test_byte);
	// The PROJSHAD coverage source of a classified key's technique, as the

	// Every shader tag in the canonical runtime renderer descriptor table, in table
	// order. Lets tooling (the render swatch probe, material pickers) iterate
	// the real table instead of duplicating the tag list.
	PackedStringArray get_known_shader_tags() const;

	// The water-plane transparent bracket (maturity REN-3,
	// docs/render/render-order-re.md): the session's water height and current
	// camera side split each blended strip into the far/camera-side priority
	// rungs. Set once per scene frame; cleared when no water pass is active.
	void set_water_plane(float height, bool camera_above);
	void clear_water_plane();
	bool has_water_plane() const;
	// Advances only when the plane (height, camera side, presence) actually
	// changes; strip classifiers compare it to skip frames where nothing the
	// ladder depends on moved.
	uint64_t get_water_plane_generation() const;
	int32_t alpha_rung_for_height(float world_height) const;

	void clear();

protected:
	static void _bind_methods();

private:
	static ObjectShaderCache *singleton;
	std::unordered_map<uint32_t, Ref<Shader>> cache;
	float water_split_height = 0.0f;
	bool water_camera_above = true;
	bool water_split_set = false;
	uint64_t water_plane_generation = 1;
};

} // namespace godot
