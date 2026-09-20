#include "render/q3_frame_adapter.h"
#include "render/q3_geometry_cache.h"
#include "render/q3_source_registry.h"
#include "render/q3_vertex_format.h"
#include "render/rd_uniforms.h"

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

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
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
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "env/mission_environment.h"
#include "util/string_convert.h"

using namespace godot;
using namespace opennova::renderer;

namespace {

constexpr std::uint32_t kPushConstantBytes = 128u;

enum class Q3DeviceBlend : std::uint8_t {
	Replace,
	Alpha,
	Add,
	Water,
};

std::atomic<std::uint64_t> g_frame_id{1};

struct DeviceCommand {
	Q3DrawCommand draw{};
	// The immutable packed stream this command draws; the render side owns
	// one device buffer per cache entry and uploads each generation once.
	std::shared_ptr<const Q3PackedStream> stream;
	// Keep every sampled server resource alive across the main-thread compile
	// -> render-thread draw handoff; the RID is only the device lookup key.
	Ref<Texture2D> primary_texture_resource;
	Ref<Texture2D> secondary_texture_resource;
	Ref<Texture2D> tertiary_texture_resource;
	RID primary_texture;
	RID secondary_texture;
	RID tertiary_texture;
};

struct DeviceFrame {
	Q3DrawList draw_list;
	std::vector<DeviceCommand> commands;
	// Cache entries evicted on the main thread whose device buffers the render
	// side frees once it has consumed this frame (no later frame names them).
	std::vector<std::uint64_t> evicted_entries;
	Vector3 light_direction = Vector3(0, 1, 0);
	Vector3 light_gain = Vector3(1, 1, 1);
	bool fog_enabled = false;
	Vector3 fog_color;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	int fog_type = 0;
};

struct Candidate {
	Q3SubmissionSnapshot submission{};
	std::shared_ptr<const Q3PackedStream> stream;
	Ref<Texture2D> primary_texture_resource;
	Ref<Texture2D> secondary_texture_resource;
	Ref<Texture2D> tertiary_texture_resource;
	RID primary_texture;
	RID secondary_texture;
	RID tertiary_texture;
};

struct Q3Push {
	std::array<float, 16> mvp{};
	std::array<float, 4> camera_local{};
	std::array<float, 4> light_local_gain{};
	std::array<float, 4> draw_color{};
	std::array<float, 4> params{};
};

static_assert(sizeof(Q3Push) == kPushConstantBytes);

// The far-band remap constants come from runtime/renderer/q3_frame.h
// (kQ3FarBandMinZ/MaxZ); q3_vertex_shader_source() splices them in.
const char *kQ3VertexShaderTemplate = R"GLSL(#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 in_custom0;
layout(location = 5) in vec4 in_custom1;
layout(location = 6) in vec4 in_custom2;
layout(location = 7) in vec2 in_uv2;

layout(push_constant, std430) uniform Q3Push {
	mat4 mvp;
	vec4 camera_local;
	vec4 light_local_gain;
	vec4 draw_color;
	vec4 params;
} pc;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out vec4 custom0;
layout(location = 3) out vec4 custom1;
layout(location = 4) out vec4 custom2;
layout(location = 5) out vec3 local_normal;
layout(location = 6) out vec3 local_position;
layout(location = 7) out vec2 detail_uv;

void main() {
	vec3 draw_position = in_position;
	if (uint(pc.params.x + 0.5) <= 2u) {
		// The tracked relative view-depth pull (env-tod-re.md row 29): the
		// water's beauty rasterization runs this exact pull, and the object
		// copies re-test their OWN beauty depth through a CPU-composed
		// viewproj * inv(cam) * model chain that differs from Godot's GPU
		// proj * (view * model) at the ulp level. Pulling every object and
		// water Q3 vertex by the same factor keeps GREATER_OR_EQUAL against
		// resolved beauty depth from rejecting a random subset of a LUM
		// object's own fragments.
		draw_position = pc.camera_local.xyz +
				(in_position - pc.camera_local.xyz) * (1.0 - 3.0e-4);
	}
	gl_Position = pc.mvp * vec4(draw_position, 1.0);
	if (uint(pc.params.x + 0.5) >= 3u && gl_Position.w > 0.0) {
		// Render_SetViewportFarDepth's D3DVIEWPORT9 MinZ/MaxZ band for the
		// celestial discs and the sun glow, expressed in reverse-Z clip depth
		// (kQ3FarBandMinZ/MaxZ): z' = (1 - MaxZ) + z * (MaxZ - MinZ). The
		// ordinary GREATER_OR_EQUAL test then keeps them only over beauty
		// depth at or near the far plane.
		float z_rev = gl_Position.z / gl_Position.w;
		gl_Position.z = (@FAR_BAND_REV_MIN@ + z_rev * @FAR_BAND_REV_SPAN@) *
				gl_Position.w;
	}
	uv = in_uv;
	color = in_color;
	custom0 = in_custom0;
	custom1 = in_custom1;
	custom2 = in_custom2;
	local_normal = in_normal;
	local_position = draw_position;
	detail_uv = in_uv2;
}
)GLSL";

// The witnessed shading constants live in runtime/renderer/q3_frame.h
// (kQ3Glass* and kQ3WaterNv*); q3_fragment_shader_source() splices them into
// the @TOKEN@ slots below so the GLSL text never restates them.
const char *kQ3FragmentShaderTemplate = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D primary_texture;
layout(set = 0, binding = 1) uniform sampler2D secondary_texture;
layout(set = 0, binding = 2) uniform sampler2D tertiary_texture;

layout(push_constant, std430) uniform Q3Push {
	mat4 mvp;
	vec4 camera_local;
	vec4 light_local_gain;
	vec4 draw_color;
	vec4 params;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 custom0;
layout(location = 3) in vec4 custom1;
layout(location = 4) in vec4 custom2;
layout(location = 5) in vec3 local_normal;
layout(location = 6) in vec3 local_position;
layout(location = 7) in vec2 detail_uv;
layout(location = 0) out vec4 frag_color;

// The primary device fog, mirrored from the engine's
// runtime/renderer/device_fog.h (device_fog_visibility): type 0 is
// exponential with density ln(64)/end, every other type is linear from the
// caller's already-resolved Render_SetFogState start. The push block carries
// start in camera_local.w and end in draw_color.w for the object techniques.
float q3_fog_visibility(float dist, float fog_start, float fog_end,
		uint fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0u) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)),
				0.0, 1.0);
	}
	return clamp((safe_end - dist) / max(safe_end - fog_start, 1.0), 0.0, 1.0);
}

