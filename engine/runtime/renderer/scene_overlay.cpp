#include <runtime/renderer/scene_overlay.h>

#include <runtime/renderer/device_fog.h>

#include <algorithm>
#include <cmath>

namespace opennova::renderer {

namespace {

void set_color(SceneOverlayVertex &v, float r, float g, float b, float a) {
	v.color[0] = r;
	v.color[1] = g;
	v.color[2] = b;
	v.color[3] = a;
}

uint32_t vertex_index(const SceneOverlayFrame &frame) {
	return static_cast<uint32_t>(frame.vertices.size());
}

// One tracer-pool draw of `ribbons` as `batch` (its slot, combine, stage and
// texture set by the caller): its triangle list depth-tested without writes,
// the diffuse unfogged and the fog term per vertex under `fog`, toward black
// for a style whose +0 word fogs to black, else toward the pass colour. The
// primary fog state's arms (CD3DDevice_SetFogParameters, renderer/device_fog.h):
// table EXP fog by the eye depth, else vertex LINEAR fog by the range, or by the
// eye depth over the parked range.
void append_fogged_tracer_draw(const TracerRibbonFrame &ribbons, const TracerDraw &draw,
		SceneOverlayBatch batch, const SceneOverlayFog &fog, SceneOverlayFrame &out) {
	const bool exponent = fog.enabled && device_fog_exponential(fog.start, fog.end, fog.type);
	const float density = exponent ? kDeviceFogLn64 / fog.end : 0.0f;
	batch.depth = SceneOverlayDepth::TestNoWrite;
	batch.geometry = SceneOverlayGeometry::World;
	batch.fog_term = exponent ? SceneOverlayFogTerm::Exponent : SceneOverlayFogTerm::Factor;
	for (int c = 0; c < 3; ++c) {
		batch.fog_color[c] = draw.fog_black ? 0.0f : fog.color[c];
	}
	batch.first_vertex = vertex_index(out);
	const uint32_t count = draw.index_count - draw.index_count % 3;
	for (uint32_t k = 0; k < count; ++k) {
		const TracerVertex &src = ribbons.vertices[ribbons.indices[draw.first_index + k]];
		SceneOverlayVertex v;
		v.position[0] = src.x;
		v.position[1] = src.y;
		v.position[2] = src.z;
		v.uv[0] = src.u0;
		v.uv[1] = src.v0;
		v.corner[0] = src.u1;
		v.corner[1] = src.v1;
		set_color(v, static_cast<float>((src.argb >> 16) & 0xFFu) / 255.0f,
				static_cast<float>((src.argb >> 8) & 0xFFu) / 255.0f,
				static_cast<float>(src.argb & 0xFFu) / 255.0f,
				static_cast<float>((src.argb >> 24) & 0xFFu) / 255.0f);
		const float dx = src.x - fog.eye[0];
		const float dy = src.y - fog.eye[1];
		const float dz = src.z - fog.eye[2];
		const float view_depth = dx * fog.forward[0] + dy * fog.forward[1] + dz * fog.forward[2];
		if (exponent) {
			// The signed eye depth: the pixel clamps it, so a strip that
			// passes beside the eye interpolates its true depth.
			v.fog = view_depth * density;
		} else {
			v.fog = device_fog_visibility(view_depth, std::sqrt(dx * dx + dy * dy + dz * dz),
					fog.start, fog.end, fog.type, fog.enabled);
		}
		out.vertices.push_back(v);
	}
	batch.vertex_count = count;
	out.batches.push_back(batch);
}

} // namespace

void compile_scene_overlay(const SceneOverlayFrame &frame, const SceneOverlaySlot *order,
		std::size_t order_count, std::vector<SceneOverlayBatch> &out) {
	out.clear();
	for (std::size_t i = 0; i < order_count; ++i) {
		for (const SceneOverlayBatch &batch : frame.batches) {
			if (batch.slot == order[i] && batch.vertex_count > 0) {
				out.push_back(batch);
			}
		}
	}
}

SceneOverlayBlend scene_overlay_blend(SceneOverlayShading shading) {
	using F = SceneOverlayBlendFactor;
	switch (shading) {
		case SceneOverlayShading::AdditiveModulate:
		case SceneOverlayShading::SelfLumAdditive:
		case SceneOverlayShading::RibbonStock:
			return {F::One, F::One};
		case SceneOverlayShading::DimMultiply:
			return {F::DestColor, F::Zero};
		case SceneOverlayShading::NvgLaser:
			return {F::SrcAlpha, F::One};
		default:
			// Modulate2xBlend, FlatBlend, RibbonSmoke.
			return {F::SrcAlpha, F::InvSrcAlpha};
	}
}

bool scene_overlay_view_draws(const SceneOverlayBatch &batch, float eye_height) {
	// [orig: @ 0x5c96ca cmp [eye+0Ch], g_EnvWaterHeightFixed; @ 0x5c96d1 jg
	//  skips only a strictly-higher eye]
	return !batch.eye_at_or_below_water_gate || eye_height <= batch.water_height;
}

void append_precipitation_overlay(const PrecipitationDrawFrame &precipitation,
		uint32_t texture, SceneOverlayFrame &out, SceneOverlaySlot slot) {
	if (precipitation.drops <= 0) {
		return;
	}
	const std::size_t count = std::min(precipitation.vertices.size() / 5,
			static_cast<std::size_t>(precipitation.drops) * 3u);
	if (count < 3) {
		return;
	}
	// The one diffuse of the frame [orig: g_EnvTerrainLightCombined | 0xFF000000].
	const uint32_t argb = precipitation.color_argb;
	const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
	const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
	const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
	const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
	SceneOverlayBatch batch;
	batch.slot = slot;
	batch.shading = SceneOverlayShading::Modulate2xBlend;
	batch.stage = TextureStage::Precipitation;
	batch.depth = SceneOverlayDepth::TestNoWrite;
	batch.geometry = SceneOverlayGeometry::World;
	batch.texture = texture;
	batch.first_vertex = vertex_index(out);
	batch.vertex_count = static_cast<uint32_t>(count - count % 3);
	for (std::size_t i = 0; i < batch.vertex_count; ++i) {
		const float *src = precipitation.vertices.data() + i * 5;
		SceneOverlayVertex v;
		v.position[0] = src[0];
		v.position[1] = src[1];
		v.position[2] = src[2];
		v.uv[0] = src[3];
		v.uv[1] = src[4];
		set_color(v, r, g, b, a);
		out.vertices.push_back(v);
	}
	out.batches.push_back(batch);
}

void append_corona_overlay(const std::vector<LightCoronaQuad> &quads, uint32_t texture,
		SceneOverlayFrame &out, SceneOverlaySlot slot) {
	if (quads.empty()) {
		return;
	}
	SceneOverlayBatch batch;
	batch.slot = slot;
	batch.shading = SceneOverlayShading::AdditiveModulate;
	batch.stage = TextureStage::LightCorona;
	batch.depth = SceneOverlayDepth::TestNoWrite;
	batch.geometry = SceneOverlayGeometry::Billboard;
	batch.texture = texture;
	batch.first_vertex = vertex_index(out);
	// Two triangles per quad: (-,-) (+,-) (-,+) and (+,-) (+,+) (-,+), uv v
	// down the view's up axis.
	static constexpr float kCorner[6][2] = {
		{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f},
		{1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
	};
	for (const LightCoronaQuad &quad : quads) {
		// Mission (x, y, z) -> render (x, z, -y).
		const float cx = quad.center[0];
		const float cy = quad.center[2];
		const float cz = -quad.center[1];
		for (const auto &corner : kCorner) {
			SceneOverlayVertex v;
			v.position[0] = cx;
			v.position[1] = cy;
			v.position[2] = cz;
			v.corner[0] = corner[0] * quad.half_size;
			v.corner[1] = corner[1] * quad.half_size;
			v.uv[0] = 0.5f + 0.5f * corner[0];
			v.uv[1] = 0.5f - 0.5f * corner[1];
			set_color(v, quad.rgb[0], quad.rgb[1], quad.rgb[2], 1.0f);
			out.vertices.push_back(v);
		}
	}
	batch.vertex_count = vertex_index(out) - batch.first_vertex;
	out.batches.push_back(batch);
}

void append_underwater_murk_overlay(const float rgb[3], uint8_t alpha_byte, float water_height,
		SceneOverlayFrame &out) {
	SceneOverlayBatch batch;
	batch.slot = SceneOverlaySlot::UnderwaterMurk;
	batch.shading = SceneOverlayShading::FlatBlend;
	batch.depth = SceneOverlayDepth::Always;
	batch.geometry = SceneOverlayGeometry::Screen;
	batch.texture = kSceneOverlayNoTexture;
	batch.eye_at_or_below_water_gate = true;
	batch.water_height = water_height;
	batch.first_vertex = vertex_index(out);
	// The viewport rectangle [orig: Render_DrawViewportColorQuad @ 0x5c38ff reads
	// CD3DDevice_GetViewportRect; one colour, alpha << 24, on all four corners].
	static constexpr float kCorner[6][2] = {
		{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f},
		{1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
	};
	const float alpha = static_cast<float>(alpha_byte) / 255.0f;
	for (const auto &corner : kCorner) {
		SceneOverlayVertex v;
		v.position[0] = corner[0];
		v.position[1] = corner[1];
		set_color(v, rgb[0], rgb[1], rgb[2], alpha);
		out.vertices.push_back(v);
	}
	batch.vertex_count = 6;
	out.batches.push_back(batch);
}

void append_self_lum_overlay(SceneOverlaySlot slot, const float *positions, const float *uvs,
		std::size_t vertex_count, const float self_lum_rgb[3], const float light_scale_rgb[3],
		float fog_visibility, uint32_t texture, SceneOverlayFrame &out,
		SceneOverlayDepth depth) {
	const std::size_t count = vertex_count - vertex_count % 3;
	if (count == 0 || positions == nullptr || uvs == nullptr) {
		return;
	}
	SceneOverlayBatch batch;
	batch.slot = slot;
	batch.shading = SceneOverlayShading::SelfLumAdditive;
	batch.stage = TextureStage::ObjectStage;
	batch.depth = depth;
	batch.geometry = SceneOverlayGeometry::World;
	batch.texture = texture;
	batch.first_vertex = vertex_index(out);
	batch.vertex_count = static_cast<uint32_t>(count);
	// sat(SelfLumColor x light scale): the SELFLUM combine's diffuse term; the
	// fog visibility rides the alpha so the shader fogs the saturated combine.
	const float r = std::clamp(self_lum_rgb[0] * light_scale_rgb[0], 0.0f, 1.0f);
	const float g = std::clamp(self_lum_rgb[1] * light_scale_rgb[1], 0.0f, 1.0f);
	const float b = std::clamp(self_lum_rgb[2] * light_scale_rgb[2], 0.0f, 1.0f);
	const float fog = std::clamp(fog_visibility, 0.0f, 1.0f);
	for (std::size_t i = 0; i < count; ++i) {
		SceneOverlayVertex v;
		v.position[0] = positions[i * 3 + 0];
		v.position[1] = positions[i * 3 + 1];
		v.position[2] = positions[i * 3 + 2];
		v.uv[0] = uvs[i * 2 + 0];
		v.uv[1] = uvs[i * 2 + 1];
		set_color(v, r, g, b, fog);
		out.vertices.push_back(v);
	}
	out.batches.push_back(batch);
}

void append_nvg_laser_overlay(const TracerRibbonFrame &ribbons, uint32_t texture,
		const SceneOverlayFog &fog, SceneOverlayFrame &out, SceneOverlaySlot slot) {
	// [orig: Render_DrawTrailOrBeamSegments @ 0x5dcb80 - CD3DDevice_SetFogAndBlendMode the
	//  call @ 0x5ddb72 on the style's +0 word @ 0x5ddb62, then the pass under the
	//  device fog, GfxShader_ApplyPassChecked(0x10520000) the call @ 0x5ddb86]
	for (const TracerDraw &draw : ribbons.draws) {
		if (draw.index_count < 3) {
			continue;
		}
		SceneOverlayBatch batch;
		batch.slot = slot;
		batch.shading = SceneOverlayShading::NvgLaser;
		batch.stage = TextureStage::TracerSmoke;
		batch.texture = texture;
		append_fogged_tracer_draw(ribbons, draw, batch, fog, out);
	}
}

void append_tracer_ribbon_overlay(const TracerRibbonFrame &ribbons, uint32_t smoke_texture,
		const SceneOverlayFog &fog, SceneOverlayFrame &out, SceneOverlaySlot slot) {
	for (const TracerDraw &draw : ribbons.draws) {
		if (draw.index_count < 3) {
			continue;
		}
		SceneOverlayBatch batch;
		batch.slot = slot;
		batch.shading = draw.shader == TracerShader::Smoke ? SceneOverlayShading::RibbonSmoke
				: draw.shader == TracerShader::NvgLaser    ? SceneOverlayShading::NvgLaser
														   : SceneOverlayShading::RibbonStock;
		if (batch.shading != SceneOverlayShading::RibbonStock) {
			batch.stage = TextureStage::TracerSmoke;
			batch.texture = smoke_texture;
		}
		append_fogged_tracer_draw(ribbons, draw, batch, fog, out);
	}
}

void append_mirror_dim_overlay(float factor, SceneOverlayFrame &out) {
	SceneOverlayBatch batch;
	batch.slot = SceneOverlaySlot::MirrorDim;
	batch.shading = SceneOverlayShading::DimMultiply;
	batch.depth = SceneOverlayDepth::Always;
	batch.geometry = SceneOverlayGeometry::Screen;
	batch.texture = kSceneOverlayNoTexture;
	batch.first_vertex = vertex_index(out);
	// The full target, one colour on all four corners [orig: Render_MainScene
	// @ 0x5c1882..0x5c1897, the strip's four diffuse stores].
	static constexpr float kCorner[6][2] = {
		{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f},
		{1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
	};
	for (const auto &corner : kCorner) {
		SceneOverlayVertex v;
		v.position[0] = corner[0];
		v.position[1] = corner[1];
		set_color(v, factor, factor, factor, 1.0f);
		out.vertices.push_back(v);
	}
	batch.vertex_count = 6;
	out.batches.push_back(batch);
}

} // namespace opennova::renderer
