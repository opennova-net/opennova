#include "render/nova_framefx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/environment.hpp>
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
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

// A normal gameplay camera keeps precisely the beauty layers left after the
// player/fly camera removes FP, shadow-only, and the twelve slot-capture bits.
// Q3 omits the otherwise-unused TERRAIN_SHADOW_RECEIVER plumbing bit. That
// gives shaders a collision-free exact-mask signature without admitting any
// hidden viewmodel, caster, or slot-capture geometry into the isolated pass.
constexpr std::uint32_t kBeautyCameraMask = 0x00018401u;
constexpr std::uint32_t kQ3CameraMask = 0x00010401u;
// FrameFX's working size: the 256-square blur targets, and the height of the
// shared-world Q3 view. Retail draws its Q3 flush into a backbuffer-sized
// altbuffer only because D3D9 keeps the beauty depth buffer bound there;
// the sole consumer of that surface is a StretchRect into a power-of-two
// capture that feeds the 256-square kernel (the altbuffer/capture witnesses
// ride D-RORD-10 in docs/render/render-order-re.md). Godot cannot share the
// beauty depth with a second view, so the Q3 view re-rasterizes depth
// occluders; it therefore renders at the kernel's own working height (beauty
// aspect preserved, 8x MSAA standing in for the StretchRect box filter)
// instead of at full resolution.
constexpr std::uint32_t kFrameFxSide = 256u;
constexpr std::uint32_t kPushConstantBytes = 48u;
constexpr float kPi = 3.14159265358979323846f;

enum class FramePass : std::uint32_t {
	Stretch = 0,
	AverageFour = 1,
	WeightedFour = 2,
	FinalAverage = 3,
	GammaDecode = 4,
	Snapshot = 5,
};

enum class BlendMode : std::uint8_t {
	Replace = 0,
	Add = 1,
	SourceAlphaAdd = 2,
};

const char *kFrameVertexShader = R"GLSL(#version 450
void main() {
	const vec2 positions[3] = vec2[3](
			vec2(-1.0, -1.0),
			vec2(3.0, -1.0),
			vec2(-1.0, 3.0));
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)GLSL";

const char *kFrameFragmentShader = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(push_constant, std430) uniform FramePush {
	vec4 target_source_size;
	vec4 base_direction;
	uint mode;
	uint padding0;
	uint padding1;
	uint padding2;
} pc;

layout(location = 0) out vec4 frag_color;

vec3 gamma_to_linear(vec3 gamma_rgb) {
	vec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}

vec2 d3d_quad_uv() {
	// D3D9's pre-transformed quad places pixel centers on integer positions.
	// Subtract the modern half-pixel before applying the witnessed base UV.
	return (gl_FragCoord.xy - vec2(0.5)) / pc.target_source_size.xy
			+ pc.base_direction.xy;
}

vec4 average_cardinal(vec2 uv, vec2 direction) {
	vec2 perpendicular = vec2(direction.y, -direction.x);
	return (texture(source_color, uv + direction)
			+ texture(source_color, uv + perpendicular)
			+ texture(source_color, uv - direction)
			+ texture(source_color, uv - perpendicular)) * 0.25;
}

void main() {
	if (pc.mode == 0u) {
		// IDirect3DDevice9::StretchRect(D3DTEXF_LINEAR): texel centers map
		// directly between the full source and destination rectangles.
		vec2 uv = gl_FragCoord.xy / pc.target_source_size.xy;
		frag_color = texture(source_color, uv);
		return;
	}
	if (pc.mode == 1u) {
		vec4 averaged = average_cardinal(d3d_quad_uv(),
				pc.base_direction.zw);
		float luma = dot(averaged.rgb, vec3(0.20, 0.30, 0.10));
		frag_color = vec4(averaged.rgb, luma * luma);
		return;
	}
	if (pc.mode == 2u) {
		vec2 uv = d3d_quad_uv();
		vec2 step_uv = pc.base_direction.zw;
		frag_color = texture(source_color, uv + step_uv * 0.5) * 0.50
				+ texture(source_color, uv + step_uv * 2.5) * 0.46
				+ texture(source_color, uv + step_uv * 4.5) * 0.35
				+ texture(source_color, uv + step_uv * 6.5) * 0.19;
		frag_color.a = 1.0;
		return;
	}
	if (pc.mode == 3u) {
		frag_color = average_cardinal(d3d_quad_uv(),
				pc.base_direction.zw);
		frag_color.a = 0.5;
		return;
	}
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec4 source = texelFetch(source_color, pixel, 0);
	if (pc.mode == 4u) {
		frag_color = vec4(gamma_to_linear(source.rgb), source.a);
	} else {
		frag_color = source;
	}
}
)GLSL";

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

