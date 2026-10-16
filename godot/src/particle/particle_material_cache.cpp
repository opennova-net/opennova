#include "particle/particle_material_cache.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/renderer/render_order.h>

namespace godot {

namespace {

String shader_path_for_pipeline(opennova::renderer::ParticlePipeline pipeline) {
	switch (pipeline) {
		case opennova::renderer::ParticlePipeline::Blend:
			return "res://shaders/particle/particle_blend_blend.gdshader";
		case opennova::renderer::ParticlePipeline::Additive:
			return "res://shaders/particle/particle_blend_additive.gdshader";
		case opennova::renderer::ParticlePipeline::Premult:
			return "res://shaders/particle/particle_blend_premult.gdshader";
		case opennova::renderer::ParticlePipeline::Bump:
			return "res://shaders/particle/particle_blend_bump.gdshader";
		case opennova::renderer::ParticlePipeline::Mod:
			return "res://shaders/particle/particle_blend_mod.gdshader";
		case opennova::renderer::ParticlePipeline::Mod2x:
			return "res://shaders/particle/particle_blend_mod2x.gdshader";
		case opennova::renderer::ParticlePipeline::Bumpadd:
			return "res://shaders/particle/particle_blend_bumpadd.gdshader";
		case opennova::renderer::ParticlePipeline::Distort:
			return "res://shaders/particle/particle_blend_distort.gdshader";
	}
	return "res://shaders/particle/particle_blend_blend.gdshader";
}

// The thermal frame's secondary materials (renderer::particle_thermal_material)
// sit after the eight PTL types in the shader cache.
constexpr std::size_t kThermalInvertedBlendShader = 8;
constexpr std::size_t kThermalDarkeningShader = 9;

String shader_path_for_index(std::size_t index) {
	if (index == kThermalInvertedBlendShader)
		return "res://shaders/particle/particle_blend_blend_thermal.gdshader";
	if (index == kThermalDarkeningShader)
		return "res://shaders/particle/particle_blend_additive_thermal.gdshader";
	return shader_path_for_pipeline(static_cast<opennova::renderer::ParticlePipeline>(index));
}

std::size_t shader_index_for(opennova::renderer::ParticlePipeline pipeline,
		opennova::renderer::ParticleThermalMaterial thermal) {
	switch (thermal) {
		case opennova::renderer::ParticleThermalMaterial::InvertedBlend:
			return kThermalInvertedBlendShader;
		case opennova::renderer::ParticleThermalMaterial::DarkeningModulate:
			return kThermalDarkeningShader;
		default:
			return static_cast<std::size_t>(pipeline);
	}
}

// A material binds its command's atlas page texture, or none.
void bind_page(const Ref<ShaderMaterial> &material, std::uint32_t page,
		const std::vector<ParticleAtlasDevice::Page> &pages) {
	if (page < pages.size() && pages[page].texture.is_valid()) {
		material->set_shader_parameter("albedo_tex", pages[page].texture);
		material->set_shader_parameter("albedo_max_lod", pages[page].max_lod);
		material->set_shader_parameter("has_texture", true);
	} else {
		material->set_shader_parameter("has_texture", false);
	}
}

} // namespace

static_assert(ParticleMaterialCache::kShaderCount == kThermalDarkeningShader + 1,
		"the thermal secondaries close the scene shader cache");

Ref<Shader> ParticleMaterialCache::shader_for_index(std::size_t p_index) {
	if (p_index >= shader_cache_.size())
		return Ref<Shader>();
	if (shader_cache_[p_index].is_null()) {
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (loader != nullptr) {
			Ref<Resource> resource = loader->load(shader_path_for_index(p_index));
			shader_cache_[p_index] = resource;
		}
	}
	return shader_cache_[p_index];
}

Ref<ShaderMaterial> ParticleMaterialCache::material_for(
		const opennova::renderer::ParticleDrawCommand &p_command, bool p_thermal,
		const std::vector<ParticleAtlasDevice::Page> &p_pages) {
	const std::size_t shader_index = shader_index_for(p_command.pipeline,
			opennova::renderer::particle_thermal_material(p_command.pipeline, p_thermal));
	const std::uint64_t key =
			(static_cast<std::uint64_t>(p_command.atlas_page) << 24) |
			(static_cast<std::uint64_t>(shader_index) << 16) |
			static_cast<std::uint64_t>(p_command.variant);
	const auto found = materials_.find(key);
	if (found != materials_.end())
		return found->second;
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader_for_index(shader_index));
	bind_page(material, p_command.atlas_page, p_pages);
	materials_.emplace(key, material);
	return material;
}

// The far-pass shader for a particle type
// (godot/shaders/particle/world_far; Distort has none: it stays on the
// distortion path).
Ref<Shader> ParticleMaterialCache::far_shader_for(opennova::renderer::ParticlePipeline p_pipeline,
		opennova::renderer::ParticleThermalMaterial p_thermal) {
	if (p_thermal != opennova::renderer::ParticleThermalMaterial::Primary) {
		const bool inverted =
				p_thermal == opennova::renderer::ParticleThermalMaterial::InvertedBlend;
		Ref<Shader> &cached = far_thermal_shader_cache_[inverted ? 0 : 1];
		if (cached.is_null()) {
			if (ResourceLoader *loader = ResourceLoader::get_singleton()) {
				cached = loader->load(inverted ?
								"res://shaders/particle/world_far/particle_far_blend_thermal.gdshader" :
								"res://shaders/particle/world_far/particle_far_additive_thermal.gdshader");
			}
		}
		return cached;
	}
	static const char *const kFarShaderNames[8] = {"blend", "additive",
			"premult", "bump", "mod", "mod2x", "bumpadd", nullptr};
	const std::size_t index = static_cast<std::size_t>(p_pipeline);
	if (index >= far_shader_cache_.size() || kFarShaderNames[index] == nullptr)
		return Ref<Shader>();
	if (far_shader_cache_[index].is_null()) {
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (loader != nullptr) {
			Ref<Resource> resource = loader->load(
					String("res://shaders/particle/world_far/particle_far_") +
					kFarShaderNames[index] + ".gdshader");
			far_shader_cache_[index] = resource;
		}
	}
	return far_shader_cache_[index];
}

Ref<ShaderMaterial> ParticleMaterialCache::far_material_for(
		const opennova::renderer::ParticleDrawCommand &p_command,
		opennova::renderer::ParticleThermalMaterial p_thermal,
		const std::vector<ParticleAtlasDevice::Page> &p_pages) {
	const std::uint64_t key =
			(static_cast<std::uint64_t>(p_thermal) << 40) |
			(static_cast<std::uint64_t>(p_command.atlas_page) << 24) |
			(static_cast<std::uint64_t>(p_command.pipeline) << 16) |
			static_cast<std::uint64_t>(p_command.variant);
	const auto found = far_materials_.find(key);
	if (found != far_materials_.end())
		return found->second;
	const Ref<Shader> shader = far_shader_for(p_command.pipeline, p_thermal);
	if (shader.is_null())
		return Ref<ShaderMaterial>();
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	material->set_render_priority(opennova::renderer::kRungParticleFarSide);
	bind_page(material, p_command.atlas_page, p_pages);
	far_materials_.emplace(key, material);
	return material;
}

} // namespace godot
