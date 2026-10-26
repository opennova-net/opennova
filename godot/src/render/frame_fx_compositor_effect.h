#pragma once

// FrameFx's terminal compositor effect, split from FrameFx's TU pair: the device leg every
// FrameFx view and every DisplayDecode view ends with (frame_fx.h, display_decode.h).

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/renderer/frame_fx_effects.h>
#include <runtime/renderer/nvg_scope_lens.h>

#include "render/frame_fx_distortion.h"
#include "render/nvg_view_device.h"

namespace godot {

class Camera3D;
class Node;
class Viewport;

// One frame's FrameFX screen-effect plan as the terminal effect consumes it:
// the planner's rows (runtime/renderer/frame_fx_effects.h), the effects
// device that draws the type-0 distortion sets, on the NVG lens arm the lens
// over the surface's overlay rect (runtime/renderer/nvg_scope_lens.h) with
// the surface size its pixels span, and on the NVG Sighted arm the SIGHTS
// card's rows in the NVG scene's pixels.
struct FrameFxScreenFrame {
	std::uint64_t frame_id = 0;
	opennova::renderer::FrameFxFramePlan plan;
	std::shared_ptr<FrameFxDistortionDrawer> distortion;
	std::shared_ptr<const opennova::renderer::NvgScopeLens> lens;
	Vector2i screen_size;
	std::vector<NvgViewDevice::SightsRow> nvg_sights;
};

// The frame's terminal compositor effect: the post-transparent device leg
// that runs retail's FrameFX over the finished scene and performs the sole
// display decode (for a view whose environment carries FrameFx's decode
// table, Godot's tonemap performs it instead). It executes the planner's DrawPass rows in dispatch order
// (distortion, death or damage blur, the bloom, thermal and monitor) or, in
// the first-person NVG view, the NVG scene/glow/composite chain in their
// place (Render_ProcessMainSceneFrame @0x5ca8f6..0x5caad5 / @0x5ca6ab; the
// bloom kernel FrameFX_BloomKernel @0x5841d0 over the Q3 source FrameFX_RenderGlowSource
// @0x582940 draws and FrameFX_CaptureAltBuffer @0x584020 captures; targets
// FrameFX_CreateRenderTargets @0x583c40).
// The Q3 source is an effect-owned full-resolution color target sharing the
// resolved beauty depth; the capture is the power-of-two floor of the frame.
class FrameFxCompositorEffect : public CompositorEffect {
	GDCLASS(FrameFxCompositorEffect, CompositorEffect)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;

	void create_glow_source_device_on_render_thread();
	void create_view_effects_device_on_render_thread();
	void create_frame_fx_device_on_render_thread();

protected:
	static void _bind_methods();

public:
	FrameFxCompositorEffect();
	~FrameFxCompositorEffect() override;

	// The device objects the renderer creates ahead of the first frame, each queued on the
	// render thread (rd_glsl.h call_device_create); the render callback's own creates then
	// find them. The Q3 glow source's (the effect set's GLOW passes) at the renderer's start;
	// the NVG view's (the view effects') and FrameFX's own shaders and samplers (its pixel
	// shaders') at the mission textures stage.
	void create_glow_source_device();
	void create_view_effects_device();
	void create_frame_fx_device();
	void compile_q3_frame(Node *p_scope, Viewport *p_viewport,
			Camera3D *p_camera);
	void clear_q3_frame();
	// Main thread: the frame's screen-effect plan, consumed by the next render
	// (a frame's one-shot parts, the NVG glow clear, run once per frame id).
	void publish_screen_effects(const std::shared_ptr<const FrameFxScreenFrame> &p_frame);
	// Main thread: the mission's "ffscan" texels (frame_fx_scanline_texels).
	void publish_scanline_texels(const std::vector<std::uint8_t> &p_texels);
	void release_device_resources();
	// F3-only GPU timing: RD timestamps around the pass carve its span out of
	// the root viewport's GPU row. capture_timestamp barriers the RD graph, so
	// this stays off unless the Stats capture is live.
	void set_gpu_timing_enabled(bool p_enabled);
	Dictionary get_backend_report() const;
	// Inspection seam for the GUT pins: the first view's focused Q3 colour
	// target as drawn by the last completed frame. Call only after
	// RenderingServer.force_sync(); an empty Ref means no target exists (no
	// RenderingDevice, no frame yet, or an unmapped attachment format).
	Ref<Image> capture_q3_target();
	// Main thread: the environment whose views Godot's tonemap decodes through
	// FrameFx's display decode table (FrameFx::install_display_table); a view
	// drawn with it runs no terminal decode here. An invalid RID clears it.
	void set_display_table_environment(const RID &p_environment);

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

// A compositor carrying every effect of `previous` (minus any earlier copy of
// `terminal`) with `terminal` appended last: the FrameFX composite and the
// display decode must run after every other post-transparent effect.
Ref<Compositor> compositor_with_terminal(const Ref<Compositor> &previous,
		const Ref<CompositorEffect> &terminal);

} // namespace godot
