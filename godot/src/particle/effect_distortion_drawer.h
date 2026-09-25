#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <runtime/renderer/tracer_frame.h>

#include "render/frame_fx_distortion.h"

namespace godot {

class ParticleCompositorEffect;

// The effects device of FrameFX's type-0 row (render/frame_fx_distortion.h):
// the effect world's distortion subset and the tracer pool's distortion
// ribbons, drawn over the finished frame with slot 2 = the row's work
// targets (retail render_projected_shadow @ 0x5838F8 / @ 0x583928).
//
// Main thread: ParticleRenderer publishes the distortion particle subset
// through its own compositor effect (attached to no camera) plus the frame's
// pass fog; FirePresenter publishes the distortion ribbons. Render thread:
// FrameFX calls draw_distortion inside its terminal effect.
class EffectDistortionDrawer final : public FrameFxDistortionDrawer {
public:
	EffectDistortionDrawer();
	~EffectDistortionDrawer() override;

	// The compositor effect carrying the distortion particle subset.
	void set_particle_effect(const Ref<ParticleCompositorEffect> &p_effect);
	Ref<ParticleCompositorEffect> get_particle_effect() const;
	// This frame's content gate halves: a live class-7 emitter (the misnamed
	// CNapiSession_HasActiveDataTransfer @ 0x5F6640) and an active channel of a
	// distortion style (CEffectEmitterPool_HasDistortionChannels @ 0x5DB7F0).
	void set_particles_present(bool p_present);
	void set_ribbon_channels_present(bool p_present);
	// The pass fog the ribbons blend toward (SetFogAndBlendMode mode 0,
	// retail CEffectChannel_RenderRibbon @ 0x5DC0A6).
	void set_pass_fog(const std::array<float, 3> &p_color, float p_start, float p_end,
			std::int32_t p_type);
	// The distortion ribbons of this frame (compile_tracer_ribbons,
	// TracerPass::Distortion); an empty frame clears them.
	void publish_ribbons(const opennova::renderer::TracerRibbonFrame &p_frame);

	bool frame_has_distortion() const override;
	bool draw_distortion(const FrameFxDistortionTarget &p_target,
			opennova::renderer::FrameFxDistortionSet p_set, std::size_t &r_draws) override;

	// Releases the ribbon RenderingDevice objects (the owning renderer's
	// shutdown, while the RenderingServer is known-live).
	void release_device_resources();

	// Diagnostics: the ribbon indices of the last publish and the draw calls
	// the last ribbon set issued.
	std::size_t published_ribbon_indices() const;
	std::size_t drawn_ribbon_draws() const;

private:
	struct RibbonSubmission {
		PackedByteArray vertices; // TracerVertex x N
		PackedByteArray indices;  // uint32 x M
		std::uint32_t index_count = 0;
	};
	struct Fog {
		std::array<float, 3> color{0.5f, 0.6f, 0.8f};
		float start = 30000.0f;
		float end = 100000.0f;
		std::int32_t type = 1;
	};

	bool draw_ribbons(const FrameFxDistortionTarget &p_target, std::size_t &r_draws);
	bool ensure_ribbon_device(RenderingDevice *p_rd);
	RID ribbon_pipeline_for(int64_t p_framebuffer_format);

	mutable std::mutex mutex_;
	Ref<ParticleCompositorEffect> particle_effect_;
	std::shared_ptr<const RibbonSubmission> ribbons_;
	Fog fog_;
	std::atomic<bool> particles_present_{false};
	std::atomic<bool> ribbon_channels_present_{false};
	std::atomic<std::size_t> drawn_ribbon_draws_{0};

	// Render-thread ribbon device state.
	RenderingDevice *rd_ = nullptr;
	RID shader_;
	int64_t vertex_format_ = -1;
	RID vertex_buffer_;
	std::uint32_t vertex_capacity_ = 0;
	RID index_buffer_;
	std::uint32_t index_capacity_ = 0;
	RID index_array_;
	RID framebuffer_;
	RID framebuffer_color_;
	RID framebuffer_depth_;
	RID uniform_set_;
	RID uniform_texture_;
	RID uniform_sampler_;
	std::map<int64_t, RID> pipelines_;
	PackedByteArray push_constants_;
};

} // namespace godot
