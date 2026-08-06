#include "nova_particle_compositor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace {

constexpr std::uint32_t kRetailVertexStride =
		static_cast<std::uint32_t>(sizeof(renderer::ParticleVertex));
constexpr std::uint32_t kTriangleVerticesPerQuad = 6;
constexpr std::uint32_t kPushConstantBytes = 128;
constexpr std::uint32_t kMinimumVertexCapacity = 4096;
constexpr std::uint32_t kFallbackSceneTextureSide = 1;
constexpr std::uint32_t kRgba8BytesPerPixel = 4;
constexpr std::uint64_t kNoAtlasGeneration =
		std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kPipelineWarmRequestedBit = 1;
constexpr std::uint64_t kPipelineWarmEpochStep = 2;

static_assert(sizeof(renderer::ParticleVertex) == 28,
		"RD upload must retain the retail particle vertex stride");

const char *kVertexShader = R"GLSL(#version 450
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_primary;
layout(location = 2) in vec4 a_secondary;
layout(location = 3) in vec2 a_uv;

layout(location = 0) out vec4 v_primary;
layout(location = 1) out vec4 v_secondary;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_position;

layout(push_constant, std430) uniform ParticlePush {
	mat4 view_projection;
	vec4 camera_position_fog_end;
	vec4 camera_forward_fog_start;
	vec4 fog_color_type;
	vec2 viewport_size;
	float theta;
	uint mode;
} pc;

void main() {
	gl_Position = pc.view_projection * vec4(a_position, 1.0);
	v_primary = a_primary;
	v_secondary = a_secondary;
	v_uv = a_uv;
	v_world_position = a_position;
}
)GLSL";

const char *kFragmentShader = R"GLSL(#version 450
layout(location = 0) in vec4 v_primary;
layout(location = 1) in vec4 v_secondary;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_position;

layout(set = 0, binding = 0) uniform sampler2D atlas_texture;
layout(set = 1, binding = 0) uniform sampler2D scene_texture;

layout(push_constant, std430) uniform ParticlePush {
	mat4 view_projection;
	vec4 camera_position_fog_end;
	vec4 camera_forward_fog_start;
	vec4 fog_color_type;
	vec2 viewport_size;
	float theta;
	uint mode;
} pc;

layout(location = 0) out vec4 frag_color;

float particle_fog_visibility() {
	float fog_start = pc.camera_forward_fog_start.w;
	float fog_end = pc.camera_position_fog_end.w;
	if (fog_start == fog_end || fog_end <= 0.0) {
		return 1.0;
	}
	vec3 camera_position = pc.camera_position_fog_end.xyz;
	int fog_type = int(pc.fog_color_type.w);
	float fog_distance;
	if (fog_type == 0) {
		fog_distance = max(dot(v_world_position - camera_position,
				pc.camera_forward_fog_start.xyz), 0.0);
		return clamp(exp(-fog_distance *
				(4.1588830833596715 / fog_end)), 0.0, 1.0);
	}
	fog_distance = length(v_world_position - camera_position);
	return clamp((fog_end - fog_distance) /
			(fog_end - fog_start), 0.0, 1.0);
}

vec3 particle_fog_target() {
	if (pc.mode == 1u || pc.mode == 2u || pc.mode == 6u) {
		return vec3(0.0);
	}
	if (pc.mode == 4u) {
		return vec3(1.0);
	}
	if (pc.mode == 5u) {
		return vec3(127.0 / 255.0);
	}
	return pc.fog_color_type.xyz;
}

void main() {
	vec4 texel = texture(atlas_texture, v_uv);
	if (pc.mode <= 2u) {
		// Blend/additive/premult share one fixed-function stage program —
		// MODULATE(TEXTURE, DIFFUSE) on color AND alpha [orig: the case 0/1/2
		// channel descs in CParticleTexture_InitTextureAndChannels @ 0x5e8347/
		// @ 0x5e8380]. Only the blend factors differ (SRCALPHA/INVSRCALPHA vs
		// ONE/INVSRCALPHA). Type-1 pages ship alpha cleared to 0
		// [orig: BuildTextureAtlases @ 0x5e9116], so an additive layer adds at
		// full strength and DIFFUSE alpha (the alpha curve) never affects it.
		frag_color = texel * v_primary;
	} else if (pc.mode == 3u || pc.mode == 6u) {
		float dot3 = clamp(dot(texel.rgb * 2.0 - 1.0,
				v_primary.rgb * 2.0 - 1.0), 0.0, 1.0);
		frag_color = vec4(vec3(dot3), texel.a * v_primary.a);
	} else if (pc.mode == 4u || pc.mode == 5u) {
		// Mod2x's factor-of-two comes from DESTCOLOR/SRCCOLOR blending, not
		// from a shader approximation.
		frag_color = texel * v_primary;
	} else {
		vec3 normal = texel.rgb * 2.0 - 1.0;
		float wave_x = sin(pc.theta) * 0.01953125;
		float wave_y = cos(pc.theta) * 0.01953125;
		float wave_z = -sin(pc.theta) * 0.01953125;
		vec2 projective_uv = gl_FragCoord.xy / pc.viewport_size;
		float alpha = v_primary.a;
		vec2 scene_uv;
		scene_uv.x = normal.x * (alpha * wave_x) +
				normal.y * (alpha * wave_y) +
				normal.z * projective_uv.x;
		scene_uv.y = normal.x * (alpha * wave_y) +
				normal.y * (alpha * wave_z) +
				normal.z * projective_uv.y;
		frag_color = vec4(texture(scene_texture, scene_uv).rgb,
				alpha * texel.a);
	}
	// Retail changes the fixed-function fog color per particle material:
	// ordinary Blend/Bump/Distort use scene fog, additive families use black,
	// Mod uses white, and Mod2x uses mid-gray. Alpha is not fogged.
	frag_color.rgb = mix(particle_fog_target(), frag_color.rgb,
			particle_fog_visibility());
}
)GLSL";

const char *kSceneSnapshotVertexShader = R"GLSL(#version 450
void main() {
	const vec2 positions[3] = vec2[3](
			vec2(-1.0, -1.0),
			vec2(3.0, -1.0),
			vec2(-1.0, 3.0));
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)GLSL";

const char *kSceneSnapshotFragmentShader = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(location = 0) out vec4 frag_color;

void main() {
	frag_color = texelFetch(source_color, ivec2(gl_FragCoord.xy), 0);
}
)GLSL";

std::string utf8_string(const String &value) {
	const CharString utf8 = value.utf8();
	return utf8.get_data() != nullptr ? std::string(utf8.get_data()) :
			std::string();
}

std::uint32_t grow_capacity(std::uint32_t required) {
	std::uint32_t capacity = kMinimumVertexCapacity;
	while (capacity < required) {
		if (capacity > std::numeric_limits<std::uint32_t>::max() / 2u)
			return required;
		capacity *= 2u;
	}
	return capacity;
}

Ref<RDUniform> sampled_texture_uniform(int binding, const RID &sampler,
		const RID &texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	uniform->set_binding(binding);
	uniform->add_id(sampler);
	uniform->add_id(texture);
	return uniform;
}

