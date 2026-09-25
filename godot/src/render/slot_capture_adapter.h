#pragma once

#include "util/color_convert.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/renderer/render_slot_shadow.h>

namespace godot {

class RenderingDevice;

// The retail slot RT clear as a device colour: renderer::kSlotCaptureClearArgb
// (0x00FFFFFF, white RGB and alpha 0) decoded once for the capture pass's
// clear and for a fresh resolve target that no capture has landed on yet.
inline Color slot_capture_clear_color() {
	return opennova::color_from_argb(opennova::renderer::kSlotCaptureClearArgb);
}

// One armed render-slot silhouette capture for this frame: the typed request
// SlotShadow publishes per armed slot order (engine/runtime/renderer/
// render_slot_shadow.h carries the planner laws that arm it).
struct SlotCaptureRequest {
	int order = 0;
	// renderer::slot_texture_size(order, detail): the square target side.
	int size = 0;
	// The capture pose (renderer::slot_capture_view_axes columns, the eye
	// backed off from the entity origin along -forward) and the
	// orthographic projection of half-extent renderer::silhouette_half_extent;
	// the adapter applies the RenderingDevice depth correction itself.
	Transform3D view;
	Projection projection;
	// The resolve texture SlotShadow owns for this order (RGBA8, sampled by the
	// drape through a Texture2DRD). Invalid without a RenderingDevice.
	RID target;
	// ObjectModel instance ids: the admitted caster and the capture-with
	// children claimed into its slot (the RenderSlot_RenderEntityAndChildren
	// child walk). The adapter walks their visible geometry.
	std::vector<uint64_t> casters;
};

// Per-frame counters for the F3 board (VALUE slots) and the perf probe.
struct SlotCaptureFrameCounters {
	// Main-thread compile of the latest published frame.
	int captures_compiled = 0;
	int surfaces_compiled = 0;
	int skinned_commands = 0;
	// Commands on the _FFP alpha-blend PROJSHAD variant (SRCALPHA/
	// INVSRCALPHA by coverage instead of the opaque replace).
	int blended_commands = 0;
	int packed_vertices = 0;
	// ArrayMesh surfaces under a caster whose material carries no registered
	// object classification (drawn nothing); non-ArrayMesh instances are
	// skipped before classification and are not counted here.
	int unclassified_surfaces = 0;
	// Classified surfaces whose technique declares no PROJSHAD pass, plus the
	// material-blend additive variants (black added is a no-op).
	int no_pass_surfaces = 0;
	// Render-thread draw of the last frame that reached the device.
	int captures_drawn = 0;
	int draw_calls = 0;
};

// Device adapter for the render-slot silhouette captures (the RD twin of the
// FrameFx focused-Q3 adapter): the main thread compiles each request's caster
// geometry into an immutable frame (Q3GeometryCache streams, the PROJSHAD
// coverage policy from the registered ObjectMaterialClassification through
// the engine's object_projected_shadow_coverage/policy tables, the textures
// that policy samples), and the compositor draws it on the render thread into
// per-order MSAA colour targets resolved into SlotShadow's textures.
class SlotCaptureAdapter {
public:
	SlotCaptureAdapter();
	~SlotCaptureAdapter();

	void compile_frame(const std::vector<SlotCaptureRequest> &p_requests);
	void clear_frame();
	// Render-side: consume evictions, upload streams and bone palettes, draw
	// every capture of the published frame, resolve into its target. Returns
	// false on a device failure (the report keeps the reason).
	bool draw(RenderingDevice *p_rd);
	// Free resources only through the caller's known-live device; a null
	// device discards cached handles without dereferencing the old pointer.
	void release_device(RenderingDevice *p_rd);

	SlotCaptureFrameCounters frame_counters() const;
	Dictionary get_report() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

// The PRE_OPAQUE compositor effect SlotShadow installs on the beauty view's
// WorldEnvironment compositor: the captures complete before the terrain
// drape samples them in the same frame, the ordering the SubViewport chain
// used to guarantee by rendering ahead of the main viewport.
class SlotCaptureCompositorEffect : public CompositorEffect {
	GDCLASS(SlotCaptureCompositorEffect, CompositorEffect)

private:
	std::unique_ptr<SlotCaptureAdapter> adapter_;
	std::atomic<bool> shutdown_requested_{false};
	// F3-only GPU timing (rd_timestamp_span.h carries the barrier contract);
	// the harvested span is atomics so the render callback takes no new lock.
	std::atomic<bool> gpu_timing_enabled_{false};
	std::atomic<std::uint64_t> gpu_span_us_{0};
	std::atomic<bool> gpu_span_valid_{false};

protected:
	static void _bind_methods();

public:
	SlotCaptureCompositorEffect();
	~SlotCaptureCompositorEffect() override;

	SlotCaptureAdapter &adapter() { return *adapter_; }
	const SlotCaptureAdapter &adapter() const { return *adapter_; }
	// Process-exit / exit-tree boundary: stop render callbacks and release
	// the device resources while the RenderingDevice is still live.
	void release_device_resources();
	bool is_shutdown() const {
		return shutdown_requested_.load(std::memory_order_acquire);
	}
	// F3 Stats capture toggle for the pass's RD GPU span (carved out of the
	// root viewport's GPU row while the Stats tab captures).
	void set_gpu_timing_enabled(bool p_enabled) {
		gpu_timing_enabled_.store(p_enabled, std::memory_order_relaxed);
	}
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

} // namespace godot