void main() {
	uint mode = uint(pc.params.x + 0.5);
	// Object-technique flag bits: 1 alpha test, 2 detail stage, 4 fog enabled,
	// 8|16 fog type, 32 regular fog (mix toward the fog colour carried in
	// light_local_gain.xyz; clear = the additive fold toward black).
	uint coverage_flags = uint(pc.params.y + 0.5);
	bool fog_enabled = (coverage_flags & 4u) != 0u;
	uint fog_type = (coverage_flags >> 3u) & 3u;
	if (mode <= 1u) {
		// Coverage: the LUM NORMAL block is the SELFLUM specialization, whose
		// MaterialDiffuse.a = 0 makes its alpha-test source 0 (the beauty
		// wrappers' OBJ_COVERAGE_ZERO); Glass.fx TECHNIQUE_GLOW keeps
		// Diffuse1's alpha only for the cutout variants. Neither consumes
		// alpha_mod (OBJ_ALPHA_MOD_NONE).
		float coverage = mode == 1u ? texture(primary_texture, uv).a : 0.0;
		if ((coverage_flags & 1u) != 0u) {
			bool passes = coverage > pc.params.z;
			if (pc.light_local_gain.w < 0.0) passes = !passes;
			if (!passes) discard;
		}
		float model_uniform_scale = max(abs(pc.light_local_gain.w), 1.0e-6);
		float fog_visibility = fog_enabled ? q3_fog_visibility(
				length(pc.camera_local.xyz - local_position) * model_uniform_scale,
				pc.camera_local.w, pc.draw_color.w, fog_type) : 1.0;
		if (mode == 0u) {
			// The LUM GLOW slot is a copy of the NORMAL block, re-shaded here as
			// the SELFLUM specialization (technique/self_lit.gdshaderinc): base =
			// Diffuse1 (x Detail MODULATE2X over UV2 for _MT), x u_rgb_mod x
			// min(gain, 1) x 2 (draw_color.rgb carries u_rgb_mod x gain x 2),
			// the wrapper's fog policy, and alpha 0 (SELFLUM MaterialDiffuse.a):
			// an AlphaBlend LUM contributes nothing, an Additive LUM adds its
			// colour, an opaque LUM replaces.
			vec3 base = texture(primary_texture, uv).rgb;
			if ((coverage_flags & 2u) != 0u) {
				base *= texture(secondary_texture, detail_uv).rgb * 2.0;
			}
			vec3 lit = base * pc.draw_color.rgb;
			vec3 fogged = (coverage_flags & 32u) != 0u ?
					mix(pc.light_local_gain.xyz, lit, fog_visibility) :
					lit * fog_visibility;
			frag_color = vec4(clamp(fogged, 0.0, 1.0), 0.0);
			return;
		}
		vec3 eye = normalize(pc.camera_local.xyz - local_position);
		vec3 reflected = reflect(-eye, normalize(local_normal));
		float aligned = max(dot(reflected,
				normalize(pc.light_local_gain.xyz)), 0.0);
		// kQ3GlassWhiteLobeGain/Power + kQ3GlassWarmLobeColor/Power.
		vec3 lobe = clamp(vec3(@GLASS_WHITE_GAIN@) *
				pow(aligned, @GLASS_WHITE_POWER@) +
				vec3(@GLASS_WARM_R@, @GLASS_WARM_G@, @GLASS_WARM_B@) *
						pow(aligned, @GLASS_WARM_POWER@), vec3(0.0), vec3(1.0));
		// TexCubeRotSpecular x ReflectColor x gain (x 2) under the additive
		// fog fold; no diffuse-alpha term.
		frag_color = vec4(clamp(pc.draw_color.rgb * lobe * fog_visibility,
				0.0, 1.0), 1.0);
		return;
	}
	if (mode == 2u) {
		vec4 noise = texture(primary_texture, uv);
		vec3 reflection = pc.draw_color.rgb;
		if (pc.params.y > 0.5) {
			vec3 dudv = texture(secondary_texture, uv).rgb * 2.0 - 1.0;
			vec2 refl_uv = vec2(dot(vec3(custom2.xy, custom0.z), dudv),
					dot(vec3(custom2.zw, custom0.w), dudv));
			refl_uv = vec2(0.5) + (refl_uv - vec2(0.5)) * pc.params.zw;
			reflection = texture(tertiary_texture,
					clamp(refl_uv, vec2(0.0), vec2(1.0))).rgb;
		}
		vec3 result = clamp(reflection * color.rgb * 2.0, 0.0, 1.0);
		result = clamp(result * noise.rgb * 4.0, 0.0, 1.0);
		result = clamp(result + custom1.rgb, 0.0, 1.0);
		// kQ3WaterNvLumaWeights + kQ3WaterNvBrightBias.
		float bright = clamp(pow(dot(result,
				vec3(@NV_LUMA_R@, @NV_LUMA_G@, @NV_LUMA_B@)), 2.0)
				- @NV_BRIGHT_BIAS@, 0.0, 1.0);
		result *= bright * custom1.a;
		// The NV blend pipeline is src ONE / dst SRC_ALPHA, so the written
		// alpha IS the retained destination weight: dst * (noiseA x diffuseA
		// x 2), exactly the premultiplied blend water.gdshader runs in beauty.
		// (The underwater opaque variant replaces and ignores it.)
		float alpha = clamp(noise.a * color.a * 2.0, 0.0, 1.0);
		frag_color = vec4(result, alpha);
		return;
	}
	// Celestial flag bit 1: the registered material blends additively
	// (celestial_additive.gdshader's premultiplied form); clear = alpha blend.
	vec4 tex = texture(primary_texture, uv);
	if ((coverage_flags & 1u) != 0u) {
		frag_color = vec4(tex.rgb * pc.draw_color.rgb * tex.a * pc.draw_color.a,
				1.0);
	} else {
		frag_color = vec4(tex.rgb * pc.draw_color.rgb,
				tex.a * pc.draw_color.a);
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

void splice_token(std::string &p_text, const char *p_token,
		const std::string &p_value) {
	const std::string token(p_token);
	for (std::size_t at = p_text.find(token); at != std::string::npos;
			at = p_text.find(token, at + p_value.size()))
		p_text.replace(at, token.size(), p_value);
}

std::string q3_vertex_shader_source() {
	std::string source(kQ3VertexShaderTemplate);
	splice_token(source, "@FAR_BAND_REV_MIN@", glsl_float(1.0f - kQ3FarBandMaxZ));
	splice_token(source, "@FAR_BAND_REV_SPAN@",
			glsl_float(kQ3FarBandMaxZ - kQ3FarBandMinZ));
	return source;
}

std::string q3_fragment_shader_source() {
	std::string source(kQ3FragmentShaderTemplate);
	splice_token(source, "@GLASS_WHITE_GAIN@", glsl_float(kQ3GlassWhiteLobeGain));
	splice_token(source, "@GLASS_WHITE_POWER@",
			glsl_float(kQ3GlassWhiteLobePower));
	splice_token(source, "@GLASS_WARM_R@", glsl_float(kQ3GlassWarmLobeColor[0]));
	splice_token(source, "@GLASS_WARM_G@", glsl_float(kQ3GlassWarmLobeColor[1]));
	splice_token(source, "@GLASS_WARM_B@", glsl_float(kQ3GlassWarmLobeColor[2]));
	splice_token(source, "@GLASS_WARM_POWER@", glsl_float(kQ3GlassWarmLobePower));
	splice_token(source, "@NV_LUMA_R@", glsl_float(kQ3WaterNvLumaWeights[0]));
	splice_token(source, "@NV_LUMA_G@", glsl_float(kQ3WaterNvLumaWeights[1]));
	splice_token(source, "@NV_LUMA_B@", glsl_float(kQ3WaterNvLumaWeights[2]));
	splice_token(source, "@NV_BRIGHT_BIAS@", glsl_float(kQ3WaterNvBrightBias));
	return source;
}

Q3Matrix4 q3_matrix(const Transform3D &p_transform) {
	Q3Matrix4 result;
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row)
			result.values[column * 4 + row] = p_transform.basis[column][row];
	}
	result.values[12] = p_transform.origin.x;
	result.values[13] = p_transform.origin.y;
	result.values[14] = p_transform.origin.z;
	return result;
}

