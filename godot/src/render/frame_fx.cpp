#include "render/frame_fx.h"
#include "render/q3_frame_adapter.h"
#include "render/q3_source_registry.h"
#include "render/rd_fullscreen.h"
#include "render/rd_timestamp_span.h"
#include "render/rd_uniforms.h"
#include "render/world_environment_lookup.h"
#include "util/string_convert.h"

#include <base/crt/crt_rng.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

using opennova::renderer::FrameFxBuffer;
using opennova::renderer::FrameFxDistortionSet;
using opennova::renderer::FrameFxFramePlan;
using opennova::renderer::FrameFxNvgPlan;
using opennova::renderer::FrameFxPass;
using opennova::renderer::FrameFxStage;
using opennova::renderer::FrameFxStep;
using opennova::renderer::FrameFxStepKind;
using opennova::renderer::FrameFxTaps;

// A normal gameplay camera keeps precisely the beauty layers left after the
// player/fly camera removes the FP body, shadow-only, and the twelve
// slot-capture bits: world, water, the terrain-shadow receiver plumbing bit,
// the no-mirror world layer, the foliage blanket bit (mirror-excluded — the
// witnessed reflection context collects no foliage, env-tod-re.md #30), and
// the first-person viewmodel (bit 11 — the gun draws inside the beauty pass
// through its shader-side renderfov projection and depth band, retail's
// "viewmodel first" step), and the empty-sector flat terrain fallback (bit
// 18, which the water mirror excludes: docs/terrain/terrain-re.md, "Empty-sector
// flat fallback"). Q3 omits the plumbing bit and the viewmodel; leaving the
// gun out is observably equivalent: retail flushes Q3 under the world
// projection and the full viewport (retail Render_SetViewport @ 0x582a45)
// against the beauty depth, where the gun's own band depth hides its copies
// (D-RORD-10). That gives shaders a collision-free exact-mask signature
// without admitting caster or slot-capture geometry anywhere.
constexpr std::uint32_t kBeautyCameraMask = 0x00078C01u;
// FrameFX's 256-square work targets. Focused Q3 is rendered at beauty
// resolution into the compositor's color attachment with resolved beauty
// depth attached; this size applies from the capture's downsample on.
constexpr std::int32_t kWorkSide = opennova::renderer::kFrameFxWorkSide;
constexpr std::uint32_t kPushConstantBytes = 64u;

// The fragment stage a draw evaluates (pc.mode_taps.x).
enum class FramePass : std::int32_t {
	Stretch = 0,
	LumaAverage = 1,
	Weighted = 2,
	Average = 3,
	GammaDecode = 4,
	Snapshot = 5,
	FanAverage = 6,
	Thermal = 7,
	Scanline = 8,
	NvgGlow = 9,
	NvgTint = 10,
	NvgGlowAdd = 11,
};

// The tap geometry a draw samples with (pc.mode_taps.y): the four DrawPass
// builders plus the NVG glow's two fixed four-tap sets.
enum class TapGeometry : std::int32_t {
	None = 0,
	Rotated = 1,
	Weighted = 2,
	RadialFan = 3,
	Tiled = 4,
	NvgDiagonal = 5,
	NvgAxial = 6,
};

enum class BlendMode : std::uint8_t {
	Replace = 0,
	Add = 1,             // ONE / ONE
	SourceAlphaAdd = 2,  // SRCALPHA / ONE
	SourceAlphaBlend = 3, // SRCALPHA / INVSRCALPHA
	DestColorSourceColor = 4, // DESTCOLOR / SRCCOLOR
	GlowAccumulate = 5,  // ONE / INVSRCALPHA
};

// How a draw list treats its target: GTexRT_Select clears a selected
// FrameFX target to 0 before a DrawPass; the frame itself is drawn over.
enum class TargetLoad : std::uint8_t {
	Keep = 0,
	Clear = 1,
	Discard = 2,
};

// One draw's push block (the 64-byte std430 FramePush below).
struct FramePush {
	FramePass pass = FramePass::Stretch;
	TapGeometry taps = TapGeometry::None;
	// The D3D rect the quad spans, in target pixels, and the source's size.
	float rect_w = 0.0f, rect_h = 0.0f;
	float source_w = 0.0f, source_h = 0.0f;
	float base = 0.0f;
	float direction_s = 0.0f, direction_c = 0.0f;
	float u_scale = 1.0f;
	float alpha = 0.0f;
	float radius = 0.0f;
	std::int32_t tile_x = 0, tile_y = 0;
};

// The witnessed pixel stages (runtime/renderer/frame_fx_effects.h carries
// the retail shader text and the CPU references); @TOKEN@ slots are spliced
// from the engine constants so the GLSL never restates them.
const char *kFrameFragmentShaderTemplate = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(push_constant, std430) uniform FramePush {
	vec4 rect_source;
	vec4 base_direction;
	vec4 params;
	ivec4 mode_taps;
} pc;

layout(location = 0) out vec4 frag_color;

const vec3 LUMA = @LUMA@;
const float WEIGHTS[4] = float[4](@WEIGHTS@);
const float WEIGHT_STEPS[4] = float[4](@WEIGHT_STEPS@);
const float FAN_STEPS[4] = float[4](@FAN_STEPS@);
const vec3 THERMAL_LUMA = @THERMAL_LUMA@;
const float THERMAL_GREEN_BIAS = @THERMAL_GREEN_BIAS@;
const vec2 SCANLINE_TILE = @SCANLINE_TILE@;
const float SCANLINE_DIFFUSE = @SCANLINE_DIFFUSE@;
const vec3 NVG_TINT_DOT = @NVG_TINT_DOT@;
const vec3 NVG_TINT_SCALE = @NVG_TINT_SCALE@;
const vec3 NVG_TINT_BIAS = @NVG_TINT_BIAS@;
const vec3 NVG_GLOW_LUMA = @NVG_GLOW_LUMA@;
const float NVG_GLOW_THRESHOLD = @NVG_GLOW_THRESHOLD@;
const vec3 NVG_GLOW_SCALE = @NVG_GLOW_SCALE@;
const float NVG_GLOW_ALPHA = @NVG_GLOW_ALPHA@;

vec3 gamma_to_linear(vec3 gamma_rgb) {
	vec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}

// D3D9's pre-transformed quads place pixel centers on integer positions:
// the rect-normalised position of this pixel's center.
vec2 rect_st() {
	return (gl_FragCoord.xy - vec2(0.5)) / pc.rect_source.xy;
}

// The four taps of the draw's geometry.
void taps4(out vec2 uv[4], out float fan_alpha) {
	int taps = pc.mode_taps.y;
	vec2 st = rect_st();
	vec2 reduced = vec2(pc.params.x, 1.0);
	fan_alpha = 1.0;
	if (taps == 3) {
		// build_scar_decal_vertices_extended: the fan's interpolated alpha and
		// taps (runtime/renderer/frame_fx_effects.h frame_fx_fan_*).
		fan_alpha = 2.0 * max(abs(st.x - 0.5), abs(st.y - 0.5));
		vec2 origin = st + pc.base_direction.xy * fan_alpha;
		vec2 toward = vec2(2.0 * pc.params.x * (0.5 - st.x), 2.0 * (0.5 - st.y))
				* pc.params.z;
		for (int k = 0; k < 4; ++k)
			uv[k] = origin + toward * FAN_STEPS[k];
		return;
	}
	vec2 origin = st + pc.base_direction.xy;
	vec2 direction = pc.base_direction.zw * reduced;
	if (taps == 2) {
		for (int k = 0; k < 4; ++k)
			uv[k] = origin + direction * WEIGHT_STEPS[k];
		return;
	}
	if (taps == 5) {
		float o = pc.params.z;
		uv[0] = origin + vec2(-o, -o);
		uv[1] = origin + vec2(o, -o);
		uv[2] = origin + vec2(-o, o);
		uv[3] = origin + vec2(o, o);
		return;
	}
	if (taps == 6) {
		float o = pc.params.z;
		uv[0] = origin + vec2(-o, 0.0);
		uv[1] = origin + vec2(o, 0.0);
		uv[2] = origin + vec2(0.0, -o);
		uv[3] = origin + vec2(0.0, o);
		return;
	}
	// build_scar_decal_quad_vertices: (+s,+c), (+c,-s), (-s,-c), (-c,+s).
	vec2 perpendicular = vec2(pc.base_direction.w, -pc.base_direction.z) * reduced;
	uv[0] = origin + direction;
	uv[1] = origin + perpendicular;
	uv[2] = origin - direction;
	uv[3] = origin - perpendicular;
}

vec4 average4(vec2 uv[4]) {
	return (texture(source_color, uv[0]) + texture(source_color, uv[1])
			+ texture(source_color, uv[2]) + texture(source_color, uv[3])) * 0.25;
}

// The NVG composite's quad spans (0, 0)..(W - 1, H - 1): the last column and
// row keep the black clear.
bool nvg_covered() {
	vec2 pixel = gl_FragCoord.xy - vec2(0.5);
	return pixel.x < pc.rect_source.x && pixel.y < pc.rect_source.y;
}

