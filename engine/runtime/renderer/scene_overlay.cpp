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

bool scene_overlay_view_draws(const SceneOverlayBatch &batch, float eye_height) {
	// [orig: @ 0x5c96ca cmp [eye+0Ch], Env_WaterHeightFixed; @ 0x5c96d1 jg
	//  skips only a strictly-higher eye]
	return !batch.eye_at_or_below_water_gate || eye_height <= batch.water_height;
}

void append_precipitation_overlay(const PrecipitationDrawFrame &precipitation,
		uint32_t texture, SceneOverlayFrame &out) {
	if (precipitation.drops <= 0) {
		return;
	}
	const std::size_t count = std::min(precipitation.vertices.size() / 5,
			static_cast<std::size_t>(precipitation.drops) * 3u);
	if (count < 3) {
		return;
	}
	// The one diffuse of the frame [orig: Env_TerrainLightCombined | 0xFF000000].
	const uint32_t argb = precipitation.color_argb;
	const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
	const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
	const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
	const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
	SceneOverlayBatch batch;
	batch.slot = SceneOverlaySlot::Precipitation;
	batch.shading = SceneOverlayShading::Modulate2xBlend;
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
		SceneOverlayFrame &out) {
	if (quads.empty()) {
		return;
	}
	SceneOverlayBatch batch;
	batch.slot = SceneOverlaySlot::LightCoronas;
	batch.shading = SceneOverlayShading::AdditiveModulate;
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
	// The viewport rectangle [orig: Terrain_DrawScissorRect @ 0x5c38ff reads
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
		const SceneOverlayFog &fog, SceneOverlayFrame &out) {
	for (const TracerDraw &draw : ribbons.draws) {
		if (draw.index_count < 3) {
			continue;
		}
		SceneOverlayBatch batch;
		batch.slot = SceneOverlaySlot::NvgLaserBeams;
		batch.shading = SceneOverlayShading::NvgLaser;
		batch.depth = SceneOverlayDepth::TestNoWrite;
		batch.geometry = SceneOverlayGeometry::World;
		batch.texture = texture;
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
			const float dx = src.x - fog.eye[0];
			const float dy = src.y - fog.eye[1];
			const float dz = src.z - fog.eye[2];
			const float depth = dx * fog.forward[0] + dy * fog.forward[1] + dz * fog.forward[2];
			const float radial = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float visibility = device_fog_visibility(
					fog.type == 0 ? depth : radial, fog.start, fog.end, fog.type, fog.enabled);
			float rgb[3] = {static_cast<float>((src.argb >> 16) & 0xFFu) / 255.0f,
					static_cast<float>((src.argb >> 8) & 0xFFu) / 255.0f,
					static_cast<float>(src.argb & 0xFFu) / 255.0f};
			for (int c = 0; c < 3; ++c) {
				rgb[c] = draw.fog_black ? rgb[c] * visibility
										: fog.color[c] + (rgb[c] - fog.color[c]) * visibility;
			}
			set_color(v, rgb[0], rgb[1], rgb[2],
					static_cast<float>((src.argb >> 24) & 0xFFu) / 255.0f);
			out.vertices.push_back(v);
		}
		batch.vertex_count = count;
		out.batches.push_back(batch);
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
	// The full target, one colour on all four corners [orig: render_main_scene
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
