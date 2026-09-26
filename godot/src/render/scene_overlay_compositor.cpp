#include "render/scene_overlay_compositor.h"
#include "render/rd_glsl.h"
#include "render/rd_uniforms.h"
#include "util/string_convert.h"

#include <runtime/renderer/q3_frame.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

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
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

using namespace godot;

namespace {

using opennova::renderer::SceneOverlayBatch;
using opennova::renderer::SceneOverlayDepth;
using opennova::renderer::SceneOverlayGeometry;
using opennova::renderer::SceneOverlayShading;
using opennova::renderer::SceneOverlayVertex;

constexpr std::uint32_t kVertexStride =
		static_cast<std::uint32_t>(sizeof(SceneOverlayVertex));
static_assert(sizeof(SceneOverlayVertex) == 44,
		"the RD vertex format below is the engine vertex, verbatim");
constexpr std::uint32_t kPushConstantBytes = 112;
constexpr std::uint32_t kMinimumVertexCapacity = 4096 * kVertexStride;

// The far depth band's two constants (runtime/renderer/q3_frame.h
// kQ3FarBandMinZ/MaxZ) splice into the vertex stage at compile.
const char *kVertexShaderTemplate = R"GLSL(#version 450
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec2 a_corner;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_uv1;

layout(push_constant, std430) uniform OverlayPush {
	mat4 view_projection;
	vec4 view_right;
	vec4 view_up;
	uvec4 mode; // x = shading, y = geometry, z = the far depth band
} pc;

void main() {
	v_uv = a_uv;
	v_color = a_color;
	// World geometry carries its second texture coordinate set in the corner.
	v_uv1 = a_corner;
	if (pc.mode.y == 2u) {
		// A viewport quad: the positions are normalized device xy already.
		gl_Position = vec4(a_position.xy, 0.0, 1.0);
		return;
	}
	vec3 world_position = a_position;
	if (pc.mode.y == 1u) {
		// A billboard faces THIS view: the corner rides its right/up axes.
		world_position += pc.view_right.xyz * a_corner.x + pc.view_up.xyz * a_corner.y;
	}
	gl_Position = pc.view_projection * vec4(world_position, 1.0);
	if (pc.mode.z == 1u && gl_Position.w > 0.0) {
		// Render_SetViewportFarDepth's viewport MinZ/MaxZ band in reverse-Z
		// clip depth: z' = (1 - MaxZ) + z * (MaxZ - MinZ).
		float z_rev = gl_Position.z / gl_Position.w;
		gl_Position.z = (@FAR_BAND_REV_MIN@ + z_rev * @FAR_BAND_REV_SPAN@) *
				gl_Position.w;
	}
}
)GLSL";

std::string vertex_shader_source() {
	std::string source(kVertexShaderTemplate);
	splice_token(source, "@FAR_BAND_REV_MIN@",
			glsl_float(1.0f - opennova::renderer::kQ3FarBandMaxZ));
	splice_token(source, "@FAR_BAND_REV_SPAN@", glsl_float(opennova::renderer::kQ3FarBandMaxZ -
			opennova::renderer::kQ3FarBandMinZ));
	return source;
}

const char *kFragmentShader = R"GLSL(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_uv1;

layout(set = 0, binding = 0) uniform sampler2D overlay_texture;

layout(push_constant, std430) uniform OverlayPush {
	mat4 view_projection;
	vec4 view_right;
	vec4 view_up;
	uvec4 mode;
} pc;

layout(location = 0) out vec4 frag_color;

