#include "render/q3_frame_adapter.h"

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
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
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
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "env/mission_environment.h"

using namespace godot;
using namespace opennova::renderer;

namespace {

constexpr std::uint32_t kVertexStride = 104u;
constexpr std::uint32_t kPushConstantBytes = 128u;

enum class Q3DeviceBlend : std::uint8_t {
	Replace,
	Alpha,
	Add,
	Water,
};

struct RegisteredQ3Source {
	std::uint64_t node_id = 0;
	Q3Source source = Q3Source::Object;
	std::uint64_t material_id = 0;
};

std::mutex g_registry_mutex;
std::map<std::uint64_t, ObjectMaterialClassification> g_materials;
std::map<std::uint64_t, RegisteredQ3Source> g_sources;
std::atomic<std::uint64_t> g_frame_id{1};

struct DeviceCommand {
	Q3DrawCommand draw{};
	std::uint32_t first_vertex = 0;
	std::uint32_t vertex_count = 0;
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
	PackedByteArray vertices;
	std::vector<DeviceCommand> commands;
	Vector3 light_direction = Vector3(0, 1, 0);
	Vector3 light_gain = Vector3(1, 1, 1);
	bool fog_enabled = false;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	int fog_type = 0;
};

struct Candidate {
	Q3SubmissionSnapshot submission{};
	PackedByteArray vertices;
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
layout(set = 0, binding = 0) uniform sampler2D beauty_color;
layout(set = 0, binding = 1) uniform sampler2D primary_texture;
layout(set = 0, binding = 2) uniform sampler2D secondary_texture;
layout(set = 0, binding = 3) uniform sampler2D tertiary_texture;

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
	// 8|16 fog type.
	uint coverage_flags = uint(pc.params.y + 0.5);
	bool fog_enabled = (coverage_flags & 4u) != 0u;
	uint fog_type = (coverage_flags >> 3u) & 3u;
	float object_alpha = texture(primary_texture, uv).a * pc.params.w;
	if (mode == 0u && (coverage_flags & 2u) != 0u) {
		object_alpha *= texture(secondary_texture, detail_uv).a;
	}
	// Glass.fx TECHNIQUE_GLOW keeps Diffuse1's alpha only as the cutout
	// variants' alpha-test source; the base glass wrappers ignore alpha_mod
	// (OBJ_ALPHA_MOD_NONE) and the glint itself is never modulated by it.
	float coverage = mode == 1u ? texture(primary_texture, uv).a : object_alpha;
	if (mode <= 1u && (coverage_flags & 1u) != 0u) {
		bool passes = coverage > pc.params.z;
		if (pc.light_local_gain.w < 0.0) passes = !passes;
		if (!passes) discard;
	}
	if (mode == 0u) {
		vec4 beauty = texelFetch(beauty_color, ivec2(gl_FragCoord.xy), 0);
		frag_color = vec4(beauty.rgb, object_alpha);
		return;
	}
	if (mode == 1u) {
		vec3 eye = normalize(pc.camera_local.xyz - local_position);
		vec3 reflected = reflect(-eye, normalize(local_normal));
		float aligned = max(dot(reflected,
				normalize(pc.light_local_gain.xyz)), 0.0);
		// kQ3GlassWhiteLobeGain/Power + kQ3GlassWarmLobeColor/Power.
		vec3 lobe = clamp(vec3(@GLASS_WHITE_GAIN@) *
				pow(aligned, @GLASS_WHITE_POWER@) +
				vec3(@GLASS_WARM_R@, @GLASS_WARM_G@, @GLASS_WARM_B@) *
						pow(aligned, @GLASS_WARM_POWER@), vec3(0.0), vec3(1.0));
		float model_uniform_scale = max(abs(pc.light_local_gain.w), 1.0e-6);
		float fog_visibility = fog_enabled ? q3_fog_visibility(
				length(pc.camera_local.xyz - local_position) * model_uniform_scale,
				pc.camera_local.w, pc.draw_color.w, fog_type) : 1.0;
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
	vec4 tex = texture(primary_texture, uv);
	if (mode == 3u) {
		frag_color = vec4(tex.rgb * pc.draw_color.rgb,
				tex.a * pc.draw_color.a);
	} else {
		frag_color = vec4(tex.rgb * pc.draw_color.rgb * tex.a * pc.draw_color.a,
				1.0);
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

Ref<RDUniform> sampled_texture_uniform(int p_binding, const RID &p_sampler,
		const RID &p_texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	uniform->set_binding(p_binding);
	uniform->add_id(p_sampler);
	uniform->add_id(p_texture);
	return uniform;
}

std::uint64_t object_id(const Ref<RefCounted> &p_object) {
	return p_object.is_valid() ? p_object->get_instance_id() : 0;
}

Q3ResourceLease lease_for(const Ref<RefCounted> &p_object,
		std::uint64_t p_generation = 1) {
	return {object_id(p_object), p_generation};
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

void append_f32(PackedByteArray &p_bytes, float p_value) {
	const int64_t offset = p_bytes.size();
	p_bytes.resize(offset + 4);
	std::memcpy(p_bytes.ptrw() + offset, &p_value, sizeof(p_value));
}

void append_vec2(PackedByteArray &p_bytes, const Vector2 &p_value) {
	append_f32(p_bytes, p_value.x);
	append_f32(p_bytes, p_value.y);
}

void append_vec3(PackedByteArray &p_bytes, const Vector3 &p_value) {
	append_f32(p_bytes, p_value.x);
	append_f32(p_bytes, p_value.y);
	append_f32(p_bytes, p_value.z);
}

void append_color(PackedByteArray &p_bytes, const Color &p_value) {
	append_f32(p_bytes, p_value.r);
	append_f32(p_bytes, p_value.g);
	append_f32(p_bytes, p_value.b);
	append_f32(p_bytes, p_value.a);
}

Vector3 transformed_normal(const Basis &p_basis, const Vector3 &p_normal) {
	const Vector3 result = p_basis.xform(p_normal);
	return result.length_squared() > 0.0f ? result.normalized() : Vector3(0, 1, 0);
}

Ref<Texture2D> texture_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name) {
	if (p_material.is_null())
		return Ref<Texture2D>();
	const Variant value = p_material->get_shader_parameter(p_name);
	if (value.get_type() != Variant::OBJECT)
		return Ref<Texture2D>();
	return value;
}

float float_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, float p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT
			? static_cast<float>(value) : p_default;
}

bool bool_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, bool p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::BOOL ? static_cast<bool>(value) : p_default;
}

Vector3 vector3_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector3 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR3 ? static_cast<Vector3>(value) : p_default;
}

Vector4 vector4_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector4 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR4 ? static_cast<Vector4>(value) : p_default;
}