void write_u32(PackedByteArray &bytes, std::uint32_t offset,
		std::uint32_t value) {
	std::memcpy(bytes.ptrw() + offset, &value, sizeof(value));
}

void write_f32(PackedByteArray &bytes, std::uint32_t offset, float value) {
	std::memcpy(bytes.ptrw() + offset, &value, sizeof(value));
}

int64_t godot_token(std::uint64_t value) {
	int64_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

} // namespace

class NovaParticleCompositorEffect::Impl {
public:
	struct Diagnostics {
		std::string status = "waiting_for_submission";
		std::string failure;
		std::uint64_t submitted_frame_id = 0;
		std::uint64_t drawn_frame_id = 0;
		std::size_t submitted_commands = 0;
		std::size_t drawn_commands = 0;
		std::size_t gpu_draw_calls = 0;
		std::size_t scene_color_copies = 0;
		std::size_t distortion_commands_skipped = 0;
		std::size_t view_count = 0;
		std::uint64_t atlas_generation = 0;
		std::size_t atlas_pages = 0;
		std::uint32_t vertex_capacity_bytes = 0;
		std::uint64_t vertex_capacity_growths = 0;
		std::uint64_t pipeline_warm_requests = 0;
		std::uint64_t pipeline_warm_requests_serviced = 0;
		std::size_t warmed_pipeline_modes = 0;
		std::size_t warmed_framebuffer_formats = 0;
		bool scene_snapshot_pipeline_warmed = false;
		bool callback_seen = false;
		bool rd_available = false;
	};

	struct GpuAtlasPage {
		RID texture;
		RID uniform_set;
	};

	struct PipelineKey {
		int64_t framebuffer_format = -1;
		std::uint8_t mode = 0;
		std::uint16_t variant = 0;

		bool operator<(const PipelineKey &other) const {
			return std::tie(framebuffer_format, mode, variant) <
					std::tie(other.framebuffer_format, other.mode, other.variant);
		}
	};

	struct ViewTarget {
		RID color;
		RID depth;
		RID scratch;
		RID framebuffer;
		RID scratch_framebuffer;
		RID source_uniform_set;
		RID scratch_uniform_set;
		Vector2i size;
	};

	mutable std::mutex submission_mutex;
	std::shared_ptr<const NovaParticleWorldSubmission> latest_submission;
	std::atomic<bool> hidden{false};
	// Low bit is the pending request; upper bits form an epoch. Both request
	// and cancel advance the epoch, so a failed callback can rearm only the
	// exact request it claimed without resurrecting a later cancellation or
	// consuming a newer request.
	std::atomic<std::uint64_t> pipeline_warm_state{0};

	mutable std::mutex diagnostics_mutex;
	Diagnostics diagnostics;

	RenderingDevice *rd = nullptr;
	RID shader;
	RID scene_snapshot_shader;
	RID sampler;
	RID fallback_scene_texture;
	RID fallback_scene_uniform_set;
	RID vertex_buffer;
	std::uint32_t vertex_capacity = 0;
	int64_t vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	TypedArray<RID> vertex_buffers;
	PackedInt64Array vertex_offsets;
	PackedByteArray push_constants;
	std::uint64_t gpu_atlas_generation = kNoAtlasGeneration;
	std::vector<GpuAtlasPage> gpu_atlas_pages;
	std::map<PipelineKey, RID> pipelines;
	std::map<int64_t, RID> scene_snapshot_pipelines;
	bool scene_snapshot_shader_initialization_failed = false;
	std::vector<ViewTarget> targets;
	std::uint64_t target_buffers_id = 0;

	~Impl() {
		release_all();
	}

	void set_failure(const std::string &reason,
			const std::string &status = "failed") {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.status = status;
		diagnostics.failure = reason;
	}

	void publish(const std::shared_ptr<const NovaParticleWorldSubmission> &submission) {
		{
			std::lock_guard<std::mutex> lock(submission_mutex);
			latest_submission = submission;
		}
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.submitted_frame_id = submission ? submission->frame_id : 0;
		diagnostics.submitted_commands = submission ? submission->commands.size() : 0;
		if (!submission) {
			diagnostics.status = "waiting_for_submission";
			diagnostics.failure.clear();
		} else if (!submission->valid) {
			diagnostics.status = "packet_invalid";
			diagnostics.failure = submission->validation_error;
		} else if (submission->commands.empty()) {
			diagnostics.status = "idle";
			diagnostics.failure.clear();
		} else {
			diagnostics.status = "submitted";
			diagnostics.failure.clear();
		}
	}

	std::shared_ptr<const NovaParticleWorldSubmission> snapshot() const {
		std::lock_guard<std::mutex> lock(submission_mutex);
		return latest_submission;
	}

	void release_rid(RID &rid) {
		if (rd != nullptr && rid.is_valid())
			rd->free_rid(rid);
		rid = RID();
	}

	void release_uniform_set_rid(RID &rid) {
		if (rd != nullptr && rid.is_valid() &&
				rd->uniform_set_is_valid(rid))
			rd->free_rid(rid);
		rid = RID();
	}

	void release_targets() {
		for (ViewTarget &target : targets) {
			release_uniform_set_rid(target.source_uniform_set);
			release_uniform_set_rid(target.scratch_uniform_set);
			release_rid(target.scratch_framebuffer);
			release_rid(target.framebuffer);
			release_rid(target.scratch);
		}
		targets.clear();
		target_buffers_id = 0;
	}

	void release_atlas() {
		for (GpuAtlasPage &page : gpu_atlas_pages) {
			release_uniform_set_rid(page.uniform_set);
			release_rid(page.texture);
		}
		gpu_atlas_pages.clear();
		gpu_atlas_generation = kNoAtlasGeneration;
	}

	void release_all() {
		RenderingServer *server = RenderingServer::get_singleton();
		rd = server != nullptr ? server->get_rendering_device() : nullptr;
		release_targets();
		release_atlas();
		release_rid(vertex_buffer);
		for (auto &entry : pipelines)
			release_rid(entry.second);
		pipelines.clear();
		for (auto &entry : scene_snapshot_pipelines)
			release_rid(entry.second);
		scene_snapshot_pipelines.clear();
		release_uniform_set_rid(fallback_scene_uniform_set);
		release_rid(fallback_scene_texture);
		release_rid(sampler);
		release_rid(scene_snapshot_shader);
		scene_snapshot_shader_initialization_failed = false;
		release_rid(shader);
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	}

	bool initialize_rd();
	bool ensure_atlas(const std::shared_ptr<const NovaParticleAtlasSnapshot> &atlas);
	bool ensure_vertex_buffer(const PackedByteArray &vertices);
	bool ensure_targets(RenderSceneBuffersRD *buffers, std::uint32_t view_count,
			const Vector2i &size);
	bool ensure_scene_snapshot_shader();
	bool ensure_scene_color_target(ViewTarget &target, std::uint32_t view);
	RID scene_snapshot_pipeline_for(int64_t framebuffer_format);
	bool snapshot_scene_color(ViewTarget &target, std::uint32_t view);
	RID pipeline_for(const renderer::ParticleDrawCommand &command,
			int64_t framebuffer_format);
	bool warm_pipelines(RenderData *render_data);
	bool validate_submission(const NovaParticleWorldSubmission &submission) const;
	bool draw(const NovaParticleWorldSubmission &submission,
			RenderData *render_data);
	Dictionary report() const;
};