void main() {
	// Gamma-domain combines into the gamma-domain scene target
	// (shaders/color.gdshaderinc carries that contract).
	vec4 texel = texture(overlay_texture, v_uv);
	uint shading = pc.mode.x;
	if (shading == 0u) {
		// COLOROP MODULATE2X(TEXTURE, DIFFUSE), ALPHAOP MODULATE.
		frag_color = vec4(min(texel.rgb * v_color.rgb * 2.0, vec3(1.0)),
				texel.a * v_color.a);
	} else if (shading == 1u) {
		// MODULATE(TEXTURE, DIFFUSE) added; the diffuse holds fade and fog.
		frag_color = vec4(clamp(texel.rgb * v_color.rgb, 0.0, 1.0), 0.0);
	} else if (shading == 2u) {
		// SELFLUM: tex x 2 x sat(SelfLumColor x gain), then the fog to black.
		frag_color = vec4(clamp(texel.rgb * 2.0 * v_color.rgb, 0.0, 1.0) * v_color.a,
				0.0);
	} else if (shading == 4u) {
		// The flat diffuse the DESTCOLOR / ZERO blend multiplies in.
		frag_color = vec4(v_color.rgb, 1.0);
	} else if (shading == 5u) {
		// The NVG laser (the tracer pool's 0x3008 material): COLOR = DIFFUSE,
		// ALPHA = DIFFUSE.a x (1 - T0.a) x (1 - T1.a), T1 on the second set;
		// SRCALPHA / ONE blends it.
		float t1 = texture(overlay_texture, v_uv1).a;
		frag_color = vec4(v_color.rgb, v_color.a * (1.0 - texel.a) * (1.0 - t1));
	} else {
		frag_color = v_color;
	}
}
)GLSL";

bool additive(SceneOverlayShading p_shading) {
	return p_shading == SceneOverlayShading::AdditiveModulate ||
			p_shading == SceneOverlayShading::SelfLumAdditive;
}

} // namespace

std::uint32_t SceneOverlaySubmission::texture_index(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null())
		return opennova::renderer::kSceneOverlayNoTexture;
	const RID rid = p_texture->get_rid();
	for (std::size_t i = 0; i < textures.size(); ++i) {
		if (textures[i] == rid)
			return static_cast<std::uint32_t>(i);
	}
	textures.push_back(rid);
	return static_cast<std::uint32_t>(textures.size() - 1);
}

const std::vector<SceneOverlayModelSurfaces::Geometry> &SceneOverlayModelSurfaces::geometry_for(
		const Ref<Mesh> &p_mesh) {
	const std::uint64_t key = p_mesh->get_rid().get_id();
	auto found = geometry_.find(key);
	if (found != geometry_.end()) {
		return found->second;
	}
	std::vector<Geometry> surfaces;
	for (int32_t surface = 0; surface < p_mesh->get_surface_count(); ++surface) {
		Geometry geometry;
		const Array arrays = p_mesh->surface_get_arrays(surface);
		if (arrays.size() > Mesh::ARRAY_INDEX) {
			const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
			const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
			const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
			const int64_t count = indices.is_empty() ? vertices.size() : indices.size();
			for (int64_t i = 0; i + 2 < count; i += 3) {
				for (int64_t corner = 0; corner < 3; ++corner) {
					const int64_t at = i + corner;
					const int64_t index = indices.is_empty() ? at : int64_t(indices[at]);
					const bool valid = index >= 0 && index < vertices.size();
					const Vector3 v = valid ? vertices[index] : Vector3();
					const Vector2 uv = valid && index < uvs.size() ? uvs[index] : Vector2();
					geometry.positions.push_back(static_cast<float>(v.x));
					geometry.positions.push_back(static_cast<float>(v.y));
					geometry.positions.push_back(static_cast<float>(v.z));
					geometry.uvs.push_back(static_cast<float>(uv.x));
					geometry.uvs.push_back(static_cast<float>(uv.y));
				}
			}
		}
		surfaces.push_back(std::move(geometry));
	}
	return geometry_.emplace(key, std::move(surfaces)).first->second;
}