// The kernel tap directions of the bloom passes (FrameFX_RenderBloomPass @0x582940 -
// docs/render/render-order-re.md).
std::array<float, 2> direction_for_degrees(float degrees, float radius) {
	const float radians = degrees * (kPi / 180.0f);
	// The retail builders store (sin(angle), cos(angle)) in texture space.
	return {std::sin(radians) * radius, std::cos(radians) * radius};
}

WorldEnvironment *find_world_environment(Node *root) {
	if (root == nullptr)
		return nullptr;
	if (WorldEnvironment *environment =
				Object::cast_to<WorldEnvironment>(root))
		return environment;
	for (int i = 0; i < root->get_child_count(); ++i) {
		if (WorldEnvironment *environment =
					find_world_environment(root->get_child(i)))
			return environment;
	}
	return nullptr;
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

		bool operator<(const PipelineKey &other) const {
			return std::tie(framebuffer_format, blend) <
					std::tie(other.framebuffer_format, other.blend);
		}
	};

	struct ViewTarget {
		RID color;
		RID color_framebuffer;
		RID scene_scratch;
		RID scene_scratch_framebuffer;
		RID capture;
		RID capture_framebuffer;
		RID low_a;
		RID low_a_framebuffer;
		RID low_b;
		RID low_b_framebuffer;
		RID color_uniform;
		RID scratch_uniform;
		RID capture_uniform;
		RID low_a_uniform;
		RID low_b_uniform;
		RID q3_uniform;
		RID q3_rd_texture;
		Vector2i q3_size;
		Vector2i size;
		Vector2i capture_size;
	};

	mutable std::mutex source_mutex;
	RID q3_server_texture;
	// The render-thread view of the Q3 source: the server RID it was resolved
	// from and the RenderingDevice texture behind it. Re-resolved only when the
	// source changes or the dependent uniform set is invalidated.
	RID q3_resolved_server_texture;
	RID q3_resolved_rd_texture;
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

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	std::map<PipelineKey, RID> pipelines;
	std::vector<ViewTarget> targets;
	std::uint64_t target_buffers_id = 0;
	PackedByteArray push_constants;
	PackedColorArray clear_black;

	~Impl() { release_all(); }

	void set_failure(const std::string &reason,
			const std::string &next_status = "failed") {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = next_status;
		failure = reason;
	}

	RID source_snapshot() const {
		std::lock_guard<std::mutex> lock(source_mutex);
		return q3_server_texture;
	}

	void set_source(const RID &texture) {
		std::lock_guard<std::mutex> lock(source_mutex);
		q3_server_texture = texture;
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
		release_uniform(target.q3_uniform);
		release_uniform(target.low_b_uniform);
		release_uniform(target.low_a_uniform);
		release_uniform(target.capture_uniform);
		release_uniform(target.scratch_uniform);
		release_uniform(target.color_uniform);
		release_framebuffer(target.low_b_framebuffer);
		release_framebuffer(target.low_a_framebuffer);
		release_framebuffer(target.capture_framebuffer);
		release_framebuffer(target.scene_scratch_framebuffer);
		release_framebuffer(target.color_framebuffer);
		release_texture(target.low_b);
		release_texture(target.low_a);
		release_texture(target.capture);
		release_texture(target.scene_scratch);
		target = ViewTarget();
	}

	void release_targets() {
		for (ViewTarget &target : targets)
			release_target(target);
		targets.clear();
		target_buffers_id = 0;
	}

	void release_all() {
		RenderingServer *server = RenderingServer::get_singleton();
		rd = server != nullptr ? server->get_rendering_device() : nullptr;
		release_targets();
		for (auto &entry : pipelines)
			release_pipeline(entry.second);
		pipelines.clear();
		release_rid(sampler);
		release_rid(shader);
	}

	bool initialize_rd();
	RID make_texture(const Vector2i &size,
			RenderingDevice::DataFormat format);
	RID make_framebuffer(const RID &texture);
	RID make_uniform(const RID &texture);
	bool ensure_targets(RenderSceneBuffersRD *buffers,
			std::uint32_t count, const Vector2i &size);
	bool ensure_q3_uniform(ViewTarget &target, const RID &q3_rd_texture);
	RID pipeline_for(int64_t framebuffer_format, BlendMode blend);
	void set_push(FramePass pass, const Vector2i &target_size,
			const Vector2i &source_size, float base_u, float base_v,
			float direction_u, float direction_v);
	bool draw_one(const RID &framebuffer, const RID &uniform,
			BlendMode blend, FramePass pass, const Vector2i &target_size,
			const Vector2i &source_size, float base_u, float base_v,
			float direction_u, float direction_v, bool clear,
			bool discard_previous);
	bool draw_weighted_pair(const RID &framebuffer, const RID &uniform,
			const Vector2i &target_size, float first_degrees);
	bool render(RenderData *render_data);
	Dictionary report() const;
};

