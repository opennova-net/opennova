#pragma once

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
	const uint32_t argb = opennova::renderer::kSlotCaptureClearArgb;
	return Color(float((argb >> 16) & 0xFF) / 255.0f,
			float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f,
			float((argb >> 24) & 0xFF) / 255.0f);
}

// One armed render-slot silhouette capture for this frame: the typed request
// SlotShadow publishes per armed slot order (engine/runtime/renderer/
// render_slot_shadow.h carries the planner laws that arm it).
struct SlotCaptureRequest {
	int order = 0;
	// renderer::slot_texture_size(order, detail): the square target side.
	int size = 0;
	// The capture pose (renderer::silhouette_capture_basis columns, the eye
	// backed off along -forward by renderer::silhouette_capture_eye) and the
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
	int unclassified_surfaces = 0;
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
	// The surfaces compiled for one order in the published frame (0 when the
	// order was not armed); an inspection seam for the GUT pins.
	int compiled_surface_count(int p_order) const;
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
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

} // namespace godot