int SceneOverlayModelSurfaces::append_instance(opennova::renderer::SceneOverlaySlot p_slot,
		MeshInstance3D *p_instance, const float p_light_scale_rgb[3], float p_fog_visibility,
		SceneOverlaySubmission &r_submission, const AppendOptions &p_options) {
	const Ref<Mesh> mesh = p_instance->get_mesh();
	if (mesh.is_null()) {
		return 0;
	}
	const std::vector<Geometry> &surfaces = geometry_for(mesh);
	Transform3D placement = p_instance->get_global_transform();
	placement.origin += p_options.offset;
	const std::array<float, 3> *self_lum_override = nullptr;
	if (p_options.self_lum != nullptr) {
		const auto found = p_options.self_lum->find(p_instance);
		if (found != p_options.self_lum->end())
			self_lum_override = &found->second;
	}
	int appended = 0;
	for (std::size_t surface = 0; surface < surfaces.size(); ++surface) {
		const Geometry &geometry = surfaces[surface];
		const std::size_t vertex_count = geometry.uvs.size() / 2;
		const Ref<ShaderMaterial> material =
				p_instance->get_active_material(static_cast<int32_t>(surface));
		if (vertex_count == 0 || material.is_null()) {
			continue;
		}
		const Vector3 self_lum = self_lum_override != nullptr ?
				Vector3((*self_lum_override)[0], (*self_lum_override)[1],
						(*self_lum_override)[2]) :
				Vector3(material->get_shader_parameter("u_rgb_mod"));
		const Ref<Texture2D> texture = material->get_shader_parameter("u_diffuse");
		const float self_lum_rgb[3] = { static_cast<float>(self_lum.x),
			static_cast<float>(self_lum.y), static_cast<float>(self_lum.z) };
		placed_.resize(vertex_count * 3);
		for (std::size_t v = 0; v < vertex_count; ++v) {
			const Vector3 world = placement.xform(Vector3(geometry.positions[v * 3 + 0],
					geometry.positions[v * 3 + 1], geometry.positions[v * 3 + 2]));
			placed_[v * 3 + 0] = static_cast<float>(world.x);
			placed_[v * 3 + 1] = static_cast<float>(world.y);
			placed_[v * 3 + 2] = static_cast<float>(world.z);
		}
		opennova::renderer::append_self_lum_overlay(p_slot, placed_.data(),
				geometry.uvs.data(), vertex_count, self_lum_rgb, p_light_scale_rgb,
				p_fog_visibility, r_submission.texture_index(texture), r_submission.frame,
				p_options.depth);
		++appended;
	}
	return appended;
}

int SceneOverlayModelSurfaces::append(opennova::renderer::SceneOverlaySlot p_slot,
		Node *p_model, const float p_light_scale_rgb[3], float p_fog_visibility,
		SceneOverlaySubmission &r_submission, const AppendOptions &p_options) {
	if (p_model == nullptr) {
		return 0;
	}
	int appended = 0;
	if (MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_model)) {
		appended += append_instance(p_slot, instance, p_light_scale_rgb, p_fog_visibility,
				r_submission, p_options);
	}
	for (int32_t i = 0; i < p_model->get_child_count(); ++i) {
		appended += append(p_slot, p_model->get_child(i), p_light_scale_rgb, p_fog_visibility,
				r_submission, p_options);
	}
	return appended;
}

void SceneOverlayModelSurfaces::take_over(Node *p_model) {
	if (p_model == nullptr) {
		return;
	}
	if (MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_model)) {
		if (instance->get_layer_mask() != 0) {
			instance->set_layer_mask(0);
		}
	}
	for (int32_t i = 0; i < p_model->get_child_count(); ++i) {
		take_over(p_model->get_child(i));
	}
}

void SceneOverlayModelSurfaces::clear() {
	geometry_.clear();
	placed_.clear();
}

class SceneOverlayCompositorEffect::Impl {
public:
	struct Diagnostics {
		std::string status = "waiting_for_submission";
		std::string failure;
		std::uint64_t submitted_frame_id = 0;
		std::uint64_t drawn_frame_id = 0;
		std::size_t submitted_batches = 0;
		std::size_t drawn_batches = 0;
		std::size_t gated_batches = 0;
		float eye_height = 0.0f;
		std::vector<int> submitted_slots;
		std::vector<int> drawn_slots;
		bool callback_seen = false;
	};

	struct PipelineKey {
		int64_t framebuffer_format = -1;
		std::uint8_t shading = 0;
		std::uint8_t depth = 0;

		bool operator<(const PipelineKey &p_other) const {
			return std::tie(framebuffer_format, shading, depth) <
					std::tie(p_other.framebuffer_format, p_other.shading, p_other.depth);
		}
	};