RID server_rid(const Ref<Texture2D> &p_texture) {
	return p_texture.is_valid() ? p_texture->get_rid() : RID();
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

Ref<Material> active_material(GeometryInstance3D *p_source,
		const Ref<Mesh> &p_mesh, int p_surface) {
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_source))
		return mesh_instance->get_active_material(p_surface);
	Ref<Material> material = p_source->get_material_override();
	if (material.is_null() && p_mesh.is_valid())
		material = p_mesh->surface_get_material(p_surface);
	return material;
}

std::vector<Transform3D> skin_palette(MeshInstance3D *p_instance) {
	std::vector<Transform3D> result;
	if (p_instance == nullptr || p_instance->get_skeleton_path().is_empty())
		return result;
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(
			p_instance->get_node_or_null(p_instance->get_skeleton_path()));
	const Ref<Skin> skin = p_instance->get_skin();
	if (skeleton == nullptr || skin.is_null() || skin->get_bind_count() <= 0)
		return result;
	result.reserve(skin->get_bind_count());
	for (int bind = 0; bind < skin->get_bind_count(); ++bind) {
		const int bone = skin->get_bind_bone(bind);
		if (bone < 0 || bone >= skeleton->get_bone_count())
			return {};
		result.push_back(skeleton->get_bone_global_pose(bone) *
				skin->get_bind_pose(bind));
	}
	return result;
}

Vector4 custom_at(const PackedFloat32Array &p_values, int p_index) {
	const int base = p_index * 4;
	if (base < 0 || base + 3 >= p_values.size())
		return Vector4();
	return Vector4(p_values[base], p_values[base + 1],
			p_values[base + 2], p_values[base + 3]);
}

