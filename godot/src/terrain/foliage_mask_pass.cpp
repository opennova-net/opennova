#include "terrain/foliage_mask_pass.h"

#include "render/rd_glsl.h"
#include "render/rd_uniforms.h"
#include "render/world_environment_lookup.h"

#include <godot_cpp/classes/camera3d.hpp>
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
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/renderer/foliage_frame.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace godot {

namespace {

constexpr uint32_t kVertexStride = 5u * sizeof(float);
constexpr uint32_t kInstanceBytes = 16u * sizeof(float);
constexpr uint32_t kPushConstantBytes = 80;
constexpr uint32_t kMinimumInstanceCapacity = 256;
// A published target outlives the frames that name it: the render side draws
// frame N while the main thread compiles N + 1.
constexpr uint64_t kTargetReleaseFrameLag = 2;

static_assert(sizeof(opennova::renderer::FoliageModelInstance) == kInstanceBytes,
		"the mask pass uploads the compiler's instance blocks verbatim");
static_assert(sizeof(opennova::renderer::FoliageModelVertex) == kVertexStride,
		"the mask pass uploads the compiler's normalized vertices verbatim");

// GridPlacementVS (literal @ 0x7de3a0) over a per-frame storage buffer of
// its c[12+4i]..c[15+4i] blocks: bilinear corner blend, the biquadratic
// fold, the halved height with its c9.x sway on render x (Godot Z).
const char *kVertexShader = R"GLSL(#version 450
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;

layout(location = 0) out vec2 v_uv;

layout(set = 1, binding = 0, std430) readonly buffer Instances {
	vec4 rows[];
} instances;

layout(push_constant, std430) uniform MaskPush {
	mat4 view_projection;
	uint first_instance;
	float alpha_reference;
	float wind_offset;
	float pad;
} pc;

void main() {
	uint base = (pc.first_instance + uint(gl_InstanceIndex)) * 4u;
	vec4 render_x = instances.rows[base];
	vec4 heights = instances.rows[base + 1u];
	vec4 render_z = instances.rows[base + 2u];
	vec4 fold = instances.rows[base + 3u];
	float x = a_position.x;
	float z = a_position.z;
	vec4 w = vec4((1.0 - x) * (1.0 - z), x * (1.0 - z), (1.0 - x) * z, x * z);
	float u = x * 2.0 - 1.0;
	float v = z * 2.0 - 1.0;
	vec4 f = vec4(1.0 - v * v, (1.0 - v * v) * u, 1.0 - u * u, (1.0 - u * u) * v);
	vec3 world;
	world.x = dot(w, render_z);
	world.y = dot(w, heights) + dot(f, fold) + a_position.y;
	world.z = dot(w, render_x) + a_position.y * pc.wind_offset;
	gl_Position = pc.view_projection * vec4(world, 1.0);
	v_uv = a_uv;
}
)GLSL";

// The MODEL draw's alpha test (D3DCMP_GREATER against the per-entity
// reference) over the :fd texture, sampled like foliage_fd_sampling; the
// surviving fragment's depth is the mask.
const char *kFragmentShader = R"GLSL(#version 450
layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D fd_texture;

layout(push_constant, std430) uniform MaskPush {
	mat4 view_projection;
	uint first_instance;
	float alpha_reference;
	float wind_offset;
	float pad;
} pc;

layout(location = 0) out vec4 frag_depth;

vec4 sample_retail_foliage_fd(vec2 uv) {
	vec2 dimensions = vec2(textureSize(fd_texture, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float terminal_lod = max(
			floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0, 0.0);
	float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));
	return textureGrad(fd_texture, uv, dx * gradient_scale, dy * gradient_scale);
}

void main() {
	if (sample_retail_foliage_fd(v_uv).a <= pc.alpha_reference) {
		discard;
	}
	frag_depth = vec4(gl_FragCoord.z);
}
)GLSL";

RenderingDevice *main_rendering_device() {
	RenderingServer *server = RenderingServer::get_singleton();
	return server != nullptr ? server->get_rendering_device() : nullptr;
}

} // namespace

// --- The render-thread half --------------------------------------------------

class FoliageMaskCompositorEffect::Impl {
public:
	struct SlotGeometry {
		uint64_t generation = 0;
		RID vertex_buffer;
		RID index_buffer;
		RID index_array;
		uint32_t vertex_count = 0;
	};

	mutable std::mutex frame_mutex;
	std::shared_ptr<const FoliageMaskFrame> frame;
	// The weapon Inset pass's own frame, drawn only for the render whose
	// camera is the one it was compiled for (FoliageMaskFrame::view_camera).
	std::shared_ptr<const FoliageMaskFrame> view_frame;