	struct ViewTarget {
		RID color;
		RID depth;
		RID framebuffer;
		Vector2i size;
	};

	std::atomic<int> view_kind{VIEW_SCENE};
	std::atomic<bool> shutdown_requested{false};
	mutable std::mutex submission_mutex;
	std::shared_ptr<const SceneOverlaySubmission> latest;
	mutable std::mutex diagnostics_mutex;
	Diagnostics diagnostics;

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID wrap_sampler; // the NVG laser's wrapping texture stages
	RID white_texture;
	RID vertex_buffer;
	std::uint32_t vertex_capacity = 0;
	std::uint64_t uploaded_frame_id = std::numeric_limits<std::uint64_t>::max();
	int64_t vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	TypedArray<RID> vertex_buffers;
	PackedInt64Array vertex_offsets;
	PackedByteArray push_constants;
	std::map<PipelineKey, RID> pipelines;
	std::map<std::uint64_t, RID> texture_uniform_sets;
	std::vector<ViewTarget> targets;
	std::uint64_t target_buffers_id = 0;
	std::vector<SceneOverlayBatch> draw_list;

	void set_status(const std::string &p_status, const std::string &p_failure = std::string()) {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.status = p_status;
		diagnostics.failure = p_failure;
	}

	std::shared_ptr<const SceneOverlaySubmission> snapshot() const {
		std::lock_guard<std::mutex> lock(submission_mutex);
		return latest;
	}

	void free_rid(RID &p_rid) {
		if (rd != nullptr && p_rid.is_valid())
			rd->free_rid(p_rid);
		p_rid = RID();
	}

	void release_targets() {
		for (ViewTarget &target : targets) {
			if (rd != nullptr && target.framebuffer.is_valid() &&
					rd->framebuffer_is_valid(target.framebuffer))
				rd->free_rid(target.framebuffer);
		}
		targets.clear();
		target_buffers_id = 0;
	}

	void release_all() {
		release_targets();
		for (auto &entry : texture_uniform_sets) {
			if (rd != nullptr && entry.second.is_valid() &&
					rd->uniform_set_is_valid(entry.second))
				rd->free_rid(entry.second);
		}
		texture_uniform_sets.clear();
		for (auto &entry : pipelines) {
			if (rd != nullptr && entry.second.is_valid() &&
					rd->render_pipeline_is_valid(entry.second))
				rd->free_rid(entry.second);
		}
		pipelines.clear();
		free_rid(vertex_buffer);
		vertex_capacity = 0;
		uploaded_frame_id = std::numeric_limits<std::uint64_t>::max();
		free_rid(white_texture);
		free_rid(wrap_sampler);
		free_rid(sampler);
		free_rid(shader);
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
		rd = nullptr;
	}

	bool initialize_rd();
	bool ensure_targets(RenderSceneBuffersRD *p_buffers, std::uint32_t p_view_count,
			const Vector2i &p_size);
	bool upload(const SceneOverlaySubmission &p_submission);
	RID pipeline_for(int64_t p_framebuffer_format, const SceneOverlayBatch &p_batch);
	RID uniform_set_for(const RID &p_server_texture, bool p_wrap);
	void draw(const SceneOverlaySubmission &p_submission, RenderData *p_render_data);
};