void main() {
	int mode = pc.mode_taps.x;
	if (mode == 0) {
		// IDirect3DDevice9::StretchRect(D3DTEXF_LINEAR): texel centers map
		// directly between the full source and destination rectangles.
		frag_color = texture(source_color, gl_FragCoord.xy / pc.rect_source.xy);
		return;
	}
	if (mode == 4 || mode == 5) {
		vec4 source = texelFetch(source_color, ivec2(gl_FragCoord.xy), 0);
		frag_color = mode == 4 ? vec4(gamma_to_linear(source.rgb), source.a) : source;
		return;
	}
	if (mode == 8) {
		// The Tiled builder over "ffscan": MODULATE2X(TEXTURE, DIFFUSE); the
		// DESTCOLOR/SRCCOLOR blend doubles it onto the frame.
		vec2 pixel = gl_FragCoord.xy - vec2(0.5) + vec2(pc.mode_taps.zw);
		vec2 uv = pc.base_direction.xy + pixel / SCANLINE_TILE;
		vec3 texel = texture(source_color, uv).rgb;
		frag_color = vec4(clamp(2.0 * texel * SCANLINE_DIFFUSE, 0.0, 1.0), 1.0);
		return;
	}
	if (mode == 10 || mode == 11) {
		if (!nvg_covered()) {
			frag_color = vec4(0.0);
			return;
		}
		vec3 texel = texture(source_color, rect_st()).rgb;
		if (mode == 10) {
			float d = dot(texel, NVG_TINT_DOT);
			frag_color = vec4(clamp(d * NVG_TINT_SCALE + NVG_TINT_BIAS, 0.0, 1.0), 0.0);
		} else {
			frag_color = vec4(clamp(2.0 * texel * SCANLINE_DIFFUSE, 0.0, 1.0), 1.0);
		}
		return;
	}
	vec2 uv[4];
	float fan_alpha;
	taps4(uv, fan_alpha);
	if (mode == 2) {
		vec4 sum = texture(source_color, uv[0]) * WEIGHTS[0]
				+ texture(source_color, uv[1]) * WEIGHTS[1]
				+ texture(source_color, uv[2]) * WEIGHTS[2]
				+ texture(source_color, uv[3]) * WEIGHTS[3];
		frag_color = vec4(sum.rgb, 1.0);
		return;
	}
	if (mode == 9) {
		vec3 sum = vec3(0.0);
		for (int k = 0; k < 4; ++k) {
			vec3 t = texture(source_color, uv[k]).rgb;
			sum += t * t;
		}
		float g = clamp(dot(sum, NVG_GLOW_LUMA) - NVG_GLOW_THRESHOLD, 0.0, 1.0);
		frag_color = vec4(g * NVG_GLOW_SCALE, NVG_GLOW_ALPHA);
		return;
	}
	vec4 averaged = average4(uv);
	if (mode == 1) {
		float luma = dot(averaged.rgb, LUMA);
		frag_color = vec4(averaged.rgb, luma * luma);
	} else if (mode == 3) {
		frag_color = vec4(averaged.rgb, pc.params.y);
	} else if (mode == 6) {
		frag_color = vec4(averaged.rgb, fan_alpha);
	} else {
		// mode 7: the thermal stage.
		float l = dot(vec3(1.0) - clamp(averaged.rgb, 0.0, 1.0), THERMAL_LUMA);
		float green = clamp(l + 1.0, 0.0, 1.0);
		frag_color = vec4(clamp(vec3(l * clamp(l, 0.0, 1.0),
				l * green + THERMAL_GREEN_BIAS, l * clamp(l, 0.0, 1.0)), 0.0, 1.0), 1.0);
	}
}
)GLSL";

// A GLSL float literal for an engine constant: %.9g round-trips every float,
// and a trailing ".0" keeps integral values typed as floats.
std::string glsl_float(float p_value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(p_value));
	std::string text(buffer);
	if (text.find_first_of(".eE") == std::string::npos)
		text += ".0";
	return text;
}

std::string glsl_vec3(const opennova::renderer::FrameFxRgb &p_value) {
	return "vec3(" + glsl_float(p_value[0]) + ", " + glsl_float(p_value[1]) + ", " +
			glsl_float(p_value[2]) + ")";
}

std::string glsl_list4(const std::array<float, 4> &p_values) {
	return glsl_float(p_values[0]) + ", " + glsl_float(p_values[1]) + ", " +
			glsl_float(p_values[2]) + ", " + glsl_float(p_values[3]);
}

void splice_token(std::string &p_text, const char *p_token, const std::string &p_value) {
	const std::string token(p_token);
	for (std::size_t at = p_text.find(token); at != std::string::npos;
			at = p_text.find(token, at + p_value.size()))
		p_text.replace(at, token.size(), p_value);
}

std::string frame_fragment_shader_source() {
	namespace r = opennova::renderer;
	std::string source(kFrameFragmentShaderTemplate);
	splice_token(source, "@LUMA@", glsl_vec3(r::kFrameFxLumaWeights));
	splice_token(source, "@WEIGHTS@", glsl_list4(r::kFrameFxWeightedTapWeights));
	splice_token(source, "@WEIGHT_STEPS@", glsl_list4(r::kFrameFxWeightedTapSteps));
	splice_token(source, "@FAN_STEPS@", glsl_list4(r::kFrameFxFanTapSteps));
	splice_token(source, "@THERMAL_LUMA@", glsl_vec3(r::kFrameFxThermalLuma));
	splice_token(source, "@THERMAL_GREEN_BIAS@", glsl_float(r::kFrameFxThermalGreenBias));
	splice_token(source, "@SCANLINE_TILE@",
			"vec2(" + glsl_float(float(r::kFrameFxScanlineTileWidth)) + ", " +
					glsl_float(float(r::kFrameFxScanlineTileHeight)) + ")");
	// The diffuse byte (0x80 per channel) as the fixed-function stage reads it.
	splice_token(source, "@SCANLINE_DIFFUSE@",
			glsl_float(float(r::kFrameFxScanlineDiffuse & 0xFFu) / 255.0f));
	splice_token(source, "@NVG_TINT_DOT@", glsl_vec3(r::kNvgTintDot));
	splice_token(source, "@NVG_TINT_SCALE@", glsl_vec3(r::kNvgTintScale));
	splice_token(source, "@NVG_TINT_BIAS@", glsl_vec3(r::kNvgTintBias));
	splice_token(source, "@NVG_GLOW_LUMA@", glsl_vec3(r::kNvgGlowLuma));
	splice_token(source, "@NVG_GLOW_THRESHOLD@", glsl_float(r::kNvgGlowThreshold));
	splice_token(source, "@NVG_GLOW_SCALE@", glsl_vec3(r::kNvgGlowScale));
	splice_token(source, "@NVG_GLOW_ALPHA@", glsl_float(r::kNvgGlowAlpha));
	return source;
}

// The pixel stage and the blend its state object carries
// (runtime/renderer/frame_fx_effects.h FrameFxStage).
FramePass pass_for(FrameFxStage p_stage) {
	switch (p_stage) {
		case FrameFxStage::LumaAverage:
			return FramePass::LumaAverage;
		case FrameFxStage::AverageBlend:
		case FrameFxStage::AverageAdd:
			return FramePass::Average;
		case FrameFxStage::WeightedAdd:
			return FramePass::Weighted;
		case FrameFxStage::FanAverage:
			return FramePass::FanAverage;
		case FrameFxStage::Thermal:
			return FramePass::Thermal;
		case FrameFxStage::Scanline:
			return FramePass::Scanline;
	}
	return FramePass::Average;
}

BlendMode blend_for(FrameFxStage p_stage) {
	switch (p_stage) {
		case FrameFxStage::LumaAverage:
		case FrameFxStage::Thermal:
			return BlendMode::Replace;
		case FrameFxStage::AverageBlend:
		case FrameFxStage::FanAverage:
			return BlendMode::SourceAlphaBlend;
		case FrameFxStage::AverageAdd:
			return BlendMode::SourceAlphaAdd;
		case FrameFxStage::WeightedAdd:
			return BlendMode::Add;
		case FrameFxStage::Scanline:
			return BlendMode::DestColorSourceColor;
	}
	return BlendMode::Replace;
}

TapGeometry taps_for(FrameFxTaps p_taps) {
	switch (p_taps) {
		case FrameFxTaps::Rotated:
			return TapGeometry::Rotated;
		case FrameFxTaps::Weighted:
			return TapGeometry::Weighted;
		case FrameFxTaps::RadialFan:
			return TapGeometry::RadialFan;
		case FrameFxTaps::Tiled:
			return TapGeometry::Tiled;
	}
	return TapGeometry::Rotated;
}

WorldEnvironment *world_environment_from_id(const ObjectID &id) {
	if (!id.is_valid())
		return nullptr;
	return Object::cast_to<WorldEnvironment>(
			ObjectDB::get_instance(static_cast<std::uint64_t>(id)));
}

// A compositor carrying every effect of `previous` (minus any earlier copy of
// `terminal`) with `terminal` appended last: the FrameFX composite and the
// display decode must run after every other post-transparent effect.
Ref<Compositor> compositor_with_terminal(const Ref<Compositor> &previous,
		const Ref<CompositorEffect> &terminal) {
	TypedArray<Ref<CompositorEffect>> effects;
	if (previous.is_valid()) {
		const TypedArray<Ref<CompositorEffect>> previous_effects =
				previous->get_compositor_effects();
		for (int64_t i = 0; i < previous_effects.size(); ++i) {
			Ref<CompositorEffect> existing = previous_effects[i];
			if (existing.is_null() ||
					existing->get_instance_id() == terminal->get_instance_id())
				continue;
			effects.push_back(existing);
		}
	}
	effects.push_back(terminal);
	Ref<Compositor> compositor;
	compositor.instantiate();
	compositor->set_compositor_effects(effects);
	return compositor;
}

} // namespace

class FrameFxCompositorEffect::Impl {
public:
	struct PipelineKey {
		int64_t framebuffer_format = -1;
		BlendMode blend = BlendMode::Replace;
		bool write_alpha = true;

		bool operator<(const PipelineKey &other) const {
			return std::tie(framebuffer_format, blend, write_alpha) <
					std::tie(other.framebuffer_format, other.blend, other.write_alpha);
		}
	};