	mutable std::mutex diagnostics_mutex;
	std::string status = "waiting_for_frame";
	std::string failure;
	uint64_t drawn_frame_id = 0;
	int64_t drawn_draws = 0;
	int64_t views = 0;
	bool callback_seen = false;

	RenderingDevice *rd = nullptr;
	RID shader;
	RID sampler;
	RID fallback_texture;
	int64_t vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	std::array<SlotGeometry, opennova::FOLIAGE_MAX_DEFS> slots;
	RID instance_buffer;
	uint32_t instance_capacity = 0;
	RID instance_uniform;
	RID depth;
	Vector2i depth_size;
	RID framebuffer;
	RID framebuffer_target;
	std::map<std::pair<int64_t, int>, RID> pipelines;
	std::vector<RID> transient_uniforms;
	TypedArray<RID> vertex_buffers;
	PackedInt64Array vertex_offsets;
	PackedByteArray push;

	void set_failure(const std::string &p_status, const std::string &p_failure) {
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = p_status;
		failure = p_failure;
	}

	void free_rid(RID &r_rid) {
		if (rd != nullptr && r_rid.is_valid()) {
			rd->free_rid(r_rid);
		}
		r_rid = RID();
	}

	void release_frame_targets() {
		free_rid(framebuffer);
		free_rid(depth);
		depth_size = Vector2i();
		framebuffer_target = RID();
	}

	void release_all() {
		for (RID &uniform : transient_uniforms) {
			if (rd != nullptr && uniform.is_valid() && rd->uniform_set_is_valid(uniform)) {
				rd->free_rid(uniform);
			}
		}
		transient_uniforms.clear();
		for (auto &entry : pipelines) {
			free_rid(entry.second);
		}
		pipelines.clear();
		release_frame_targets();
		if (rd != nullptr && instance_uniform.is_valid() &&
				rd->uniform_set_is_valid(instance_uniform)) {
			rd->free_rid(instance_uniform);
		}
		instance_uniform = RID();
		free_rid(instance_buffer);
		instance_capacity = 0;
		for (SlotGeometry &slot : slots) {
			free_rid(slot.index_array);
			free_rid(slot.index_buffer);
			free_rid(slot.vertex_buffer);
			slot = SlotGeometry{};
		}
		free_rid(fallback_texture);
		free_rid(sampler);
		free_rid(shader);
		vertex_format = RenderingDevice::INVALID_FORMAT_ID;
	}

