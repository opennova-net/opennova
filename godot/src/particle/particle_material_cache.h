#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>

#include <runtime/renderer/particle_frame.h>

#include "particle/particle_atlas_device.h"

namespace godot {

// The particle renderer's shaders and materials: one shader per PTL type plus
// the two thermal secondaries (renderer::particle_thermal_material), loaded
// once, and one material per (atlas page, shader, variant) binding that page's
// texture. The first-person batch draws with the scene materials, the far-side
// pass A runs (ParticleFarPass) with the far-pass ones, whose shaders draw
// inside the transparent list at kRungParticleFarSide.
class ParticleMaterialCache {
public:
	// The scene shaders: the eight PTL types, then the thermal Blend
	// inversion and the thermal Additive/Premult darkening.
	static constexpr std::size_t kShaderCount = 10;

	Ref<Shader> shader_for_index(std::size_t p_index);
	Ref<ShaderMaterial> material_for(const opennova::renderer::ParticleDrawCommand &p_command,
			bool p_thermal, const std::vector<ParticleAtlasDevice::Page> &p_pages);
	Ref<ShaderMaterial> far_material_for(const opennova::renderer::ParticleDrawCommand &p_command,
			opennova::renderer::ParticleThermalMaterial p_thermal,
			const std::vector<ParticleAtlasDevice::Page> &p_pages);
	// Drops the scene materials, whose page textures a catalog rebuild
	// replaces.
	void clear_scene_materials() { materials_.clear(); }

private:
	Ref<Shader> far_shader_for(opennova::renderer::ParticlePipeline p_pipeline,
			opennova::renderer::ParticleThermalMaterial p_thermal);

	std::array<Ref<Shader>, kShaderCount> shader_cache_;
	std::map<std::uint64_t, Ref<ShaderMaterial>> materials_;
	std::array<Ref<Shader>, 8> far_shader_cache_;
	// The thermal secondary materials' pass-A shaders: Blend's inversion and
	// Additive/Premult's darkening (renderer::particle_thermal_material).
	std::array<Ref<Shader>, 2> far_thermal_shader_cache_;
	std::map<std::uint64_t, Ref<ShaderMaterial>> far_materials_;
};

} // namespace godot
