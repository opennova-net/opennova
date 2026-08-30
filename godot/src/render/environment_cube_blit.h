#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/renderer/environment_cube.h>

namespace godot {

// The environment cube's device copy leg. After the six face viewports have
// rendered, each face's RenderingDevice target is drawn into one layer of an
// RD cubemap (RGBA8 UNORM, six layers) by a full-screen triangle that applies
// the face's orientation and the witnessed 0x60 byte multiply
// (opennova::renderer::environment_cube_dim_byte) in integer arithmetic, so
// the layer bytes equal the former CPU convert exactly. Nothing is read back
// to the CPU and no Image or Cubemap resource exists; the cube RID is
// published through a TextureCubemapRD by the capture node.
//
// Threads: set_request runs on the main thread with no request in flight
// (completed_requests() == the caller's request count); publish and release
// run on the render thread through RenderingServer::call_on_render_thread.
// The counters are the only state read across threads.
class EnvironmentCubeBlit {
public:
	static constexpr int kFaceCount =
			opennova::renderer::kEnvironmentCubeFaceCount;
	static constexpr int kFaceSize = opennova::renderer::kEnvironmentCubeFaceSize;

	// How a rendered face lands in its cube layer. A capture camera looks
	// outward from the cube center; the cubemap layer convention addresses a
	// face as viewed inward, so each side face reflects U, and the bottom/top
	// faces (whose mapped retail up axes are +X/-X) are anti-transposed and
	// transposed. The index mapping equals the former Image path exactly:
	// MirrorU = flip_x; AntiTranspose = rotate_90(CLOCKWISE) + flip_y;
	// Transpose = rotate_90(COUNTERCLOCKWISE) + flip_y.
	enum class Orientation : std::uint32_t {
		Copy = 0,
		MirrorU = 1,
		AntiTranspose = 2,
		Transpose = 3,
	};

	struct FaceSource {
		// The RenderingServer texture of the face viewport (its render target).
		RID viewport_texture;
		Orientation orientation = Orientation::Copy;
	};
	// Indexed by cube layer: +X, -X, +Y, -Y, +Z, -Z.
	using Request = std::array<FaceSource, kFaceCount>;

	EnvironmentCubeBlit() = default;
	EnvironmentCubeBlit(const EnvironmentCubeBlit &) = delete;
	EnvironmentCubeBlit &operator=(const EnvironmentCubeBlit &) = delete;

	void set_request(const Request &p_request);
	// Render thread: resolves the request's device textures, lazily creates
	// the shader, pipeline, cube and per-layer framebuffers, and draws the six
	// faces. Completes the request either way; a shader/pipeline failure is
	// latched until release().
	void publish();
	// Render thread (or the main thread after RenderingServer::force_sync):
	// frees every device object and resets the counters.
	void release();

	// The RD cubemap; valid once a publish has succeeded, until release().
	RID cube_texture() const { return cube_texture_; }
	std::uint64_t completed_requests() const {
		return completed_requests_.load(std::memory_order_acquire);
	}
	std::uint64_t published_cubes() const {
		return published_cubes_.load(std::memory_order_acquire);
	}
	bool last_request_ok() const {
		return last_request_ok_.load(std::memory_order_acquire);
	}
	bool device_failed() const {
		return device_failed_.load(std::memory_order_acquire);
	}
	String failure() const;

private:
	bool ensure_shader(RenderingDevice *rd);
	bool ensure_cube(RenderingDevice *rd);
	bool ensure_pipeline(RenderingDevice *rd);
	bool ensure_layer_uniform(RenderingDevice *rd, int layer,
			const RID &face_texture);
	bool draw_layer(RenderingDevice *rd, int layer, Orientation orientation);
	void set_failure(const std::string &reason);
	void finish(bool ok);

	Request request_{};
	RenderingDevice *rd_ = nullptr;
	RID shader_;
	RID sampler_;
	RID pipeline_;
	RID cube_texture_;
	std::array<RID, kFaceCount> layer_views_{};
	std::array<RID, kFaceCount> layer_framebuffers_{};
	std::array<RID, kFaceCount> layer_uniforms_{};
	std::array<RID, kFaceCount> layer_uniform_sources_{};
	PackedByteArray push_constants_;

	mutable std::mutex failure_mutex_;
	std::string failure_;
	std::atomic<std::uint64_t> completed_requests_{0};
	std::atomic<std::uint64_t> published_cubes_{0};
	std::atomic<bool> last_request_ok_{false};
	std::atomic<bool> device_failed_{false};
};

} // namespace godot
