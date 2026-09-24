#pragma once

// Portable tracer-ribbon compilation — the styled half of the tracer/trail
// pool split (the SIM owns the point rings in world/tracer_trails.h; this
// module owns the witnessed style tables and the ribbon build). No Godot or
// simulator dependencies: channels arrive as raw point runs, the result is a
// vertex/index list an embedding renderer uploads verbatim, one draw per
// channel in pool order.
//
// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0 — the normal pass
// (@ 0x5DC0BB..0x5DC8E0) and the distortion pass (@ 0x5DB8D4..0x5DC0B6);
// Render_DrawTrailOrBeamSegments @ 0x5DCB80 is the same build over a caller's
// point run (the NVG laser). Styles: the 0x830-B blocks — static .data
// g_TracerStyle_Std*/Rapid*/Sniper* @ 0x8437F0..0x8460E0, runtime-built
// DF1/rocket/at4/grenade/NVG in CEffectEmitterPool_ResetAndBuildStyles
// @ 0x5DB3A0; the channel's materials in CEffectChannel_Init @ 0x5DB130.]

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// The material a ribbon draw binds [orig: CEffectChannel_Init @ 0x5DB130 —
// channel+0x20 is the normal-pass shader, channel+0x24 the distortion shader;
// the shaders are built by create_effect_channel_render_textures @ 0x5DC8F0].
enum class TracerShader : std::uint8_t {
	// The device's stock slot 6 (mode word 0x222): ONE/ONE, colour = alpha =
	// DIFFUSE, untextured [orig: CD3DDevice_GetRenderStateByIndex(6) @ 0x5DB1D6;
	// the stock block built by sub_6780A0 @ 0x678113]. std/rapid/sniper/df1.
	Stock = 0,
	// pool+0x3004 (rocket/at4/grenade): smoktest.pcx twice, SRCALPHA/INVSRCALPHA,
	// colour = DIFFUSE, alpha = sat(sat(DIFFUSE.a - T0.a) - T1.a) (T1 on uv1).
	Smoke = 1,
	// pool+0x3008 (the NVG laser): smoktest.pcx twice, SRCALPHA/ONE, colour =
	// DIFFUSE, alpha = DIFFUSE.a * (1 - T0.a) * (1 - T1.a).
	NvgLaser = 2,
	// pool+0x300C, the distortion pass: one stage, SRCALPHA/INVSRCALPHA, colour =
	// the FrameFX screen capture sampled at the vertex's own projected position,
	// alpha = DIFFUSE.a.
	Distortion = 3,
};

// One style row [orig: the 0x830-B style block].
struct TracerStyleView {
	const std::uint32_t *colors = nullptr; // +0x18 ARGB ramp, count +0x10
	int color_count = 0;
	const float *sizes = nullptr;          // +0x41C half-width curve, count +0x418
	int size_count = 0;
	std::uint32_t base_argb = 0;           // +0x14: the oldest point's colour
	// +0: the normal pass fogs to BLACK (SetFogAndBlendMode mode 2) instead of
	// the scene fog colour (mode 0) [orig: @ 0x5DC84F..0x5DC860].
	bool fog_black = false;
	// +4: the four-vertex textured cross-section (and the append-time width
	// jitter, world/tracer_trails.h) instead of the two-vertex strip.
	bool cross_section = false;
	// +0x81C / +0x820 / +0x824: the cross-section texture scroll terms.
	float wave_u = 0.0f;
	float wave_v = 0.0f;
	float wave_t = 0.0f;
	// +0x828: the channel also draws in the distortion pass
	// [orig: CEffectEmitterPool_RenderDistortionPass @ 0x5DCB58].
	bool distortion = false;
	TracerShader shader = TracerShader::Stock; // the normal-pass material
};

// Style ids 1..12 are the ammo.def `tracer_type` ids (stdred 1, stdgreen 2,
// rocket 3, at4 4, grenade 5, rapidred 6, rapidgreen 7, 8 = the NVG laser,
// sniperred 9, snipergreen 10, df1red 11, df1green 12); any other id takes
// the stdred block [orig: the CEffectChannel_Init default case @ 0x5db1c8].
const TracerStyleView &tracer_style(int style_id);