bool NovaParticleCompositorEffect::Impl::initialize_rd() {
	if (rd != nullptr && shader.is_valid() && sampler.is_valid() &&
			fallback_scene_texture.is_valid() &&
			fallback_scene_uniform_set.is_valid() &&
			vertex_format != RenderingDevice::INVALID_FORMAT_ID)
		return true;

	// A failed initialization may leave a valid shader or sampler behind. Tear
	// down the whole dependent cache before retrying so no RID is overwritten
	// and atlas/scratch uniform sets can never outlive the shader they bind.
	release_all();

	RenderingServer *server = RenderingServer::get_singleton();
	rd = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		set_failure("RenderingDevice is unavailable; the particle compositor "
				"requires Forward+ or Mobile",
				"compatibility_renderer_unsupported");
		return false;
	}
	if (rd->limit_get(RenderingDevice::LIMIT_MAX_PUSH_CONSTANT_SIZE) <
			kPushConstantBytes) {
		set_failure("RenderingDevice does not support the required 128-byte "
				"particle push constants", "push_constants_unsupported");
		return false;
	}

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		set_failure("RenderingDevice returned no SPIR-V for the particle shader",
				"shader_compile_failed");
		return false;
	}
	const String vertex_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!vertex_error.is_empty() || !fragment_error.is_empty() ||
			spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		set_failure("Particle shader compilation failed: vertex=" +
				utf8_string(vertex_error) + "; fragment=" +
				utf8_string(fragment_error), "shader_compile_failed");
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova particle compositor");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the particle shader",
				"shader_create_failed");
		return false;
	}

	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_repeat_u(
			RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_v(
			RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_w(
			RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler = rd->sampler_create(sampler_state);
	if (!sampler.is_valid()) {
		set_failure("RenderingDevice could not create the particle sampler",
				"sampler_create_failed");
		release_all();
		return false;
	}

	// Every pipeline shares one shader layout, so set 1 must be bound even when
	// the color-only branches never sample scene_texture. Keep that descriptor
	// valid with a tiny shared texture; full-resolution per-view scratch targets
	// are allocated lazily only for packets that actually contain Distort.
	Ref<RDTextureFormat> fallback_format;
	fallback_format.instantiate();
	fallback_format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fallback_format->set_width(kFallbackSceneTextureSide);
	fallback_format->set_height(kFallbackSceneTextureSide);
	fallback_format->set_depth(1);
	fallback_format->set_array_layers(1);
	fallback_format->set_mipmaps(1);
	fallback_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	fallback_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	fallback_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT));
	Ref<RDTextureView> fallback_view;
	fallback_view.instantiate();
	PackedByteArray fallback_pixel;
	fallback_pixel.resize(kRgba8BytesPerPixel);
	std::memset(fallback_pixel.ptrw(), 0, kRgba8BytesPerPixel);
	TypedArray<PackedByteArray> fallback_data;
	fallback_data.push_back(fallback_pixel);
	fallback_scene_texture = rd->texture_create(
			fallback_format, fallback_view, fallback_data);
	if (!fallback_scene_texture.is_valid()) {
		set_failure("RenderingDevice could not create the fallback scene texture",
				"scene_fallback_failed");
		release_all();
		return false;
	}
	TypedArray<Ref<RDUniform>> fallback_uniforms;
	fallback_uniforms.push_back(sampled_texture_uniform(0, sampler,
			fallback_scene_texture));
	fallback_scene_uniform_set = rd->uniform_set_create(
			fallback_uniforms, shader, 1);
	if (!fallback_scene_uniform_set.is_valid()) {
		set_failure("RenderingDevice could not bind the fallback scene texture",
				"scene_fallback_failed");
		release_all();
		return false;
	}

	TypedArray<Ref<RDVertexAttribute>> attributes;
	auto append_attribute = [&](std::uint32_t location,
			RenderingDevice::DataFormat format, std::uint32_t offset) {
		Ref<RDVertexAttribute> attribute;
		attribute.instantiate();
		attribute->set_location(location);
		attribute->set_binding(0);
		attribute->set_format(format);
		attribute->set_offset(offset);
		attribute->set_stride(kRetailVertexStride);
		attribute->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
		attributes.push_back(attribute);
	};
	append_attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0);
	append_attribute(1, RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM, 12);
	append_attribute(2, RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM, 16);
	append_attribute(3, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 20);
	vertex_format = rd->vertex_format_create(attributes);
	if (vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
		set_failure("RenderingDevice rejected the retail 28-byte vertex format",
				"vertex_format_failed");
		release_all();
		return false;
	}

	vertex_buffers.resize(1);
	vertex_offsets.resize(1);
	push_constants.resize(kPushConstantBytes);
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.rd_available = true;
		diagnostics.status = "ready";
		diagnostics.failure.clear();
	}
	return true;
}

bool NovaParticleCompositorEffect::Impl::ensure_scene_snapshot_shader() {
	if (scene_snapshot_shader.is_valid())
		return true;
	if (scene_snapshot_shader_initialization_failed) {
		set_failure("Scene-color snapshot shader is unavailable after an "
				"initialization failure",
				"scene_snapshot_shader_compile_failed");
		return false;
	}

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kSceneSnapshotVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kSceneSnapshotFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		scene_snapshot_shader_initialization_failed = true;
		set_failure("RenderingDevice returned no SPIR-V for the scene-color "
				"snapshot shader", "scene_snapshot_shader_compile_failed");
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
		scene_snapshot_shader_initialization_failed = true;
		set_failure("Scene-color snapshot shader compilation failed: vertex=" +
				utf8_string(vertex_error) + "; fragment=" +
				utf8_string(fragment_error),
				"scene_snapshot_shader_compile_failed");
		return false;
	}
	scene_snapshot_shader = rd->shader_create_from_spirv(spirv,
			"OpenNova particle scene-color snapshot");
	if (!scene_snapshot_shader.is_valid()) {
		scene_snapshot_shader_initialization_failed = true;
		set_failure("RenderingDevice rejected the scene-color snapshot shader",
				"scene_snapshot_shader_create_failed");
		return false;
	}
	return true;
}