	struct ViewTarget {
		RID color;
		RID depth;
		RID color_framebuffer;
		RID scene_scratch;
		RID scene_scratch_framebuffer;
		RID q3_color;
		RID q3_framebuffer;
		RID capture;
		RID capture_framebuffer;
		RID low_a;
		RID low_a_framebuffer;
		RID low_b;
		RID low_b_framebuffer;
		RID nvg_scene;
		RID nvg_scene_framebuffer;
		RID nvg_glow;
		RID nvg_glow_framebuffer;
		RID color_uniform;
		RID scratch_uniform;
		RID capture_uniform;
		RID low_a_uniform;
		RID low_b_uniform;
		RID q3_uniform;
		RID nvg_scene_uniform;
		RID nvg_glow_uniform;
		Vector2i size;
		Vector2i capture_size;
	};

	Q3FrameAdapter q3_adapter;
	mutable std::mutex diagnostics_mutex;
	std::string status = "waiting_for_frame";
	std::string failure;
	bool callback_seen = false;
	bool rd_available = false;
	std::uint64_t rendered_frames = 0;
	std::size_t gpu_draw_calls = 0;
	std::size_t view_count = 0;
	Vector2i last_size;
	Vector2i last_capture_size;
	bool q3_sampled = false;
	std::uint64_t gpu_span_us = 0;
	bool gpu_span_valid = false;
	std::uint64_t gpu_composite_us = 0;
	bool gpu_composite_valid = false;
	std::uint64_t gpu_decode_us = 0;
	bool gpu_decode_valid = false;
	// The last rendered frame's screen-effect trace for the GUT pins.
	std::uint64_t screen_frame_id = 0;
	std::size_t screen_steps = 0;
	std::size_t distortion_sets = 0;
	bool nvg_scene_drawn = false;
	bool nvg_composited = false;
	std::uint64_t nvg_glow_clears = 0;
	std::atomic<bool> shutdown_requested{false};
	// F3-only GPU timing (rd_timestamp_span.h carries the barrier contract).
	std::atomic<bool> gpu_timing_enabled{false};

	// Main thread -> render thread: the latest screen-effect plan and the
	// mission's scanline texels, each behind the one publication mutex.
	std::mutex publication_mutex;
	std::shared_ptr<const FrameFxScreenFrame> published_screen;
	std::vector<std::uint8_t> published_scanlines;
	std::uint64_t published_scanline_generation = 0;
	// Render thread: the frame id whose one-shot parts already ran.
	std::uint64_t consumed_screen_frame = 0;
	std::uint64_t uploaded_scanline_generation = 0;

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID repeat_sampler;
	RID scanlines;
	RID scanline_uniform;
	std::map<PipelineKey, RID> pipelines;
	std::vector<ViewTarget> targets;
	std::uint64_t target_buffers_id = 0;
	PackedByteArray push_constants;
	PackedColorArray clear_black;
	PackedColorArray clear_nvg_glow;

	// RenderingServer owns the RenderingDevice. FrameFx::shutdown() releases
	// live RIDs explicitly; destruction can occur after server teardown and must
	// not query or call through that process-owned singleton.
	~Impl() = default;

	void set_failure(const std::string &reason,
			const std::string &next_status = "failed") {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = next_status;
		failure = reason;
	}

	void release_rid(RID &rid) {
		if (rd != nullptr && rid.is_valid())
			rd->free_rid(rid);
		rid = RID();
	}

