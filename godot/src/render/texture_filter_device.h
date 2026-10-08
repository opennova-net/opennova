#pragma once

// The device leg of the per-stage texture filters (D-RMAT-22). The engine
// decides every stage's sampler from game.cfg's texfilter_level
// (runtime/renderer/texture_filter.h); this leg programs the Godot side:
// - the shader globals opennova_texfilter_device / opennova_texfilter_effect
//   (texture_filter.gdshaderinc), the filter codes of the terrain detail
//   family and of the model stages;
// - the world viewports' hardware anisotropy, which only the detail family's
//   sampler hint reads;
// - the effect code the RenderingDevice object passes (the Q3 copies and the
//   slot captures) read at their draw.

#include <runtime/renderer/texture_filter.h>

#include <godot_cpp/classes/viewport.hpp>

namespace godot {

class TextureFilterDevice {
public:
	// Publish a frame state: both shader globals and the RD passes' code.
	static void publish(const opennova::renderer::TexFilterState &p_state);
	// The filter codes last published (the fresh profile's before the first
	// publish, as project.godot's shader globals hold them).
	static int device_filter_code();
	static int effect_filter_code();
	// The viewport anisotropy the terrain detail family's sampler needs: off
	// unless the device mode is anisotropic, else the power of two at or
	// below its MaxAnisotropy (Godot's levels stop at 16x).
	static Viewport::AnisotropicFiltering viewport_anisotropy(
			const opennova::renderer::TexFilterState &p_state);
	static void apply_viewport(Viewport *p_viewport,
			const opennova::renderer::TexFilterState &p_state);
};

} // namespace godot
