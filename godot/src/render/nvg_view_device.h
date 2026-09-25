#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/renderer/nvg_scope_lens.h>

namespace godot {

// The NVG view's draws beyond the full-screen composite, on the
// RenderingDevice: the Sighted arm's SIGHTS card into the NVG scene (retail
// terrain_scene_render @0x5d08cb..0x5d0952 -> draw_weapon_sight_overlays
// @0x4dce00), and the Scoped arm's lens -- the 64 x 16 polar unwrap of the
// NVG scene, then the frame cleared black and the lens's five band passes and
// ring drawn over it (runtime/renderer/nvg_scope_lens.h carries the geometry
// and every witness; retail draw_minimap_compass_border @0x5d1d10 and
// terrain_scene_render @0x5d0a0e..0x5d0eb4). Every draw is submitted as
// retail submits it: pre-transformed vertices whose pixel-space coordinates
// land on D3D9 pixel centres. FrameFxCompositorEffect owns one and calls it
// from its render thread.
class NvgViewDevice {
public:
	// One SIGHTS card row in the NVG scene's pixels: its RD texture, its rect
	// (retail's fill_fullscreen_quad_vertices corners, draw_weapon_sight_overlays
	// @0x4dd123) and its DefSightBlendMode (the card's six-mode material map,
	// godot/game/world/hud_sights_card.gd; retail
	// WeaponDef_CreateBlendNamedMaterial @0x540180).
	struct SightsRow {
		RID texture;
		float x1 = 0.0f;
		float y1 = 0.0f;
		float x2 = 0.0f;
		float y2 = 0.0f;
		int blend = 0;
	};

	struct Inputs {
		RID frame_framebuffer;   // the frame colour, drawn over
		RID scene;               // the 512-square NVG scene
		RID glow;                // the persistent 256-square glow
		RID clamp_sampler;       // the NVG targets' LINEAR / CLAMP sampler
		RID repeat_sampler;      // the polar target's default WRAP
		Vector2i screen_size;    // the surface the lens's pixels span
		std::shared_ptr<const opennova::renderer::NvgScopeLens> lens;
	};

	NvgViewDevice();
	~NvgViewDevice();

	// Draws the polar passes and the lens; false with `r_failure` set when the
	// device refused a resource. `r_draws` counts the strip draws.
	bool draw(RenderingDevice *p_rd, const Inputs &p_inputs, std::size_t &r_draws,
			std::string &r_failure);
	// Draws the card's rows, in order, over the scene target (its size
	// `p_scene_size`) before its glow and tint read it.
	bool draw_sights(RenderingDevice *p_rd, const RID &p_scene_framebuffer,
			const Vector2i &p_scene_size, const RID &p_sampler,
			const std::vector<SightsRow> &p_rows, std::size_t &r_draws,
			std::string &r_failure);
	void release(RenderingDevice *p_rd);

private:
	enum class Blend : std::uint8_t {
		Replace = 0,
		Add = 1,                  // ONE / ONE
		SourceAlphaBlend = 2,     // SRCALPHA / INVSRCALPHA
		SourceAlphaAdd = 3,       // SRCALPHA / ONE
		DestColorSourceColor = 4, // DESTCOLOR / SRCCOLOR
	};

	RID shader_;
	RID vertices_;
	RID vertex_uniform_;
	RID polar_;
	RID polar_framebuffer_;
	std::map<std::tuple<int64_t, Blend, bool>, RID> pipelines_;
	// The texture uniform sets by (texture, sampler), rebuilt when a target
	// is reallocated.
	std::map<std::tuple<RID, RID>, RID> texture_uniforms_;
	std::shared_ptr<const opennova::renderer::NvgScopeLens> uploaded_lens_;
	PackedByteArray push_;

	bool initialize(RenderingDevice *p_rd, std::string &r_failure);
	RID pipeline_for(RenderingDevice *p_rd, int64_t p_format, Blend p_blend,
			bool p_write_alpha);
	RID texture_uniform(RenderingDevice *p_rd, const RID &p_texture, const RID &p_sampler);
	bool upload_lens(RenderingDevice *p_rd,
			const std::shared_ptr<const opennova::renderer::NvgScopeLens> &p_lens,
			std::string &r_failure);
	void set_push(const Vector2i &p_target, int p_mode, int p_first,
			float p_x1 = 0.0f, float p_y1 = 0.0f, float p_x2 = 0.0f, float p_y2 = 0.0f);
};

} // namespace godot