	void release_uniform(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->uniform_set_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_texture(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->texture_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_framebuffer(RID &rid) {
		if (rd != nullptr && rid.is_valid() && rd->framebuffer_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_pipeline(RID &rid) {
		if (rd != nullptr && rid.is_valid() &&
				rd->render_pipeline_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_target(ViewTarget &target) {
		release_uniform(target.nvg_glow_uniform);
		release_uniform(target.nvg_scene_uniform);
		release_uniform(target.q3_uniform);
		release_uniform(target.low_b_uniform);
		release_uniform(target.low_a_uniform);
		release_uniform(target.capture_uniform);
		release_uniform(target.scratch_uniform);
		release_uniform(target.color_uniform);
		release_framebuffer(target.nvg_glow_framebuffer);
		release_framebuffer(target.nvg_scene_framebuffer);
		release_framebuffer(target.low_b_framebuffer);
		release_framebuffer(target.low_a_framebuffer);
		release_framebuffer(target.capture_framebuffer);
		release_framebuffer(target.scene_scratch_framebuffer);
		release_framebuffer(target.q3_framebuffer);
		release_framebuffer(target.color_framebuffer);
		release_texture(target.nvg_glow);
		release_texture(target.nvg_scene);
		release_texture(target.low_b);
		release_texture(target.low_a);
		release_texture(target.capture);
		release_texture(target.scene_scratch);
		release_texture(target.q3_color);
		target = ViewTarget();
	}

	void release_targets() {
		for (ViewTarget &target : targets)
			release_target(target);
		targets.clear();
		target_buffers_id = 0;
	}

	void release_scanlines() {
		release_uniform(scanline_uniform);
		release_texture(scanlines);
		uploaded_scanline_generation = 0;
	}

	void release_all() {
		RenderingServer *server = RenderingServer::get_singleton();
		rd = server != nullptr ? server->get_rendering_device() : nullptr;
		q3_adapter.release_device(rd);
		release_targets();
		release_scanlines();
		for (auto &entry : pipelines)
			release_pipeline(entry.second);
		pipelines.clear();
		release_rid(repeat_sampler);
		release_rid(sampler);
		release_rid(shader);
	}

	bool initialize_rd();
	RID make_texture(const Vector2i &size,
			RenderingDevice::DataFormat format, bool readback = false);
	RID make_framebuffer(const RID &texture);
	RID make_uniform(const RID &texture, const RID &texture_sampler);
	bool ensure_targets(RenderSceneBuffersRD *buffers,
			std::uint32_t count, const Vector2i &size);
	bool ensure_scanlines();
	RID pipeline_for(int64_t framebuffer_format, BlendMode blend, bool write_alpha);
	void set_push(const FramePush &push);
	bool draw_pushes(const RID &framebuffer, const RID &uniform, BlendMode blend,
			const FramePush *pushes, std::size_t count, TargetLoad load,
			const PackedColorArray &clear_colors, bool write_alpha = true);
	bool stretch(const RID &framebuffer, const Vector2i &target_size,
			const RID &uniform, const Vector2i &source_size, std::size_t &draws);
	bool run_pass(ViewTarget &target, const FrameFxPass &pass, std::size_t &draws);
	bool run_steps(ViewTarget &target, RenderData *render_data, std::uint32_t view,
			const std::vector<FrameFxStep> &steps, const FrameFxScreenFrame *screen,
			std::size_t &draws);
	bool composite_q3(ViewTarget &target, RenderData *render_data,
			std::uint32_t view, std::size_t &draws);
	bool run_nvg(ViewTarget &target, const FrameFxNvgPlan &nvg, bool first_use,
			std::size_t &draws);
	bool render(RenderData *render_data);
	Ref<Image> capture_q3_target();
	Dictionary report() const;
};

Ref<Image> FrameFxCompositorEffect::Impl::capture_q3_target() {
	if (rd == nullptr || targets.empty() ||
			shutdown_requested.load(std::memory_order_acquire))
		return Ref<Image>();
	const ViewTarget &target = targets.front();
	if (!target.q3_color.is_valid() || !rd->texture_is_valid(target.q3_color))
		return Ref<Image>();
	const Ref<RDTextureFormat> format = rd->texture_get_format(target.q3_color);
	if (format.is_null())
		return Ref<Image>();
	Image::Format image_format = Image::FORMAT_RGBA8;
	switch (format->get_format()) {
		case RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT:
			image_format = Image::FORMAT_RGBAH;
			break;
		case RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT:
			image_format = Image::FORMAT_RGBAF;
			break;
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
			image_format = Image::FORMAT_RGBA8;
			break;
		default:
			return Ref<Image>();
	}
	const PackedByteArray data = rd->texture_get_data(target.q3_color, 0);
	if (data.is_empty())
		return Ref<Image>();
	return Image::create_from_data(format->get_width(), format->get_height(),
			false, image_format, data);
}

bool FrameFxCompositorEffect::Impl::initialize_rd() {
	if (shutdown_requested.load(std::memory_order_acquire))
		return false;
	if (rd != nullptr && shader.is_valid() && sampler.is_valid() &&
			repeat_sampler.is_valid())
		return true;
	release_all();
	RenderingServer *server = RenderingServer::get_singleton();
	rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		set_failure("RenderingDevice is unavailable; FrameFX requires "
				"Forward+ or Mobile", "compatibility_renderer_unsupported");
		return false;
	}
	if (rd->limit_get(RenderingDevice::LIMIT_MAX_PUSH_CONSTANT_SIZE) <
			kPushConstantBytes) {
		set_failure("RenderingDevice does not support the 64-byte FrameFX "
				"push constant block", "push_constants_unsupported");
		return false;
	}

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kRdFullscreenVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(frame_fragment_shader_source().c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		set_failure("RenderingDevice returned no SPIR-V for FrameFX",
				"shader_compile_failed");
		return false;
	}
	const String vertex_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!vertex_error.is_empty() || !fragment_error.is_empty() ||
			spirv->get_stage_bytecode(
					RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			spirv->get_stage_bytecode(
					RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		set_failure("FrameFX shader compilation failed: vertex=" +
				opennova::to_std(vertex_error) + "; fragment=" +
				opennova::to_std(fragment_error),
				"shader_compile_failed");
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova FrameFX");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the FrameFX shader",
				"shader_create_failed");
		return false;
	}

	// The FrameFX render targets sample LINEAR and CLAMP (GTexture flags 1,
	// retail create_frame_effect_render_targets @0x583cf2, read by
	// apply_texture_stages); the "ffscan" texture keeps the default WRAP.
	auto make_sampler = [&](RenderingDevice::SamplerRepeatMode p_repeat) {
		Ref<RDSamplerState> sampler_state;
		sampler_state.instantiate();
		sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
		sampler_state->set_repeat_u(p_repeat);
		sampler_state->set_repeat_v(p_repeat);
		sampler_state->set_repeat_w(p_repeat);
		return rd->sampler_create(sampler_state);
	};
	sampler = make_sampler(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	repeat_sampler = make_sampler(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	if (!sampler.is_valid() || !repeat_sampler.is_valid()) {
		set_failure("RenderingDevice could not create the FrameFX samplers",
				"sampler_create_failed");
		release_all();
		return false;
	}
	push_constants.resize(kPushConstantBytes);
	if (clear_black.is_empty())
		clear_black.push_back(Color(0, 0, 0, 0));
	if (clear_nvg_glow.is_empty()) {
		const std::uint32_t argb = opennova::renderer::kNvgGlowClearColor;
		clear_nvg_glow.push_back(Color(float((argb >> 16) & 0xFFu) / 255.0f,
				float((argb >> 8) & 0xFFu) / 255.0f, float(argb & 0xFFu) / 255.0f,
				float((argb >> 24) & 0xFFu) / 255.0f));
	}
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		rd_available = true;
		status = "ready";
		failure.clear();
	}
	return true;
}

RID FrameFxCompositorEffect::Impl::make_texture(const Vector2i &size,
		RenderingDevice::DataFormat format, bool readback) {
	Ref<RDTextureFormat> texture_format;
	texture_format.instantiate();
	texture_format->set_format(format);
	texture_format->set_width(size.x);
	texture_format->set_height(size.y);
	texture_format->set_depth(1);
	texture_format->set_array_layers(1);
	texture_format->set_mipmaps(1);
	texture_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	texture_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	int64_t usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	if (readback) {
		// capture_q3_target() reads this attachment back for the GUT pins.
		usage |= RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	}
	texture_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(usage));
	Ref<RDTextureView> view;
	view.instantiate();
	return rd->texture_create(texture_format, view);
}

RID FrameFxCompositorEffect::Impl::make_framebuffer(const RID &texture) {
	TypedArray<RID> attachments;
	attachments.push_back(texture);
	return rd->framebuffer_create(attachments);
}

RID FrameFxCompositorEffect::Impl::make_uniform(const RID &texture,
		const RID &texture_sampler) {
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, texture_sampler, texture));
	return rd->uniform_set_create(uniforms, shader, 0);
}

bool FrameFxCompositorEffect::Impl::ensure_targets(
		RenderSceneBuffersRD *buffers, std::uint32_t count,
		const Vector2i &size) {
	if (buffers == nullptr || count == 0 || size.x <= 0 || size.y <= 0) {
		set_failure("Compositor callback has invalid scene render buffers",
				"render_targets_invalid");
		return false;
	}
	const std::uint64_t buffers_id = buffers->get_instance_id();
	bool matches = target_buffers_id == buffers_id && targets.size() == count;
	if (matches) {
		for (std::uint32_t view = 0; view < count; ++view) {
			const ViewTarget &target = targets[view];
			if (target.size != size || target.color != buffers->get_color_layer(view) ||
					target.depth != buffers->get_depth_layer(view) ||
					!target.color_framebuffer.is_valid() ||
					!rd->framebuffer_is_valid(target.color_framebuffer)) {
				matches = false;
				break;
			}
		}
	}
	if (matches)
		return true;

	release_targets();
	// The focused Q3 renderer draws at beauty resolution against the resolved
	// beauty depth. The frame and the Q3 source both capture into the
	// power-of-two floor of the frame, whose downsample feeds the 256-square
	// work targets.
	const Vector2i capture_size(opennova::renderer::frame_fx_capture_side(size.x),
			opennova::renderer::frame_fx_capture_side(size.y));
	if (capture_size.x <= 0 || capture_size.y <= 0) {
		set_failure("The frame is too small for a FrameFX capture target",
				"render_targets_invalid");
		return false;
	}
	const Vector2i work(kWorkSide, kWorkSide);
	const Vector2i nvg_scene(opennova::renderer::kNvgSceneSide,
			opennova::renderer::kNvgSceneSide);
	const Vector2i nvg_glow(opennova::renderer::kNvgGlowSide,
			opennova::renderer::kNvgGlowSide);
	constexpr RenderingDevice::DataFormat kRgba8 =
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	targets.reserve(count);
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget target;
		target.color = buffers->get_color_layer(view);
		target.depth = buffers->get_depth_layer(view);
		target.size = size;
		target.capture_size = capture_size;
		const Ref<RDTextureFormat> color_format =
				rd->texture_get_format(target.color);
		if (!target.color.is_valid() || !target.depth.is_valid() ||
				color_format.is_null()) {
			set_failure("Resolved scene color/depth is unavailable for FrameFX view " +
					std::to_string(view), "render_targets_invalid");
			release_target(target);
			release_targets();
			return false;
		}
		target.color_framebuffer = make_framebuffer(target.color);
		target.scene_scratch = make_texture(size, color_format->get_format());
		target.scene_scratch_framebuffer = make_framebuffer(target.scene_scratch);
		target.q3_color = make_texture(size, color_format->get_format(), true);
		TypedArray<RID> q3_attachments;
		q3_attachments.push_back(target.q3_color);
		q3_attachments.push_back(target.depth);
		target.q3_framebuffer = rd->framebuffer_create(q3_attachments);
		target.capture = make_texture(capture_size, kRgba8);
		target.capture_framebuffer = make_framebuffer(target.capture);
		target.low_a = make_texture(work, kRgba8);
		target.low_a_framebuffer = make_framebuffer(target.low_a);
		target.low_b = make_texture(work, kRgba8);
		target.low_b_framebuffer = make_framebuffer(target.low_b);
		target.nvg_scene = make_texture(nvg_scene, kRgba8);
		target.nvg_scene_framebuffer = make_framebuffer(target.nvg_scene);
		target.nvg_glow = make_texture(nvg_glow, kRgba8);
		target.nvg_glow_framebuffer = make_framebuffer(target.nvg_glow);
		target.color_uniform = make_uniform(target.color, sampler);
		target.scratch_uniform = make_uniform(target.scene_scratch, sampler);
		target.capture_uniform = make_uniform(target.capture, sampler);
		target.low_a_uniform = make_uniform(target.low_a, sampler);
		target.low_b_uniform = make_uniform(target.low_b, sampler);
		target.q3_uniform = make_uniform(target.q3_color, sampler);
		target.nvg_scene_uniform = make_uniform(target.nvg_scene, sampler);
		target.nvg_glow_uniform = make_uniform(target.nvg_glow, sampler);
		const bool valid = target.color_framebuffer.is_valid() &&
				target.scene_scratch.is_valid() &&
				target.scene_scratch_framebuffer.is_valid() &&
				target.q3_color.is_valid() && target.q3_framebuffer.is_valid() &&
				target.capture.is_valid() && target.capture_framebuffer.is_valid() &&
				target.low_a.is_valid() && target.low_a_framebuffer.is_valid() &&
				target.low_b.is_valid() && target.low_b_framebuffer.is_valid() &&
				target.nvg_scene.is_valid() && target.nvg_scene_framebuffer.is_valid() &&
				target.nvg_glow.is_valid() && target.nvg_glow_framebuffer.is_valid() &&
				target.color_uniform.is_valid() && target.scratch_uniform.is_valid() &&
				target.capture_uniform.is_valid() && target.low_a_uniform.is_valid() &&
				target.low_b_uniform.is_valid() && target.q3_uniform.is_valid() &&
				target.nvg_scene_uniform.is_valid() && target.nvg_glow_uniform.is_valid();
		if (!valid) {
			set_failure("RenderingDevice could not allocate the FrameFX "
					"target chain for view " + std::to_string(view),
					"render_targets_failed");
			release_target(target);
			release_targets();
			return false;
		}
		// The glow target starts empty; retail clears it only on a toggle frame.
		const int64_t list = rd->draw_list_begin(target.nvg_glow_framebuffer,
				RenderingDevice::DRAW_CLEAR_COLOR_0, clear_black);
		if (list != RenderingDevice::INVALID_ID)
			rd->draw_list_end();
		targets.push_back(target);
	}
	target_buffers_id = buffers_id;
	return true;
}

// The mission's "ffscan" texture: the engine's luminance texels replicated
// into all four channels (A8R8G8B8 bytes 0x01010101 x texel).
bool FrameFxCompositorEffect::Impl::ensure_scanlines() {
	std::vector<std::uint8_t> texels;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(publication_mutex);
		generation = published_scanline_generation;
		if (generation != uploaded_scanline_generation)
			texels = published_scanlines;
	}
	if (generation == 0)
		return false;
	if (generation == uploaded_scanline_generation && scanline_uniform.is_valid())
		return true;
	const int side = opennova::renderer::kFrameFxScanlineSide;
	if (texels.size() != static_cast<std::size_t>(side * side))
		return false;
	PackedByteArray rgba;
	rgba.resize(static_cast<int64_t>(texels.size()) * 4);
	std::uint8_t *out = rgba.ptrw();
	for (std::size_t i = 0; i < texels.size(); ++i) {
		out[i * 4 + 0] = texels[i];
		out[i * 4 + 1] = texels[i];
		out[i * 4 + 2] = texels[i];
		out[i * 4 + 3] = texels[i];
	}
	if (!scanlines.is_valid()) {
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		format->set_width(side);
		format->set_height(side);
		format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT));
		Ref<RDTextureView> view;
		view.instantiate();
		TypedArray<PackedByteArray> data;
		data.push_back(rgba);
		scanlines = rd->texture_create(format, view, data);
		if (!scanlines.is_valid())
			return false;
		scanline_uniform = make_uniform(scanlines, repeat_sampler);
	} else {
		rd->texture_update(scanlines, 0, rgba);
	}
	uploaded_scanline_generation = generation;
	return scanline_uniform.is_valid();
}

