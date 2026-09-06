#pragma once

#include "util/preview_properties.h"

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_cubemap_rd.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <runtime/renderer/environment_cube.h>

#include "render/environment_cube_blit.h"
#include "terrain/terrain_data.h"

namespace godot {

// Hosts retail's highest-quality TexCubeEnvironment: six 256-square sky/
// celestial captures at the local player, refreshed together every 128 render
// frames. The callback intentionally sees only the sky dome and sun/moon; the
// static sun-aligned CubeRotSpecular sphere is evaluated analytically by the
// object shader after the captured sky has received its 0x60 dim multiply.
// The constants, cadence, eye placement and dim byte are the engine's
// <runtime/renderer/environment_cube.h>; this node is the device presenter: six
// SubViewport faces and the RenderingDevice copy of their targets into the
// published cubemap (render/environment_cube_blit.h). Exact witness
// addresses and the quality selector are maintained in
// docs/render/render-lighting-re.md (CubeEnvironment section).
class EnvironmentCubeCapture : public Node {
	GDCLASS(EnvironmentCubeCapture, Node)

	PreviewProperties preview_properties_;

public:
	// GameWorld owns the source-derived state while its editor preview is active.
	void set_preview_configuration(bool enabled) {
		if (enabled) preview_properties_.begin(this, { "terrain_data" });
		else preview_properties_.end(this);
	}

protected:
	void _validate_property(PropertyInfo &property) const { preview_properties_.validate(property); }

public:
	static constexpr int kFaceCount =
			opennova::renderer::kEnvironmentCubeFaceCount;
	static constexpr int kCaptureSize =
			opennova::renderer::kEnvironmentCubeFaceSize;
	static constexpr int64_t kRefreshFrames =
			opennova::renderer::kEnvironmentCubeRefreshFrames;
	static constexpr uint8_t kSkyDimByte =
			opennova::renderer::kEnvironmentCubeDimByte;

	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const { return terrain_data_; }

	// GameWorld's ordered device leg. p_player_position is the simulation's
	// local-player origin, not the first-person camera offset.
	void advance_frame(const Vector3 &p_player_position);
	void force_capture();

	bool is_cube_ready() const { return cube_ready_; }
	bool is_capture_pending() const { return capture_pending_; }
	int64_t get_render_frame_index() const { return render_frame_index_; }
	Vector3 get_capture_origin() const { return capture_origin_; }
	// The published cube: a TextureCubemapRD over the blit's RD cubemap, the
	// same object the opennova_environment_cube shader global carries.
	Ref<TextureCubemapRD> get_environment_cube() const {
		return environment_cube_;
	}
	// Device publications (six-face copies) completed since the blit was
	// armed; the GUT/probe pins read it, nothing else does.
	int64_t get_device_publish_count() const;
	// The blit's last failure reason (empty when the device leg is healthy).
	String get_device_failure() const;
	PackedVector3Array get_face_directions() const;
	PackedVector3Array get_face_up_vectors() const;

	void _notification(int p_what);

protected:
	static void _bind_methods();

private:
	void _ensure_capture_nodes();
	void _request_capture(const Vector3 &p_player_position);
	bool _publish_completed_capture();
	bool _wire_cube(RenderingServer *p_rs);
	void _publish_on_render_thread();
	void _release_on_render_thread();
	void _publish_inactive();

	Ref<TerrainData> terrain_data_;
	SubViewport *viewports_[kFaceCount]{};
	Camera3D *cameras_[kFaceCount]{};
	std::unique_ptr<EnvironmentCubeBlit> blit_;
	Ref<TextureCubemapRD> environment_cube_;
	Vector3 capture_origin_;
	int64_t render_frame_index_ = 0;
	// Copies handed to the render thread, and the number of them whose cube
	// has been wired to the shader global; the blit's completed count closes
	// the loop (one frame later under a threaded render model, immediately
	// under the default one).
	uint64_t requested_publishes_ = 0;
	uint64_t wired_publishes_ = 0;
	bool capture_pending_ = false;
	bool cube_ready_ = false;
	bool force_pending_ = true;
	bool warned_device_failure_ = false;
};

} // namespace godot