bool FrameFxCompositorEffect::Impl::initialize_rd() {
	if (rd != nullptr && shader.is_valid() && sampler.is_valid())
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
		set_failure("RenderingDevice does not support the 48-byte FrameFX "
				"push constant block", "push_constants_unsupported");
		return false;
	}

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kFrameVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kFrameFragmentShader));
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
				std::string(vertex_error.utf8().get_data()) + "; fragment=" +
				std::string(fragment_error.utf8().get_data()),
				"shader_compile_failed");
		return false;
	}
	shader = rd->shader_create_from_spirv(spirv, "OpenNova FrameFX");
	if (!shader.is_valid()) {
		set_failure("RenderingDevice rejected the FrameFX shader",
				"shader_create_failed");
		return false;
	}

	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler = rd->sampler_create(sampler_state);
	if (!sampler.is_valid()) {
		set_failure("RenderingDevice could not create the FrameFX sampler",
				"sampler_create_failed");
		release_all();
		return false;
	}
	push_constants.resize(kPushConstantBytes);
	if (clear_black.is_empty())
		clear_black.push_back(Color(0, 0, 0, 0));
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		rd_available = true;
		status = "ready";
		failure.clear();
	}
	return true;
}

RID FrameFxCompositorEffect::Impl::make_texture(const Vector2i &size,
		RenderingDevice::DataFormat format) {
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
	texture_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	return rd->texture_create(texture_format, view);
}

RID FrameFxCompositorEffect::Impl::make_framebuffer(const RID &texture) {
	TypedArray<RID> attachments;
	attachments.push_back(texture);
	return rd->framebuffer_create(attachments);
}