RID FrameFxCompositorEffect::Impl::pipeline_for(
		int64_t framebuffer_format, BlendMode blend, bool write_alpha) {
	const PipelineKey key{framebuffer_format, blend, write_alpha};
	const auto found = pipelines.find(key);
	if (found != pipelines.end())
		return found->second;

	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test(false);
	depth->set_enable_depth_write(false);
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(blend != BlendMode::Replace);
	// D3D9 applies the colour factors to alpha by their alpha counterparts.
	auto factors = [&](RenderingDevice::BlendFactor p_src_color,
			RenderingDevice::BlendFactor p_dst_color,
			RenderingDevice::BlendFactor p_src_alpha,
			RenderingDevice::BlendFactor p_dst_alpha) {
		attachment->set_src_color_blend_factor(p_src_color);
		attachment->set_dst_color_blend_factor(p_dst_color);
		attachment->set_src_alpha_blend_factor(p_src_alpha);
		attachment->set_dst_alpha_blend_factor(p_dst_alpha);
	};
	switch (blend) {
		case BlendMode::Replace:
			break;
		case BlendMode::Add:
			factors(RenderingDevice::BLEND_FACTOR_ONE, RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_ONE, RenderingDevice::BLEND_FACTOR_ONE);
			break;
		case BlendMode::SourceAlphaAdd:
			factors(RenderingDevice::BLEND_FACTOR_SRC_ALPHA, RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA, RenderingDevice::BLEND_FACTOR_ONE);
			break;
		case BlendMode::SourceAlphaBlend:
			factors(RenderingDevice::BLEND_FACTOR_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			break;
		case BlendMode::DestColorSourceColor:
			factors(RenderingDevice::BLEND_FACTOR_DST_COLOR,
					RenderingDevice::BLEND_FACTOR_SRC_COLOR,
					RenderingDevice::BLEND_FACTOR_DST_ALPHA,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
			break;
		case BlendMode::GlowAccumulate:
			factors(RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			break;
	}
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_write_a(write_alpha);
	Ref<RDPipelineColorBlendState> color_blend;
	color_blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	color_blend->set_attachments(attachments);
	RID pipeline = rd->render_pipeline_create(shader, framebuffer_format,
			RenderingDevice::INVALID_FORMAT_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample,
			depth, color_blend);
	if (!pipeline.is_valid()) {
		set_failure("RenderingDevice rejected a FrameFX pipeline",
				"pipeline_create_failed");
		return RID();
	}
	pipelines.emplace(key, pipeline);
	return pipeline;
}

void FrameFxCompositorEffect::Impl::set_push(const FramePush &push) {
	write_f32(push_constants, 0, push.rect_w);
	write_f32(push_constants, 4, push.rect_h);
	write_f32(push_constants, 8, push.source_w);
	write_f32(push_constants, 12, push.source_h);
	write_f32(push_constants, 16, push.base);
	write_f32(push_constants, 20, push.base);
	write_f32(push_constants, 24, push.direction_s);
	write_f32(push_constants, 28, push.direction_c);
	write_f32(push_constants, 32, push.u_scale);
	write_f32(push_constants, 36, push.alpha);
	write_f32(push_constants, 40, push.radius);
	write_f32(push_constants, 44, 0.0f);
	write_u32(push_constants, 48, static_cast<std::uint32_t>(push.pass));
	write_u32(push_constants, 52, static_cast<std::uint32_t>(push.taps));
	write_u32(push_constants, 56, static_cast<std::uint32_t>(push.tile_x));
	write_u32(push_constants, 60, static_cast<std::uint32_t>(push.tile_y));
}

bool FrameFxCompositorEffect::Impl::draw_pushes(const RID &framebuffer,
		const RID &uniform, BlendMode blend, const FramePush *pushes,
		std::size_t count, TargetLoad load, const PackedColorArray &clear_colors,
		bool write_alpha) {
	const int64_t format = rd->framebuffer_get_format(framebuffer);
	const RID pipeline = pipeline_for(format, blend, write_alpha);
	if (!pipeline.is_valid() || !uniform.is_valid())
		return false;
	BitField<RenderingDevice::DrawFlags> flags(0);
	if (load == TargetLoad::Clear) {
		flags = RenderingDevice::DRAW_CLEAR_COLOR_0;
	} else if (load == TargetLoad::Discard) {
		flags = RenderingDevice::DRAW_IGNORE_COLOR_ALL;
	}
	const int64_t draw_list = rd->draw_list_begin(framebuffer, flags,
			load == TargetLoad::Clear ? clear_colors : PackedColorArray());
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin a FrameFX draw list",
				"draw_list_failed");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
	for (std::size_t i = 0; i < count; ++i) {
		set_push(pushes[i]);
		rd->draw_list_set_push_constant(draw_list, push_constants,
				kPushConstantBytes);
		rd->draw_list_draw(draw_list, false, 1, 3);
	}
	rd->draw_list_end();
	return true;
}

// IDirect3DDevice9::StretchRect(LINEAR) of a whole source into a whole target.
bool FrameFxCompositorEffect::Impl::stretch(const RID &framebuffer,
		const Vector2i &target_size, const RID &uniform, const Vector2i &source_size,
		std::size_t &draws) {
	FramePush push;
	push.pass = FramePass::Stretch;
	push.rect_w = static_cast<float>(target_size.x);
	push.rect_h = static_cast<float>(target_size.y);
	push.source_w = static_cast<float>(source_size.x);
	push.source_h = static_cast<float>(source_size.y);
	if (!draw_pushes(framebuffer, uniform, BlendMode::Replace, &push, 1,
			TargetLoad::Discard, clear_black))
		return false;
	++draws;
	return true;
}

// One DrawPass row (runtime/renderer/frame_fx_effects.h FrameFxPass): the
// descriptor's source, target, stage and taps, `count` draws stepping the
// angle (retail render_scar_decal_batch @0x582b9b..0x582d82).
bool FrameFxCompositorEffect::Impl::run_pass(ViewTarget &target,
		const FrameFxPass &pass, std::size_t &draws) {
	RID framebuffer;
	Vector2i target_size;
	switch (pass.target) {
		case FrameFxBuffer::Frame:
			framebuffer = target.color_framebuffer;
			target_size = target.size;
			break;
		case FrameFxBuffer::Capture:
			framebuffer = target.capture_framebuffer;
			target_size = target.capture_size;
			break;
		case FrameFxBuffer::WorkA:
			framebuffer = target.low_a_framebuffer;
			target_size = Vector2i(kWorkSide, kWorkSide);
			break;
		case FrameFxBuffer::WorkB:
			framebuffer = target.low_b_framebuffer;
			target_size = Vector2i(kWorkSide, kWorkSide);
			break;
		case FrameFxBuffer::Scanlines:
			set_failure("A FrameFX pass cannot target the scanline texture");
			return false;
	}
	RID uniform;
	Vector2i source_size;
	switch (pass.source) {
		case FrameFxBuffer::Frame:
			uniform = target.color_uniform;
			source_size = target.size;
			break;
		case FrameFxBuffer::Capture:
			uniform = target.capture_uniform;
			source_size = target.capture_size;
			break;
		case FrameFxBuffer::WorkA:
			uniform = target.low_a_uniform;
			source_size = Vector2i(kWorkSide, kWorkSide);
			break;
		case FrameFxBuffer::WorkB:
			uniform = target.low_b_uniform;
			source_size = Vector2i(kWorkSide, kWorkSide);
			break;
		case FrameFxBuffer::Scanlines:
			if (!ensure_scanlines())
				return true; // No mission texture yet: retail's texture is always built.
			uniform = scanline_uniform;
			source_size = Vector2i(opennova::renderer::kFrameFxScanlineSide,
					opennova::renderer::kFrameFxScanlineSide);
			break;
	}
	// The viewport rect (CD3DDevice_GetViewportRect), else the 256 square.
	const Vector2i rect = pass.viewport_rect ? target_size : Vector2i(kWorkSide, kWorkSide);
	const float base = pass.base_from_capture ?
			0.5f / static_cast<float>(std::min(target.capture_size.x, target.capture_size.y)) :
			pass.base;
	std::array<FramePush, 4> pushes;
	const std::size_t count = static_cast<std::size_t>(
			std::clamp<std::int32_t>(pass.count, 0, static_cast<std::int32_t>(pushes.size())));
	for (std::size_t i = 0; i < count; ++i) {
		FramePush &push = pushes[i];
		push.pass = pass_for(pass.stage);
		push.taps = taps_for(pass.taps);
		push.rect_w = static_cast<float>(rect.x);
		push.rect_h = static_cast<float>(rect.y);
		push.source_w = static_cast<float>(source_size.x);
		push.source_h = static_cast<float>(source_size.y);
		push.base = base;
		const double radians = static_cast<double>(pass.angle_degrees +
				static_cast<std::int32_t>(i) * pass.angle_step_degrees) *
				static_cast<double>(opennova::renderer::kFrameFxDegreesToRadians);
		push.direction_s = static_cast<float>(std::sin(radians)) * pass.radius;
		push.direction_c = static_cast<float>(std::cos(radians)) * pass.radius;
		push.u_scale = pass.reduced_u ? opennova::renderer::kFrameFxReducedU : 1.0f;
		push.alpha = pass.constant_alpha;
		push.radius = pass.radius;
		push.tile_x = pass.tile_offset_x;
		push.tile_y = pass.tile_offset_y;
	}
	// GTexRT_Select clears a selected FrameFX target to 0; the frame is drawn
	// over, its alpha (an X8R8G8B8 backbuffer's padding) left untouched.
	const bool frame = pass.target == FrameFxBuffer::Frame;
	if (!draw_pushes(framebuffer, uniform, blend_for(pass.stage), pushes.data(), count,
			frame ? TargetLoad::Keep : TargetLoad::Clear, clear_black, !frame))
		return false;
	draws += count;
	return true;
}

bool FrameFxCompositorEffect::Impl::run_steps(ViewTarget &target,
		RenderData *render_data, std::uint32_t view,
		const std::vector<FrameFxStep> &steps, const FrameFxScreenFrame *screen,
		std::size_t &draws) {
	for (const FrameFxStep &step : steps) {
		switch (step.kind) {
			case FrameFxStepKind::CaptureFrame:
				if (!stretch(target.capture_framebuffer, target.capture_size,
						target.color_uniform, target.size, draws))
					return false;
				break;
			case FrameFxStepKind::Pass:
				if (!run_pass(target, step.pass, draws))
					return false;
				break;
			case FrameFxStepKind::Distortion: {
				if (screen == nullptr || !screen->distortion)
					break;
				FrameFxDistortionTarget distortion;
				distortion.rd = rd;
				distortion.render_data = render_data;
				distortion.view = view;
				distortion.color = target.color;
				distortion.depth = target.depth;
				distortion.screen_texture = step.screen_texture == FrameFxBuffer::WorkB ?
						target.low_b : target.low_a;
				distortion.screen_sampler = sampler;
				if (!screen->distortion->draw_distortion(distortion, step.set, draws))
					return false;
				++distortion_sets;
				break;
			}
		}
		++screen_steps;
	}
	return true;
}

// The focused Q3 draw into the black-cleared Q3 target (retail's altbuffer),
// its capture, and the bloom kernel over the frame: every Q3 technique
// re-shades from its own leased inputs; the beauty colour is never sampled,
// only its resolved depth is tested.
bool FrameFxCompositorEffect::Impl::composite_q3(ViewTarget &target,
		RenderData *render_data, std::uint32_t view, std::size_t &draws) {
	if (!q3_adapter.draw_view(rd, render_data, view, target.q3_framebuffer,
			draws))
		return false;
	if (!stretch(target.capture_framebuffer, target.capture_size,
			target.q3_uniform, target.size, draws))
		return false;
	static const std::vector<FrameFxPass> kBloomPasses =
			opennova::renderer::frame_fx_bloom_passes();
	for (const FrameFxPass &pass : kBloomPasses) {
		if (!run_pass(target, pass, draws))
			return false;
	}
	return true;
}

// The first-person NVG view: the scene into the 512-square target, the two
// glow passes into the persistent 256-square target (cleared green on the
// toggle frame), and the tint + glow composite replacing the frame (retail
// sub_5D28D0 @0x5d296d; render_water_caustic_overlay @0x5d0290..0x5d048a;
// render_fullscreen_overlay @0x5d0f28..0x5d1059). The scene here is the
// finished beauty frame resampled to 512 x 512.
bool FrameFxCompositorEffect::Impl::run_nvg(ViewTarget &target,
		const FrameFxNvgPlan &nvg, bool first_use, std::size_t &draws) {
	namespace r = opennova::renderer;
	const Vector2i scene_size(r::kNvgSceneSide, r::kNvgSceneSide);
	const Vector2i glow_size(r::kNvgGlowSide, r::kNvgGlowSide);
	if (nvg.scene) {
		if (!stretch(target.nvg_scene_framebuffer, scene_size, target.color_uniform,
				target.size, draws))
			return false;
		std::array<FramePush, 2> glow;
		for (std::size_t i = 0; i < glow.size(); ++i) {
			FramePush &push = glow[i];
			push.pass = FramePass::NvgGlow;
			push.taps = i == 0 ? TapGeometry::NvgDiagonal : TapGeometry::NvgAxial;
			push.rect_w = static_cast<float>(glow_size.x);
			push.rect_h = static_cast<float>(glow_size.y);
			push.source_w = static_cast<float>(scene_size.x);
			push.source_h = static_cast<float>(scene_size.y);
			push.radius = r::kNvgGlowTapStep / static_cast<float>(scene_size.x) *
					static_cast<float>(i + 1);
		}
		const bool clear = nvg.clear_glow && first_use;
		if (!draw_pushes(target.nvg_glow_framebuffer, target.nvg_scene_uniform,
				BlendMode::GlowAccumulate, glow.data(), glow.size(),
				clear ? TargetLoad::Clear : TargetLoad::Keep, clear_nvg_glow))
			return false;
		draws += glow.size();
		if (clear)
			++nvg_glow_clears;
		nvg_scene_drawn = true;
	}
	if (nvg.composite) {
		FramePush tint;
		tint.pass = FramePass::NvgTint;
		tint.rect_w = static_cast<float>(target.size.x - 1);
		tint.rect_h = static_cast<float>(target.size.y - 1);
		tint.source_w = static_cast<float>(scene_size.x);
		tint.source_h = static_cast<float>(scene_size.y);
		if (!draw_pushes(target.color_framebuffer, target.nvg_scene_uniform,
				BlendMode::Replace, &tint, 1, TargetLoad::Keep, clear_black, false))
			return false;
		FramePush add = tint;
		add.pass = FramePass::NvgGlowAdd;
		add.source_w = static_cast<float>(glow_size.x);
		add.source_h = static_cast<float>(glow_size.y);
		if (!draw_pushes(target.color_framebuffer, target.nvg_glow_uniform,
				BlendMode::Add, &add, 1, TargetLoad::Keep, clear_black, false))
			return false;
		draws += 2;
		nvg_composited = true;
	}
	return true;
}

bool FrameFxCompositorEffect::Impl::render(RenderData *render_data) {
	if (!initialize_rd() || render_data == nullptr) {
		if (render_data == nullptr)
			set_failure("FrameFX callback received no RenderData",
					"render_data_missing");
		return false;
	}
	Ref<RenderSceneBuffers> generic_buffers =
			render_data->get_render_scene_buffers();
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(
			generic_buffers.ptr());
	if (buffers == nullptr) {
		set_failure("FrameFX requires RenderSceneBuffersRD",
				"render_data_unsupported");
		return false;
	}
	const std::uint32_t count = buffers->get_view_count();
	const Vector2i size = buffers->get_internal_size();
	if (!ensure_targets(buffers, count, size))
		return false;
	const bool gpu_timing =
			gpu_timing_enabled.load(std::memory_order_relaxed);
	if (gpu_timing) {
		std::uint64_t span_us = 0;
		const bool span_valid = rd_timestamp_span_us(rd,
				"opennova_framefx_begin", "opennova_framefx_end", span_us);
		// The two sub-spans attribute the pass: the FrameFX chain (screen
		// effects, Q3 source draw + capture + blur + composite) versus the
		// terminal display decode's two full-screen draws.
		std::uint64_t composite_us = 0;
		const bool composite_valid = rd_timestamp_span_us(rd,
				"opennova_q3_composite_begin", "opennova_q3_composite_end",
				composite_us);
		std::uint64_t decode_us = 0;
		const bool decode_valid = rd_timestamp_span_us(rd,
				"opennova_decode_begin", "opennova_decode_end", decode_us);
		{
			std::lock_guard<std::mutex> lock(diagnostics_mutex);
			gpu_span_valid = span_valid;
			gpu_span_us = span_us;
			gpu_composite_valid = composite_valid;
			gpu_composite_us = composite_us;
			gpu_decode_valid = decode_valid;
			gpu_decode_us = decode_us;
		}
		rd->capture_timestamp("opennova_framefx_begin");
	}
	std::shared_ptr<const FrameFxScreenFrame> screen;
	{
		std::lock_guard<std::mutex> lock(publication_mutex);
		screen = published_screen;
	}
	// Without a published plan (the decode-only terminal, a view before its
	// first planned frame) the frame runs the default FBEFFECTS 3 chain: the
	// bloom alone.
	const FrameFxFramePlan default_plan = [] {
		FrameFxFramePlan plan;
		plan.bloom = true;
		return plan;
	}();
	const FrameFxFramePlan &plan = screen ? screen->plan : default_plan;
	const bool first_use = screen && screen->frame_id != consumed_screen_frame;
	if (screen)
		consumed_screen_frame = screen->frame_id;
	screen_steps = 0;
	distortion_sets = 0;
	nvg_scene_drawn = false;
	nvg_composited = false;
	std::size_t draws = 0;
	bool sampled_q3 = false;
	bool q3_failed = false;
	bool effects_failed = false;
	// The published Q3 frame is consumed every render, commands or not: a
	// frame with nothing to draw still names the cache entries evicted since
	// the last consumed one, and their device buffers are freed here rather
	// than held until the next non-empty frame.
	q3_adapter.consume_frame(rd);
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget &target = targets[view];
		if (gpu_timing && view == 0)
			rd->capture_timestamp("opennova_q3_composite_begin");
		// A FrameFX device failure keeps its diagnostic and skips the rest of
		// the chain for this frame, but must never skip the terminal display
		// decode below: a frame presented without it is double-encoded and
		// flickers as glow sources enter and leave.
		if (plan.nvg.scene || plan.nvg.composite) {
			if (!run_nvg(target, plan.nvg, first_use, draws))
				effects_failed = true;
		}
		if (!plan.nvg.composite && !effects_failed) {
			if (!run_steps(target, render_data, view, plan.before_bloom, screen.get(), draws))
				effects_failed = true;
			if (!effects_failed && plan.bloom && q3_adapter.has_commands() && !q3_failed) {
				if (composite_q3(target, render_data, view, draws))
					sampled_q3 = true;
				else
					q3_failed = true;
			}
			if (!effects_failed &&
					!run_steps(target, render_data, view, plan.after_bloom, screen.get(), draws))
				effects_failed = true;
		}
		if (gpu_timing && view == 0)
			rd->capture_timestamp("opennova_q3_composite_end");

		// All 3D retail draws have blended as gamma-domain numeric values. Copy
		// once, then apply the display-backend transfer immediately before Godot's
		// sRGB output encoding. Canvas/viewmodel/HUD passes run afterward.
		if (gpu_timing && view == 0)
			rd->capture_timestamp("opennova_decode_begin");
		if (!stretch(target.scene_scratch_framebuffer, target.size,
				target.color_uniform, target.size, draws))
			return false;
		FramePush decode;
		decode.pass = FramePass::GammaDecode;
		decode.rect_w = static_cast<float>(target.size.x);
		decode.rect_h = static_cast<float>(target.size.y);
		decode.source_w = decode.rect_w;
		decode.source_h = decode.rect_h;
		if (!draw_pushes(target.color_framebuffer, target.scratch_uniform,
				BlendMode::Replace, &decode, 1, TargetLoad::Discard, clear_black))
			return false;
		++draws;
		if (gpu_timing && view == 0)
			rd->capture_timestamp("opennova_decode_end");
	}
	if (gpu_timing)
		rd->capture_timestamp("opennova_framefx_end");
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		if (q3_failed || effects_failed)
			status = q3_failed ? "drawn_q3_failed" : "drawn_effects_failed";
		else
			status = sampled_q3 ? "drawn" : "drawn_without_q3";
		if (!q3_failed && !effects_failed)
			failure.clear();
		++rendered_frames;
		gpu_draw_calls = draws;
		view_count = count;
		last_size = size;
		last_capture_size = targets.empty() ? Vector2i() :
				targets.front().capture_size;
		q3_sampled = sampled_q3;
		screen_frame_id = screen ? screen->frame_id : 0;
	}
	return true;
}