struct TracerVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// The camera terms of one ribbon build. Coordinates are any right-handed frame
// that is a rotation of the mission frame (mission or Godot); see
// compile_tracer_ribbons for the one handedness-dependent step.
struct TracerView {
	TracerVec3 camera;                  // the eye [orig: byte_A78364]
	// The view matrix's third column: the look direction, either sign
	// [orig: viewMatrix @ 0xA7845C, _13/_23/_33 = flt_A78464/74/84].
	TracerVec3 forward{0.0f, 0.0f, 1.0f};
	// The projection's _11 (cot(horizontal fov / 2)) [orig: `mat` @ 0x2721980].
	float projection_x_scale = 1.0f;
	// The wall clock in milliseconds [orig: GetTickCount @ 0x5DC104].
	std::uint32_t tick_ms = 0;
};

// One live trail channel: `points` is count x 4 floats {x, y, z, w} where w is
// the per-point width multiplier stamped at append time (world/tracer_trails.h).
struct TracerChannelInput {
	int style_id = 0;
	int age = 0;
	int count = 0;
	const float *points = nullptr;
};

// One ribbon vertex [orig: the 36-B FVF XYZ|DIFFUSE|SPECULAR|TEX2 vertex
// in g_TrailStripVertexScratch; the specular dword is never written]. The
// two-vertex strip writes no texture coordinates (they stay zero here).
struct TracerVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint32_t argb = 0;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
};

// One channel's draw: a triangle list over `indices[first_index ..
// first_index + index_count)`. The strip form is expanded to its triangle
// list (the pass culls nothing, so the strip's alternating winding is
// immaterial).
struct TracerDraw {
	TracerShader shader = TracerShader::Stock;
	bool fog_black = false;
	std::uint32_t first_vertex = 0; // the draw's vertices are contiguous
	std::uint32_t vertex_count = 0;
	std::uint32_t first_index = 0;
	std::uint32_t index_count = 0;
};

struct TracerRibbonFrame {
	std::vector<TracerVertex> vertices;
	std::vector<std::uint32_t> indices;
	std::vector<TracerDraw> draws;
	int channels = 0;

	void clear() {
		vertices.clear();
		indices.clear();
		draws.clear();
		channels = 0;
	}
};

enum class TracerPass : std::uint8_t {
	Main = 0,       // CEffectEmitterPool_RenderMainPass @ 0x5DCAF0
	Distortion = 1, // CEffectEmitterPool_RenderDistortionPass @ 0x5DCB40
};

// The witnessed ribbon build over points [0 .. count-2] (the newest point
// steers direction only). The distortion pass takes only styles with the
// +0x828 flag. Channels with a positive count are counted in `channels`
// whether or not they produce a draw. Retail builds the segment's plane in
// its Y-negated upload frame, a mirror of the mission frame; there
// normalize(p - eye) x normalize(next - p) is, in this frame,
// normalize(next - p) x normalize(p - eye).
void compile_tracer_ribbons(const TracerChannelInput *channels, std::size_t count,
		const TracerView &view, TracerPass pass, TracerRibbonFrame &out);

// The ladder rung the frame's ribbon draws take. The pool draws once per frame,
// in whichever of its two scene calls has exactly one nonzero argument: the
// far-side call while the eye is below the water, the camera-side call when
// it is at or above it [orig: CEffectEmitterPool_RenderMainPass
// @ 0x5DCAF0..0x5DCB05; calls (0, eyeBelow) @ 0x5C95AC and (1, eyeBelow)
// @ 0x5C9687, eyeBelow = setz @ 0x5C959D over the eye-above latch
// setnl @ 0x5C93B0].
int tracer_rung(bool camera_above_water);

// One caller-supplied point run drawn in the normal pass with a style's
// material [orig: Render_DrawTrailOrBeamSegments @ 0x5DCB80]: the same
// build, the ramp anchored to its end (age = colour count - point count when
// the run is shorter than the ramp, else 0 @ 0x5DCE77). Appends one draw.
void append_tracer_beam(const float *points, int count, int style_id,
		const TracerView &view, TracerRibbonFrame &out);

}  // namespace opennova::renderer