bool NovaParticleCompositorEffect::Impl::ensure_atlas(
		const std::shared_ptr<const NovaParticleAtlasSnapshot> &atlas) {
	if (!atlas) {
		set_failure("World particle submission has no atlas snapshot",
				"atlas_missing");
		return false;
	}
	if (gpu_atlas_generation == atlas->generation &&
			gpu_atlas_pages.size() == atlas->pages.size())
		return true;

	release_atlas();
	gpu_atlas_pages.reserve(atlas->pages.size());
	for (std::size_t page_index = 0; page_index < atlas->pages.size();
			++page_index) {
		const NovaParticleAtlasPageSnapshot &source = atlas->pages[page_index];
		const std::uint64_t expected = static_cast<std::uint64_t>(source.side) *
				static_cast<std::uint64_t>(source.side) * 4u;
		if (source.side == 0 || expected >
				static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max()) ||
				static_cast<std::uint64_t>(source.rgba8.size()) != expected) {
			set_failure("Atlas page " + std::to_string(page_index) +
					" has invalid RGBA dimensions", "atlas_upload_failed");
			release_atlas();
			return false;
		}

		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		format->set_width(source.side);
		format->set_height(source.side);
		format->set_depth(1);
		format->set_array_layers(1);
		format->set_mipmaps(1);
		format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
		format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT));
		Ref<RDTextureView> view;
		view.instantiate();
		TypedArray<PackedByteArray> data;
		data.push_back(source.rgba8);

		GpuAtlasPage uploaded;
		uploaded.texture = rd->texture_create(format, view, data);
		if (!uploaded.texture.is_valid()) {
			set_failure("RenderingDevice rejected atlas page " +
					std::to_string(page_index), "atlas_upload_failed");
			release_atlas();
			return false;
		}
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(sampled_texture_uniform(0, sampler,
				uploaded.texture));
		uploaded.uniform_set = rd->uniform_set_create(uniforms, shader, 0);
		if (!uploaded.uniform_set.is_valid()) {
			release_rid(uploaded.texture);
			set_failure("RenderingDevice could not bind atlas page " +
					std::to_string(page_index), "atlas_uniform_failed");
			release_atlas();
			return false;
		}
		gpu_atlas_pages.push_back(uploaded);
	}
	gpu_atlas_generation = atlas->generation;
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.atlas_generation = atlas->generation;
		diagnostics.atlas_pages = atlas->pages.size();
	}
	return true;
}

bool NovaParticleCompositorEffect::Impl::ensure_vertex_buffer(
		const PackedByteArray &vertices) {
	if (vertices.is_empty())
		return true;
	if (vertices.size() > static_cast<int64_t>(
			std::numeric_limits<std::uint32_t>::max())) {
		set_failure("Expanded particle vertices exceed RenderingDevice's "
				"32-bit buffer size", "vertex_upload_failed");
		return false;
	}
	const std::uint32_t required = static_cast<std::uint32_t>(vertices.size());
	if (!vertex_buffer.is_valid() || required > vertex_capacity) {
		release_rid(vertex_buffer);
		vertex_capacity = grow_capacity(required);
		vertex_buffer = rd->vertex_buffer_create(vertex_capacity);
		if (!vertex_buffer.is_valid()) {
			vertex_capacity = 0;
			set_failure("RenderingDevice could not allocate the retained particle "
					"vertex buffer", "vertex_buffer_failed");
			return false;
		}
		vertex_buffers[0] = vertex_buffer;
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		++diagnostics.vertex_capacity_growths;
		diagnostics.vertex_capacity_bytes = vertex_capacity;
	}
	if (rd->buffer_update(vertex_buffer, 0, required, vertices) != OK) {
		set_failure("RenderingDevice rejected the particle vertex upload",
				"vertex_upload_failed");
		return false;
	}
	return true;
}

bool NovaParticleCompositorEffect::Impl::ensure_targets(
		RenderSceneBuffersRD *buffers, std::uint32_t view_count,
		const Vector2i &size) {
	if (buffers == nullptr || view_count == 0 || size.x <= 0 || size.y <= 0) {
		set_failure("Compositor callback has invalid scene render buffers",
				"render_targets_invalid");
		return false;
	}
	const std::uint64_t buffers_id = buffers->get_instance_id();
	bool matches = target_buffers_id == buffers_id &&
			targets.size() == view_count;
	if (matches) {
		for (std::uint32_t view = 0; view < view_count; ++view) {
			const RID color = buffers->get_color_layer(view);
			const RID depth = buffers->get_depth_layer(view);
			const ViewTarget &target = targets[view];
			if (target.size != size || target.color != color ||
					target.depth != depth || !target.framebuffer.is_valid() ||
					!rd->framebuffer_is_valid(target.framebuffer)) {
				matches = false;
				break;
			}
		}
	}
	if (matches)
		return true;

	release_targets();
	targets.reserve(view_count);
	for (std::uint32_t view = 0; view < view_count; ++view) {
		ViewTarget target;
		target.color = buffers->get_color_layer(view);
		target.depth = buffers->get_depth_layer(view);
		target.size = size;
		if (!target.color.is_valid() || !target.depth.is_valid()) {
			set_failure("Resolved color/depth layer is unavailable for view " +
					std::to_string(view), "render_targets_invalid");
			release_targets();
			return false;
		}
		TypedArray<RID> attachments;
		attachments.push_back(target.color);
		attachments.push_back(target.depth);
		target.framebuffer = rd->framebuffer_create(attachments);
		if (!target.framebuffer.is_valid() ||
				!rd->framebuffer_is_valid(target.framebuffer)) {
			set_failure("Could not create the particle framebuffer for view " +
					std::to_string(view), "framebuffer_failed");
			if (target.framebuffer.is_valid())
				release_rid(target.framebuffer);
			release_targets();
			return false;
		}
		targets.push_back(target);
	}
	target_buffers_id = buffers_id;
	return true;
}

bool NovaParticleCompositorEffect::Impl::ensure_scene_color_target(
		ViewTarget &target, std::uint32_t view) {
	if (!ensure_scene_snapshot_shader())
		return false;
	if (target.scratch.is_valid() &&
			target.scratch_framebuffer.is_valid() &&
			rd->framebuffer_is_valid(target.scratch_framebuffer) &&
			target.source_uniform_set.is_valid() &&
			rd->uniform_set_is_valid(target.source_uniform_set) &&
			target.scratch_uniform_set.is_valid() &&
			rd->uniform_set_is_valid(target.scratch_uniform_set))
		return true;

	auto release_scene_color_target = [&]() {
		release_uniform_set_rid(target.source_uniform_set);
		release_uniform_set_rid(target.scratch_uniform_set);
		release_rid(target.scratch_framebuffer);
		release_rid(target.scratch);
	};
	release_scene_color_target();
	const Ref<RDTextureFormat> color_format =
			rd->texture_get_format(target.color);
	if (color_format.is_null()) {
		set_failure("Resolved color format is unavailable for view " +
				std::to_string(view), "render_targets_invalid");
		return false;
	}
	// Direct ownership avoids sharing a named RenderSceneBuffersRD context
	// across compositor effects and makes teardown a deferred RD free rather
	// than an unsafe main-thread mutation of renderer-owned buffer maps.
	Ref<RDTextureFormat> scratch_format;
	scratch_format.instantiate();
	scratch_format->set_format(color_format->get_format());
	scratch_format->set_width(target.size.x);
	scratch_format->set_height(target.size.y);
	scratch_format->set_depth(1);
	scratch_format->set_array_layers(1);
	scratch_format->set_mipmaps(1);
	scratch_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	scratch_format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	scratch_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT));
	Ref<RDTextureView> scratch_view;
	scratch_view.instantiate();
	TypedArray<PackedByteArray> scratch_data;
	target.scratch = rd->texture_create(
			scratch_format, scratch_view, scratch_data);
	if (!target.scratch.is_valid()) {
		set_failure("Could not allocate scene-color scratch for view " +
				std::to_string(view), "scene_snapshot_target_failed");
		return false;
	}

	TypedArray<RID> scratch_attachments;
	scratch_attachments.push_back(target.scratch);
	target.scratch_framebuffer = rd->framebuffer_create(scratch_attachments);
	if (!target.scratch_framebuffer.is_valid() ||
			!rd->framebuffer_is_valid(target.scratch_framebuffer)) {
		release_scene_color_target();
		set_failure("Could not create scene-color snapshot framebuffer for view " +
				std::to_string(view), "scene_snapshot_target_failed");
		return false;
	}

	TypedArray<Ref<RDUniform>> source_uniforms;
	source_uniforms.push_back(sampled_texture_uniform(0, sampler,
			target.color));
	target.source_uniform_set = rd->uniform_set_create(
			source_uniforms, scene_snapshot_shader, 0);
	if (!target.source_uniform_set.is_valid() ||
			!rd->uniform_set_is_valid(target.source_uniform_set)) {
		release_scene_color_target();
		set_failure("Could not bind resolved scene color for view " +
				std::to_string(view), "scene_snapshot_source_failed");
		return false;
	}

	TypedArray<Ref<RDUniform>> scratch_uniforms;
	scratch_uniforms.push_back(sampled_texture_uniform(0, sampler,
			target.scratch));
	target.scratch_uniform_set = rd->uniform_set_create(
			scratch_uniforms, shader, 1);
	if (!target.scratch_uniform_set.is_valid() ||
			!rd->uniform_set_is_valid(target.scratch_uniform_set)) {
		release_scene_color_target();
		set_failure("Could not bind scene-color scratch for view " +
				std::to_string(view), "scene_snapshot_target_failed");
		return false;
	}
	return true;
}