Dictionary FrameFxCompositorEffect::Impl::report() const {
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	Dictionary result;
	result["backend"] = "rendering_device_framefx";
	result["callback"] = "post_transparent_terminal";
	result["callback_type"] = static_cast<int>(
			CompositorEffect::EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	result["quality_path"] = opennova::renderer::kLockedFrameEffectsLevel;
	result["q3_isolated_target"] = true;
	result["capture_filter"] = "linear_rgba8_highest_quality";
	result["capture_power_of_two_floor"] = true;
	result["blur_target_size"] = static_cast<int64_t>(kWorkSide);
	result["framebuffer_blend_domain"] = "gamma";
	result["terminal_transfer"] = "srgb_inverse_then_display_encode";
	result["callback_seen"] = callback_seen;
	result["rd_available"] = rd_available;
	result["status"] = opennova::to_gd(status);
	result["failure"] = opennova::to_gd(failure);
	result["rendered_frames"] = static_cast<int64_t>(rendered_frames);
	result["gpu_draw_calls"] = static_cast<int64_t>(gpu_draw_calls);
	result["view_count"] = static_cast<int64_t>(view_count);
	result["frame_size"] = last_size;
	result["capture_size"] = last_capture_size;
	result["q3_sampled"] = q3_sampled;
	result["screen_frame_id"] = static_cast<int64_t>(screen_frame_id);
	result["screen_steps"] = static_cast<int64_t>(screen_steps);
	result["distortion_sets"] = static_cast<int64_t>(distortion_sets);
	result["nvg_scene_drawn"] = nvg_scene_drawn;
	result["nvg_composited"] = nvg_composited;
	result["nvg_glow_clears"] = static_cast<int64_t>(nvg_glow_clears);
	result["q3_gpu_us"] = static_cast<int64_t>(gpu_span_us);
	result["q3_gpu_valid"] = gpu_span_valid;
	// The composite/decode halves of q3_gpu_us are raw-report diagnostics
	// (probe dumps, MCP reads); q3_gpu_us alone feeds a FrameStats slot.
	result["q3_gpu_composite_us"] = static_cast<int64_t>(gpu_composite_us);
	result["q3_gpu_composite_valid"] = gpu_composite_valid;
	result["q3_gpu_decode_us"] = static_cast<int64_t>(gpu_decode_us);
	result["q3_gpu_decode_valid"] = gpu_decode_valid;
	result["shutdown"] = shutdown_requested.load(std::memory_order_acquire);
	const Dictionary q3_report = q3_adapter.get_report();
	const Array q3_keys = q3_report.keys();
	for (int64_t i = 0; i < q3_keys.size(); ++i)
		result[q3_keys[i]] = q3_report[q3_keys[i]];
	return result;
}

FrameFxCompositorEffect::FrameFxCompositorEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	set_enabled(true);
}