bool SceneOverlayCompositorEffect::Impl::initialize_rd() {
	if (rd != nullptr && shader.is_valid() && sampler.is_valid() &&
			wrap_sampler.is_valid() && white_texture.is_valid() &&
			vertex_format != RenderingDevice::INVALID_FORMAT_ID)
		return true;
	release_all();
	RenderingServer *server = RenderingServer::get_singleton();
	rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		set_status("compatibility_renderer_unsupported",
				"RenderingDevice is unavailable; the overlay stage requires Forward+ or Mobile");
		return false;
	}
	Ref<RDShaderSPIRV> spirv;
	const std::string errors =
			compile_rd_spirv(rd, vertex_shader_source(), kFragmentShader, spirv);
	if (!errors.empty()) {
		set_status("shader_compile_failed", errors);
		release_all();
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova scene overlay stage");
	if (!shader.is_valid()) {
		set_status("shader_create_failed", "RenderingDevice rejected the overlay shader");
		release_all();
		return false;
	}
	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler = rd->sampler_create(sampler_state);
	// The ribbon's texture coordinates run past 1 (the NVG laser's two
	// stages): the device default WRAP address mode.
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	wrap_sampler = rd->sampler_create(sampler_state);
	// The untextured batches (the murk quad) and an unresolved texture sample
	// white, so the combine reduces to the diffuse.
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format->set_width(1);
	format->set_height(1);
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray white;
	white.resize(4);
	std::memset(white.ptrw(), 0xFF, 4);
	TypedArray<PackedByteArray> data;
	data.push_back(white);
	white_texture = rd->texture_create(format, view, data);
	if (!sampler.is_valid() || !wrap_sampler.is_valid() || !white_texture.is_valid()) {
		set_status("device_resource_failed", "the overlay sampler or fallback texture failed");
		release_all();
		return false;
	}
	TypedArray<Ref<RDVertexAttribute>> attributes;
	auto attribute = [&](std::uint32_t p_location, RenderingDevice::DataFormat p_format,
			std::uint32_t p_offset) {
		Ref<RDVertexAttribute> a;
		a.instantiate();
		a->set_location(p_location);
		a->set_binding(0);
		a->set_format(p_format);
		a->set_offset(p_offset);
		a->set_stride(kVertexStride);
		a->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
		attributes.push_back(a);
	};
	attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT,
			static_cast<std::uint32_t>(offsetof(SceneOverlayVertex, position)));
	attribute(1, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT,
			static_cast<std::uint32_t>(offsetof(SceneOverlayVertex, uv)));
	attribute(2, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT,
			static_cast<std::uint32_t>(offsetof(SceneOverlayVertex, color)));
	attribute(3, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT,
			static_cast<std::uint32_t>(offsetof(SceneOverlayVertex, corner)));
	vertex_format = rd->vertex_format_create(attributes);
	if (vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
		set_status("vertex_format_failed", "RenderingDevice rejected the overlay vertex format");
		release_all();
		return false;
	}
	vertex_buffers.resize(1);
	vertex_offsets.resize(1);
	push_constants.resize(kPushConstantBytes);
	return true;
}

bool SceneOverlayCompositorEffect::Impl::ensure_targets(RenderSceneBuffersRD *p_buffers,
		std::uint32_t p_view_count, const Vector2i &p_size) {
	const std::uint64_t buffers_id = p_buffers->get_instance_id();
	bool matches = target_buffers_id == buffers_id && targets.size() == p_view_count;
	for (std::uint32_t view = 0; matches && view < p_view_count; ++view) {
		const ViewTarget &target = targets[view];
		matches = target.size == p_size && target.color == p_buffers->get_color_layer(view) &&
				target.depth == p_buffers->get_depth_layer(view) &&
				target.framebuffer.is_valid() && rd->framebuffer_is_valid(target.framebuffer);
	}
	if (matches)
		return true;
	release_targets();
	for (std::uint32_t view = 0; view < p_view_count; ++view) {
		ViewTarget target;
		target.color = p_buffers->get_color_layer(view);
		target.depth = p_buffers->get_depth_layer(view);
		target.size = p_size;
		TypedArray<RID> attachments;
		attachments.push_back(target.color);
		attachments.push_back(target.depth);
		target.framebuffer = rd->framebuffer_create(attachments);
		if (!target.framebuffer.is_valid()) {
			set_status("framebuffer_failed", "the overlay framebuffer could not be created");
			release_targets();
			return false;
		}
		targets.push_back(target);
	}
	target_buffers_id = buffers_id;
	return true;
}