	bool initialize() {
		if (rd != nullptr && shader.is_valid()) {
			return true;
		}
		release_all();
		rd = main_rendering_device();
		if (rd == nullptr) {
			set_failure("compatibility_renderer_unsupported",
					"RenderingDevice is unavailable");
			return false;
		}
		Ref<RDShaderSPIRV> spirv;
		const std::string compile_errors =
				compile_rd_spirv(rd, kVertexShader, kFragmentShader, spirv);
		if (!compile_errors.empty()) {
			set_failure("shader_compile_failed", compile_errors);
			return false;
		}
		shader = rd->shader_create_from_spirv(spirv, "OpenNova foliage depth masks");
		if (!shader.is_valid()) {
			set_failure("shader_create_failed", "RenderingDevice rejected the mask shader");
			return false;
		}
		// The :fd sampler: linear, linear mips, the anisotropic mode of the
		// highest-quality texfilter setting, wrap (foliage_fd_sampling).
		Ref<RDSamplerState> sampler_state;
		sampler_state.instantiate();
		sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		sampler_state->set_use_anisotropy(true);
		sampler_state->set_anisotropy_max(16.0f);
		sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
		sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
		sampler = rd->sampler_create(sampler_state);
		// A slot without an :fd texture draws the Godot fallback's opaque
		// alpha (foliage_silhouette.gdshader's vec4(0.5, 0.5, 0.5, 1.0)).
		Ref<RDTextureFormat> fallback_format;
		fallback_format.instantiate();
		fallback_format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		fallback_format->set_width(1);
		fallback_format->set_height(1);
		fallback_format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT));
		Ref<RDTextureView> fallback_view;
		fallback_view.instantiate();
		PackedByteArray pixel;
		pixel.resize(4);
		pixel.set(0, 128);
		pixel.set(1, 128);
		pixel.set(2, 128);
		pixel.set(3, 255);
		TypedArray<PackedByteArray> fallback_data;
		fallback_data.push_back(pixel);
		fallback_texture = rd->texture_create(fallback_format, fallback_view, fallback_data);

		TypedArray<Ref<RDVertexAttribute>> attributes;
		Ref<RDVertexAttribute> position;
		position.instantiate();
		position->set_location(0);
		position->set_format(RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT);
		position->set_offset(0);
		position->set_stride(kVertexStride);
		attributes.push_back(position);
		Ref<RDVertexAttribute> uv;
		uv.instantiate();
		uv->set_location(1);
		uv->set_format(RenderingDevice::DATA_FORMAT_R32G32_SFLOAT);
		uv->set_offset(3u * sizeof(float));
		uv->set_stride(kVertexStride);
		attributes.push_back(uv);
		vertex_format = rd->vertex_format_create(attributes);
		vertex_buffers.resize(1);
		vertex_offsets.resize(1);
		push.resize(kPushConstantBytes);
		if (!sampler.is_valid() || !fallback_texture.is_valid() ||
				vertex_format == RenderingDevice::INVALID_FORMAT_ID) {
			set_failure("device_setup_failed", "sampler, fallback or vertex format rejected");
			release_all();
			return false;
		}
		return true;
	}

	bool ensure_slot(int p_slot, const std::shared_ptr<const FoliageMaskMesh> &p_mesh) {
		SlotGeometry &slot = slots[static_cast<size_t>(p_slot)];
		if (!p_mesh || p_mesh->index_count == 0) {
			return false;
		}
		if (slot.generation == p_mesh->generation && slot.index_array.is_valid()) {
			return true;
		}
		free_rid(slot.index_array);
		free_rid(slot.index_buffer);
		free_rid(slot.vertex_buffer);
		slot.vertex_buffer = rd->vertex_buffer_create(
				static_cast<uint32_t>(p_mesh->vertices.size()), p_mesh->vertices);
		slot.index_buffer = rd->index_buffer_create(p_mesh->index_count,
				RenderingDevice::INDEX_BUFFER_FORMAT_UINT32, p_mesh->indices);
		if (!slot.vertex_buffer.is_valid() || !slot.index_buffer.is_valid()) {
			set_failure("mesh_upload_failed", "RenderingDevice rejected a mask mesh");
			return false;
		}
		slot.index_array = rd->index_array_create(slot.index_buffer, 0, p_mesh->index_count);
		slot.vertex_count = p_mesh->vertex_count;
		slot.generation = p_mesh->generation;
		return slot.index_array.is_valid();
	}

	bool upload_instances(const PackedByteArray &p_rows) {
		const uint32_t required = static_cast<uint32_t>(
				std::max<int64_t>(p_rows.size(), kInstanceBytes));
		if (!instance_buffer.is_valid() || required > instance_capacity) {
			if (instance_uniform.is_valid() && rd->uniform_set_is_valid(instance_uniform)) {
				rd->free_rid(instance_uniform);
			}
			instance_uniform = RID();
			free_rid(instance_buffer);
			instance_capacity = std::max(required, kMinimumInstanceCapacity * kInstanceBytes);
			instance_buffer = rd->storage_buffer_create(instance_capacity);
			if (!instance_buffer.is_valid()) {
				instance_capacity = 0;
				set_failure("instance_buffer_failed", "storage buffer rejected");
				return false;
			}
		}
		if (!p_rows.is_empty() &&
				rd->buffer_update(instance_buffer, 0, static_cast<uint32_t>(p_rows.size()),
						p_rows) != OK) {
			set_failure("instance_upload_failed", "storage buffer update rejected");
			return false;
		}
		if (!instance_uniform.is_valid() || !rd->uniform_set_is_valid(instance_uniform)) {
			Ref<RDUniform> uniform;
			uniform.instantiate();
			uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
			uniform->set_binding(0);
			uniform->add_id(instance_buffer);
			TypedArray<Ref<RDUniform>> uniforms;
			uniforms.push_back(uniform);
			instance_uniform = rd->uniform_set_create(uniforms, shader, 1);
		}
		return instance_uniform.is_valid();
	}

	bool ensure_framebuffer(const RID &p_target, const Vector2i &p_size) {
		if (framebuffer.is_valid() && rd->framebuffer_is_valid(framebuffer) &&
				framebuffer_target == p_target && depth_size == p_size) {
			return true;
		}
		release_frame_targets();
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
		format->set_width(p_size.x);
		format->set_height(p_size.y);
		format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
				RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
		Ref<RDTextureView> view;
		view.instantiate();
		depth = rd->texture_create(format, view);
		if (!depth.is_valid()) {
			set_failure("depth_target_failed", "D32 mask depth rejected");
			return false;
		}
		TypedArray<RID> attachments;
		attachments.push_back(p_target);
		attachments.push_back(depth);
		framebuffer = rd->framebuffer_create(attachments);
		if (!framebuffer.is_valid()) {
			set_failure("framebuffer_failed", "mask framebuffer rejected");
			release_frame_targets();
			return false;
		}
		framebuffer_target = p_target;
		depth_size = p_size;
		return true;
	}

	// One pipeline per framebuffer format and wave: the far wave writes R,
	// the camera wave G; reverse-Z nearest-wins depth; no culling (the MODEL
	// pass state is CULLNONE).
	RID pipeline_for(int64_t p_format, bool p_far_side) {
		const auto key = std::make_pair(p_format, p_far_side ? 0 : 1);
		const auto found = pipelines.find(key);
		if (found != pipelines.end()) {
			return found->second;
		}
		Ref<RDPipelineRasterizationState> raster;
		raster.instantiate();
		raster->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
		Ref<RDPipelineMultisampleState> multisample;
		multisample.instantiate();
		Ref<RDPipelineDepthStencilState> depth_state;
		depth_state.instantiate();
		depth_state->set_enable_depth_test(true);
		depth_state->set_enable_depth_write(true);
		depth_state->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
		Ref<RDPipelineColorBlendStateAttachment> attachment;
		attachment.instantiate();
		attachment->set_enable_blend(false);
		attachment->set_write_r(p_far_side);
		attachment->set_write_g(!p_far_side);
		attachment->set_write_b(false);
		attachment->set_write_a(false);
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
		attachments.push_back(attachment);
		Ref<RDPipelineColorBlendState> blend;
		blend.instantiate();
		blend->set_attachments(attachments);
		RID pipeline = rd->render_pipeline_create(shader, p_format, vertex_format,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample,
				depth_state, blend);
		if (pipeline.is_valid()) {
			pipelines.emplace(key, pipeline);
		} else {
			set_failure("pipeline_failed", "mask pipeline rejected");
		}
		return pipeline;
	}

	RID texture_uniform(const RID &p_server_texture) {
		RenderingServer *server = RenderingServer::get_singleton();
		RID texture = server != nullptr && p_server_texture.is_valid()
				? server->texture_get_rd_texture(p_server_texture, false)
				: RID();
		if (!texture.is_valid()) {
			texture = fallback_texture;
		}
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(sampled_texture_uniform(0, sampler, texture));
		RID uniform = rd->uniform_set_create(uniforms, shader, 0);
		if (uniform.is_valid()) {
			transient_uniforms.push_back(uniform);
		}
		return uniform;
	}

	void draw(const FoliageMaskFrame &p_frame, RenderData *p_render_data) {
		if (!initialize()) {
			return;
		}
		if (!p_frame.target.is_valid() || !rd->texture_is_valid(p_frame.target) ||
				p_frame.target_size.x <= 0 || p_frame.target_size.y <= 0) {
			set_failure("no_target", std::string());
			return;
		}
		Ref<RenderSceneBuffers> generic = p_render_data->get_render_scene_buffers();
		RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(generic.ptr());
		RenderSceneData *scene = p_render_data->get_render_scene_data();
		if (buffers == nullptr || scene == nullptr) {
			set_failure("render_data_unsupported", "RenderSceneBuffersRD required");
			return;
		}
		for (RID &uniform : transient_uniforms) {
			if (uniform.is_valid() && rd->uniform_set_is_valid(uniform)) {
				rd->free_rid(uniform);
			}
		}
		transient_uniforms.clear();
		// Uploads precede the draw lists (a buffer update inside one is
		// rejected).
		std::array<bool, opennova::FOLIAGE_MAX_DEFS> slot_ready{};
		for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
			slot_ready[static_cast<size_t>(slot)] =
					ensure_slot(slot, p_frame.meshes[static_cast<size_t>(slot)]);
		}
		if (!upload_instances(p_frame.instance_rows) ||
				!ensure_framebuffer(p_frame.target, p_frame.target_size)) {
			return;
		}
		std::array<RID, opennova::FOLIAGE_MAX_DEFS> textures;
		for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
			textures[static_cast<size_t>(slot)] =
					texture_uniform(p_frame.fd_textures[static_cast<size_t>(slot)]);
		}
		const int64_t format = rd->framebuffer_get_format(framebuffer);
		const RID far_pipeline = pipeline_for(format, true);
		const RID camera_pipeline = pipeline_for(format, false);
		if (!far_pipeline.is_valid() || !camera_pipeline.is_valid()) {
			return;
		}
		const Vector2i internal = buffers->get_internal_size();
		const Rect2 region(0, 0,
				static_cast<float>(std::min(internal.x, p_frame.target_size.x)),
				static_cast<float>(std::min(internal.y, p_frame.target_size.y)));
		// The masks are per view; every view draws both waves into the same
		// target region its own opaque pass then samples.
		const Projection view_projection = scene->get_view_projection(0) *
				Projection(scene->get_cam_transform().affine_inverse());
		float *push_floats = reinterpret_cast<float *>(push.ptrw());
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				push_floats[column * 4 + row] =
						static_cast<float>(view_projection[column][row]);
			}
		}
		PackedColorArray clear;
		clear.push_back(Color(0, 0, 0, 0));
		int64_t drawn = 0;
		for (int wave = 0; wave < 2; ++wave) {
			const bool far_side = wave == 0;
			// The far wave clears the target and depth; the camera wave keeps
			// the far channel and clears only its own depth.
			const int64_t list = rd->draw_list_begin(framebuffer,
					far_side ? RenderingDevice::DRAW_CLEAR_ALL :
							RenderingDevice::DRAW_CLEAR_DEPTH,
					clear, 0.0f, 0, region);
			if (list == RenderingDevice::INVALID_ID) {
				set_failure("draw_list_failed", "mask draw list rejected");
				return;
			}
			rd->draw_list_bind_render_pipeline(list,
					far_side ? far_pipeline : camera_pipeline);
			rd->draw_list_bind_uniform_set(list, instance_uniform, 1);
			for (const FoliageMaskDraw &draw : p_frame.draws) {
				if (draw.far_side != far_side || draw.slot < 0 ||
						draw.slot >= opennova::FOLIAGE_MAX_DEFS ||
						!slot_ready[static_cast<size_t>(draw.slot)] ||
						!textures[static_cast<size_t>(draw.slot)].is_valid() ||
						draw.instance_count == 0) {
					continue;
				}
				const SlotGeometry &slot = slots[static_cast<size_t>(draw.slot)];
				rd->draw_list_bind_uniform_set(list,
						textures[static_cast<size_t>(draw.slot)], 0);
				vertex_buffers[0] = slot.vertex_buffer;
				vertex_offsets[0] = 0;
				rd->draw_list_bind_vertex_buffers_format(list, vertex_format,
						slot.vertex_count, vertex_buffers, vertex_offsets);
				rd->draw_list_bind_index_array(list, slot.index_array);
				write_u32(push, 64, draw.first_instance);
				write_f32(push, 68, draw.alpha_reference);
				write_f32(push, 72, draw.wind_offset);
				write_f32(push, 76, 0.0f);
				rd->draw_list_set_push_constant(list, push, kPushConstantBytes);
				rd->draw_list_draw(list, true, draw.instance_count);
				++drawn;
			}
			rd->draw_list_end();
		}
		std::lock_guard<std::mutex> lock(diagnostics_mutex);
		status = "drawn";
		failure.clear();
		drawn_frame_id = p_frame.frame_id;
		drawn_draws = drawn;
		++views;
	}
};