Transform3D godot_transform(const Q3Matrix4 &p_matrix) {
	Basis basis;
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row)
			basis[column][row] = p_matrix.values[column * 4 + row];
	}
	return Transform3D(basis, Vector3(p_matrix.values[12],
			p_matrix.values[13], p_matrix.values[14]));
}

bool belongs_to_scope(Node *p_node, Node *p_scope, Viewport *p_viewport) {
	if (p_node == nullptr || p_scope == nullptr || p_viewport == nullptr ||
			!p_node->is_inside_tree() || p_node->get_viewport() != p_viewport)
		return false;
	for (Node *cursor = p_node; cursor != nullptr; cursor = cursor->get_parent()) {
		if (cursor == p_scope)
			return true;
	}
	return false;
}

struct CameraFrustum {
	std::vector<Plane> planes;
	Vector3 interior_point;
	bool valid = false;

	bool outside(const AABB &p_bounds) const {
		if (!valid || !p_bounds.position.is_finite() ||
				!p_bounds.size.is_finite())
			return false;
		constexpr float kPlaneTolerance = 1.0e-3f;
		for (const Plane &plane : planes) {
			const float interior_distance = plane.distance_to(interior_point);
			const float interior_sign = interior_distance >= 0.0f ? 1.0f : -1.0f;
			bool has_inside_endpoint = false;
			for (int endpoint = 0; endpoint < 8; ++endpoint) {
				if (plane.distance_to(p_bounds.get_endpoint(endpoint)) *
						interior_sign >= -kPlaneTolerance) {
					has_inside_endpoint = true;
					break;
				}
			}
			if (!has_inside_endpoint)
				return true;
		}
		return false;
	}
};

CameraFrustum camera_frustum(Camera3D *p_camera, Viewport *p_viewport) {
	CameraFrustum result;
	if (p_camera == nullptr || p_viewport == nullptr ||
			p_camera->get_far() <= p_camera->get_near())
		return result;
	const TypedArray<Plane> godot_planes = p_camera->get_frustum();
	if (godot_planes.size() < 4)
		return result;
	result.planes.reserve(godot_planes.size());
	for (int index = 0; index < godot_planes.size(); ++index)
		result.planes.push_back(godot_planes[index]);
	const Rect2 visible_rect = p_viewport->get_visible_rect();
	const Vector2 viewport_center = visible_rect.position +
			visible_rect.size * 0.5f;
	const float interior_depth = p_camera->get_near() +
			(p_camera->get_far() - p_camera->get_near()) * 0.5f;
	result.interior_point = p_camera->project_position(viewport_center,
			interior_depth);
	result.valid = result.interior_point.is_finite();
	return result;
}

void merge_bounds(AABB &r_bounds, bool &r_has_bounds,
		const AABB &p_addition) {
	if (!p_addition.position.is_finite() || !p_addition.size.is_finite())
		return;
	r_bounds = r_has_bounds ? r_bounds.merge(p_addition) : p_addition;
	r_has_bounds = true;
}

MissionEnvironment *environment_in_scope(Node *p_root,
		Viewport *p_viewport) {
	if (p_root == nullptr || p_viewport == nullptr ||
			(p_root->is_inside_tree() && p_root->get_viewport() != p_viewport))
		return nullptr;
	if (MissionEnvironment *environment =
			Object::cast_to<MissionEnvironment>(p_root))
		return environment;
	for (int index = 0; index < p_root->get_child_count(); ++index) {
		if (MissionEnvironment *environment = environment_in_scope(
				p_root->get_child(index), p_viewport))
			return environment;
	}
	return nullptr;
}

Q3DeviceBlend blend_for(const Q3DrawCommand &p_command) {
	switch (p_command.technique) {
		case Q3Technique::NormalCopy:
			switch (p_command.object.classification.blend) {
				case ObjectBlendMode::Opaque:
					return Q3DeviceBlend::Replace;
				case ObjectBlendMode::AlphaBlend:
					return Q3DeviceBlend::Alpha;
				case ObjectBlendMode::Additive:
					return Q3DeviceBlend::Add;
				case ObjectBlendMode::Multiplicative:
					break; // Q3FrameCompiler rejects this unwitnessed combination.
			}
			return Q3DeviceBlend::Replace;
		case Q3Technique::RotatedSpecularGlass:
		case Q3Technique::SunGlow:
			return Q3DeviceBlend::Add;
		case Q3Technique::WaterNightVision:
			return p_command.water.underwater_view ?
					Q3DeviceBlend::Replace : Q3DeviceBlend::Water;
		case Q3Technique::CelestialBody:
			return p_command.celestial.additive ?
					Q3DeviceBlend::Add : Q3DeviceBlend::Alpha;
		case Q3Technique::Count:
			break;
	}
	return Q3DeviceBlend::Replace;
}

} // namespace

class Q3FrameAdapter::Impl {
public:
	struct PipelineKey {
		int64_t framebuffer_format = -1;
		Q3DeviceBlend blend = Q3DeviceBlend::Replace;
		bool two_sided = true;

		bool operator<(const PipelineKey &p_other) const {
			return std::tie(framebuffer_format, blend, two_sided) <
					std::tie(p_other.framebuffer_format, p_other.blend,
							p_other.two_sided);
		}
	};

	mutable std::mutex frame_mutex;
	std::shared_ptr<const DeviceFrame> published_frame;
	Q3FrameCompiler compiler;
	mutable std::mutex diagnostics_mutex;
	std::string status = "waiting_for_frame";
	std::string failure;
	std::uint64_t submitted_frame_id = 0;
	std::uint64_t drawn_frame_id = 0;
	std::size_t submitted_commands = 0;
	std::size_t rejected_commands = 0;
	std::size_t drawn_commands = 0;
	std::size_t gpu_draw_calls = 0;
	std::size_t registered_sources = 0;
	// The in-tree records the walk iterates, and how many of them had cached
	// state refreshed this frame (0 on a stable frame).
	std::size_t records = 0;
	std::size_t records_touched = 0;
	// Surfaces whose material block was read through get_shader_parameter
	// this frame: object surfaces only after a parameter invalidation, the
	// per-frame water and celestial producers at every sight.
	std::size_t material_reads = 0;
	std::size_t frustum_culled_sources = 0;
	std::size_t frustum_culled_instances = 0;
	// Per-frame packing work (0 packed vertices and 0 readbacks on a stable
	// frame) beside the retained cache footprint.
	std::size_t packed_vertices = 0;
	std::size_t packed_vertex_bytes = 0;
	std::size_t repacked_entries = 0;
	std::size_t readbacks_this_frame = 0;
	std::size_t instance_row_reads_this_frame = 0;
	std::size_t cached_entries = 0;
	std::size_t cached_vertex_bytes = 0;
	// Device vertex buffers currently held (one per uploaded cache entry),
	// republished from the render side after every consume/draw.
	std::size_t device_buffers = 0;