bool pack_surface(const Array &p_arrays, const Ref<ShaderMaterial> &p_material,
		Q3Source p_source, const Camera3D *p_camera,
		const std::vector<Transform3D> &p_skin_palette,
		PackedByteArray &r_vertices) {
	if (p_arrays.size() <= Mesh::ARRAY_INDEX)
		return false;
	const PackedVector3Array positions = p_arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	const PackedVector2Array uvs = p_arrays[Mesh::ARRAY_TEX_UV];
	const PackedVector2Array uv2s = p_arrays[Mesh::ARRAY_TEX_UV2];
	const PackedColorArray colors = p_arrays[Mesh::ARRAY_COLOR];
	const PackedFloat32Array custom0 = p_arrays[Mesh::ARRAY_CUSTOM0];
	const PackedFloat32Array custom1 = p_arrays[Mesh::ARRAY_CUSTOM1];
	const PackedFloat32Array custom2 = p_arrays[Mesh::ARRAY_CUSTOM2];
	const PackedInt32Array bones = p_arrays[Mesh::ARRAY_BONES];
	const PackedFloat32Array weights = p_arrays[Mesh::ARRAY_WEIGHTS];
	const PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
	if (positions.is_empty())
		return false;
	const int vertex_count = indices.is_empty() ? positions.size() : indices.size();
	if (vertex_count <= 0)
		return false;
	r_vertices.resize(0);
	const Vector3 uv_u = vector3_parameter(p_material, "u_uv_transform_u",
			Vector3(1, 0, 0));
	const Vector3 uv_v = vector3_parameter(p_material, "u_uv_transform_v",
			Vector3(0, 1, 0));
	const Vector4 water_uv = vector4_parameter(p_material, "u_water_uv",
			Vector4(1.0f, 0.2f, 0.0f, 0.0f));
	const Vector3 camera_position = p_camera != nullptr ?
			p_camera->get_global_position() : Vector3();
	for (int element = 0; element < vertex_count; ++element) {
		const int index = indices.is_empty() ? element : indices[element];
		if (index < 0 || index >= positions.size())
			return false;
		Vector3 position = positions[index];
		Vector3 normal = index < normals.size() ? normals[index] : Vector3(0, 1, 0);
		if (!p_skin_palette.empty() && bones.size() >= (index + 1) * 4 &&
				weights.size() >= (index + 1) * 4) {
			Vector3 skinned_position;
			Vector3 skinned_normal;
			float total = 0.0f;
			for (int influence = 0; influence < 4; ++influence) {
				const int offset = index * 4 + influence;
				const int bone = bones[offset];
				const float weight = weights[offset];
				if (weight <= 0.0f || bone < 0 ||
						bone >= static_cast<int>(p_skin_palette.size()))
					continue;
				const Transform3D &transform = p_skin_palette[bone];
				skinned_position += transform.xform(position) * weight;
				skinned_normal += transform.basis.xform(normal) * weight;
				total += weight;
			}
			if (total > 0.0f) {
				position = skinned_position / total;
				normal = skinned_normal.length_squared() > 0.0f ?
						skinned_normal.normalized() : normal;
			}
		}
		Vector2 uv = index < uvs.size() ? uvs[index] : Vector2();
		if (p_source == Q3Source::Object) {
			uv = Vector2(uv_u.x * uv.x + uv_u.y * uv.y + uv_u.z,
					uv_v.x * uv.x + uv_v.y * uv.y + uv_v.z);
		} else if (p_source == Q3Source::Water) {
			const Vector2 relative(position.z - camera_position.z,
					position.x - camera_position.x);
			uv = relative * (water_uv.x / 128.0f) - Vector2(water_uv.y, water_uv.y) +
					Vector2(water_uv.z, water_uv.w);
		}
		append_vec3(r_vertices, position);
		append_vec3(r_vertices, normal);
		append_vec2(r_vertices, uv);
		append_color(r_vertices, index < colors.size() ? colors[index] : Color(1, 1, 1, 1));
		const Vector4 c0 = custom_at(custom0, index);
		const Vector4 c1 = custom_at(custom1, index);
		const Vector4 c2 = custom_at(custom2, index);
		append_f32(r_vertices, c0.x); append_f32(r_vertices, c0.y);
		append_f32(r_vertices, c0.z); append_f32(r_vertices, c0.w);
		append_f32(r_vertices, c1.x); append_f32(r_vertices, c1.y);
		append_f32(r_vertices, c1.z); append_f32(r_vertices, c1.w);
		append_f32(r_vertices, c2.x); append_f32(r_vertices, c2.y);
		append_f32(r_vertices, c2.z); append_f32(r_vertices, c2.w);
		append_vec2(r_vertices,
				index < uv2s.size() ? uv2s[index] : Vector2());
	}
	return r_vertices.size() == static_cast<int64_t>(vertex_count) * kVertexStride;
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
	std::size_t cpu_skinned_commands = 0;
	std::size_t registered_sources = 0;
	std::size_t frustum_culled_sources = 0;
	std::size_t frustum_culled_instances = 0;
	std::size_t packed_vertices = 0;
	std::size_t packed_vertex_bytes = 0;

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID fallback_texture;
	RID vertex_buffer;
	std::uint32_t vertex_capacity = 0;
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
		vertex_buffer = RID();
		fallback_texture = RID();
		sampler = RID();
		shader = RID();
		vertex_buffers.clear();
		vertex_offsets.clear();
		vertex_capacity = 0;
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
		rd = nullptr;
	}

	void release_device(RenderingDevice *p_rd) {
		// A null current device is an invalidation signal, not permission to
		// call through the previously cached server-owned pointer. Likewise, RIDs
		// from one device must never be freed through a replacement device.
		if (p_rd == nullptr || (rd != nullptr && rd != p_rd)) {
			discard_device_state();
			return;
		}
		rd = p_rd;
		for (RID &uniform : transient_uniforms)
			release_uniform_rid(uniform);
		for (auto &entry : pipelines)
			release_pipeline_rid(entry.second);
		release_rid(vertex_buffer);
		release_texture_rid(fallback_texture);
		release_rid(sampler);
		release_rid(shader);
		discard_device_state();
	}

	bool initialize(RenderingDevice *p_rd);
	RID pipeline_for(int64_t p_framebuffer_format,
			const Q3DrawCommand &p_command);
	bool ensure_vertices(const PackedByteArray &p_vertices);
	RID make_uniform(const RID &p_beauty, const DeviceCommand &p_command);
	bool draw(RenderData *p_render_data, std::uint32_t p_view,
			const RID &p_framebuffer, const RID &p_beauty,
			std::size_t &r_draw_calls);
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
			String::utf8(q3_vertex_shader_source().c_str()));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(q3_fragment_shader_source().c_str()));
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

	TypedArray<Ref<RDVertexAttribute>> attributes;
	auto add_attribute = [&](std::uint32_t p_location,
			RenderingDevice::DataFormat p_format, std::uint32_t p_offset) {
		Ref<RDVertexAttribute> attribute;
		attribute.instantiate();
		attribute->set_location(p_location);
		attribute->set_binding(0);
		attribute->set_format(p_format);
		attribute->set_offset(p_offset);
		attribute->set_stride(kVertexStride);
		attribute->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
		attributes.push_back(attribute);
	};
	add_attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0);
	add_attribute(1, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 12);
	add_attribute(2, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 24);
	add_attribute(3, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 32);
	add_attribute(4, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 48);
	add_attribute(5, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 64);
	add_attribute(6, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 80);
	add_attribute(7, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 96);
	vertex_format = rd->vertex_format_create(attributes);
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

