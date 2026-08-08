#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <renderer/particle_frame.h>

namespace godot {

// Immutable, upload-ready atlas catalog. The main thread builds this only
// when the mounted PTL catalog changes; render callbacks retain it by value.
struct NovaParticleAtlasPageSnapshot {
	std::uint8_t type = 0;
	std::uint32_t side = 0;
	PackedByteArray rgba8;
};

struct NovaParticleAtlasSnapshot {
	std::uint64_t generation = 0;
	std::vector<NovaParticleAtlasPageSnapshot> pages;
};

// One immutable World-domain submission. ParticleFrameCompiler remains the
// ordering authority. Vertices are the compiler's quads expanded to triangles
// so every adjacent-state command can bind a byte offset into one retained GPU
// buffer without allocating command-local arrays or index slices.
struct NovaParticleWorldSubmission {
	std::uint64_t frame_id = 0;
	PackedByteArray triangle_vertices;
	std::vector<renderer::ParticleDrawCommand> commands;
	std::shared_ptr<const NovaParticleAtlasSnapshot> atlas;
	std::array<float, 3> camera_position{};
	std::array<float, 3> camera_forward{0.0f, 0.0f, 1.0f};
	std::array<float, 3> fog_color{};
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	std::int32_t fog_type = 1;
	bool valid = true;
	std::string validation_error;
};

// Deep Godot adapter at the packet-to-GPU seam. Callers publish one immutable
// value; this resource owns render-thread synchronization, persistent buffers,
// atlas textures, shader/pipeline/uniform caches, framebuffer attachment, the
// scene-color copy required by Distort, and explicit failure diagnostics.
class NovaParticleCompositorEffect : public CompositorEffect {
	GDCLASS(NovaParticleCompositorEffect, CompositorEffect)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;

protected:
	static void _bind_methods();

public:
	NovaParticleCompositorEffect();
	~NovaParticleCompositorEffect() override;

	void publish(const std::shared_ptr<const NovaParticleWorldSubmission> &p_submission);
	void clear_submission();
	void set_particles_hidden(bool p_hidden);
	// The real World path owns RenderingDevice pipelines whose framebuffer
	// format is known only inside the compositor callback. A loading owner
	// requests the warm here, then force_draw() services it synchronously.
	void request_pipeline_warm();
	void cancel_pipeline_warm();
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

} // namespace godot