RID NovaParticleCompositorEffect::Impl::scene_snapshot_pipeline_for(
		int64_t framebuffer_format) {
	const auto found = scene_snapshot_pipelines.find(framebuffer_format);
	if (found != scene_snapshot_pipelines.end())
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
	attachment->set_enable_blend(false);
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	blend->set_attachments(attachments);

	RID pipeline = rd->render_pipeline_create(scene_snapshot_shader,
			framebuffer_format, RenderingDevice::INVALID_FORMAT_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
			raster, multisample, depth, blend);
	if (!pipeline.is_valid() || !rd->render_pipeline_is_valid(pipeline)) {
		if (pipeline.is_valid())
			release_rid(pipeline);
		set_failure("RenderingDevice rejected the scene-color snapshot pipeline",
				"scene_snapshot_pipeline_failed");
		return RID();
	}
	scene_snapshot_pipelines.emplace(framebuffer_format, pipeline);
	return pipeline;
}

bool NovaParticleCompositorEffect::Impl::snapshot_scene_color(
		ViewTarget &target, std::uint32_t view) {
	if (!target.source_uniform_set.is_valid() ||
			!rd->uniform_set_is_valid(target.source_uniform_set) ||
			!target.scratch_uniform_set.is_valid() ||
			!rd->uniform_set_is_valid(target.scratch_uniform_set) ||
			!target.scratch_framebuffer.is_valid() ||
			!rd->framebuffer_is_valid(target.scratch_framebuffer)) {
		set_failure("Scene-color snapshot resources are invalid for view " +
				std::to_string(view), "scene_snapshot_target_failed");
		return false;
	}

	const int64_t framebuffer_format =
			rd->framebuffer_get_format(target.scratch_framebuffer);
	const RID pipeline = scene_snapshot_pipeline_for(framebuffer_format);
	if (!pipeline.is_valid())
		return false;

	const int64_t draw_list = rd->draw_list_begin(
			target.scratch_framebuffer,
			BitField<RenderingDevice::DrawFlags>(
					RenderingDevice::DRAW_IGNORE_COLOR_ALL));
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin the scene-color snapshot "
				"draw list for view " + std::to_string(view),
				"scene_snapshot_draw_list_failed");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd->draw_list_bind_uniform_set(draw_list, target.source_uniform_set, 0);
	rd->draw_list_draw(draw_list, false, 1, 3);
	rd->draw_list_end();
	return true;
}

RID NovaParticleCompositorEffect::Impl::pipeline_for(
		const renderer::ParticleDrawCommand &command,
		int64_t framebuffer_format) {
	const PipelineKey key{framebuffer_format,
			static_cast<std::uint8_t>(command.pipeline), command.variant};
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
	depth->set_enable_depth_test(true);
	depth->set_enable_depth_write(false);
	depth->set_depth_compare_operator(
			RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);

	RenderingDevice::BlendFactor source_color =
			RenderingDevice::BLEND_FACTOR_SRC_ALPHA;
	RenderingDevice::BlendFactor destination_color =
			RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	RenderingDevice::BlendFactor source_alpha = source_color;
	RenderingDevice::BlendFactor destination_alpha = destination_color;
	switch (command.pipeline) {
		case renderer::ParticlePipeline::Blend:
		case renderer::ParticlePipeline::Bump:
		case renderer::ParticlePipeline::Distort:
			break;
		case renderer::ParticlePipeline::Additive:
		case renderer::ParticlePipeline::Premult:
			// Additive and premult share one witnessed pair — ONE/INVSRCALPHA
			// [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 (case 1/2
			// SRCBLEND=ONE) + @ 0x5e85a9 (LABEL_16 DESTBLEND=INVSRCALPHA)]. The
			// type-1 atlas alpha clear zeroes the fragment alpha, which is what
			// turns this pair into a pure add for additive layers.
			source_color = RenderingDevice::BLEND_FACTOR_ONE;
			source_alpha = RenderingDevice::BLEND_FACTOR_ONE;
			break;
		case renderer::ParticlePipeline::Bumpadd:
			// [orig: @ 0x5e84dc/@ 0x5e84d8 — SRCALPHA/ONE]
			destination_color = RenderingDevice::BLEND_FACTOR_ONE;
			destination_alpha = RenderingDevice::BLEND_FACTOR_ONE;
			break;
		case renderer::ParticlePipeline::Mod:
			source_color = RenderingDevice::BLEND_FACTOR_DST_COLOR;
			destination_color = RenderingDevice::BLEND_FACTOR_ZERO;
			source_alpha = RenderingDevice::BLEND_FACTOR_DST_ALPHA;
			destination_alpha = RenderingDevice::BLEND_FACTOR_ZERO;
			break;
		case renderer::ParticlePipeline::Mod2x:
			source_color = RenderingDevice::BLEND_FACTOR_DST_COLOR;
			destination_color = RenderingDevice::BLEND_FACTOR_SRC_COLOR;
			source_alpha = RenderingDevice::BLEND_FACTOR_DST_ALPHA;
			destination_alpha = RenderingDevice::BLEND_FACTOR_SRC_ALPHA;
			break;
	}
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(true);
	attachment->set_src_color_blend_factor(source_color);
	attachment->set_dst_color_blend_factor(destination_color);
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	// D3D9's DESTCOLOR/SRCCOLOR factors are component-wise: their alpha
	// component is destination/source alpha. RenderingDevice exposes separate
	// color and alpha slots, and D3D12 rejects color-only factors in the alpha
	// slots, so use the explicit alpha equivalents without changing the
	// witnessed blend equation.
	attachment->set_src_alpha_blend_factor(source_alpha);
	attachment->set_dst_alpha_blend_factor(destination_alpha);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	blend->set_attachments(attachments);

	RID pipeline = rd->render_pipeline_create(shader, framebuffer_format,
			vertex_format, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
			raster, multisample, depth, blend);
	if (!pipeline.is_valid() || !rd->render_pipeline_is_valid(pipeline)) {
		if (pipeline.is_valid())
			release_rid(pipeline);
		set_failure("RenderingDevice rejected particle pipeline " +
				std::to_string(static_cast<int>(command.pipeline)),
				"pipeline_create_failed");
		return RID();
	}
	pipelines.emplace(key, pipeline);
	return pipeline;
}

