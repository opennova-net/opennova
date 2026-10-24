#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
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

public:
	static constexpr int kFaceCount =
			opennova::renderer::kEnvironmentCubeFaceCount;
	static constexpr int kCaptureSize =
			opennova::renderer::kEnvironmentCubeFaceSize;
	static constexpr int64_t kRefreshFrames =
			opennova::renderer::kEnvironmentCubeRefreshFrames;
	static constexpr uint8_t kSkyDimByte =
			opennova::renderer::kEnvironmentCubeDimByte;

	// The mission start's terrain: the next capture takes all six faces, and
	// the previous mission's cube is unready until it does.
	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const { return terrain_data_; }

	// GameWorld's ordered device leg. p_player_position is the simulation's
	// local-player origin, not the first-person camera offset.
	void advance_frame(const Vector3 &p_player_position);
	void force_capture();
	// The mission load's render subsystems: the cube's device objects (the blit's shader,
	// cubemap and pipeline) ahead of the first capture's copy (engine
	// renderer/environment_cube.h creates_environment_cube carries the witness,
	// D-NET-NEW-LF-13).
	void create_cube_device();

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

	// The hold (ADR 0046 S14, E13), as MissionEnvironment's: while held, the
	// capture writes neither shader global (the cube, its ready flag); it still
	// captures and publishes its own cube. republish_globals writes this
	// capture's pair whatever the hold; publish_absent leaves the pair as a
	// process has it with no capture (not ready, no cube). The game never
	// holds.
	void set_globals_held(bool p_held) { globals_held_ = p_held; }
	bool is_globals_held() const { return globals_held_; }
	void republish_globals();
	static void publish_absent();
	// How many times a capture wrote its globals since the process began, the
	// absent values too: one count per process-wide value written (a read-back
	// for the hold's tests).
	static int64_t get_global_writes() { return global_writes_; }

	void _notification(int p_what);

protected:
	static void _bind_methods();

private:
	void _ensure_capture_nodes();
	void _ensure_blit();
	void _ensure_draw_effect();
	void _uninstall_draw_effect();
	void _request_capture(const Vector3 &p_player_position);
	bool _arm_copy(RenderingServer *p_rs);
	bool _publish_completed_capture();
	bool _bind_cube(RenderingServer *p_rs);
	bool _wire_cube(RenderingServer *p_rs);
	void _arm_on_render_thread();
	void _publish_on_render_thread();
	void _create_on_render_thread();
	void _release_on_render_thread();
	void _publish_inactive();

	Ref<TerrainData> terrain_data_;
	SubViewport *viewports_[kFaceCount]{};
	Camera3D *cameras_[kFaceCount]{};
	std::unique_ptr<EnvironmentCubeBlit> blit_;
	// The copy into the cube inside the frame that renders the faces: the
	// pre-opaque effect on the WorldEnvironment's compositor, and the request
	// the render thread arms it with.
	Ref<EnvironmentCubeCompositorEffect> draw_effect_;
	Ref<Compositor> draw_effect_installed_into_;
	ObjectID world_environment_id_;
	EnvironmentCubeBlit::Request armed_request_{};
	Ref<TextureCubemapRD> environment_cube_;
	Vector3 capture_origin_;
	int64_t render_frame_index_ = 0;
	// Copies armed on the render thread, and the number of them whose cube
	// has been wired to the shader global; the blit's completed count closes
	// the loop (in the request frame's draw, else at the next advance).
	uint64_t requested_publishes_ = 0;
	uint64_t wired_publishes_ = 0;
	// The pending capture's copy is armed (its count is in requested_publishes_).
	bool copy_armed_ = false;
	bool capture_pending_ = false;
	bool cube_ready_ = false;
	bool force_pending_ = true;
	bool warned_device_failure_ = false;
	bool globals_held_ = false;
	static inline int64_t global_writes_ = 0;
	static void _write_global(
			RenderingServer *p_rs, const StringName &p_name, const Variant &p_value);
};

} // namespace godot