bool Q3FrameAdapter::Impl::ensure_vertices(const PackedByteArray &p_vertices) {
	if (p_vertices.is_empty())
		return false;
	const std::uint32_t required = static_cast<std::uint32_t>(p_vertices.size());
	if (!vertex_buffer.is_valid() || required > vertex_capacity) {
		release_rid(vertex_buffer);
		vertex_capacity = std::max(required, vertex_capacity + vertex_capacity / 2u);
		vertex_buffer = rd->vertex_buffer_create(vertex_capacity);
		if (!vertex_buffer.is_valid()) {
			set_failure("RenderingDevice could not allocate the focused Q3 vertex buffer");
			return false;
		}
		vertex_buffers[0] = vertex_buffer;
	}
	if (rd->buffer_update(vertex_buffer, 0, required, p_vertices) != OK) {
		set_failure("RenderingDevice rejected the focused Q3 vertex upload");
		return false;
	}
	return true;
}

RID Q3FrameAdapter::Impl::make_uniform(const RID &p_beauty,
		const DeviceCommand &p_command) {
	RenderingServer *server = RenderingServer::get_singleton();
	auto resolve = [&](const RID &p_server_rid) {
		const RID result = server != nullptr && p_server_rid.is_valid() ?
				server->texture_get_rd_texture(p_server_rid, false) : RID();
		return result.is_valid() ? result : fallback_texture;
	};
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, sampler, p_beauty));
	uniforms.push_back(sampled_texture_uniform(1, sampler,
			resolve(p_command.primary_texture)));
	uniforms.push_back(sampled_texture_uniform(2, sampler,
			resolve(p_command.secondary_texture)));
	uniforms.push_back(sampled_texture_uniform(3, sampler,
			resolve(p_command.tertiary_texture)));
	return rd->uniform_set_create(uniforms, shader, 0);
}