FrameFxCompositorEffect::~FrameFxCompositorEffect() = default;

void FrameFxCompositorEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&FrameFxCompositorEffect::get_backend_report);
}

void FrameFxCompositorEffect::compile_q3_frame(Node *p_scope,
		Viewport *p_viewport, Camera3D *p_camera) {
	if (impl_ && !impl_->shutdown_requested.load(std::memory_order_acquire))
		impl_->q3_adapter.compile_frame(p_scope, p_viewport, p_camera);
}

void FrameFxCompositorEffect::clear_q3_frame() {
	if (impl_)
		impl_->q3_adapter.clear_frame();
}

void FrameFxCompositorEffect::publish_screen_effects(
		const std::shared_ptr<const FrameFxScreenFrame> &p_frame) {
	if (!impl_)
		return;
	std::lock_guard<std::mutex> lock(impl_->publication_mutex);
	impl_->published_screen = p_frame;
}

void FrameFxCompositorEffect::publish_scanline_texels(
		const std::vector<std::uint8_t> &p_texels) {
	if (!impl_)
		return;
	std::lock_guard<std::mutex> lock(impl_->publication_mutex);
	impl_->published_scanlines = p_texels;
	++impl_->published_scanline_generation;
}

void FrameFxCompositorEffect::release_device_resources() {
	set_enabled(false);
	if (!impl_ || impl_->shutdown_requested.exchange(true,
			std::memory_order_acq_rel))
		return;
	impl_->q3_adapter.clear_frame();
	impl_->release_all();
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	impl_->rd_available = false;
	impl_->status = "shutdown";
	impl_->failure.clear();
}

void FrameFxCompositorEffect::set_gpu_timing_enabled(bool p_enabled) {
	if (impl_)
		impl_->gpu_timing_enabled.store(p_enabled, std::memory_order_relaxed);
}

Dictionary FrameFxCompositorEffect::get_backend_report() const {
	return impl_ ? impl_->report() : Dictionary();
}

Ref<Image> FrameFxCompositorEffect::capture_q3_target() {
	return impl_ ? impl_->capture_q3_target() : Ref<Image>();
}

void FrameFxCompositorEffect::_render_callback(
		int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (!impl_ || impl_->shutdown_requested.load(std::memory_order_acquire))
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->callback_seen = true;
	}
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT) {
		impl_->set_failure("FrameFX invoked at the wrong callback",
				"callback_mismatch");
		return;
	}
	impl_->render(p_render_data);
}

FrameFx::FrameFx() = default;

FrameFx::~FrameFx() = default;

void FrameFx::register_q3_object_material(const Ref<Material> &p_material,
		const opennova::renderer::ObjectMaterialClassification &p_classification) {
	Q3SourceRegistry::register_object_material(p_material, p_classification);
}

void FrameFx::clone_q3_object_material(const Ref<Material> &p_source,
		const Ref<Material> &p_clone) {
	Q3SourceRegistry::clone_object_material(p_source, p_clone);
}

void FrameFx::invalidate_q3_object_material(const Ref<Material> &p_material) {
	Q3SourceRegistry::invalidate_object_material(p_material);
}

void FrameFx::register_q3_object_source(GeometryInstance3D *p_source,
		const Ref<Material> &p_material) {
	Q3SourceRegistry::register_object_source(p_source, p_material);
}

void FrameFx::unregister_q3_source(GeometryInstance3D *p_source) {
	Q3SourceRegistry::unregister_source(p_source);
}

void FrameFx::set_q3_celestial_self_lum(GeometryInstance3D *p_source,
		const Vector3 &p_self_lum) {
	Q3SourceRegistry::set_celestial_self_lum(p_source,
			{p_self_lum.x, p_self_lum.y, p_self_lum.z});
}

bool FrameFx::q3_object_material_classification(const Ref<Material> &p_material,
		opennova::renderer::ObjectMaterialClassification &r_classification) {
	return Q3SourceRegistry::object_material_classification(p_material,
			r_classification);
}

void FrameFx::register_q3_source(GeometryInstance3D *p_source,
		opennova::renderer::Q3Source p_kind) {
	Q3SourceRegistry::register_source(p_source, p_kind);
}

void FrameFx::publish_q3_geometry(GeometryInstance3D *p_source, int p_surface,
		const Array &p_arrays) {
	Q3SourceRegistry::publish_geometry(p_source, p_surface, p_arrays);
}

void FrameFx::invalidate_q3_source(GeometryInstance3D *p_source) {
	Q3SourceRegistry::invalidate_source(p_source);
}

void FrameFx::invalidate_q3_instances(GeometryInstance3D *p_source) {
	Q3SourceRegistry::invalidate_instances(p_source);
}

