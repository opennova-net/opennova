#include "render/texture_filter_device.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <atomic>

namespace godot {

namespace {

using opennova::renderer::TexFilterState;
using opennova::renderer::TextureStage;

const opennova::renderer::TexFilterState kFreshProfile =
		opennova::renderer::texfilter_state(opennova::renderer::kTexFilterLevelFreshProfile,
				opennova::renderer::kTexFilterLevelFreshProfile);

// The RD object passes read the effect code on the render thread.
std::atomic<int> g_device_code{opennova::renderer::shader_filter_code(
		opennova::renderer::stage_sampler(TextureStage::TerrainDetail, kFreshProfile))};
std::atomic<int> g_effect_code{opennova::renderer::shader_filter_code(
		opennova::renderer::stage_sampler(TextureStage::ObjectStage, kFreshProfile))};

} // namespace

void TextureFilterDevice::publish(const TexFilterState &p_state) {
	const int device_code = opennova::renderer::shader_filter_code(
			opennova::renderer::stage_sampler(TextureStage::TerrainDetail, p_state));
	const int effect_code = opennova::renderer::shader_filter_code(
			opennova::renderer::stage_sampler(TextureStage::ObjectStage, p_state));
	g_device_code.store(device_code);
	g_effect_code.store(effect_code);
	RenderingServer *server = RenderingServer::get_singleton();
	if (server == nullptr) {
		return;
	}
	server->global_shader_parameter_set("opennova_texfilter_device", device_code);
	server->global_shader_parameter_set("opennova_texfilter_effect", effect_code);
}

int TextureFilterDevice::device_filter_code() {
	return g_device_code.load();
}

int TextureFilterDevice::effect_filter_code() {
	return g_effect_code.load();
}

Viewport::AnisotropicFiltering TextureFilterDevice::viewport_anisotropy(const TexFilterState &p_state) {
	const int anisotropy = opennova::renderer::hardware_max_anisotropy(
			opennova::renderer::stage_sampler(TextureStage::TerrainDetail, p_state));
	if (anisotropy >= 16) return Viewport::ANISOTROPY_16X;
	if (anisotropy >= 8) return Viewport::ANISOTROPY_8X;
	if (anisotropy >= 4) return Viewport::ANISOTROPY_4X;
	if (anisotropy >= 2) return Viewport::ANISOTROPY_2X;
	return Viewport::ANISOTROPY_DISABLED;
}

void TextureFilterDevice::apply_viewport(Viewport *p_viewport, const TexFilterState &p_state) {
	if (p_viewport == nullptr) {
		return;
	}
	const Viewport::AnisotropicFiltering level = viewport_anisotropy(p_state);
	if (p_viewport->get_anisotropic_filtering_level() != level) {
		p_viewport->set_anisotropic_filtering_level(level);
	}
}

} // namespace godot
