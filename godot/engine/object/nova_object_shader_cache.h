#pragma once

// Godot wrapper around libs/renderer shader classification and composition.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <unordered_map>

namespace godot {

class NovaObjectShaderCache : public Object {
	GDCLASS(NovaObjectShaderCache, Object)

public:
	static NovaObjectShaderCache *get_singleton();
	// Extension objects must not outlive module deinit: a leaked singleton
	// reaches ObjectDB::cleanup() after the library's class info is gone and
	// the late teardown walk lands in freed memory (the packaging boot-smoke
	// 0xC0000005). Called from uninitialize_opennova_module (SCENE level).
	static void destroy_singleton();

	NovaObjectShaderCache();
	~NovaObjectShaderCache();

	Ref<Shader> get_shader_for_key(int32_t key);

	int32_t classify(const String &shader_tag,
			int32_t material_flags,
			int32_t emissive_type,
			int32_t is_glass_flag,
			int32_t alpha_test_byte);

	int32_t family_for_key(int32_t key) const;
	int32_t blend_for_key(int32_t key) const;

	// Every shader tag in the canonical descriptor table (libs/oed), in table
	// order. Lets tooling (the render swatch probe, material pickers) iterate
	// the real table instead of duplicating the tag list.
	PackedStringArray get_known_shader_tags() const;

	// The water-plane transparent bracket (maturity REN-3,
	// docs/render/render-order-re.md): the session's water height splits
	// blended world materials into the far/camera-side priority rungs.
	// Set by the water owner node when a water plane exists; cleared with it.
	void set_water_split_height(float height);
	void clear_water_split_height();
	bool has_water_split_height() const;
	int32_t alpha_rung_for_height(float world_height) const;

	void clear();

protected:
	static void _bind_methods();

private:
	static NovaObjectShaderCache *singleton;
	std::unordered_map<uint32_t, Ref<Shader>> cache;
	float water_split_height = 0.0f;
	bool water_split_set = false;
};

} // namespace godot
