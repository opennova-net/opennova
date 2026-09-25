#pragma once

#include <cstddef>
#include <cstdint>

#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <runtime/renderer/frame_fx_effects.h>

namespace godot {

// The device seam of FrameFX's type-0 row (retail FrameFX_DistortionPass
// @0x583720): after the frame capture, its 256A downsample and the jittered
// 256A -> 256B pass, the effect world draws its distortion particles with
// texture slot 2 = 256A (EffectWorld_DrawParticles renderFlags 4 through the
// EffectWorld_RenderDistortionPass, called @0x5838f8), then the tracer pool its
// distortion ribbons with slot 2 = 256B (CEffectEmitterPool_RenderDistortionPass
// @0x583928). The terminal FrameFX effect owns the capture, the work targets
// and the order; the effects device owns what it draws.
struct FrameFxDistortionTarget {
	RenderingDevice *rd = nullptr;
	RenderData *render_data = nullptr;
	std::uint32_t view = 0;
	// The frame colour (gamma domain, before the terminal decode) and the
	// resolved beauty depth the sets draw against.
	RID color;
	RID depth;
	// Slot 2: the 256 x 256 RGBA8 work target of this set, and the FrameFX
	// render-target sampler (clamp, linear).
	RID screen_texture;
	RID screen_sampler;
	// The projective screen-texture transform retail builds from the scene
	// matrix (FrameFX_DistortionPass @0x5837ff..0x5838d2):
	// u = 0.5 ndc.x + bias, v = -0.5 ndc.y + bias, bias = 0.5 + half a 256 texel.
	float ndc_scale_u = 0.5f;
	float ndc_scale_v = -0.5f;
	float bias = 0.501953125f;
};

// Implemented by the effects device; registered with
// FrameFx::set_distortion_drawer.
class FrameFxDistortionDrawer {
public:
	virtual ~FrameFxDistortionDrawer() = default;
	// Main thread, after this frame's particle and tracer publication: the
	// row's content gate (CEffectEmitterPool_HasDistortionChannels @0x5db7f0
	// || the effect world's distortion-particle test, the misnamed
	// EffectWorld_HasDistortionParticles @0x5f6640).
	virtual bool frame_has_distortion() const = 0;
	// Render thread, inside the terminal FrameFX effect. Returns false on a
	// device failure; adds its draw calls to `r_draws`.
	virtual bool draw_distortion(const FrameFxDistortionTarget &p_target,
			opennova::renderer::FrameFxDistortionSet p_set, std::size_t &r_draws) = 0;
};

} // namespace godot