void FrameFx::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&FrameFx::get_backend_report);
	ClassDB::bind_method(D_METHOD("advance_frame"),
			&FrameFx::advance_frame);
	ClassDB::bind_method(D_METHOD("shutdown"), &FrameFx::shutdown);
	ClassDB::bind_method(D_METHOD("get_q3_target_image"),
			&FrameFx::get_q3_target_image);
	ClassDB::bind_method(D_METHOD("set_view_effects", "red_word", "camera_mode",
			"local_dead", "in_session", "death_elapsed_ticks", "thermal_view",
			"monitor_view", "nvg_active", "death_screen_active"),
			&FrameFx::set_view_effects_values);
	ClassDB::bind_method(D_METHOD("advance_screen_effects"),
			&FrameFx::advance_screen_effects);
	ClassDB::bind_method(D_METHOD("init_mission_textures"),
			&FrameFx::init_mission_textures);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("invalidate_q3_source", "source"),
			&FrameFx::invalidate_q3_source);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("invalidate_q3_object_material", "material"),
			&FrameFx::invalidate_q3_object_material);
	BIND_CONSTANT(kBeautyCameraMask);
}

Ref<Image> FrameFx::get_q3_target_image() const {
	return terminal_effect_.is_valid() ?
			terminal_effect_->capture_q3_target() : Ref<Image>();
}

void FrameFx::build_compositor() {
	if (shutdown_)
		return;
	if (terminal_effect_.is_null())
		terminal_effect_.instantiate();
	terminal_effect_->set_gpu_timing_enabled(gpu_timing_enabled_);
	// A rebuilt terminal effect (tree re-entry) keeps the mission's texture.
	if (!scanline_texels_.empty())
		terminal_effect_->publish_scanline_texels(scanline_texels_);
}

void FrameFx::set_gpu_timing_enabled(bool p_enabled) {
	gpu_timing_enabled_ = p_enabled;
	if (terminal_effect_.is_valid())
		terminal_effect_->set_gpu_timing_enabled(p_enabled);
}

void FrameFx::install_compositor() {
	if (shutdown_)
		return;
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment == nullptr || terminal_effect_.is_null())
		return;
	world_environment_id_ = ObjectID(world_environment->get_instance_id());
	previous_compositor_ = world_environment->get_compositor();
	installed_compositor_ = compositor_with_terminal(previous_compositor_,
			terminal_effect_);
	world_environment->set_compositor(installed_compositor_);
}

void FrameFx::uninstall_compositor() {
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	if (world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_)
		world_environment->set_compositor(previous_compositor_);
	installed_compositor_.unref();
	previous_compositor_.unref();
	world_environment_id_ = ObjectID();
}

void FrameFx::restore_synced_camera_mask() {
	if (!has_synced_camera_mask_ || !synced_camera_id_.is_valid()) {
		has_synced_camera_mask_ = false;
		synced_camera_id_ = ObjectID();
		return;
	}
	Camera3D *camera = Object::cast_to<Camera3D>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(synced_camera_id_)));
	if (camera != nullptr && camera->get_cull_mask() == kBeautyCameraMask)
		camera->set_cull_mask(synced_camera_original_mask_);
	has_synced_camera_mask_ = false;
	synced_camera_id_ = ObjectID();
}

void FrameFx::advance_frame() {
	if (shutdown_ || terminal_effect_.is_null())
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const bool active = camera != nullptr && is_visible_in_tree();
	if (!active) {
		restore_synced_camera_mask();
		terminal_effect_->clear_q3_frame();
		return;
	}
	const ObjectID camera_id(camera->get_instance_id());
	if (!has_synced_camera_mask_ || synced_camera_id_ != camera_id) {
		restore_synced_camera_mask();
		synced_camera_id_ = camera_id;
		synced_camera_original_mask_ = camera->get_cull_mask();
		has_synced_camera_mask_ = true;
	}
	// Standardize the highest-quality retail beauty pass. The layers removed
	// here are rendered only by their dedicated capture/viewmodel devices.
	camera->set_cull_mask(kBeautyCameraMask);
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	terminal_effect_->compile_q3_frame(scope, viewport, camera);
}

void FrameFx::set_view_effects(const opennova::renderer::FrameFxViewInputs &p_view) {
	view_effects_ = p_view;
}

void FrameFx::set_view_effects_values(int p_red_word, int p_camera_mode,
		bool p_local_dead, bool p_in_session, int p_death_elapsed_ticks,
		bool p_thermal_view, bool p_monitor_view, bool p_nvg_active,
		bool p_death_screen_active) {
	opennova::renderer::FrameFxViewInputs view;
	view.red_word = p_red_word;
	view.camera_mode = p_camera_mode;
	view.local_dead = p_local_dead;
	view.in_session = p_in_session;
	view.death_elapsed_ticks = p_death_elapsed_ticks;
	view.thermal_view = p_thermal_view;
	view.monitor_view = p_monitor_view;
	view.nvg_active = p_nvg_active;
	view.death_screen_active = p_death_screen_active;
	set_view_effects(view);
}

void FrameFx::set_distortion_drawer(
		const std::shared_ptr<FrameFxDistortionDrawer> &p_drawer) {
	distortion_drawer_ = p_drawer;
}

void FrameFx::advance_screen_effects() {
	if (shutdown_ || terminal_effect_.is_null())
		return;
	opennova::renderer::FrameFxFrameInputs inputs;
	inputs.view = view_effects_;
	inputs.distortion_present = distortion_drawer_ &&
			distortion_drawer_->frame_has_distortion();
	// GetTickCount's millisecond clock; only its low bits reach the angle.
	inputs.clock_ms = static_cast<std::uint32_t>(Time::get_singleton()->get_ticks_msec());
	auto frame = std::make_shared<FrameFxScreenFrame>();
	frame->frame_id = ++screen_frame_id_;
	frame->plan = opennova::renderer::plan_frame_fx(inputs, planner_state_,
			[]() { return opennova::crt::crt_rand15(); });
	frame->distortion = distortion_drawer_;
	terminal_effect_->publish_screen_effects(frame);
}

void FrameFx::init_mission_textures() {
	scanline_texels_ = opennova::renderer::frame_fx_scanline_texels(
			[]() { return opennova::crt::crt_rand15(); });
	if (terminal_effect_.is_valid())
		terminal_effect_->publish_scanline_texels(scanline_texels_);
}

void FrameFx::shutdown() {
	if (shutdown_)
		return;
	shutdown_ = true;

	Ref<FrameFxCompositorEffect> effect = terminal_effect_;
	if (effect.is_valid())
		effect->set_enabled(false);
	restore_synced_camera_mask();
	uninstall_compositor();

	// Detaching only changes the next render setup. Drain any callback already
	// submitted before freeing the RenderingDevice objects it may still read.
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr)
		server->force_sync();
	if (effect.is_valid())
		effect->release_device_resources();
	terminal_effect_.unref();
	effect.unref();
}

void FrameFx::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Re-entry after an exit-tree (or explicit) shutdown, the same
		// contract DisplayDecode keeps: the released terminal effect is rebuilt
		// by the READY leg below, so clear the latch and ask for that leg again
		// (READY fires only once on its own).
		if (shutdown_) {
			shutdown_ = false;
			request_ready();
		}
	} else if (p_what == NOTIFICATION_READY) {
		build_compositor();
		install_compositor();
		// One placement-independent sync so a headless/no-pipeline embedder
		// still boots with coherent views; the live per-frame sync is the
		// ordered GameWorld leg, never a process callback.
		advance_frame();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		shutdown();
	}
}

Dictionary FrameFx::get_backend_report() const {
	Dictionary result = terminal_effect_.is_valid() ?
			terminal_effect_->get_backend_report() :
			Dictionary();
	result["beauty_camera_mask"] = static_cast<int64_t>(kBeautyCameraMask);
	result["q3_working_height"] = static_cast<int64_t>(kWorkSide);
	result["shutdown"] = shutdown_;
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	result["terminal_compositor_installed"] = world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_;
	return result;
}

void DisplayDecode::_bind_methods() {}

void DisplayDecode::install() {
	if (effect_.is_null())
		effect_.instantiate();
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment != nullptr) {
		world_environment_id_ = ObjectID(world_environment->get_instance_id());
		previous_compositor_ = world_environment->get_compositor();
		installed_compositor_ = compositor_with_terminal(previous_compositor_,
				effect_);
		world_environment->set_compositor(installed_compositor_);
		return;
	}
	// No WorldEnvironment in scope: attach the compositor to the viewport's
	// scenario directly. A view without a WorldEnvironment has no compositor
	// of its own, so there is nothing to merge or restore beyond clearing it.
	Viewport *viewport = get_viewport();
	world_ = viewport != nullptr ? viewport->find_world_3d() : Ref<World3D>();
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_null() || server == nullptr)
		return;
	installed_compositor_ = compositor_with_terminal(Ref<Compositor>(), effect_);
	server->scenario_set_compositor(world_->get_scenario(),
			installed_compositor_->get_rid());
}

void DisplayDecode::uninstall() {
	Ref<FrameFxCompositorEffect> effect = effect_;
	if (effect.is_valid())
		effect->set_enabled(false);
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	if (world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_)
		world_environment->set_compositor(previous_compositor_);
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_valid() && server != nullptr)
		server->scenario_set_compositor(world_->get_scenario(), RID());
	world_environment_id_ = ObjectID();
	world_.unref();
	installed_compositor_.unref();
	previous_compositor_.unref();
	if (effect.is_valid() && server != nullptr &&
			server->get_rendering_device() != nullptr)
		server->force_sync();
	if (effect.is_valid())
		effect->release_device_resources();
	effect_.unref();
	effect.unref();
}

void DisplayDecode::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		install();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		uninstall();
	}
}