bool SceneOverlayCompositorEffect::Impl::upload(const SceneOverlaySubmission &p_submission) {
	if (uploaded_frame_id == p_submission.frame_id)
		return true;
	const std::size_t bytes = p_submission.frame.vertices.size() * kVertexStride;
	if (bytes == 0) {
		uploaded_frame_id = p_submission.frame_id;
		return true;
	}
	if (bytes > std::numeric_limits<std::uint32_t>::max()) {
		set_status("vertex_upload_failed", "the overlay vertices exceed a 32-bit buffer");
		return false;
	}
	const std::uint32_t required = static_cast<std::uint32_t>(bytes);
	if (!vertex_buffer.is_valid() || required > vertex_capacity) {
		free_rid(vertex_buffer);
		vertex_capacity = std::max(kMinimumVertexCapacity, required * 2u);
		vertex_buffer = rd->vertex_buffer_create(vertex_capacity);
		if (!vertex_buffer.is_valid()) {
			vertex_capacity = 0;
			set_status("vertex_buffer_failed", "the overlay vertex buffer could not be allocated");
			return false;
		}
		vertex_buffers[0] = vertex_buffer;
	}
	PackedByteArray upload_bytes;
	upload_bytes.resize(static_cast<int64_t>(bytes));
	std::memcpy(upload_bytes.ptrw(), p_submission.frame.vertices.data(), bytes);
	if (rd->buffer_update(vertex_buffer, 0, required, upload_bytes) != OK) {
		set_status("vertex_upload_failed", "RenderingDevice rejected the overlay vertex upload");
		return false;
	}
	uploaded_frame_id = p_submission.frame_id;
	return true;
}

RID SceneOverlayCompositorEffect::Impl::pipeline_for(int64_t p_framebuffer_format,
		const SceneOverlayBatch &p_batch) {
	const PipelineKey key{p_framebuffer_format, static_cast<std::uint8_t>(p_batch.shading),
			static_cast<std::uint8_t>(p_batch.depth)};
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
	// No overlay draw writes depth. LESSEQUAL on retail's forward depth is
	// GREATER_OR_EQUAL on Godot's reversed depth; ALWAYS drops the test.
	depth->set_enable_depth_write(false);
	depth->set_enable_depth_test(p_batch.depth != SceneOverlayDepth::Always);
	depth->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(true);
	if (p_batch.shading == SceneOverlayShading::DimMultiply) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_DST_COLOR);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ZERO);
		attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ZERO);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	} else if (additive(p_batch.shading)) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ZERO);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	} else if (p_batch.shading == SceneOverlayShading::NvgLaser) {
		// SRCALPHA / ONE (retail's 0x3008 descriptor words 1/2).
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ZERO);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	} else {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	}
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	blend->set_attachments(attachments);
	const RID pipeline = rd->render_pipeline_create(shader, p_framebuffer_format, vertex_format,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample, depth, blend);
	if (!pipeline.is_valid()) {
		set_status("pipeline_create_failed", "RenderingDevice rejected an overlay pipeline");
		return RID();
	}
	pipelines.emplace(key, pipeline);
	return pipeline;
}

RID SceneOverlayCompositorEffect::Impl::uniform_set_for(const RID &p_server_texture,
		bool p_wrap) {
	RenderingServer *server = RenderingServer::get_singleton();
	RID texture = server != nullptr && p_server_texture.is_valid() ?
			server->texture_get_rd_texture(p_server_texture, false) : RID();
	if (!texture.is_valid())
		texture = white_texture;
	// One set per (texture, address mode): the low bit carries the wrap.
	const std::uint64_t key = (texture.get_id() << 1) | (p_wrap ? 1u : 0u);
	const auto found = texture_uniform_sets.find(key);
	if (found != texture_uniform_sets.end()) {
		if (rd->uniform_set_is_valid(found->second))
			return found->second;
		texture_uniform_sets.erase(found);
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, p_wrap ? wrap_sampler : sampler, texture));
	const RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	if (uniform_set.is_valid())
		texture_uniform_sets.emplace(key, uniform_set);
	return uniform_set;
}