RID FrameFxCompositorEffect::Impl::make_uniform(const RID &texture) {
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, sampler, texture));
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
	// The Q3 view is already kernel-sized, so the retail StretchRect into a
	// power-of-two capture reduces to the format conversion into this
	// 256-square RGBA8 surface (the 1/2048-base four-tap downsample then runs
	// against it exactly as witnessed).
	const Vector2i capture_size(kFrameFxSide, kFrameFxSide);
	targets.reserve(count);
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget target;
		target.color = buffers->get_color_layer(view);
		target.size = size;
		target.capture_size = capture_size;
		const Ref<RDTextureFormat> color_format =
				rd->texture_get_format(target.color);
		if (!target.color.is_valid() || color_format.is_null()) {
			set_failure("Resolved scene color is unavailable for FrameFX view " +
					std::to_string(view), "render_targets_invalid");
			release_target(target);
			release_targets();
			return false;
		}
		target.color_framebuffer = make_framebuffer(target.color);
		target.scene_scratch = make_texture(size, color_format->get_format());
		target.scene_scratch_framebuffer = make_framebuffer(target.scene_scratch);
		target.capture = make_texture(capture_size,
				RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		target.capture_framebuffer = make_framebuffer(target.capture);
		target.low_a = make_texture(Vector2i(kFrameFxSide, kFrameFxSide),
				RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		target.low_a_framebuffer = make_framebuffer(target.low_a);
		target.low_b = make_texture(Vector2i(kFrameFxSide, kFrameFxSide),
				RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		target.low_b_framebuffer = make_framebuffer(target.low_b);
		target.color_uniform = make_uniform(target.color);
		target.scratch_uniform = make_uniform(target.scene_scratch);
		target.capture_uniform = make_uniform(target.capture);
		target.low_a_uniform = make_uniform(target.low_a);
		target.low_b_uniform = make_uniform(target.low_b);
		const bool valid = target.color_framebuffer.is_valid() &&
				target.scene_scratch.is_valid() &&
				target.scene_scratch_framebuffer.is_valid() &&
				target.capture.is_valid() && target.capture_framebuffer.is_valid() &&
				target.low_a.is_valid() && target.low_a_framebuffer.is_valid() &&
				target.low_b.is_valid() && target.low_b_framebuffer.is_valid() &&
				target.color_uniform.is_valid() && target.scratch_uniform.is_valid() &&
				target.capture_uniform.is_valid() && target.low_a_uniform.is_valid() &&
				target.low_b_uniform.is_valid();
		if (!valid) {
			set_failure("RenderingDevice could not allocate the FrameFX "
					"target chain for view " + std::to_string(view),
					"render_targets_failed");
			release_target(target);
			release_targets();
			return false;
		}
		targets.push_back(target);
	}
	target_buffers_id = buffers_id;
	return true;
}

bool FrameFxCompositorEffect::Impl::ensure_q3_uniform(
		ViewTarget &target, const RID &q3_rd_texture) {
	if (target.q3_rd_texture == q3_rd_texture && target.q3_uniform.is_valid() &&
			rd->uniform_set_is_valid(target.q3_uniform))
		return true;
	release_uniform(target.q3_uniform);
	target.q3_rd_texture = q3_rd_texture;
	target.q3_size = Vector2i();
	if (!q3_rd_texture.is_valid())
		return false;
	const Ref<RDTextureFormat> q3_format = rd->texture_get_format(q3_rd_texture);
	if (q3_format.is_valid())
		target.q3_size = Vector2i(q3_format->get_width(), q3_format->get_height());
	target.q3_uniform = make_uniform(q3_rd_texture);
	return target.q3_uniform.is_valid() &&
			rd->uniform_set_is_valid(target.q3_uniform);
}

RID FrameFxCompositorEffect::Impl::pipeline_for(
		int64_t framebuffer_format, BlendMode blend) {
	const PipelineKey key{framebuffer_format, blend};
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
	if (blend == BlendMode::Add) {
		attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	} else if (blend == BlendMode::SourceAlphaAdd) {
		attachment->set_src_color_blend_factor(
				RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
		attachment->set_src_alpha_blend_factor(
				RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
		attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
	}
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
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

void FrameFxCompositorEffect::Impl::set_push(FramePass pass,
		const Vector2i &target_size, const Vector2i &source_size,
		float base_u, float base_v, float direction_u, float direction_v) {
	write_f32(push_constants, 0, static_cast<float>(target_size.x));
	write_f32(push_constants, 4, static_cast<float>(target_size.y));
	write_f32(push_constants, 8, static_cast<float>(source_size.x));
	write_f32(push_constants, 12, static_cast<float>(source_size.y));
	write_f32(push_constants, 16, base_u);
	write_f32(push_constants, 20, base_v);
	write_f32(push_constants, 24, direction_u);
	write_f32(push_constants, 28, direction_v);
	write_u32(push_constants, 32, static_cast<std::uint32_t>(pass));
	write_u32(push_constants, 36, 0);
	write_u32(push_constants, 40, 0);
	write_u32(push_constants, 44, 0);
}

bool FrameFxCompositorEffect::Impl::draw_one(
		const RID &framebuffer, const RID &uniform, BlendMode blend,
		FramePass pass, const Vector2i &target_size,
		const Vector2i &source_size, float base_u, float base_v,
		float direction_u, float direction_v, bool clear,
		bool discard_previous) {
	const int64_t format = rd->framebuffer_get_format(framebuffer);
	const RID pipeline = pipeline_for(format, blend);
	if (!pipeline.is_valid() || !uniform.is_valid())
		return false;
	set_push(pass, target_size, source_size, base_u, base_v,
			direction_u, direction_v);
	BitField<RenderingDevice::DrawFlags> flags(0);
	if (clear) {
		flags = RenderingDevice::DRAW_CLEAR_COLOR_0;
	} else if (discard_previous) {
		flags = RenderingDevice::DRAW_IGNORE_COLOR_ALL;
	}
	const int64_t draw_list = rd->draw_list_begin(framebuffer, flags,
			clear ? clear_black : PackedColorArray());
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin a FrameFX draw list",
				"draw_list_failed");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
	rd->draw_list_set_push_constant(draw_list, push_constants,
			kPushConstantBytes);
	rd->draw_list_draw(draw_list, false, 1, 3);
	rd->draw_list_end();
	return true;
}

bool FrameFxCompositorEffect::Impl::draw_weighted_pair(
		const RID &framebuffer, const RID &uniform,
		const Vector2i &target_size, float first_degrees) {
	const int64_t format = rd->framebuffer_get_format(framebuffer);
	const RID pipeline = pipeline_for(format, BlendMode::Add);
	if (!pipeline.is_valid() || !uniform.is_valid())
		return false;
	const int64_t draw_list = rd->draw_list_begin(framebuffer,
			RenderingDevice::DRAW_CLEAR_COLOR_0, clear_black);
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin a weighted FrameFX pass",
				"draw_list_failed");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd->draw_list_bind_uniform_set(draw_list, uniform, 0);
	for (float degrees : {first_degrees, first_degrees + 180.0f}) {
		const auto direction = direction_for_degrees(degrees, 1.0f / 256.0f);
		set_push(FramePass::WeightedFour, target_size, target_size,
				1.0f / 512.0f, 1.0f / 512.0f,
				direction[0], direction[1]);
		rd->draw_list_set_push_constant(draw_list, push_constants,
				kPushConstantBytes);
		rd->draw_list_draw(draw_list, false, 1, 3);
	}
	rd->draw_list_end();
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

	const RID q3_texture = source_snapshot();
	const bool q3_uniform_stale = targets.empty() ||
			!targets.front().q3_uniform.is_valid() ||
			!rd->uniform_set_is_valid(targets.front().q3_uniform);
	if (q3_texture != q3_resolved_server_texture || q3_uniform_stale) {
		RenderingServer *server = RenderingServer::get_singleton();
		q3_resolved_server_texture = q3_texture;
		q3_resolved_rd_texture = server != nullptr && q3_texture.is_valid()
				? server->texture_get_rd_texture(q3_texture, false)
				: RID();
	}
	const RID q3_rd_texture = q3_resolved_rd_texture;
	std::size_t draws = 0;
	bool sampled_q3 = false;
	for (std::uint32_t view = 0; view < count; ++view) {
		ViewTarget &target = targets[view];
		if (ensure_q3_uniform(target, q3_rd_texture)) {
			const Vector2i q3_size = target.q3_size != Vector2i()
					? target.q3_size
					: target.capture_size;
			if (!draw_one(target.capture_framebuffer, target.q3_uniform,
					BlendMode::Replace, FramePass::Stretch,
					target.capture_size, q3_size, 0, 0, 0, 0,
					false, true))
				return false;
			++draws;
			const auto downsample_direction =
					direction_for_degrees(30.0f, 1.0f / 1024.0f);
			if (!draw_one(target.low_a_framebuffer, target.capture_uniform,
					BlendMode::Replace, FramePass::AverageFour,
					Vector2i(kFrameFxSide, kFrameFxSide), target.capture_size,
					1.0f / 2048.0f, 1.0f / 2048.0f,
					downsample_direction[0], downsample_direction[1],
					false, true))
				return false;
			++draws;
			if (!draw_weighted_pair(target.low_b_framebuffer,
					target.low_a_uniform,
					Vector2i(kFrameFxSide, kFrameFxSide), 90.0f))
				return false;
			draws += 2;
			if (!draw_weighted_pair(target.low_a_framebuffer,
					target.low_b_uniform,
					Vector2i(kFrameFxSide, kFrameFxSide), 0.0f))
				return false;
			draws += 2;
			const auto final_direction =
					direction_for_degrees(45.0f, 0.0027621093f);
			if (!draw_one(target.color_framebuffer, target.low_a_uniform,
					BlendMode::SourceAlphaAdd, FramePass::FinalAverage,
					target.size, Vector2i(kFrameFxSide, kFrameFxSide),
					1.0f / 512.0f, 1.0f / 512.0f,
					final_direction[0], final_direction[1], false, false))
				return false;
			++draws;
			sampled_q3 = true;
		}

		// All 3D retail draws have blended as gamma-domain numeric values. Copy
		// once, then apply the display-backend transfer immediately before Godot's
		// sRGB output encoding. Canvas/viewmodel/HUD passes run afterward.
		if (!draw_one(target.scene_scratch_framebuffer, target.color_uniform,
				BlendMode::Replace, FramePass::Snapshot, target.size,
				target.size, 0, 0, 0, 0, false, true))
			return false;
		++draws;
		if (!draw_one(target.color_framebuffer, target.scratch_uniform,
				BlendMode::Replace, FramePass::GammaDecode, target.size,
				target.size, 0, 0, 0, 0, false, true))
			return false;
		++draws;
	}
	{
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = sampled_q3 ? "drawn" : "drawn_without_q3";
		failure.clear();
		++rendered_frames;
		gpu_draw_calls = draws;
		view_count = count;
		last_size = size;
		last_capture_size = targets.empty() ? Vector2i() :
				targets.front().capture_size;
		q3_sampled = sampled_q3;
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
	result["quality_path"] = 3;
	result["q3_isolated_target"] = true;
	result["capture_filter"] = "linear_rgba8_highest_quality";
	result["capture_power_of_two_floor"] = true;
	result["blur_target_size"] = static_cast<int64_t>(kFrameFxSide);
	result["downsample_angle_degrees"] = 30.0;
	result["downsample_base_uv"] = 1.0 / 2048.0;
	result["downsample_radius_uv"] = 1.0 / 1024.0;
	result["weighted_taps"] = "0.50@0.5,0.46@2.5,0.35@4.5,0.19@6.5";
	result["blur_angles_degrees"] = "90,270,0,180";
	result["final_angle_degrees"] = 45.0;
	result["final_radius_uv"] = 0.0027621093;
	result["final_blend"] = "SRCALPHA,ONE";
	result["final_alpha"] = 0.5;
	result["framebuffer_blend_domain"] = "gamma";
	result["terminal_transfer"] = "srgb_inverse_then_display_encode";
	result["callback_seen"] = callback_seen;
	result["rd_available"] = rd_available;
	result["status"] = String::utf8(status.c_str());
	result["failure"] = String::utf8(failure.c_str());
	result["rendered_frames"] = static_cast<int64_t>(rendered_frames);
	result["gpu_draw_calls"] = static_cast<int64_t>(gpu_draw_calls);
	result["view_count"] = static_cast<int64_t>(view_count);
	result["frame_size"] = last_size;
	result["capture_size"] = last_capture_size;
	result["q3_sampled"] = q3_sampled;
	return result;
}

FrameFxCompositorEffect::FrameFxCompositorEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(false);
	set_enabled(true);
}

FrameFxCompositorEffect::~FrameFxCompositorEffect() = default;

void FrameFxCompositorEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&FrameFxCompositorEffect::get_backend_report);
}