	// Main-thread geometry cache; its streams are shared immutably with the
	// render side, which keeps one device buffer per entry below.
	Q3GeometryCache geometry_cache;
	// The MissionEnvironment found under the scope, re-validated by identity
	// each frame and searched for again only once it has gone.
	ObjectID environment_id;
	std::vector<std::uint64_t> dead_sources;
	std::vector<Transform3D> emitted_transforms;
	struct DeviceGeometry {
		RID buffer;
		std::uint32_t capacity = 0;
		std::uint64_t uploaded_generation = 0;
	};
	std::map<std::uint64_t, DeviceGeometry> device_geometry;
	// The last frame the render side consumed: evictions minted at or before
	// it have had their device buffers freed.
	std::atomic<std::uint64_t> consumed_frame_id{0};

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID fallback_texture;
	int64_t vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	TypedArray<RID> vertex_buffers;
	PackedInt64Array vertex_offsets;
	std::map<PipelineKey, RID> pipelines;
	std::vector<RID> transient_uniforms;
	PackedColorArray clear_black;

	// RenderingDevice is server-owned. The compositor lifecycle releases live
	// resources explicitly; destruction may run after that device has gone, so
	// it must never dereference the cached raw pointer.
	~Impl() { discard_device_state(); }

	void set_failure(const std::string &p_failure) {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = "failed";
		failure = p_failure;
	}

	std::shared_ptr<const DeviceFrame> frame_snapshot() const {
		std::lock_guard<std::mutex> lock(frame_mutex);
		return published_frame;
	}

	void publish(std::shared_ptr<const DeviceFrame> p_frame) {
		std::lock_guard<std::mutex> lock(frame_mutex);
		published_frame = std::move(p_frame);
	}

	void release_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid())
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_uniform_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() &&
				rd->uniform_set_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_pipeline_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() &&
				rd->render_pipeline_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_texture_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid() && rd->texture_is_valid(p_rid))
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void discard_device_state() {
		transient_uniforms.clear();
		pipelines.clear();
		device_geometry.clear();
		fallback_texture = RID();
		sampler = RID();
		shader = RID();
		vertex_buffers.clear();
		vertex_offsets.clear();
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
		rd = nullptr;
	}

	void release_device_geometry(std::uint64_t p_entry_id) {
		const auto found = device_geometry.find(p_entry_id);
		if (found == device_geometry.end())
			return;
		release_rid(found->second.buffer);
		device_geometry.erase(found);
	}

	void publish_device_buffer_count() {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		device_buffers = device_geometry.size();
	}

	// Evicted entries are named by every frame published until one is
	// consumed, so freeing here can never race a later frame that draws them.
	// Runs on the render side for every frame, drawn or not.
	void consume_frame(RenderingDevice *p_rd) {
		const std::shared_ptr<const DeviceFrame> frame = frame_snapshot();
		if (!frame)
			return;
		// Buffers belong to the device that created them: a replaced device
		// is discarded through initialize()/release_device, never freed here.
		if (rd != nullptr && rd != p_rd)
			return;
		for (const std::uint64_t entry_id : frame->evicted_entries)
			release_device_geometry(entry_id);
		consumed_frame_id.store(frame->draw_list.frame_id,
				std::memory_order_release);
		publish_device_buffer_count();
	}

	void release_device(RenderingDevice *p_rd) {
		// A null current device is an invalidation signal, not permission to
		// call through the previously cached server-owned pointer. Likewise, RIDs
		// from one device must never be freed through a replacement device.
		if (p_rd == nullptr || (rd != nullptr && rd != p_rd)) {
			discard_device_state();
			publish_device_buffer_count();
			return;
		}
		rd = p_rd;
		for (RID &uniform : transient_uniforms)
			release_uniform_rid(uniform);
		for (auto &entry : pipelines)
			release_pipeline_rid(entry.second);
		for (auto &entry : device_geometry)
			release_rid(entry.second.buffer);
		release_texture_rid(fallback_texture);
		release_rid(sampler);
		release_rid(shader);
		discard_device_state();
		publish_device_buffer_count();
	}

	bool initialize(RenderingDevice *p_rd);
	MissionEnvironment *scope_environment(Node *p_scope, Viewport *p_viewport);
	RID pipeline_for(int64_t p_framebuffer_format,
			const Q3DrawCommand &p_command);
	bool upload_stream(const Q3PackedStream &p_stream);
	RID make_uniform(const DeviceCommand &p_command);
	bool draw(RenderData *p_render_data, std::uint32_t p_view,
			const RID &p_framebuffer, std::size_t &r_draw_calls);
	Dictionary report() const;
};

bool Q3FrameAdapter::Impl::initialize(RenderingDevice *p_rd) {
	if (rd == p_rd && shader.is_valid() && sampler.is_valid() &&
			vertex_format != RenderingDevice::INVALID_FORMAT_ID)
		return true;
	release_device(p_rd);
	rd = p_rd;
	if (rd == nullptr) {
		set_failure("RenderingDevice is unavailable for the focused Q3 renderer");
		return false;
	}
	if (rd->limit_get(RenderingDevice::LIMIT_MAX_PUSH_CONSTANT_SIZE) <
			kPushConstantBytes) {
		set_failure("RenderingDevice does not support the Q3 128-byte push block");
		return false;
	}
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			opennova::to_gd(q3_vertex_shader_source()));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			opennova::to_gd(q3_fragment_shader_source()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null() || !spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			!spirv->get_stage_compile_error(
					RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		set_failure("Focused Q3 shader compilation failed");
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova focused Q3");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the focused Q3 shader");
		return false;
	}
	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler = rd->sampler_create(sampler_state);

	Ref<RDTextureFormat> fallback_format;
	fallback_format.instantiate();
	fallback_format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fallback_format->set_width(1);
	fallback_format->set_height(1);
	fallback_format->set_depth(1);
	fallback_format->set_array_layers(1);
	fallback_format->set_mipmaps(1);
	fallback_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	fallback_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	fallback_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> fallback_view;
	fallback_view.instantiate();
	PackedByteArray fallback_pixel;
	fallback_pixel.resize(4);
	fallback_pixel.ptrw()[0] = 255;
	fallback_pixel.ptrw()[1] = 255;
	fallback_pixel.ptrw()[2] = 255;
	fallback_pixel.ptrw()[3] = 255;
	TypedArray<PackedByteArray> fallback_data;
	fallback_data.push_back(fallback_pixel);
	fallback_texture = rd->texture_create(fallback_format, fallback_view,
			fallback_data);
	if (!sampler.is_valid() || !fallback_texture.is_valid()) {
		set_failure("RenderingDevice could not create focused Q3 sampled resources");
		return false;
	}

	vertex_format = rd->vertex_format_create(q3_vertex_attributes());
	vertex_buffers.resize(1);
	vertex_offsets.resize(1);
	if (vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
		set_failure("RenderingDevice rejected the focused Q3 vertex format");
		return false;
	}
	if (clear_black.is_empty())
		clear_black.push_back(Color(0, 0, 0, 0));
	return true;
}