bool NovaParticleCompositorEffect::Impl::warm_pipelines(
		RenderData *render_data) {
	if (!initialize_rd())
		return false;
	if (render_data == nullptr) {
		set_failure("Compositor pipeline warm received no RenderData",
				"pipeline_warm_render_data_missing");
		return false;
	}
	Ref<RenderSceneBuffers> generic_buffers =
			render_data->get_render_scene_buffers();
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(
			generic_buffers.ptr());
	if (buffers == nullptr) {
		set_failure("Particle pipeline warm requires RenderSceneBuffersRD",
				"pipeline_warm_render_data_unsupported");
		return false;
	}
	const std::uint32_t view_count = buffers->get_view_count();
	const Vector2i size = buffers->get_internal_size();
	if (!ensure_targets(buffers, view_count, size))
		return false;

	std::vector<int64_t> framebuffer_formats;
	for (std::uint32_t view = 0; view < view_count; ++view) {
		ViewTarget &target = targets[view];
		const int64_t framebuffer_format =
				rd->framebuffer_get_format(target.framebuffer);
		if (std::find(framebuffer_formats.begin(), framebuffer_formats.end(),
					framebuffer_format) == framebuffer_formats.end()) {
			framebuffer_formats.push_back(framebuffer_format);
			for (std::uint8_t mode = 0; mode < 8; ++mode) {
				renderer::ParticleDrawCommand command;
				command.pipeline =
						static_cast<renderer::ParticlePipeline>(mode);
				if (!pipeline_for(command, framebuffer_format).is_valid())
					return false;
			}
		}
		// Distort has a second real RD pipeline and retained scratch resources
		// for the resolved-scene copy. Exercise its draw now as well so first
		// live distortion cannot move that one-time behind the loading screen.
		if (!ensure_scene_color_target(target, view) ||
				!snapshot_scene_color(target, view)) {
			return false;
		}
	}

	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	++diagnostics.pipeline_warm_requests_serviced;
	diagnostics.warmed_pipeline_modes = 8;
	diagnostics.warmed_framebuffer_formats = framebuffer_formats.size();
	diagnostics.scene_snapshot_pipeline_warmed = true;
	return true;
}

bool NovaParticleCompositorEffect::Impl::validate_submission(
		const NovaParticleWorldSubmission &submission) const {
	if (!submission.valid) {
		const_cast<Impl *>(this)->set_failure(submission.validation_error,
				"packet_invalid");
		return false;
	}
	if (!submission.atlas) {
		const_cast<Impl *>(this)->set_failure("World packet has no atlas snapshot",
				"atlas_missing");
		return false;
	}
	if (submission.triangle_vertices.size() % kRetailVertexStride != 0) {
		const_cast<Impl *>(this)->set_failure(
				"Expanded World packet does not retain the 28-byte vertex stride",
				"packet_invalid");
		return false;
	}
	const std::uint64_t vertex_count = static_cast<std::uint64_t>(
			submission.triangle_vertices.size() / kRetailVertexStride);
	if (vertex_count % kTriangleVerticesPerQuad != 0) {
		const_cast<Impl *>(this)->set_failure(
				"Expanded World packet is not a whole number of quads",
				"packet_invalid");
		return false;
	}
	const std::uint64_t quad_count = vertex_count / kTriangleVerticesPerQuad;
	for (std::size_t i = 0; i < submission.commands.size(); ++i) {
		const renderer::ParticleDrawCommand &command = submission.commands[i];
		const std::uint8_t mode = static_cast<std::uint8_t>(command.pipeline);
		if (command.domain != renderer::ParticleRenderDomain::World || mode > 7 ||
				command.variant != 0 || command.atlas_page >=
					submission.atlas->pages.size() || command.quad_count == 0) {
			const_cast<Impl *>(this)->set_failure(
					"Unsupported state in World draw command " +
							std::to_string(i), "packet_invalid");
			return false;
		}
		const bool distortion = command.pipeline ==
				renderer::ParticlePipeline::Distort;
		if ((distortion && command.pass !=
					renderer::ParticleRenderPass::Distortion) ||
				(!distortion && command.pass !=
					renderer::ParticleRenderPass::Color)) {
			const_cast<Impl *>(this)->set_failure(
					"Render pass/pipeline mismatch in World draw command " +
							std::to_string(i), "packet_invalid");
			return false;
		}
		const std::uint64_t first = command.first_quad;
		const std::uint64_t count = command.quad_count;
		if (first > quad_count || count > quad_count - first ||
				count * kTriangleVerticesPerQuad >
						std::numeric_limits<std::uint32_t>::max()) {
			const_cast<Impl *>(this)->set_failure(
					"Out-of-range geometry in World draw command " +
							std::to_string(i), "packet_invalid");
			return false;
		}
	}
	return true;
}