bool Q3FrameAdapter::Impl::draw(RenderData *p_render_data, std::uint32_t p_view,
		const RID &p_framebuffer, const RID &p_beauty,
		std::size_t &r_draw_calls) {
	const std::shared_ptr<const DeviceFrame> frame = frame_snapshot();
	if (!frame || frame->commands.empty())
		return true;
	RenderSceneData *scene_data = p_render_data != nullptr ?
			p_render_data->get_render_scene_data() : nullptr;
	if (scene_data == nullptr || !ensure_vertices(frame->vertices)) {
		if (scene_data == nullptr)
			set_failure("Focused Q3 draw has no RenderSceneData");
		return false;
	}
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
		RID uniform = make_uniform(p_beauty, command);
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
			if (draw.technique == Q3Technique::RotatedSpecularGlass) {
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
			}
			vertex_offsets[0] = static_cast<int64_t>(command.first_vertex) *
					kVertexStride;
			rd->draw_list_bind_render_pipeline(draw_list, pipeline);
			rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
			rd->draw_list_bind_vertex_buffers_format(draw_list, vertex_format,
					command.vertex_count, vertex_buffers, vertex_offsets);
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
	result["q3_status"] = String::utf8(status.c_str());
	result["q3_failure"] = String::utf8(failure.c_str());
	result["q3_submitted_frame_id"] = static_cast<int64_t>(submitted_frame_id);
	result["q3_drawn_frame_id"] = static_cast<int64_t>(drawn_frame_id);
	result["q3_submitted_commands"] = static_cast<int64_t>(submitted_commands);
	result["q3_rejected_commands"] = static_cast<int64_t>(rejected_commands);
	result["q3_drawn_commands"] = static_cast<int64_t>(drawn_commands);
	result["q3_gpu_draw_calls"] = static_cast<int64_t>(gpu_draw_calls);
	result["q3_cpu_skinned_commands"] = static_cast<int64_t>(cpu_skinned_commands);
	result["q3_registered_sources"] = static_cast<int64_t>(registered_sources);
	result["q3_frustum_culled_sources"] =
			static_cast<int64_t>(frustum_culled_sources);
	result["q3_frustum_culled_instances"] =
			static_cast<int64_t>(frustum_culled_instances);
	result["q3_packed_vertices"] = static_cast<int64_t>(packed_vertices);
	result["q3_packed_vertex_bytes"] =
			static_cast<int64_t>(packed_vertex_bytes);
	result["q3_static_instance_submission"] = "retained_transform_ranges";
	result["q3_skinned_submission"] = "immutable_cpu_pose_snapshot";
	return result;
}

Q3FrameAdapter::Q3FrameAdapter() : impl_(std::make_unique<Impl>()) {}

Q3FrameAdapter::~Q3FrameAdapter() = default;

void Q3FrameAdapter::register_object_material(const Ref<Material> &p_material,
		const ObjectMaterialClassification &p_classification) {
	if (p_material.is_null())
		return;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	g_materials[p_material->get_instance_id()] = p_classification;
}

void Q3FrameAdapter::clone_object_material(const Ref<Material> &p_source,
		const Ref<Material> &p_clone) {
	if (p_source.is_null() || p_clone.is_null())
		return;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	const auto found = g_materials.find(p_source->get_instance_id());
	if (found != g_materials.end())
		g_materials[p_clone->get_instance_id()] = found->second;
}

void Q3FrameAdapter::register_object_source(GeometryInstance3D *p_source,
		const Ref<Material> &p_material) {
	if (p_source == nullptr || p_material.is_null())
		return;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	const auto classification = g_materials.find(p_material->get_instance_id());
	if (classification == g_materials.end() ||
			!classification->second.is_glow_capable)
		return;
	g_sources[p_source->get_instance_id()] = {p_source->get_instance_id(),
			Q3Source::Object, p_material->get_instance_id()};
}

void Q3FrameAdapter::register_source(GeometryInstance3D *p_source,
		Q3Source p_kind) {
	if (p_source == nullptr || p_kind == Q3Source::Object ||
			p_kind == Q3Source::LightCorona)
		return;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	g_sources[p_source->get_instance_id()] = {p_source->get_instance_id(),
			p_kind, 0};
}