MissionEnvironment *Q3FrameAdapter::Impl::scope_environment(Node *p_scope,
		Viewport *p_viewport) {
	if (environment_id.is_valid()) {
		MissionEnvironment *environment = Object::cast_to<MissionEnvironment>(
				ObjectDB::get_instance(environment_id));
		if (environment != nullptr &&
				belongs_to_scope(environment, p_scope, p_viewport))
			return environment;
		environment_id = ObjectID();
	}
	MissionEnvironment *environment = environment_in_scope(p_scope, p_viewport);
	if (environment != nullptr)
		environment_id = ObjectID(environment->get_instance_id());
	return environment;
}

RID Q3FrameAdapter::Impl::pipeline_for(int64_t p_framebuffer_format,
		const Q3DrawCommand &p_command) {
	const bool object_technique = p_command.technique == Q3Technique::NormalCopy ||
			p_command.technique == Q3Technique::RotatedSpecularGlass;
	const bool two_sided = !object_technique ||
			p_command.object.classification.is_two_sided;
	// Every Q3 draw ends in the ordinary z-tested flush; the discs and the
	// sun glow only reach it through the far-band viewport remap in the
	// vertex shader.
	const PipelineKey key{p_framebuffer_format, blend_for(p_command), two_sided};
	const auto found = pipelines.find(key);
	if (found != pipelines.end())
		return found->second;
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode(two_sided ? RenderingDevice::POLYGON_CULL_DISABLED :
			RenderingDevice::POLYGON_CULL_BACK);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test(true);
	depth->set_enable_depth_write(false);
	depth->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	const Q3DeviceBlend blend = key.blend;
	attachment->set_enable_blend(blend != Q3DeviceBlend::Replace);
	if (blend == Q3DeviceBlend::Alpha) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_color_blend_factor(
				RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	} else if (blend == Q3DeviceBlend::Add) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	} else if (blend == Q3DeviceBlend::Water) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
	}
	attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	Ref<RDPipelineColorBlendState> color_blend;
	color_blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	color_blend->set_attachments(attachments);
	RID pipeline = rd->render_pipeline_create(shader, p_framebuffer_format,
			vertex_format, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster,
			multisample, depth, color_blend);
	if (!pipeline.is_valid())
		set_failure("RenderingDevice rejected a focused Q3 pipeline");
	else
		pipelines.emplace(key, pipeline);
	return pipeline;
}

// One device buffer per cache entry, uploaded only when the entry's packed
// generation moved since the last upload (a stable entry uploads once).
bool Q3FrameAdapter::Impl::upload_stream(const Q3PackedStream &p_stream) {
	if (p_stream.bytes.is_empty())
		return false;
	DeviceGeometry &geometry = device_geometry[p_stream.entry_id];
	const std::uint32_t required = static_cast<std::uint32_t>(p_stream.bytes.size());
	if (!geometry.buffer.is_valid() || required > geometry.capacity) {
		release_rid(geometry.buffer);
		geometry.capacity = required;
		geometry.uploaded_generation = 0;
		geometry.buffer = rd->vertex_buffer_create(geometry.capacity);
		if (!geometry.buffer.is_valid()) {
			set_failure("RenderingDevice could not allocate a focused Q3 vertex buffer");
			return false;
		}
	}
	if (geometry.uploaded_generation == p_stream.generation)
		return true;
	if (rd->buffer_update(geometry.buffer, 0, required, p_stream.bytes) != OK) {
		set_failure("RenderingDevice rejected the focused Q3 vertex upload");
		return false;
	}
	geometry.uploaded_generation = p_stream.generation;
	return true;
}

RID Q3FrameAdapter::Impl::make_uniform(const DeviceCommand &p_command) {
	RenderingServer *server = RenderingServer::get_singleton();
	auto resolve = [&](const RID &p_server_rid) {
		const RID result = server != nullptr && p_server_rid.is_valid() ?
				server->texture_get_rd_texture(p_server_rid, false) : RID();
		return result.is_valid() ? result : fallback_texture;
	};
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, sampler,
			resolve(p_command.primary_texture)));
	uniforms.push_back(sampled_texture_uniform(1, sampler,
			resolve(p_command.secondary_texture)));
	uniforms.push_back(sampled_texture_uniform(2, sampler,
			resolve(p_command.tertiary_texture)));
	return rd->uniform_set_create(uniforms, shader, 0);
}