bool NovaParticleCompositorEffect::Impl::draw(
		const NovaParticleWorldSubmission &submission, RenderData *render_data) {
	if (!initialize_rd() || !validate_submission(submission) ||
			!ensure_atlas(submission.atlas) ||
			!ensure_vertex_buffer(submission.triangle_vertices))
		return false;
	if (render_data == nullptr) {
		set_failure("Compositor callback received no RenderData",
				"render_data_missing");
		return false;
	}
	Ref<RenderSceneBuffers> generic_buffers =
			render_data->get_render_scene_buffers();
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(
			generic_buffers.ptr());
	RenderSceneData *scene_data = render_data->get_render_scene_data();
	if (buffers == nullptr || scene_data == nullptr) {
		set_failure("Particle compositor requires RenderSceneBuffersRD and "
				"RenderSceneData", "render_data_unsupported");
		return false;
	}
	const std::uint32_t view_count = buffers->get_view_count();
	const Vector2i size = buffers->get_internal_size();
	const bool needs_scene_color = std::any_of(submission.commands.begin(),
			submission.commands.end(),
			[](const renderer::ParticleDrawCommand &command) {
				return command.pipeline == renderer::ParticlePipeline::Distort;
			});
	if (!ensure_targets(buffers, view_count, size))
		return false;

	// Preflight every resource before beginning a pass. A failed command must
	// reject the whole packet instead of drawing a reordered or partial prefix.
	for (const ViewTarget &target : targets) {
		const int64_t format = rd->framebuffer_get_format(target.framebuffer);
		for (const renderer::ParticleDrawCommand &command : submission.commands) {
			if (!pipeline_for(command, format).is_valid())
				return false;
		}
	}

	std::size_t gpu_draw_calls = 0;
	std::size_t drawn_commands = 0;
	std::size_t scene_color_copies = 0;
	std::size_t distortion_commands_skipped = 0;
	for (std::uint32_t view = 0; view < view_count; ++view) {
		ViewTarget &target = targets[view];
		const bool scene_color_available = needs_scene_color &&
				ensure_scene_color_target(target, view) &&
				snapshot_scene_color(target, view);
		if (scene_color_available)
			++scene_color_copies;

		// RenderSceneData::get_view_projection(view) already includes Godot's
		// depth/Y correction and TAA jitter. Applying another depth correction
		// makes this packet disagree with ordinary scene geometry as the camera
		// turns. Only the world-to-view camera inverse remains to be composed.
		const Projection view_projection =
				scene_data->get_view_projection(view) *
				Projection(scene_data->get_cam_transform().affine_inverse());
		for (std::uint32_t column = 0; column < 4; ++column) {
			for (std::uint32_t row = 0; row < 4; ++row) {
				write_f32(push_constants, (column * 4u + row) * 4u,
						static_cast<float>(view_projection[column][row]));
			}
		}
		write_f32(push_constants, 64, submission.camera_position[0]);
		write_f32(push_constants, 68, submission.camera_position[1]);
		write_f32(push_constants, 72, submission.camera_position[2]);
		write_f32(push_constants, 76, submission.fog_end);
		write_f32(push_constants, 80, submission.camera_forward[0]);
		write_f32(push_constants, 84, submission.camera_forward[1]);
		write_f32(push_constants, 88, submission.camera_forward[2]);
		write_f32(push_constants, 92, submission.fog_start);
		write_f32(push_constants, 96, submission.fog_color[0]);
		write_f32(push_constants, 100, submission.fog_color[1]);
		write_f32(push_constants, 104, submission.fog_color[2]);
		write_f32(push_constants, 108,
				static_cast<float>(submission.fog_type));
		write_f32(push_constants, 112, static_cast<float>(size.x));
		write_f32(push_constants, 116, static_cast<float>(size.y));
		const std::uint64_t ticks = Time::get_singleton() != nullptr ?
				Time::get_singleton()->get_ticks_msec() : 0;
		write_f32(push_constants, 120,
				static_cast<float>(static_cast<std::uint32_t>(ticks)) * 0.004f);

		const int64_t draw_list = rd->draw_list_begin(target.framebuffer);
		if (draw_list == RenderingDevice::INVALID_ID) {
			set_failure("RenderingDevice could not begin the particle draw list",
					"draw_list_failed");
			return false;
		}
		const int64_t format = rd->framebuffer_get_format(target.framebuffer);
		const RID scene_uniform_set = scene_color_available ?
				target.scratch_uniform_set : fallback_scene_uniform_set;
		for (const renderer::ParticleDrawCommand &command : submission.commands) {
			if (command.pipeline == renderer::ParticlePipeline::Distort &&
					!scene_color_available) {
				++distortion_commands_skipped;
				continue;
			}
			const std::uint32_t mode = static_cast<std::uint32_t>(command.pipeline);
			write_u32(push_constants, 124, mode);
			const RID pipeline = pipeline_for(command, format);
			const GpuAtlasPage &atlas_page =
					gpu_atlas_pages[command.atlas_page];
			const std::uint64_t byte_offset =
					static_cast<std::uint64_t>(command.first_quad) *
					kTriangleVerticesPerQuad * kRetailVertexStride;
			const std::uint32_t command_vertices = command.quad_count *
					kTriangleVerticesPerQuad;
			vertex_offsets[0] = static_cast<int64_t>(byte_offset);
			rd->draw_list_bind_render_pipeline(draw_list, pipeline);
			rd->draw_list_bind_uniform_set(draw_list,
					atlas_page.uniform_set, 0);
			rd->draw_list_bind_uniform_set(draw_list,
					scene_uniform_set, 1);
			rd->draw_list_bind_vertex_buffers_format(draw_list, vertex_format,
					command_vertices, vertex_buffers, vertex_offsets);
			rd->draw_list_set_push_constant(draw_list, push_constants,
					kPushConstantBytes);
			rd->draw_list_draw(draw_list, false, 1);
			++gpu_draw_calls;
			++drawn_commands;
		}
		rd->draw_list_end();
	}

	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		diagnostics.status = distortion_commands_skipped == 0 ?
				"drawn" : "drawn_with_distortion_skipped";
		if (distortion_commands_skipped == 0)
			diagnostics.failure.clear();
		diagnostics.drawn_frame_id = submission.frame_id;
		diagnostics.drawn_commands = drawn_commands;
		diagnostics.gpu_draw_calls = gpu_draw_calls;
		diagnostics.scene_color_copies = scene_color_copies;
		diagnostics.distortion_commands_skipped =
				distortion_commands_skipped;
		diagnostics.view_count = view_count;
	}
	return true;
}

