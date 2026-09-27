#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/xr_interface_extension.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot {

class SubViewport;

// A device mechanism, not a port: the one-view XR interface through which a
// square SubViewport rasterises a projection no Camera3D can draw on it.
// Godot builds every non-XR camera projection from its viewport's own
// width/height ratio (square texels only), but the renderer's XR branch takes
// the view transform and the whole projection matrix from the primary XR
// interface, and with one view every consumer (culling, each material's
// PROJECTION_MATRIX, the compositor effects' scene data) reads that matrix.
// Retail rasterises two 512 x 512 targets through the MAIN view's frustum,
// with non-square texels: the NVG scene (engine world::nvg_view_projection)
// and the water mirror (engine env::build_water_mirror_view). Each owner
// serves its target every frame it draws: the target's render target keys
// the camera transform and projection the renderer reads back while it draws
// that target (_pre_draw_viewport selects the entry), and serving turns the
// target's use_xr on. The interface exists (added to the XRServer,
// initialized, primary) only while a target is served, and never takes the
// primary slot from another interface.
class TargetProjectionXrInterface : public XRInterfaceExtension {
	GDCLASS(TargetProjectionXrInterface, XRInterfaceExtension)

public:
	// The raster every served target draws: the XR render-target size is one
	// value for all of the primary interface's viewports, and both retail
	// targets are this square (renderer::kNvgSceneSide, env::kReflectionRttSize).
	static constexpr int kTargetSide = 512;

	// Serve `p_target` for its next draw: its camera transform (the
	// Camera3D::get_camera_transform of the camera it draws through) and the
	// projection in Camera3D's own convention (the matrix get_camera_projection
	// would return). Turns the target's use_xr on, so its raster is
	// kTargetSide x kTargetSide whatever its node size; set that node size
	// before the first serve. Returns false, serving nothing, when no XR
	// server exists or another interface is primary.
	static bool serve(SubViewport *p_target, const Transform3D &p_transform,
			const Projection &p_projection);
	// Stop serving `p_target` (its use_xr goes off, its raster is its node
	// size again). The last release retires the interface.
	static void release(SubViewport *p_target);
	static bool is_serving(SubViewport *p_target);
	// The projection/transform last served for `p_target` (identity when it
	// is not served): the matrices its raster is drawn with.
	static Projection served_projection(SubViewport *p_target);
	static Transform3D served_transform(SubViewport *p_target);
	// Module teardown: the XRServer outlives the extension's classes, so a
	// live interface leaves it here.
	static void cleanup_statics();

	StringName _get_name() const override;
	uint32_t _get_capabilities() const override;
	bool _is_initialized() const override;
	bool _initialize() override;
	void _uninitialize() override;
	Vector2 _get_render_target_size() override;
	uint32_t _get_view_count() override;
	Transform3D _get_camera_transform() override;
	Transform3D _get_transform_for_view(uint32_t p_view, const Transform3D &p_cam_transform) override;
	PackedFloat64Array _get_projection_for_view(uint32_t p_view, double p_aspect,
			double p_z_near, double p_z_far) override;
	bool _pre_draw_viewport(const RID &p_render_target) override;

protected:
	static void _bind_methods();

private:
	struct Entry {
		uint64_t target_id = 0; // the SubViewport's ObjectID
		RID viewport;
		RID render_target;
		Transform3D transform;
		Projection projection;
	};

	static TargetProjectionXrInterface *live();
	static TargetProjectionXrInterface *acquire();
	static void retire(TargetProjectionXrInterface *p_interface);
	// Drop the entries whose SubViewport is gone (callers hold mutex_).
	void prune_dead_entries();
	const Entry *find_by_viewport(const RID &p_viewport) const;

	mutable std::mutex mutex_;
	std::vector<Entry> entries_;
	// The entry the renderer is drawing (selected by _pre_draw_viewport).
	Transform3D drawing_transform_;
	Projection drawing_projection_;
	bool initialized_ = false;
};

} // namespace godot