bool Q3FrameAdapter::Impl::draw(RenderData *p_render_data, std::uint32_t p_view,
		const RID &p_framebuffer, std::size_t &r_draw_calls) {
	const std::shared_ptr<const DeviceFrame> frame = frame_snapshot();
	if (!frame)
		return true;
	consume_frame(rd);
	if (frame->commands.empty())
		return true;
	RenderSceneData *scene_data = p_render_data != nullptr ?
			p_render_data->get_render_scene_data() : nullptr;
	if (scene_data == nullptr) {
		set_failure("Focused Q3 draw has no RenderSceneData");
		return false;
	}
	// Uploads precede the draw list: a buffer update inside one is rejected.
	for (const DeviceCommand &command : frame->commands) {
		if (!command.stream || !upload_stream(*command.stream)) {
			publish_device_buffer_count();
			return false;
		}
	}
	publish_device_buffer_count();
	for (RID &uniform : transient_uniforms)
		release_uniform_rid(uniform);
	transient_uniforms.clear();
	const Projection world_to_clip = scene_data->get_view_projection(p_view) *
			Projection(scene_data->get_cam_transform().affine_inverse());
	const Vector3 camera_forward =
			-scene_data->get_cam_transform().basis.get_column(2).normalized();
	const Vector3 light_direction = frame->light_direction;
	const Vector3 light_gain = frame->light_gain;
	// The environment already resolved the Render_SetFogState start for the
	// fog type (engine compute_fog_params); the device evaluation in the
	// shader mirrors renderer/device_fog.h and never re-derives it.
	const bool fog_enabled = frame->fog_enabled;
	const float fog_start = frame->fog_start;
	const float fog_end = frame->fog_end;
	const float fog_flags = (fog_enabled ? 4.0f : 0.0f) +
			static_cast<float>((std::clamp(frame->fog_type, 0, 3)) << 3);
	const int64_t framebuffer_format = rd->framebuffer_get_format(p_framebuffer);
	for (const DeviceCommand &command : frame->commands) {
		if (!pipeline_for(framebuffer_format, command.draw).is_valid())
			return false;
	}
	const int64_t draw_list = rd->draw_list_begin(p_framebuffer,
			RenderingDevice::DRAW_CLEAR_COLOR_0, clear_black);
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin the focused Q3 draw list");
		return false;
	}
	std::size_t draws = 0;
	for (const DeviceCommand &command : frame->commands) {
		RID uniform = make_uniform(command);
		if (!uniform.is_valid()) {
			rd->draw_list_end();
			set_failure("RenderingDevice could not bind focused Q3 textures");
			return false;
		}
		transient_uniforms.push_back(uniform);
		const RID pipeline = pipeline_for(framebuffer_format, command.draw);
		for (std::uint32_t instance = 0; instance < command.draw.transform_count;
				++instance) {
			const Q3Matrix4 &matrix = frame->draw_list.transforms[
					command.draw.first_transform + instance];
			const Transform3D model = godot_transform(matrix);
			const Projection mvp = world_to_clip * Projection(model);
			Q3Push push{};
			for (std::uint32_t column = 0; column < 4; ++column) {
				for (std::uint32_t row = 0; row < 4; ++row)
					push.mvp[column * 4 + row] = mvp[column][row];
			}
			const Transform3D inverse = model.affine_inverse();
			const Vector3 camera_local = inverse.xform(
					scene_data->get_cam_transform().origin);
			push.camera_local = {camera_local.x, camera_local.y,
					camera_local.z, 0.0f};
			const Vector3 local_light = inverse.basis.xform(light_direction).normalized();
			push.light_local_gain = {local_light.x, local_light.y,
					local_light.z, 0.0f};
			const Q3DrawCommand &draw = command.draw;
			push.params[0] = static_cast<float>(draw.technique);
			if (draw.technique == Q3Technique::NormalCopy ||
					draw.technique == Q3Technique::RotatedSpecularGlass) {
				push.params[1] =
						(draw.object.classification.alpha_test ? 1.0f : 0.0f) +
						(draw.object.classification.has_detail ? 2.0f : 0.0f) +
						fog_flags;
				push.params[2] = draw.object.classification.alpha_test_value;
				push.params[3] = draw.object.alpha_mod;
				const float model_uniform_scale = std::max(1.0e-6f,
						std::cbrt(std::abs(model.basis.determinant())));
				push.light_local_gain[3] =
						draw.object.classification.alpha_test_invert ?
								-model_uniform_scale : model_uniform_scale;
			}
			if (draw.technique == Q3Technique::NormalCopy) {
				// The SELFLUM NORMAL block: u_rgb_mod x min(gain, 1) x 2 rides
				// draw_color.rgb, the wrapper's fog policy follows the blend
				// (fog/additive.gdshaderinc for _AD, fog/regular.gdshaderinc for
				// _OP/_AB) and the regular fog colour rides light_local_gain.xyz.
				push.draw_color = {
					draw.object.self_lum_color.x * std::min(light_gain.x, 1.0f) * 2.0f,
					draw.object.self_lum_color.y * std::min(light_gain.y, 1.0f) * 2.0f,
					draw.object.self_lum_color.z * std::min(light_gain.z, 1.0f) * 2.0f,
					fog_end};
				push.camera_local[3] = fog_start;
				push.light_local_gain[0] = frame->fog_color.x;
				push.light_local_gain[1] = frame->fog_color.y;
				push.light_local_gain[2] = frame->fog_color.z;
				if (draw.object.classification.blend != ObjectBlendMode::Additive)
					push.params[1] += 32.0f;
			} else if (draw.technique == Q3Technique::RotatedSpecularGlass) {
				push.draw_color = {draw.object.reflect_color.x,
						draw.object.reflect_color.y, draw.object.reflect_color.z,
						draw.object.reflect_color.w};
				push.draw_color[0] *= light_gain.x * 2.0f;
				push.draw_color[1] *= light_gain.y * 2.0f;
				push.draw_color[2] *= light_gain.z * 2.0f;
				push.camera_local[3] = fog_start;
				push.draw_color[3] = fog_end;
			} else if (draw.technique == Q3Technique::WaterNightVision) {
				push.draw_color = {draw.water.water_color.x, draw.water.water_color.y,
						draw.water.water_color.z, 1.0f};
				push.params[1] = draw.water.has_reflection ? 1.0f : 0.0f;
				push.params[2] = draw.water.reflection_uv_scale.x;
				push.params[3] = draw.water.reflection_uv_scale.y;
			} else {
				float opacity = draw.celestial.opacity;
				if (draw.technique == Q3Technique::SunGlow &&
						draw.celestial.glare_view_fade) {
					Vector3 glare_direction(draw.celestial.glare_direction.x,
							draw.celestial.glare_direction.y,
							draw.celestial.glare_direction.z);
					if (glare_direction.length_squared() > 0.0f) {
						const float view_dot = std::max(0.0f,
								static_cast<float>(camera_forward.dot(
										glare_direction.normalized())));
						const float view_dot_sq = view_dot * view_dot;
						opacity *= view_dot_sq * view_dot_sq;
					} else {
						opacity = 0.0f;
					}
				}
				push.draw_color = {draw.celestial.tint.x, draw.celestial.tint.y,
						draw.celestial.tint.z, opacity};
				push.params[1] = draw.celestial.additive ? 1.0f : 0.0f;
			}
			vertex_buffers[0] = device_geometry[command.stream->entry_id].buffer;
			vertex_offsets[0] = 0;
			rd->draw_list_bind_render_pipeline(draw_list, pipeline);
			rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
			rd->draw_list_bind_vertex_buffers_format(draw_list, vertex_format,
					command.stream->vertex_count, vertex_buffers, vertex_offsets);
			PackedByteArray push_bytes;
			push_bytes.resize(kPushConstantBytes);
			std::memcpy(push_bytes.ptrw(), &push, sizeof(push));
			rd->draw_list_set_push_constant(draw_list, push_bytes,
					kPushConstantBytes);
			rd->draw_list_draw(draw_list, false, 1);
			++draws;
		}
	}
	rd->draw_list_end();
	r_draw_calls += draws;
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = "drawn";
		failure.clear();
		drawn_frame_id = frame->draw_list.frame_id;
		drawn_commands = frame->commands.size();
		gpu_draw_calls = draws;
	}
	return true;
}