void FrameFxCompositorEffect::set_q3_texture_rid(const RID &p_texture) {
	if (impl_)
		impl_->set_source(p_texture);
}

void FrameFxCompositorEffect::clear_q3_texture_rid() {
	if (impl_)
		impl_->set_source(RID());
}

Dictionary FrameFxCompositorEffect::get_backend_report() const {
	return impl_ ? impl_->report() : Dictionary();
}

void FrameFxCompositorEffect::_render_callback(
		int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (!impl_)
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

void FrameFx::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&FrameFx::get_backend_report);
	ClassDB::bind_method(D_METHOD("advance_frame"),
			&FrameFx::advance_frame);
	BIND_CONSTANT(kBeautyCameraMask);
	BIND_CONSTANT(kQ3CameraMask);
}

void FrameFx::build_auxiliary_views() {
	if (q3_viewport_ != nullptr)
		return;
	if (terminal_effect_.is_null())
		terminal_effect_.instantiate();

	q3_viewport_ = memnew(SubViewport);
	q3_viewport_->set_name("Q3View");
	q3_viewport_->set_size(Vector2i(kFrameFxSide, kFrameFxSide));
	q3_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	q3_viewport_->set_clear_mode(SubViewport::CLEAR_MODE_ALWAYS);
	q3_viewport_->set_transparent_background(true);
	q3_viewport_->set_disable_3d(false);
	q3_viewport_->set_use_own_world_3d(false);
	q3_viewport_->set_handle_input_locally(false);
	q3_viewport_->set_positional_shadow_atlas_size(0);
	// Retail's altbuffer inherits the backbuffer multisample mode
	// (FrameFX_CreateAltBufferTexture @0x582120, CreateRenderTarget @0x58217e -
	// docs/render/render-order-re.md D-RORD-10); here the
	// samples also stand in for the StretchRect box filter over the missing
	// full-resolution source (see kFrameFxSide).
	q3_viewport_->set_msaa_3d(Viewport::MSAA_8X);
	q3_viewport_->set_screen_space_aa(Viewport::SCREEN_SPACE_AA_DISABLED);
	q3_viewport_->set_use_taa(false);
	q3_viewport_->set_use_debanding(false);
	// Every retail pass writes gamma-domain numeric values and this view has
	// no terminal decode, so keep Godot's sRGB output encode OFF: an HDR 2D
	// target stores the numbers the Q3 shaders wrote, which is what the
	// FrameFX capture samples (the display transfer runs once, on the beauty
	// target, after the composite).
	q3_viewport_->set_use_hdr_2d(true);
	add_child(q3_viewport_);

	q3_camera_ = memnew(Camera3D);
	q3_camera_->set_name("Q3Camera");
	q3_camera_->set_cull_mask(kQ3CameraMask);
	Ref<Environment> black_environment;
	black_environment.instantiate();
	black_environment->set_background(Environment::BG_COLOR);
	black_environment->set_bg_color(Color(0, 0, 0, 0));
	black_environment->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	black_environment->set_glow_enabled(false);
	q3_camera_->set_environment(black_environment);
	// The shared WorldEnvironment owns the terminal effect. Q3 is its source,
	// never another consumer of it.
	Ref<Compositor> empty_compositor;
	empty_compositor.instantiate();
	q3_camera_->set_compositor(empty_compositor);
	q3_viewport_->add_child(q3_camera_);
	q3_camera_->make_current();
	Ref<ViewportTexture> q3_texture = q3_viewport_->get_texture();
	if (q3_texture.is_valid())
		terminal_effect_->set_q3_texture_rid(q3_texture->get_rid());
}