void SceneOverlayCompositorEffect::Impl::draw(const SceneOverlaySubmission &p_submission,
		RenderData *p_render_data) {
	Ref<RenderSceneBuffers> generic_buffers = p_render_data->get_render_scene_buffers();
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(generic_buffers.ptr());
	RenderSceneData *scene_data = p_render_data->get_render_scene_data();
	if (buffers == nullptr || scene_data == nullptr) {
		set_status("render_data_unsupported",
				"the overlay stage requires RenderSceneBuffersRD and RenderSceneData");
		return;
	}
	const bool mirror = view_kind.load(std::memory_order_acquire) == VIEW_MIRROR;
	if (mirror) {
		opennova::renderer::compile_scene_overlay(p_submission.frame,
				opennova::renderer::kMirrorOverlayOrder.data(),
				opennova::renderer::kMirrorOverlayOrder.size(), draw_list);
	} else {
		opennova::renderer::compile_scene_overlay(p_submission.frame,
				opennova::renderer::kSceneOverlayOrder.data(),
				opennova::renderer::kSceneOverlayOrder.size(), draw_list);
	}
	if (draw_list.empty()) {
		set_status("idle");
		return;
	}
	const std::uint32_t view_count = buffers->get_view_count();
	if (!ensure_targets(buffers, view_count, buffers->get_internal_size()) ||
			!upload(p_submission))
		return;
	const Transform3D camera = scene_data->get_cam_transform();
	const Vector3 right = camera.basis.get_column(0).normalized();
	const Vector3 up = camera.basis.get_column(1).normalized();
	const float eye_height = static_cast<float>(camera.origin.y);
	std::size_t drawn = 0;
	std::size_t gated = 0;
	std::vector<int> drawn_slots;
	for (std::uint32_t view = 0; view < view_count; ++view) {
		ViewTarget &target = targets[view];
		const int64_t format = rd->framebuffer_get_format(target.framebuffer);
		// RenderSceneData::get_view_projection already carries Godot's depth
		// correction and jitter; only the camera inverse remains.
		const Projection view_projection = scene_data->get_view_projection(view) *
				Projection(camera.affine_inverse());
		for (std::uint32_t column = 0; column < 4; ++column) {
			for (std::uint32_t row = 0; row < 4; ++row) {
				write_f32(push_constants, (column * 4u + row) * 4u,
						static_cast<float>(view_projection[column][row]));
			}
		}
		write_f32(push_constants, 64, static_cast<float>(right.x));
		write_f32(push_constants, 68, static_cast<float>(right.y));
		write_f32(push_constants, 72, static_cast<float>(right.z));
		write_f32(push_constants, 76, 0.0f);
		write_f32(push_constants, 80, static_cast<float>(up.x));
		write_f32(push_constants, 84, static_cast<float>(up.y));
		write_f32(push_constants, 88, static_cast<float>(up.z));
		write_f32(push_constants, 92, 0.0f);
		const int64_t list = rd->draw_list_begin(target.framebuffer);
		if (list == RenderingDevice::INVALID_ID) {
			set_status("draw_list_failed", "RenderingDevice could not begin the overlay draw list");
			return;
		}
		for (const SceneOverlayBatch &batch : draw_list) {
			if (!opennova::renderer::scene_overlay_view_draws(batch, eye_height)) {
				++gated;
				continue;
			}
			const RID pipeline = pipeline_for(format, batch);
			const RID texture = batch.texture < p_submission.textures.size() ?
					p_submission.textures[batch.texture] : RID();
			const RID uniform_set = uniform_set_for(texture,
					batch.shading == SceneOverlayShading::NvgLaser);
			if (!pipeline.is_valid() || !uniform_set.is_valid())
				continue;
			write_u32(push_constants, 96, static_cast<std::uint32_t>(batch.shading));
			write_u32(push_constants, 100, static_cast<std::uint32_t>(batch.geometry));
			write_u32(push_constants, 104,
					batch.depth == SceneOverlayDepth::FarBand ? 1u : 0u);
			write_u32(push_constants, 108, 0u);
			vertex_offsets[0] = static_cast<int64_t>(batch.first_vertex) * kVertexStride;
			rd->draw_list_bind_render_pipeline(list, pipeline);
			rd->draw_list_bind_uniform_set(list, uniform_set, 0);
			rd->draw_list_bind_vertex_buffers_format(list, vertex_format, batch.vertex_count,
					vertex_buffers, vertex_offsets);
			rd->draw_list_set_push_constant(list, push_constants, kPushConstantBytes);
			rd->draw_list_draw(list, false, 1);
			++drawn;
			if (view == 0)
				drawn_slots.push_back(static_cast<int>(batch.slot));
		}
		rd->draw_list_end();
	}
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	diagnostics.status = "drawn";
	diagnostics.failure.clear();
	diagnostics.drawn_frame_id = p_submission.frame_id;
	diagnostics.drawn_batches = drawn;
	diagnostics.gated_batches = gated;
	diagnostics.eye_height = eye_height;
	diagnostics.drawn_slots = drawn_slots;
}