Dictionary Q3FrameAdapter::Impl::report() const {
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	Dictionary result;
	result["q3_backend"] = "typed_rendering_device_draw_list";
	result["q3_source_contract"] = "Q3FrameCompiler";
	result["q3_target_ownership"] = "terminal_compositor";
	result["q3_uses_resolved_beauty_depth"] = true;
	result["q3_depth_compare"] = "greater_or_equal";
	result["q3_depth_write"] = false;
	result["q3_sun_depth_test"] = true;
	result["q3_far_band"] = Vector2(kQ3FarBandMinZ, kQ3FarBandMaxZ);
	result["q3_auxiliary_view"] = false;
	result["q3_camera_mask"] = false;
	result["q3_status"] = opennova::to_gd(status);
	result["q3_failure"] = opennova::to_gd(failure);
	result["q3_submitted_frame_id"] = static_cast<int64_t>(submitted_frame_id);
	result["q3_drawn_frame_id"] = static_cast<int64_t>(drawn_frame_id);
	result["q3_submitted_commands"] = static_cast<int64_t>(submitted_commands);
	result["q3_rejected_commands"] = static_cast<int64_t>(rejected_commands);
	result["q3_drawn_commands"] = static_cast<int64_t>(drawn_commands);
	result["q3_gpu_draw_calls"] = static_cast<int64_t>(gpu_draw_calls);
	result["q3_registered_sources"] = static_cast<int64_t>(registered_sources);
	result["q3_records"] = static_cast<int64_t>(records);
	result["q3_records_touched_this_frame"] =
			static_cast<int64_t>(records_touched);
	result["q3_material_reads_this_frame"] =
			static_cast<int64_t>(material_reads);
	result["q3_frustum_culled_sources"] =
			static_cast<int64_t>(frustum_culled_sources);
	result["q3_frustum_culled_instances"] =
			static_cast<int64_t>(frustum_culled_instances);
	// Packed THIS frame versus the retained cache.
	result["q3_packed_vertices"] = static_cast<int64_t>(packed_vertices);
	result["q3_packed_vertex_bytes"] =
			static_cast<int64_t>(packed_vertex_bytes);
	result["q3_repacked_entries"] = static_cast<int64_t>(repacked_entries);
	result["q3_readbacks_this_frame"] =
			static_cast<int64_t>(readbacks_this_frame);
	// MultiMesh sources whose rows were re-read this frame (an instance
	// invalidation or a count change); a stable frame reports 0.
	result["q3_instance_row_reads_this_frame"] =
			static_cast<int64_t>(instance_row_reads_this_frame);
	result["q3_cached_entries"] = static_cast<int64_t>(cached_entries);
	result["q3_cached_vertex_bytes"] =
			static_cast<int64_t>(cached_vertex_bytes);
	result["q3_device_buffers"] = static_cast<int64_t>(device_buffers);
	result["q3_geometry_submission"] = "cached_per_source_surface_streams";
	result["q3_static_instance_submission"] = "retained_transform_ranges";
	return result;
}

Q3FrameAdapter::Q3FrameAdapter() : impl_(std::make_unique<Impl>()) {}

Q3FrameAdapter::~Q3FrameAdapter() = default;