FoliageMaskCompositorEffect::FoliageMaskCompositorEffect() :
		impl_(std::make_unique<Impl>()) {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	set_enabled(true);
}

FoliageMaskCompositorEffect::~FoliageMaskCompositorEffect() = default;

void FoliageMaskCompositorEffect::_bind_methods() {}

void FoliageMaskCompositorEffect::publish(
		const std::shared_ptr<const FoliageMaskFrame> &p_frame) {
	if (shutdown_requested_.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lock(impl_->frame_mutex);
	impl_->frame = p_frame;
}

void FoliageMaskCompositorEffect::publish_view(
		const std::shared_ptr<const FoliageMaskFrame> &p_frame) {
	if (shutdown_requested_.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lock(impl_->frame_mutex);
	impl_->view_frame = p_frame;
}

void FoliageMaskCompositorEffect::release_device_resources() {
	set_enabled(false);
	if (shutdown_requested_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(impl_->frame_mutex);
		impl_->frame.reset();
		impl_->view_frame.reset();
	}
	impl_->rd = main_rendering_device();
	impl_->release_all();
	impl_->set_failure("shutdown", std::string());
}

void FoliageMaskCompositorEffect::fill_report(FoliageMaskReport &r_report) const {
	std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
	r_report.callback_seen = impl_->callback_seen;
	r_report.status = impl_->status;
	r_report.failure = impl_->failure;
	r_report.drawn_frame_id = impl_->drawn_frame_id;
	r_report.drawn_draws = impl_->drawn_draws;
	r_report.views = impl_->views;
}

void FoliageMaskCompositorEffect::_render_callback(int32_t p_effect_callback_type,
		RenderData *p_render_data) {
	if (shutdown_requested_.load(std::memory_order_acquire) ||
			p_effect_callback_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE ||
			p_render_data == nullptr) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(impl_->diagnostics_mutex);
		impl_->callback_seen = true;
	}
	std::shared_ptr<const FoliageMaskFrame> frame;
	std::shared_ptr<const FoliageMaskFrame> view_frame;
	{
		std::lock_guard<std::mutex> lock(impl_->frame_mutex);
		frame = impl_->frame;
		view_frame = impl_->view_frame;
	}
	// Each scene pass draws its own view's masks: the weapon Inset's render
	// (its camera the one the Inset frame was compiled for) takes the Inset
	// frame, every other view the main one.
	if (view_frame) {
		RenderSceneData *scene = p_render_data->get_render_scene_data();
		if (scene != nullptr &&
				scene->get_cam_transform().is_equal_approx(view_frame->view_camera)) {
			frame = view_frame;
		}
	}
	if (!frame) {
		return;
	}
	impl_->draw(*frame, p_render_data);
}

// --- The main-thread owner ---------------------------------------------------

FoliageMaskPass::FoliageMaskPass() = default;

FoliageMaskPass::~FoliageMaskPass() {
	// Destruction may follow the server teardown; release() is the explicit
	// device boundary the owning node calls on exit.
	effect_.unref();
	installed_into_.unref();
	target_texture_.unref();
}

void FoliageMaskPass::_install(Node *p_scope) {
	if (effect_.is_null()) {
		effect_.instantiate();
	}
	// The scope's viewport WorldEnvironment, as the particle renderer resolves
	// it: its compositor is the one the beauty camera inherits.
	WorldEnvironment *environment = find_world_environment(
			p_scope != nullptr && p_scope->is_inside_tree() ? p_scope->get_viewport()
															: nullptr);
	if (environment == nullptr) {
		return;
	}
	Ref<Compositor> compositor = environment->get_compositor();
	if (compositor.is_null()) {
		compositor.instantiate();
		environment->set_compositor(compositor);
	}
	if (installed_into_ == compositor) {
		return;
	}
	_uninstall();
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	for (int64_t i = 0; i < effects.size(); ++i) {
		Ref<CompositorEffect> existing = effects[i];
		if (existing.is_valid() && existing->get_instance_id() == effect_->get_instance_id()) {
			installed_into_ = compositor;
			return;
		}
	}
	// PRE_OPAQUE effects run in list order; the masks precede the opaque pass
	// like the slot captures.
	effects.push_back(effect_);
	compositor->set_compositor_effects(effects);
	installed_into_ = compositor;
}

void FoliageMaskPass::_uninstall() {
	if (installed_into_.is_valid() && effect_.is_valid()) {
		TypedArray<Ref<CompositorEffect>> effects = installed_into_->get_compositor_effects();
		TypedArray<Ref<CompositorEffect>> kept;
		for (int64_t i = 0; i < effects.size(); ++i) {
			Ref<CompositorEffect> existing = effects[i];
			if (existing.is_valid() && existing->get_instance_id() == effect_->get_instance_id()) {
				continue;
			}
			kept.push_back(existing);
		}
		installed_into_->set_compositor_effects(kept);
	}
	installed_into_.unref();
}

bool FoliageMaskPass::_ensure_target(const Vector2i &p_size) {
	RenderingDevice *rd = main_rendering_device();
	if (rd == nullptr || p_size.x <= 0 || p_size.y <= 0) {
		return false;
	}
	if (target_.is_valid() && target_size_ == p_size && rd->texture_is_valid(target_)) {
		return true;
	}
	if (target_.is_valid()) {
		deferred_frees_.push_back({target_, frame_});
		target_ = RID();
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R32G32_SFLOAT);
	format->set_width(p_size.x);
	format->set_height(p_size.y);
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	target_ = rd->texture_create(format, view);
	if (!target_.is_valid()) {
		target_size_ = Vector2i();
		return false;
	}
	rd->texture_clear(target_, Color(0, 0, 0, 0), 0, 1, 0, 1);
	target_size_ = p_size;
	if (target_texture_.is_null()) {
		target_texture_.instantiate();
	}
	target_texture_->set_texture_rd_rid(target_);
	if (!texture_bound_) {
		if (RenderingServer *server = RenderingServer::get_singleton()) {
			server->global_shader_parameter_set("opennova_foliage_mask_depth",
					target_texture_);
			texture_bound_ = true;
		}
	}
	return true;
}

void FoliageMaskPass::_flush_deferred_frees(bool p_all) {
	RenderingDevice *rd = main_rendering_device();
	std::vector<DeferredFree> kept;
	for (const DeferredFree &entry : deferred_frees_) {
		if (p_all || frame_ >= entry.frame + kTargetReleaseFrameLag) {
			if (rd != nullptr && entry.rid.is_valid() && rd->texture_is_valid(entry.rid)) {
				rd->free_rid(entry.rid);
			}
		} else {
			kept.push_back(entry);
		}
	}
	deferred_frees_ = std::move(kept);
}

void FoliageMaskPass::_set_active(bool p_active) {
	if (active_written_ && active_ == p_active) {
		return;
	}
	if (RenderingServer *server = RenderingServer::get_singleton()) {
		server->global_shader_parameter_set("opennova_foliage_mask_active", p_active);
		active_ = p_active;
		active_written_ = true;
	}
}

void FoliageMaskPass::_set_eye(const Vector3 &p_eye) {
	if (eye_written_ && eye_ == p_eye) {
		return;
	}
	if (RenderingServer *server = RenderingServer::get_singleton()) {
		server->global_shader_parameter_set("opennova_foliage_mask_eye", p_eye);
		eye_ = p_eye;
		eye_written_ = true;
	}
}

// The Inset view's pair: its persons test the Inset's own eye, so each scene
// pass's persons take the masks compiled for that pass.
void FoliageMaskPass::_set_view_active(bool p_active) {
	if (view_active_written_ && view_active_ == p_active) {
		return;
	}
	if (RenderingServer *server = RenderingServer::get_singleton()) {
		server->global_shader_parameter_set("opennova_foliage_mask_inset_active", p_active);
		view_active_ = p_active;
		view_active_written_ = true;
	}
}

void FoliageMaskPass::_set_view_eye(const Vector3 &p_eye) {
	if (view_eye_written_ && view_eye_ == p_eye) {
		return;
	}
	if (RenderingServer *server = RenderingServer::get_singleton()) {
		server->global_shader_parameter_set("opennova_foliage_mask_inset_eye", p_eye);
		view_eye_ = p_eye;
		view_eye_written_ = true;
	}
}

void FoliageMaskPass::publish(Node *p_scope,
		const opennova::renderer::FoliageDrawList &p_draw_list,
		const opennova::renderer::FoliageFrameCompiler &p_compiler,
		const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures) {
	++frame_;
	_flush_deferred_frees(false);
	if (p_scope == nullptr || !p_scope->is_inside_tree() ||
			main_rendering_device() == nullptr) {
		_set_active(false);
		return;
	}
	_install(p_scope);
	Viewport *viewport = p_scope->get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	if (camera == nullptr) {
		_set_active(false);
		return;
	}
	// The rendered eye (camera offsets included) is what the person shaders
	// see as CAMERA_POSITION_WORLD.
	_set_eye(camera->get_camera_transform().origin);
	Vector2i size;
	if (viewport != nullptr) {
		const Vector2 visible = viewport->get_visible_rect().size;
		const float scale = std::max(1.0f, viewport->get_scaling_3d_scale());
		size = Vector2i(static_cast<int32_t>(std::ceil(visible.x * scale)),
				static_cast<int32_t>(std::ceil(visible.y * scale)));
	}
	if (!_ensure_target(size)) {
		_set_active(false);
		return;
	}
	int64_t instances = 0;
	const std::shared_ptr<FoliageMaskFrame> frame =
			_build_frame(p_draw_list, p_compiler, p_fd_textures, instances);
	last_draws_ = static_cast<int64_t>(frame->draws.size());
	last_instances_ = instances;
	main_draws_ = !frame->draws.empty();
	effect_->publish(frame);
	_set_active(main_draws_);
}

void FoliageMaskPass::publish_view(Camera3D *p_camera,
		const opennova::renderer::FoliageDrawList &p_draw_list,
		const opennova::renderer::FoliageFrameCompiler &p_compiler,
		const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures) {
	// The Inset view rides the main pass's installed effect and target: the
	// two scene passes render one after the other, each rasterizing its own
	// masks into the target before its own opaque pass samples them.
	if (p_camera == nullptr || effect_.is_null() || !target_.is_valid()) {
		clear_view();
		return;
	}
	int64_t instances = 0;
	const std::shared_ptr<FoliageMaskFrame> frame =
			_build_frame(p_draw_list, p_compiler, p_fd_textures, instances);
	frame->view_camera = p_camera->get_camera_transform();
	// The Inset render's eye (its own composed pose, offsets included) is
	// what its persons see as CAMERA_POSITION_WORLD.
	_set_view_eye(frame->view_camera.origin);
	view_draws_ = !frame->draws.empty();
	effect_->publish_view(frame);
	_set_view_active(view_draws_);
}

void FoliageMaskPass::clear_view() {
	view_draws_ = false;
	if (effect_.is_valid()) {
		effect_->publish_view(nullptr);
	}
	_set_view_active(false);
}

std::shared_ptr<FoliageMaskFrame> FoliageMaskPass::_build_frame(
		const opennova::renderer::FoliageDrawList &p_draw_list,
		const opennova::renderer::FoliageFrameCompiler &p_compiler,
		const std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> &p_fd_textures,
		int64_t &r_instances) {
	if (mesh_generation_ != p_compiler.model_mesh_generation()) {
		mesh_generation_ = p_compiler.model_mesh_generation();
		for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
			const opennova::renderer::FoliageSlotModelMesh &source =
					p_compiler.model_mesh(slot);
			if (!source.valid) {
				meshes_[static_cast<size_t>(slot)].reset();
				continue;
			}
			auto mesh = std::make_shared<FoliageMaskMesh>();
			mesh->generation = mesh_generation_;
			mesh->vertex_count = static_cast<uint32_t>(source.vertices.size());
			mesh->index_count = static_cast<uint32_t>(source.indices.size());
			mesh->vertices.resize(static_cast<int64_t>(source.vertices.size() * kVertexStride));
			std::memcpy(mesh->vertices.ptrw(), source.vertices.data(),
					source.vertices.size() * kVertexStride);
			mesh->indices.resize(static_cast<int64_t>(source.indices.size() * sizeof(uint32_t)));
			std::memcpy(mesh->indices.ptrw(), source.indices.data(),
					source.indices.size() * sizeof(uint32_t));
			meshes_[static_cast<size_t>(slot)] = mesh;
		}
	}
	auto frame = std::make_shared<FoliageMaskFrame>();
	frame->frame_id = p_draw_list.frame_id;
	frame->meshes = meshes_;
	for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
		const Ref<Texture2D> &texture = p_fd_textures[static_cast<size_t>(slot)];
		frame->fd_textures[static_cast<size_t>(slot)] =
				texture.is_valid() ? texture->get_rid() : RID();
	}
	frame->instance_rows.resize(
			static_cast<int64_t>(p_draw_list.model_instances.size() * kInstanceBytes));
	if (!p_draw_list.model_instances.empty()) {
		std::memcpy(frame->instance_rows.ptrw(), p_draw_list.model_instances.data(),
				p_draw_list.model_instances.size() * kInstanceBytes);
	}
	r_instances = 0;
	for (const opennova::renderer::FoliageDrawCommand &command : p_draw_list.commands) {
		if (command.tier != opennova::renderer::FoliageTier::Silhouette) {
			continue;
		}
		FoliageMaskDraw draw;
		draw.slot = command.slot;
		draw.far_side = command.far_side;
		draw.first_instance = command.first_instance;
		draw.instance_count = command.instance_count;
		draw.alpha_reference = command.alpha_reference;
		draw.wind_offset = command.wind_offset;
		r_instances += command.instance_count;
		frame->draws.push_back(draw);
	}
	frame->target = target_;
	frame->target_size = target_size_;
	return frame;
}