SceneOverlayCompositorEffect::SceneOverlayCompositorEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	set_enabled(true);
}

SceneOverlayCompositorEffect::~SceneOverlayCompositorEffect() = default;

void SceneOverlayCompositorEffect::_bind_methods() {}

void SceneOverlayCompositorEffect::set_view_kind(ViewKind p_kind) {
	impl_->view_kind.store(p_kind, std::memory_order_release);
}

void SceneOverlayCompositorEffect::publish(
		const std::shared_ptr<const SceneOverlaySubmission> &p_submission) {
	if (impl_->shutdown_requested.load(std::memory_order_acquire))
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->submission_mutex);
		impl_->latest = p_submission;
	}
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	impl_->diagnostics.submitted_frame_id = p_submission ? p_submission->frame_id : 0;
	impl_->diagnostics.submitted_batches = p_submission ? p_submission->frame.batches.size() : 0;
	impl_->diagnostics.submitted_slots.clear();
	if (p_submission) {
		for (const SceneOverlayBatch &batch : p_submission->frame.batches)
			impl_->diagnostics.submitted_slots.push_back(static_cast<int>(batch.slot));
	}
}

void SceneOverlayCompositorEffect::clear_submission() {
	std::lock_guard<std::mutex> lock(impl_->submission_mutex);
	impl_->latest.reset();
}

void SceneOverlayCompositorEffect::release_device_resources() {
	set_enabled(false);
	if (impl_->shutdown_requested.exchange(true, std::memory_order_acq_rel))
		return;
	clear_submission();
	impl_->release_all();
	impl_->set_status("shutdown");
}

void SceneOverlayCompositorEffect::write_backend_report(Dictionary &result) const {
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	const Impl::Diagnostics &d = impl_->diagnostics;
	result["backend"] = "rendering_device_compositor";
	result["callback"] = "post_transparent";
	result["view_kind"] = impl_->view_kind.load(std::memory_order_acquire) == VIEW_MIRROR ?
			String("mirror") : String("scene");
	result["status"] = opennova::to_gd(d.status);
	result["failure"] = opennova::to_gd(d.failure);
	result["shutdown"] = impl_->shutdown_requested.load(std::memory_order_acquire);
	result["callback_seen"] = d.callback_seen;
	result["submitted_frame_id"] = static_cast<int64_t>(d.submitted_frame_id);
	result["drawn_frame_id"] = static_cast<int64_t>(d.drawn_frame_id);
	result["submitted_batches"] = static_cast<int64_t>(d.submitted_batches);
	result["drawn_batches"] = static_cast<int64_t>(d.drawn_batches);
	result["gated_batches"] = static_cast<int64_t>(d.gated_batches);
	result["eye_height"] = d.eye_height;
	Array submitted;
	for (int slot : d.submitted_slots)
		submitted.push_back(slot);
	result["submitted_slots"] = submitted;
	Array drawn;
	for (int slot : d.drawn_slots)
		drawn.push_back(slot);
	result["drawn_slots"] = drawn;
}

void SceneOverlayCompositorEffect::_render_callback(int32_t p_effect_callback_type,
		RenderData *p_render_data) {
	if (impl_->shutdown_requested.load(std::memory_order_acquire) || p_render_data == nullptr)
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->diagnostics.callback_seen = true;
	}
	if (p_effect_callback_type != static_cast<int32_t>(get_effect_callback_type()))
		return;
	const std::shared_ptr<const SceneOverlaySubmission> submission = impl_->snapshot();
	if (!submission) {
		impl_->set_status("waiting_for_submission");
		return;
	}
	if (!impl_->initialize_rd())
		return;
	impl_->draw(*submission, p_render_data);
}