void Q3FrameAdapter::compile_frame(Node *p_scope, Viewport *p_viewport,
		Camera3D *p_camera) {
	if (!impl_ || p_scope == nullptr || p_viewport == nullptr || p_camera == nullptr) {
		clear_frame();
		return;
	}
	// The registry lock spans the walk: producers and the tree/visibility
	// signals never interleave with a compile.
	std::lock_guard<std::recursive_mutex> registry_lock(Q3SourceRegistry::mutex());
	Q3FrameSnapshot snapshot;
	snapshot.frame_id = g_frame_id.fetch_add(1);
	snapshot.scene_generation = p_viewport->get_instance_id();
	std::vector<Candidate> candidates;
	std::size_t frustum_culled_sources = 0;
	std::size_t frustum_culled_instances = 0;
	Q3GeometryCache &cache = impl_->geometry_cache;
	cache.begin_frame(snapshot.frame_id);
	Q3SourceRegistry::begin_frame(impl_->dead_sources);
	for (const std::uint64_t source_id : impl_->dead_sources)
		cache.evict_source(source_id);
	Q3SourceRegistry::FrameCounters counters;
	const CameraFrustum frustum = camera_frustum(p_camera, p_viewport);
	const std::uint64_t scope_id = p_scope->get_instance_id();
	const Vector3 camera_position = p_camera->get_global_position();
	const Vector3 camera_forward = -p_camera->get_global_basis().get_column(2);
	// Focused Q3 admits world/local-body object geometry, never the
	// first-person render-FOV/depth-band or shadow/capture-only layers.
	// World-no-mirror is still ordinary beauty geometry and remains eligible.
	constexpr std::uint32_t kWorldLayer = 1u << 0;
	constexpr std::uint32_t kWorldNoMirrorLayer = 1u << 16;
	std::vector<Transform3D> &emitted_transforms = impl_->emitted_transforms;
	const std::vector<Q3SourceRecord *> &live = Q3SourceRegistry::live_records();
	for (std::size_t index = 0; index < live.size(); ++index) {
		Q3SourceRecord &record = *live[index];
		if (!record.active || record.viewport != p_viewport)
			continue;
		if (record.scope_id != scope_id) {
			record.scope_id = scope_id;
			record.in_scope = belongs_to_scope(record.node, p_scope, p_viewport);
			++counters.records_touched;
		}
		if (!record.in_scope)
			continue;
		if (record.visibility_dirty) {
			record.visibility_dirty = false;
			record.visible = record.node->is_visible_in_tree();
			++counters.records_touched;
		}
		if (!record.visible)
			continue;
		if (record.source == Q3Source::Object &&
				(record.node->get_layer_mask() &
						(kWorldLayer | kWorldNoMirrorLayer)) == 0)
			continue;
		// The one per-frame probe of a visible record: Godot exposes no
		// transform-changed signal for an engine-class node to an extension,
		// so the cached bounds and rows follow a compare of the transform.
		const Transform3D global_transform = record.node->get_global_transform();
		if (!record.transform_valid || global_transform != record.global_transform) {
			record.global_transform = global_transform;
			record.transform_valid = true;
			record.bounds_dirty = true;
			record.rows_world_dirty = true;
		}
		if (record.bounds_dirty) {
			record.bounds_dirty = false;
			record.world_bounds = global_transform.xform(record.node->get_aabb());
			++counters.records_touched;
		}
		if (frustum.outside(record.world_bounds)) {
			++frustum_culled_sources;
			continue;
		}
		if (record.surfaces_dirty)
			Q3SourceRegistry::refresh_surfaces(record, counters);
		if (record.mesh.is_null() || record.surfaces.empty())
			continue;

		emitted_transforms.clear();
		AABB emitted_world_bounds;
		bool has_emitted_world_bounds = false;
		if (record.is_multimesh) {
			// Rows are read once per instance generation and their world
			// bounds once per move; only the frustum test runs per frame.
			Q3SourceRegistry::refresh_rows(record, counters);
			emitted_transforms.reserve(record.rows.size());
			for (const Q3InstanceRow &row : record.rows) {
				if (frustum.outside(row.world_bounds)) {
					++frustum_culled_instances;
					continue;
				}
				emitted_transforms.push_back(row.world_transform);
				merge_bounds(emitted_world_bounds, has_emitted_world_bounds,
						row.world_bounds);
			}
		} else {
			emitted_transforms.push_back(global_transform);
			merge_bounds(emitted_world_bounds, has_emitted_world_bounds,
					record.world_bounds);
		}
		if (emitted_transforms.empty())
			continue;
		const Vector3 population_center = has_emitted_world_bounds ?
				emitted_world_bounds.get_center() : global_transform.origin;
		const float population_view_depth = std::max(0.0f,
				(population_center - camera_position).dot(camera_forward));

		for (Q3SurfaceRecord &surface : record.surfaces) {
			Q3SourceRegistry::refresh_surface_parameters(record, surface, counters);
			Candidate candidate;
			candidate.submission.submission_id = record.node_id ^
					(static_cast<std::uint64_t>(surface.surface + 1) << 48u);
			candidate.submission.source = record.source;
			candidate.submission.material = {surface.material_id, 1};
			candidate.submission.surface_index = surface.surface;
			Q3GeometryCache::Request request;
			request.key = {record.node_id, surface.surface};
			request.geometry_generation = record.geometry_generation;
			const auto publication = record.published.find(surface.surface);
			if (publication != record.published.end()) {
				request.published = &publication->second.arrays;
				request.published_generation = publication->second.generation;
			}
			request.pack = surface.pack;
			if (record.source == Q3Source::Water)
				request.pack.camera_position = camera_position;
			const int surface_index = surface.surface;
			candidate.stream = cache.acquire(request, [&record, surface_index]() {
				return record.mesh->surface_get_arrays(surface_index);
			});
			if (!candidate.stream)
				continue;
			// The geometry lease is the cache entry and the packed generation
			// this producer saw; the cache publishes its own generation table
			// after the walk (append_generations) and the compiler holds every
			// lease to that table.
			candidate.submission.geometry = {candidate.stream->entry_id,
					candidate.stream->generation};
			candidate.submission.first_transform = snapshot.transforms.size();
			candidate.submission.geometry_kind = record.is_multimesh ?
					Q3GeometryKind::StaticInstances : Q3GeometryKind::Rigid;
			for (const Transform3D &transform : emitted_transforms)
				snapshot.transforms.push_back(q3_matrix(transform));
			candidate.submission.transform_count = snapshot.transforms.size() -
					candidate.submission.first_transform;
			candidate.submission.view_depth = population_view_depth;
			candidate.submission.object = surface.object;
			candidate.submission.water = surface.water;
			candidate.submission.celestial = surface.celestial;
			candidate.primary_texture_resource = surface.primary_texture_resource;
			candidate.secondary_texture_resource = surface.secondary_texture_resource;
			candidate.tertiary_texture_resource = surface.tertiary_texture_resource;
			candidate.primary_texture = surface.primary_texture;
			candidate.secondary_texture = surface.secondary_texture;
			candidate.tertiary_texture = surface.tertiary_texture;
			snapshot.submissions.push_back(candidate.submission);
			candidates.push_back(std::move(candidate));
		}
	}
	// The lease table comes from the cache's entry table, not from the
	// candidates: a lease is checked against what its owner currently holds.
	cache.append_generations(snapshot.resource_generations);
	const Q3DrawList &draw_list = impl_->compiler.compile(snapshot);
	auto frame = std::make_shared<DeviceFrame>();
	frame->draw_list = draw_list;
	Ref<EnvLightValues> light_values = EnvLightValues::retail_noon_defaults();
	if (MissionEnvironment *environment = impl_->scope_environment(p_scope,
			p_viewport)) {
		const Ref<EnvLightState> light_state = environment->get_light_state();
		if (light_state.is_valid() && light_state->get_values().is_valid())
			light_values = light_state->get_values();
		frame->fog_start = environment->get_scene_fog_start();
		frame->fog_end = environment->get_scene_fog_end();
		frame->fog_type = environment->get_scene_fog_type();
		frame->fog_color = environment->get_scene_fog_color();
	}
	if (light_values.is_valid()) {
		frame->light_direction = light_values->dir;
		frame->light_gain = light_values->gain;
		frame->fog_enabled = light_values->fog_enabled;
		if (frame->fog_end <= 0.0f) {
			frame->fog_start = light_values->fog_start;
			frame->fog_end = light_values->fog_end;
			frame->fog_type = light_values->fog_type;
			frame->fog_color = light_values->fog_color;
		}
	}
	for (const Q3DrawCommand &draw : draw_list.commands) {
		if (draw.input_index >= candidates.size())
			continue;
		const Candidate &candidate = candidates[draw.input_index];
		if (!candidate.stream ||
				draw.geometry.generation != candidate.stream->generation)
			continue;
		DeviceCommand command;
		command.draw = draw;
		command.stream = candidate.stream;
		command.primary_texture_resource = candidate.primary_texture_resource;
		command.secondary_texture_resource = candidate.secondary_texture_resource;
		command.tertiary_texture_resource = candidate.tertiary_texture_resource;
		command.primary_texture = candidate.primary_texture;
		command.secondary_texture = candidate.secondary_texture;
		command.tertiary_texture = candidate.tertiary_texture;
		frame->commands.push_back(command);
	}
	frame->evicted_entries = cache.pending_evictions(
			impl_->consumed_frame_id.load(std::memory_order_acquire));
	impl_->publish(frame);
	const Q3GeometryCache::FrameCounters &cache_counters = cache.frame_counters();
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->status = frame->commands.empty() ? "compiled_empty" : "compiled";
		impl_->failure.clear();
		impl_->submitted_frame_id = snapshot.frame_id;
		impl_->submitted_commands = frame->commands.size();
		impl_->rejected_commands = draw_list.rejected.size();
		impl_->registered_sources = Q3SourceRegistry::record_count();
		impl_->records = live.size();
		impl_->records_touched = counters.records_touched;
		impl_->material_reads = counters.material_reads;
		impl_->frustum_culled_sources = frustum_culled_sources;
		impl_->frustum_culled_instances = frustum_culled_instances;
		impl_->packed_vertices = cache_counters.packed_vertices;
		impl_->packed_vertex_bytes = cache_counters.packed_vertex_bytes;
		impl_->repacked_entries = cache_counters.repacked_entries;
		impl_->readbacks_this_frame = cache_counters.readbacks;
		impl_->instance_row_reads_this_frame = counters.instance_row_reads;
		impl_->cached_entries = cache.entry_count();
		impl_->cached_vertex_bytes = cache.cached_vertex_bytes();
	}
}

void Q3FrameAdapter::clear_frame() {
	if (impl_)
		impl_->publish(nullptr);
}

bool Q3FrameAdapter::has_commands() const {
	if (!impl_)
		return false;
	const std::shared_ptr<const DeviceFrame> frame = impl_->frame_snapshot();
	return frame && !frame->commands.empty();
}

void Q3FrameAdapter::consume_frame(RenderingDevice *p_rd) {
	if (impl_)
		impl_->consume_frame(p_rd);
}

bool Q3FrameAdapter::draw_view(RenderingDevice *p_rd, RenderData *p_render_data,
		std::uint32_t p_view, const RID &p_framebuffer,
		std::size_t &r_draw_calls) {
	return impl_ && impl_->initialize(p_rd) && impl_->draw(p_render_data, p_view,
			p_framebuffer, r_draw_calls);
}

void Q3FrameAdapter::release_device(RenderingDevice *p_rd) {
	if (impl_)
		impl_->release_device(p_rd);
}

Dictionary Q3FrameAdapter::get_report() const {
	return impl_ ? impl_->report() : Dictionary();
}