void FoliageMaskPass::clear() {
	last_draws_ = 0;
	last_instances_ = 0;
	main_draws_ = false;
	view_draws_ = false;
	if (effect_.is_valid()) {
		effect_->publish(nullptr);
		effect_->publish_view(nullptr);
	}
	_set_active(false);
	_set_view_active(false);
}

void FoliageMaskPass::release() {
	_set_active(false);
	_set_view_active(false);
	_uninstall();
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr) {
		server->force_sync();
	}
	if (effect_.is_valid()) {
		effect_->release_device_resources();
		effect_.unref();
	}
	if (target_texture_.is_valid()) {
		target_texture_->set_texture_rd_rid(RID());
	}
	if (target_.is_valid()) {
		deferred_frees_.push_back({target_, frame_});
		target_ = RID();
	}
	target_size_ = Vector2i();
	_flush_deferred_frees(true);
	meshes_ = {};
	mesh_generation_ = 0;
}

FoliageMaskReport FoliageMaskPass::get_report() const {
	FoliageMaskReport report;
	if (effect_.is_valid()) {
		effect_->fill_report(report);
	}
	report.installed = installed_into_.is_valid();
	report.active = active_;
	report.target_size = target_size_;
	report.draws = last_draws_;
	report.instances = last_instances_;
	report.view_active = view_active_;
	report.view_eye = view_eye_;
	return report;
}

} // namespace godot