void FrameFx::install_compositor() {
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
	if (q3_viewport_ == nullptr || q3_camera_ == nullptr ||
			terminal_effect_.is_null())
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const bool active = camera != nullptr && is_visible_in_tree();
	if (!active) {
		restore_synced_camera_mask();
		q3_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
		terminal_effect_->clear_q3_texture_rid();
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

	// Kernel height, beauty aspect: the camera projection copied below then
	// frames exactly the beauty view, and the FrameFX capture squashes it
	// into the 256-square like retail's StretchRect of the altbuffer
	// (FrameFX_CaptureRenderTarget @0x584020 - docs/render/render-order-re.md).
	const Vector2 visible_size = viewport->get_visible_rect().size;
	const float aspect = visible_size.y > 0.0f
			? visible_size.x / visible_size.y
			: 1.0f;
	const Vector2i target_size = aspect >= 1.0f
			? Vector2i(std::max(1, static_cast<int>(std::round(
					  static_cast<float>(kFrameFxSide) * aspect))),
					  static_cast<int>(kFrameFxSide))
			: Vector2i(static_cast<int>(kFrameFxSide),
					  std::max(1, static_cast<int>(std::round(
					  static_cast<float>(kFrameFxSide) / aspect))));
	if (q3_viewport_->get_size() != target_size)
		q3_viewport_->set_size(target_size);
	q3_viewport_->set_world_3d(viewport->get_world_3d());
	q3_camera_->set_global_transform(camera->get_global_transform());
	q3_camera_->set_projection(camera->get_projection());
	q3_camera_->set_fov(camera->get_fov());
	q3_camera_->set_size(camera->get_size());
	q3_camera_->set_frustum_offset(camera->get_frustum_offset());
	q3_camera_->set_near(camera->get_near());
	q3_camera_->set_far(camera->get_far());
	q3_camera_->set_keep_aspect_mode(camera->get_keep_aspect_mode());
	q3_camera_->set_h_offset(camera->get_h_offset());
	q3_camera_->set_v_offset(camera->get_v_offset());
	q3_camera_->set_attributes(camera->get_attributes());
	q3_camera_->set_cull_mask(kQ3CameraMask);
	q3_viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
	Ref<ViewportTexture> q3_texture = q3_viewport_->get_texture();
	if (q3_texture.is_valid())
		terminal_effect_->set_q3_texture_rid(q3_texture->get_rid());
}

void FrameFx::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		build_auxiliary_views();
		install_compositor();
		// One placement-independent sync so a headless/no-pipeline embedder
		// still boots with coherent views; the live per-frame sync is the
		// ordered GameFramePipeline leg, never a process callback.
		advance_frame();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (q3_viewport_ != nullptr)
			q3_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
		if (terminal_effect_.is_valid())
			terminal_effect_->clear_q3_texture_rid();
		restore_synced_camera_mask();
		uninstall_compositor();
	}
}

Dictionary FrameFx::get_backend_report() const {
	Dictionary result = terminal_effect_.is_valid() ?
			terminal_effect_->get_backend_report() :
			Dictionary();
	result["beauty_camera_mask"] = static_cast<int64_t>(kBeautyCameraMask);
	result["q3_camera_mask"] = static_cast<int64_t>(kQ3CameraMask);
	result["q3_viewport_present"] = q3_viewport_ != nullptr;
	result["q3_viewport_size"] = q3_viewport_ != nullptr ?
			q3_viewport_->get_size() : Vector2i();
	result["q3_working_height"] = static_cast<int64_t>(kFrameFxSide);
	result["q3_msaa_3d"] = q3_viewport_ != nullptr ?
			static_cast<int>(q3_viewport_->get_msaa_3d()) :
			static_cast<int>(Viewport::MSAA_DISABLED);
	result["q3_hdr_2d"] = q3_viewport_ != nullptr &&
			q3_viewport_->is_using_hdr_2d();
	result["q3_update_mode"] = q3_viewport_ != nullptr ?
			static_cast<int>(q3_viewport_->get_update_mode()) :
			static_cast<int>(SubViewport::UPDATE_DISABLED);
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
}

void DisplayDecode::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		install();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		uninstall();
	}
}