Dictionary NovaParticleCompositorEffect::Impl::report() const {
	std::lock_guard<std::mutex> lock(diagnostics_mutex);
	RenderingServer *server = RenderingServer::get_singleton();
	const bool renderer_supported = diagnostics.rd_available ||
			(server != nullptr && server->get_rendering_device() != nullptr);
	Dictionary result;
	result["backend"] = "rendering_device_compositor";
	result["callback"] = "post_transparent";
	result["callback_type"] = static_cast<int>(
			CompositorEffect::EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	result["forward_plus_mobile_only"] = true;
	result["depth_test"] = true;
	result["depth_write"] = false;
	result["reverse_z_compare"] = "greater_or_equal";
	result["packet_order_preserved"] = true;
	result["packet_command_cap"] = static_cast<int64_t>(0);
	result["uncapped_commands"] = true;
	result["packet_commands_dropped"] = static_cast<int64_t>(0);
	result["immutable_submission_copy"] = true;
	result["retains_compiler_packet_pointer"] = false;
	result["push_constant_bytes"] = static_cast<int64_t>(kPushConstantBytes);
	result["push_constant_layout"] =
			"mat4@0,vec4@64,vec4@80,vec4@96,vec2@112,float@120,uint@124";
	result["fog_source"] = "immutable_opennova_environment_snapshot";
	result["fog_distance_policy"] = "type0_eye_depth_else_radial";
	result["fog_material_targets"] =
			"scene,black,black,scene,white,gray127,black,scene";
	result["view_projection_source"] = "render_scene_data_corrected";
	result["adds_view_projection_depth_correction"] = false;
	result["scene_color_copy_policy"] = "once_per_distorting_view";
	result["scene_color_snapshot_backend"] = "fullscreen_sampled_blit";
	result["scene_color_source_requirement"] = "sampling_only";
	result["scene_color_requires_copy_from"] = false;
	result["scene_color_scratch_ownership"] =
			"effect_owned_rd_texture";
	result["scene_color_scratch_allocation"] = "lazy_on_first_distort";
	result["scene_color_scratch_retention"] = "until_target_rebuild";
	result["scene_color_fallback_binding"] = "shared_1x1_texture";
	result["scene_color_copy_without_distort"] = 0;
	result["all_eight_blend_modes"] = true;
	result["status"] = renderer_supported ?
			String::utf8(diagnostics.status.c_str()) :
			String("compatibility_renderer_unsupported");
	result["failure"] = renderer_supported ?
			String::utf8(diagnostics.failure.c_str()) :
			String("RenderingDevice is unavailable; use Forward+ or Mobile");
	result["callback_seen"] = diagnostics.callback_seen;
	result["rd_available"] = renderer_supported;
	result["submitted_frame_id"] = godot_token(diagnostics.submitted_frame_id);
	result["drawn_frame_id"] = godot_token(diagnostics.drawn_frame_id);
	result["submitted_commands"] =
			static_cast<int64_t>(diagnostics.submitted_commands);
	result["drawn_commands"] = static_cast<int64_t>(diagnostics.drawn_commands);
	result["gpu_draw_calls"] = static_cast<int64_t>(diagnostics.gpu_draw_calls);
	result["scene_color_copies"] =
			static_cast<int64_t>(diagnostics.scene_color_copies);
	result["distortion_commands_skipped"] =
			static_cast<int64_t>(diagnostics.distortion_commands_skipped);
	result["view_count"] = static_cast<int64_t>(diagnostics.view_count);
	result["atlas_generation"] = godot_token(diagnostics.atlas_generation);
	result["atlas_pages"] = static_cast<int64_t>(diagnostics.atlas_pages);
	result["vertex_capacity_bytes"] =
			static_cast<int64_t>(diagnostics.vertex_capacity_bytes);
	result["vertex_capacity_growths"] =
			godot_token(diagnostics.vertex_capacity_growths);
	result["pipeline_warm_requests"] =
			godot_token(diagnostics.pipeline_warm_requests);
	result["pipeline_warm_requests_serviced"] =
			godot_token(diagnostics.pipeline_warm_requests_serviced);
	result["warmed_pipeline_modes"] =
			static_cast<int64_t>(diagnostics.warmed_pipeline_modes);
	result["warmed_framebuffer_formats"] =
			static_cast<int64_t>(diagnostics.warmed_framebuffer_formats);
	result["scene_snapshot_pipeline_warmed"] =
			diagnostics.scene_snapshot_pipeline_warmed;
	return result;
}

NovaParticleCompositorEffect::NovaParticleCompositorEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	set_enabled(true);
}

NovaParticleCompositorEffect::~NovaParticleCompositorEffect() = default;

void NovaParticleCompositorEffect::_bind_methods() {}

void NovaParticleCompositorEffect::publish(
		const std::shared_ptr<const NovaParticleWorldSubmission> &p_submission) {
	if (impl_)
		impl_->publish(p_submission);
}

void NovaParticleCompositorEffect::clear_submission() {
	if (impl_)
		impl_->publish(nullptr);
}

void NovaParticleCompositorEffect::set_particles_hidden(bool p_hidden) {
	if (!impl_)
		return;
	impl_->hidden.store(p_hidden, std::memory_order_release);
	set_enabled(!p_hidden);
	const std::shared_ptr<const NovaParticleWorldSubmission> submission =
			impl_->snapshot();
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	if (p_hidden) {
		impl_->diagnostics.status = "hidden";
		impl_->diagnostics.failure.clear();
	} else if (!submission) {
		impl_->diagnostics.status = "waiting_for_submission";
		impl_->diagnostics.failure.clear();
	} else if (!submission->valid) {
		impl_->diagnostics.status = "packet_invalid";
		impl_->diagnostics.failure = submission->validation_error;
	} else {
		impl_->diagnostics.status = submission->commands.empty() ?
				"idle" : "submitted";
		impl_->diagnostics.failure.clear();
	}
}

void NovaParticleCompositorEffect::request_pipeline_warm() {
	if (!impl_)
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		++impl_->diagnostics.pipeline_warm_requests;
		impl_->diagnostics.warmed_pipeline_modes = 0;
		impl_->diagnostics.warmed_framebuffer_formats = 0;
		impl_->diagnostics.scene_snapshot_pipeline_warmed = false;
	}
	// Publish the request only after its diagnostics are initialized; the
	// compositor callback may be running concurrently on the render thread.
	std::uint64_t observed =
			impl_->pipeline_warm_state.load(std::memory_order_relaxed);
	for (;;) {
		const std::uint64_t requested =
				((observed + kPipelineWarmEpochStep) |
						kPipelineWarmRequestedBit);
		if (impl_->pipeline_warm_state.compare_exchange_weak(observed, requested,
					std::memory_order_release, std::memory_order_relaxed)) {
			break;
		}
	}
}

void NovaParticleCompositorEffect::cancel_pipeline_warm() {
	if (!impl_)
		return;
	std::uint64_t observed =
			impl_->pipeline_warm_state.load(std::memory_order_relaxed);
	for (;;) {
		const std::uint64_t canceled =
				(observed + kPipelineWarmEpochStep) &
				~kPipelineWarmRequestedBit;
		if (impl_->pipeline_warm_state.compare_exchange_weak(observed, canceled,
					std::memory_order_release, std::memory_order_relaxed)) {
			break;
		}
	}
}

Dictionary NovaParticleCompositorEffect::get_backend_report() const {
	return impl_ ? impl_->report() : Dictionary();
}

void NovaParticleCompositorEffect::_render_callback(
		int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (!impl_)
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->diagnostics.callback_seen = true;
	}
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT) {
		impl_->set_failure("Particle compositor invoked at the wrong callback",
				"callback_mismatch");
		return;
	}
	if (impl_->hidden.load(std::memory_order_acquire))
		return;
	std::uint64_t requested =
			impl_->pipeline_warm_state.load(std::memory_order_acquire);
	while ((requested & kPipelineWarmRequestedBit) != 0) {
		const std::uint64_t claimed =
				requested & ~kPipelineWarmRequestedBit;
		if (!impl_->pipeline_warm_state.compare_exchange_weak(requested, claimed,
					std::memory_order_acq_rel, std::memory_order_acquire)) {
			continue;
		}
		if (!impl_->warm_pipelines(p_render_data)) {
			// A transient target/resource failure may recover on the next
			// frame, but only if request/cancel has not advanced the epoch
			// while this callback was working. This CAS cannot resurrect a
			// cancellation or overwrite a newer request.
			std::uint64_t expected = claimed;
			impl_->pipeline_warm_state.compare_exchange_strong(expected,
					requested, std::memory_order_release,
					std::memory_order_relaxed);
			return;
		}
		break;
	}
	const std::shared_ptr<const NovaParticleWorldSubmission> submission =
			impl_->snapshot();
	if (!submission) {
		impl_->set_failure(std::string(), "waiting_for_submission");
		return;
	}
	if (!submission->valid) {
		impl_->set_failure(submission->validation_error, "packet_invalid");
		return;
	}
	if (submission->commands.empty()) {
		impl_->set_failure(std::string(), "idle");
		return;
	}
	impl_->draw(*submission, p_render_data);
}