void Q3FrameAdapter::compile_frame(Node *p_scope, Viewport *p_viewport,
		Camera3D *p_camera) {
	if (!impl_ || p_scope == nullptr || p_viewport == nullptr || p_camera == nullptr) {
		clear_frame();
		return;
	}
	std::vector<RegisteredQ3Source> registrations;
	std::map<std::uint64_t, ObjectMaterialClassification> materials;
	{
		std::lock_guard<std::mutex> lock(g_registry_mutex);
		for (auto it = g_sources.begin(); it != g_sources.end();) {
			if (ObjectDB::get_instance(it->first) == nullptr) {
				it = g_sources.erase(it);
			} else {
				registrations.push_back(it->second);
				++it;
			}
		}
		for (auto it = g_materials.begin(); it != g_materials.end();) {
			// A cached ShaderMaterial may deliberately outlive every current
			// source and later be reused. ObjectDB lifetime, not the live-source
			// set, is therefore the safe pruning authority.
			if (ObjectDB::get_instance(it->first) == nullptr) {
				it = g_materials.erase(it);
			} else {
				++it;
			}
		}
		materials = g_materials;
	}
	Q3FrameSnapshot snapshot;
	snapshot.frame_id = g_frame_id.fetch_add(1);
	snapshot.scene_generation = p_viewport->get_instance_id();
	std::vector<Candidate> candidates;
	std::size_t cpu_skinned = 0;
	std::size_t frustum_culled_sources = 0;
	std::size_t frustum_culled_instances = 0;
	std::size_t packed_vertices = 0;
	std::size_t packed_vertex_bytes = 0;
	const CameraFrustum frustum = camera_frustum(p_camera, p_viewport);
	for (const RegisteredQ3Source &registration : registrations) {
		GeometryInstance3D *source = Object::cast_to<GeometryInstance3D>(
				ObjectDB::get_instance(registration.node_id));
		if (source == nullptr || !source->is_visible_in_tree() ||
				!belongs_to_scope(source, p_scope, p_viewport))
			continue;
		// Focused Q3 admits world/local-body object geometry, never the
		// first-person render-FOV/depth-band or shadow/capture-only layers.
		// World-no-mirror is still ordinary beauty geometry and remains eligible.
		constexpr std::uint32_t kWorldLayer = 1u << 0;
		constexpr std::uint32_t kWorldNoMirrorLayer = 1u << 16;
		if (registration.source == Q3Source::Object &&
				(source->get_layer_mask() &
						(kWorldLayer | kWorldNoMirrorLayer)) == 0)
			continue;
		Ref<Mesh> mesh;
		Ref<MultiMesh> multimesh;
		if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(source))
			mesh = mesh_instance->get_mesh();
		else if (MultiMeshInstance3D *multi_instance =
				Object::cast_to<MultiMeshInstance3D>(source)) {
			multimesh = multi_instance->get_multimesh();
			if (multimesh.is_valid())
				mesh = multimesh->get_mesh();
		}
		Ref<ArrayMesh> array_mesh = mesh;
		if (array_mesh.is_null())
			continue;
		const AABB source_world_bounds = source->get_global_transform().xform(
				source->get_aabb());
		if (frustum.outside(source_world_bounds)) {
			++frustum_culled_sources;
			continue;
		}

		const std::vector<Transform3D> palette =
				registration.source == Q3Source::Object ?
					skin_palette(Object::cast_to<MeshInstance3D>(source)) :
					std::vector<Transform3D>();
		std::vector<Transform3D> emitted_transforms;
		AABB emitted_world_bounds;
		bool has_emitted_world_bounds = false;
		if (multimesh.is_valid()) {
			const int visible = multimesh->get_visible_instance_count();
			const int count = visible < 0 ? multimesh->get_instance_count() : visible;
			emitted_transforms.reserve(count);
			for (int instance = 0; instance < count; ++instance) {
				const Transform3D transform = source->get_global_transform() *
						multimesh->get_instance_transform(instance);
				if (std::abs(transform.basis.determinant()) <= 1.0e-8f)
					continue;
				const AABB instance_world_bounds = transform.xform(mesh->get_aabb());
				if (frustum.outside(instance_world_bounds)) {
					++frustum_culled_instances;
					continue;
				}
				emitted_transforms.push_back(transform);
				merge_bounds(emitted_world_bounds, has_emitted_world_bounds,
						instance_world_bounds);
			}
		} else {
			emitted_transforms.push_back(source->get_global_transform());
			merge_bounds(emitted_world_bounds, has_emitted_world_bounds,
					source_world_bounds);
		}
		if (emitted_transforms.empty())
			continue;
		const Vector3 population_center = has_emitted_world_bounds ?
				emitted_world_bounds.get_center() : source->get_global_position();
		const Vector3 camera_position = p_camera->get_global_position();
		const Vector3 camera_forward = -p_camera->get_global_basis().get_column(2);
		const float population_view_depth = std::max(0.0f,
				(population_center - camera_position).dot(camera_forward));

		for (int surface = 0; surface < array_mesh->get_surface_count(); ++surface) {
			const Ref<Material> material = active_material(source, mesh, surface);
			const Ref<ShaderMaterial> shader_material = material;
			if (shader_material.is_null())
				continue;
			ObjectMaterialClassification object_classification;
			if (registration.source == Q3Source::Object) {
				const auto classification = materials.find(material->get_instance_id());
				if (classification == materials.end())
					continue;
				object_classification = classification->second;
			}
			Candidate candidate;
			candidate.submission.submission_id = registration.node_id ^
					(static_cast<std::uint64_t>(surface + 1) << 48u);
			candidate.submission.source = registration.source;
			candidate.submission.geometry = {object_id(mesh), snapshot.frame_id};
			candidate.submission.material = lease_for(material);
			candidate.submission.surface_index = surface;
			if (!pack_surface(array_mesh->surface_get_arrays(surface), shader_material,
					registration.source, p_camera, palette, candidate.vertices))
				continue;
			packed_vertex_bytes += candidate.vertices.size();
			packed_vertices += candidate.vertices.size() / kVertexStride;
			candidate.submission.first_transform = snapshot.transforms.size();
			if (multimesh.is_valid())
				candidate.submission.geometry_kind = Q3GeometryKind::StaticInstances;
			else
				candidate.submission.geometry_kind = palette.empty() ?
						Q3GeometryKind::Rigid : Q3GeometryKind::Skinned;
			for (const Transform3D &transform : emitted_transforms)
				snapshot.transforms.push_back(q3_matrix(transform));
			candidate.submission.transform_count = snapshot.transforms.size() -
					candidate.submission.first_transform;
			if (!palette.empty()) {
				candidate.submission.first_bone = snapshot.bone_palette.size();
				for (const Transform3D &bone : palette)
					snapshot.bone_palette.push_back(q3_matrix(bone));
				candidate.submission.bone_count = palette.size();
				++cpu_skinned;
			}
			candidate.submission.view_depth = population_view_depth;
			if (registration.source == Q3Source::Object) {
				candidate.submission.object.classification = object_classification;
				const Ref<Texture2D> base = texture_parameter(shader_material, "u_diffuse");
				const Ref<Texture2D> detail = texture_parameter(shader_material, "u_detail");
				candidate.submission.object.base_texture = lease_for(base);
				candidate.submission.object.detail_texture = lease_for(detail);
				candidate.primary_texture_resource = base;
				candidate.secondary_texture_resource = detail;
				candidate.primary_texture = server_rid(base);
				candidate.secondary_texture = server_rid(detail);
				const Vector4 reflect = vector4_parameter(shader_material,
						"u_reflect_color", Vector4(0.7f, 0.8f, 0.9f, 0.35f));
				candidate.submission.object.reflect_color = {reflect.x, reflect.y,
						reflect.z, reflect.w};
				candidate.submission.object.alpha_mod = float_parameter(shader_material,
						"u_alpha_mod", 1.0f);
			} else if (registration.source == Q3Source::Water) {
				const Ref<Texture2D> reflection = texture_parameter(shader_material,
						"u_reflection");
				const Ref<Texture2D> noise_color = texture_parameter(shader_material,
						"u_noise_color");
				const Ref<Texture2D> noise_normal = texture_parameter(shader_material,
						"u_noise_normal");
				candidate.submission.water.has_reflection = bool_parameter(shader_material,
						"u_has_reflection", reflection.is_valid());
				candidate.submission.water.reflection_texture = lease_for(reflection);
				candidate.submission.water.noise_color_texture = lease_for(noise_color);
				candidate.submission.water.noise_normal_texture = lease_for(noise_normal);
				const Vector3 water_color = vector3_parameter(shader_material,
						"u_water_color", Vector3(0.408f, 0.314f, 0.224f));
				candidate.submission.water.water_color = {water_color.x,
						water_color.y, water_color.z};
				const Vector4 uv = vector4_parameter(shader_material, "u_water_uv",
						Vector4(1, 0.2f, 0, 0));
				candidate.submission.water.water_uv = {uv.x, uv.y, uv.z, uv.w};
				const Variant scale_variant = shader_material->get_shader_parameter(
						"u_reflection_uv_scale");
				const Vector2 scale = scale_variant.get_type() == Variant::VECTOR2 ?
						static_cast<Vector2>(scale_variant) : Vector2(1, 1);
				candidate.submission.water.reflection_uv_scale = {scale.x, scale.y};
				candidate.submission.water.underwater_view = bool_parameter(shader_material,
						"u_underwater_view", false);
				candidate.primary_texture_resource = noise_color;
				candidate.secondary_texture_resource = noise_normal;
				candidate.tertiary_texture_resource = reflection;
				candidate.primary_texture = server_rid(noise_color);
				candidate.secondary_texture = server_rid(noise_normal);
				candidate.tertiary_texture = server_rid(reflection);
			} else {
				const Ref<Texture2D> diffuse = texture_parameter(shader_material,
						"u_diffuse");
				candidate.submission.celestial.diffuse_texture = lease_for(diffuse);
				const Vector3 tint = vector3_parameter(shader_material, "u_tint",
						Vector3(1, 1, 1));
				candidate.submission.celestial.tint = {tint.x, tint.y, tint.z};
				candidate.submission.celestial.opacity = float_parameter(shader_material,
						registration.source == Q3Source::SunGlow ? "u_q3_opacity" :
						"u_opacity", 1.0f);
				const Vector3 glare = vector3_parameter(shader_material,
						"u_glare_direction", Vector3(0, 1, 0));
				candidate.submission.celestial.glare_direction =
						{glare.x, glare.y, glare.z};
				candidate.submission.celestial.glare_view_fade = bool_parameter(
						shader_material, "u_glare_view_fade", false);
				candidate.submission.celestial.additive =
						registration.source == Q3Source::SunGlow;
				candidate.primary_texture_resource = diffuse;
				candidate.primary_texture = server_rid(diffuse);
			}
			snapshot.submissions.push_back(candidate.submission);
			candidates.push_back(std::move(candidate));
		}
	}
	const Q3DrawList &draw_list = impl_->compiler.compile(snapshot);
	auto frame = std::make_shared<DeviceFrame>();
	frame->draw_list = draw_list;
	Ref<EnvLightValues> light_values = EnvLightValues::retail_noon_defaults();
	if (MissionEnvironment *environment = environment_in_scope(p_scope,
			p_viewport)) {
		const Ref<EnvLightState> light_state = environment->get_light_state();
		if (light_state.is_valid() && light_state->get_values().is_valid())
			light_values = light_state->get_values();
		frame->fog_start = environment->get_scene_fog_start();
		frame->fog_end = environment->get_scene_fog_end();
		frame->fog_type = environment->get_scene_fog_type();
	}
	if (light_values.is_valid()) {
		frame->light_direction = light_values->dir;
		frame->light_gain = light_values->gain;
		frame->fog_enabled = light_values->fog_enabled;
		if (frame->fog_end <= 0.0f) {
			frame->fog_start = light_values->fog_start;
			frame->fog_end = light_values->fog_end;
			frame->fog_type = light_values->fog_type;
		}
	}
	for (const Q3DrawCommand &draw : draw_list.commands) {
		if (draw.input_index >= candidates.size())
			continue;
		const Candidate &candidate = candidates[draw.input_index];
		DeviceCommand command;
		command.draw = draw;
		command.first_vertex = frame->vertices.size() / kVertexStride;
		command.vertex_count = candidate.vertices.size() / kVertexStride;
		command.primary_texture_resource = candidate.primary_texture_resource;
		command.secondary_texture_resource = candidate.secondary_texture_resource;
		command.tertiary_texture_resource = candidate.tertiary_texture_resource;
		command.primary_texture = candidate.primary_texture;
		command.secondary_texture = candidate.secondary_texture;
		command.tertiary_texture = candidate.tertiary_texture;
		frame->vertices.append_array(candidate.vertices);
		frame->commands.push_back(command);
	}
	impl_->publish(frame);
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->status = frame->commands.empty() ? "compiled_empty" : "compiled";
		impl_->failure.clear();
		impl_->submitted_frame_id = snapshot.frame_id;
		impl_->submitted_commands = frame->commands.size();
		impl_->rejected_commands = draw_list.rejected.size();
		impl_->cpu_skinned_commands = cpu_skinned;
		impl_->registered_sources = registrations.size();
		impl_->frustum_culled_sources = frustum_culled_sources;
		impl_->frustum_culled_instances = frustum_culled_instances;
		impl_->packed_vertices = packed_vertices;
		impl_->packed_vertex_bytes = packed_vertex_bytes;
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

bool Q3FrameAdapter::draw_view(RenderingDevice *p_rd, RenderData *p_render_data,
		std::uint32_t p_view, const RID &p_framebuffer,
		const RID &p_beauty_snapshot, const Vector2i &,
		std::size_t &r_draw_calls) {
	return impl_ && impl_->initialize(p_rd) && impl_->draw(p_render_data, p_view,
			p_framebuffer, p_beauty_snapshot, r_draw_calls);
}

void Q3FrameAdapter::release_device(RenderingDevice *p_rd) {
	if (impl_)
		impl_->release_device(p_rd);
}

Dictionary Q3FrameAdapter::get_report() const {
	return impl_ ? impl_->report() : Dictionary();
}
