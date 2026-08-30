#pragma once

#include <cstdint>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/cubemap.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <runtime/renderer/environment_cube.h>

#include "terrain/terrain_data.h"

namespace godot {

// Hosts retail's highest-quality TexCubeEnvironment: six 256-square sky/
// celestial captures at the local player, refreshed together every 128 render
// frames. The callback intentionally sees only the sky dome and sun/moon; the
// static sun-aligned CubeRotSpecular sphere is evaluated analytically by the
// object shader after the captured sky has received its 0x60 dim multiply.
// The constants, cadence, eye placement and dim byte are the engine's
// <runtime/renderer/environment_cube.h>; this node is the device host.
// Exact witness addresses and the quality selector are maintained in
// docs/render/render-lighting-re.md (CubeEnvironment section):
// (update_environment_cubemap @0x6106a0; GTexture_RenderCubeMapFace @0x6864d0;
// environment face callback @0x5c3700 - docs/render/render-lighting-re.md).
class EnvironmentCubeCapture : public Node {
	GDCLASS(EnvironmentCubeCapture, Node)

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
	Ref<Cubemap> get_environment_cube() const { return environment_cube_; }
	PackedVector3Array get_face_directions() const;
	PackedVector3Array get_face_up_vectors() const;

	void _notification(int p_what);

protected:
	static void _bind_methods();

private:
	void _ensure_capture_nodes();
	void _request_capture(const Vector3 &p_player_position);
	bool _publish_completed_capture();
	void _publish_inactive();
	static Ref<Image> _to_retail_dimmed_face(const Ref<Image> &p_source,
			int p_orientation);

	Ref<TerrainData> terrain_data_;
	SubViewport *viewports_[kFaceCount]{};
	Camera3D *cameras_[kFaceCount]{};
	Ref<Cubemap> environment_cube_;
	Vector3 capture_origin_;
	int64_t render_frame_index_ = 0;
	bool capture_pending_ = false;
	bool cube_ready_ = false;
	bool force_pending_ = true;
};

} // namespace godot
