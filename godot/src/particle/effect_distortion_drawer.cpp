#include "particle/effect_distortion_drawer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "particle/particle_compositor.h"
#include "render/rd_uniforms.h"

namespace godot {

namespace {

constexpr std::uint32_t kRibbonVertexStride =
		static_cast<std::uint32_t>(sizeof(opennova::renderer::TracerVertex));
constexpr std::uint32_t kRibbonPushBytes = 128;
static_assert(sizeof(opennova::renderer::TracerVertex) == 32 &&
				offsetof(opennova::renderer::TracerVertex, argb) == 12,
		"the ribbon vertex is position, ARGB colour and two coordinate sets");

// The distortion ribbon material, pool+0x300C (retail
// CEffectEmitterPool_CreateShaders @ 0x5DC8F0): one stage, colour =
// SELECTARG2(TEXTURE) = slot 2, alpha = SELECTARG1(DIFFUSE),
// SRCALPHA/INVSRCALPHA. Its coordinates are generated from the camera-space
// position through the projective screen matrix (state 14 = the
// TCI_CAMERASPACEPOSITION + COUNT3|PROJECTED texgen and transform 8,
// retail set_texture_stage_state @ 0x680A2E), i.e. each pixel samples its own
// screen position; the pass fogs toward the scene fog colour.
const char *kRibbonVertexShader = R"GLSL(#version 450
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_world_position;

layout(push_constant, std430) uniform RibbonPush {
	mat4 view_projection;
	vec4 camera_position_fog_end;
	vec4 camera_forward_fog_start;
	vec4 fog_color_type;
	vec2 viewport_size;
	vec2 unused;
} pc;

void main() {
	gl_Position = pc.view_projection * vec4(a_position, 1.0);
	v_color = a_color;
	v_world_position = a_position;
}
)GLSL";

const char *kRibbonFragmentShader = R"GLSL(#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_world_position;

layout(set = 0, binding = 0) uniform sampler2D screen_texture;

layout(push_constant, std430) uniform RibbonPush {
	mat4 view_projection;
	vec4 camera_position_fog_end;
	vec4 camera_forward_fog_start;
	vec4 fog_color_type;
	vec2 viewport_size;
	vec2 unused;
} pc;

layout(location = 0) out vec4 frag_color;

float fog_visibility() {
	float fog_start = pc.camera_forward_fog_start.w;
	float fog_end = pc.camera_position_fog_end.w;
	if (fog_start == fog_end || fog_end <= 0.0) {
		return 1.0;
	}
	vec3 camera_position = pc.camera_position_fog_end.xyz;
	int fog_type = int(pc.fog_color_type.w);
	if (fog_type == 0) {
		float depth = max(dot(v_world_position - camera_position,
				pc.camera_forward_fog_start.xyz), 0.0);
		return clamp(exp(-depth * (4.1588830833596715 / fog_end)), 0.0, 1.0);
	}
	float start = fog_start;
	if (fog_type == 2) {
		start = fog_end * 0.5;
	} else if (fog_type == 3) {
		start = fog_end * 0.25;
	}
	float distance_to_eye = length(v_world_position - camera_position);
	return clamp((fog_end - distance_to_eye) / (fog_end - start), 0.0, 1.0);
}

void main() {
	// The pixel's own screen position in the 256-square target: u = 0.5 ndc.x
	// + 0.5 + half a texel (the FrameFX screen matrix), from a pixel centre
	// on the integer grid.
	vec2 uv = (gl_FragCoord.xy - 0.5) / pc.viewport_size +
			0.5 / vec2(textureSize(screen_texture, 0));
	vec3 rgb = texture(screen_texture, uv).rgb;
	frag_color = vec4(mix(pc.fog_color_type.xyz, rgb, fog_visibility()), v_color.a);
}
)GLSL";

void release(RenderingDevice *rd, RID &rid) {
	if (rd != nullptr && rid.is_valid())
		rd->free_rid(rid);
	rid = RID();
}

} // namespace

EffectDistortionDrawer::EffectDistortionDrawer() {
	push_constants_.resize(kRibbonPushBytes);
}

EffectDistortionDrawer::~EffectDistortionDrawer() = default;

void EffectDistortionDrawer::set_particle_effect(const Ref<ParticleCompositorEffect> &p_effect) {
	std::lock_guard<std::mutex> lock(mutex_);
	particle_effect_ = p_effect;
}

Ref<ParticleCompositorEffect> EffectDistortionDrawer::get_particle_effect() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return particle_effect_;
}

void EffectDistortionDrawer::set_particles_present(bool p_present) {
	particles_present_.store(p_present, std::memory_order_release);
}

void EffectDistortionDrawer::set_ribbon_channels_present(bool p_present) {
	ribbon_channels_present_.store(p_present, std::memory_order_release);
}

void EffectDistortionDrawer::set_pass_fog(const std::array<float, 3> &p_color, float p_start,
		float p_end, std::int32_t p_type) {
	std::lock_guard<std::mutex> lock(mutex_);
	fog_.color = p_color;
	fog_.start = p_start;
	fog_.end = p_end;
	fog_.type = p_type;
}

void EffectDistortionDrawer::publish_ribbons(const opennova::renderer::TracerRibbonFrame &p_frame) {
	std::shared_ptr<RibbonSubmission> submission;
	if (!p_frame.indices.empty()) {
		submission = std::make_shared<RibbonSubmission>();
		const std::size_t vertex_bytes = p_frame.vertices.size() * kRibbonVertexStride;
		submission->vertices.resize(static_cast<int64_t>(vertex_bytes));
		std::memcpy(submission->vertices.ptrw(), p_frame.vertices.data(), vertex_bytes);
		const std::size_t index_bytes = p_frame.indices.size() * sizeof(std::uint32_t);
		submission->indices.resize(static_cast<int64_t>(index_bytes));
		std::memcpy(submission->indices.ptrw(), p_frame.indices.data(), index_bytes);
		submission->index_count = static_cast<std::uint32_t>(p_frame.indices.size());
	}
	std::lock_guard<std::mutex> lock(mutex_);
	ribbons_ = std::move(submission);
}

bool EffectDistortionDrawer::frame_has_distortion() const {
	return particles_present_.load(std::memory_order_acquire) ||
			ribbon_channels_present_.load(std::memory_order_acquire);
}

std::size_t EffectDistortionDrawer::published_ribbon_indices() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return ribbons_ ? ribbons_->index_count : 0;
}

std::size_t EffectDistortionDrawer::drawn_ribbon_draws() const {
	return drawn_ribbon_draws_.load(std::memory_order_acquire);
}

bool EffectDistortionDrawer::draw_distortion(const FrameFxDistortionTarget &p_target,
		opennova::renderer::FrameFxDistortionSet p_set, std::size_t &r_draws) {
	if (p_set == opennova::renderer::FrameFxDistortionSet::Particles) {
		const Ref<ParticleCompositorEffect> effect = get_particle_effect();
		return effect.is_null() || effect->draw_distortion_set(p_target, r_draws);
	}
	return draw_ribbons(p_target, r_draws);
}

bool EffectDistortionDrawer::ensure_ribbon_device(RenderingDevice *p_rd) {
	if (p_rd == nullptr)
		return false;
	if (rd_ != nullptr && rd_ != p_rd)
		release_device_resources();
	rd_ = p_rd;
	if (shader_.is_valid() && vertex_format_ != RenderingDevice::INVALID_FORMAT_ID)
		return true;
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX, String::utf8(kRibbonVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kRibbonFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd_->shader_compile_spirv_from_source(source);
	if (spirv.is_null() ||
			!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty())
		return false;
	shader_ = rd_->shader_create_from_spirv(spirv, "OpenNova tracer distortion ribbons");
	if (!shader_.is_valid())
		return false;
	TypedArray<RDVertexAttribute> attributes;
	Ref<RDVertexAttribute> position;
	position.instantiate();
	position->set_location(0);
	position->set_format(RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT);
	position->set_offset(0);
	position->set_stride(kRibbonVertexStride);
	attributes.push_back(position);
	// The D3D ARGB dword is B, G, R, A in memory.
	Ref<RDVertexAttribute> color;
	color.instantiate();
	color->set_location(1);
	color->set_format(RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM);
	color->set_offset(12);
	color->set_stride(kRibbonVertexStride);
	attributes.push_back(color);
	vertex_format_ = rd_->vertex_format_create(attributes);
	return vertex_format_ != RenderingDevice::INVALID_FORMAT_ID;
}

RID EffectDistortionDrawer::ribbon_pipeline_for(int64_t p_framebuffer_format) {
	const auto found = pipelines_.find(p_framebuffer_format);
	if (found != pipelines_.end())
		return found->second;
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	// Pass flags 0x10520000: cull none, z-write off, z-test on (retail
	// CEffectChannel_RenderRibbon @ 0x5DC86C).
	raster->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test(true);
	depth->set_enable_depth_write(false);
	depth->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(true);
	attachment->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
	attachment->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
	attachment->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	blend->set_attachments(attachments);
	const RID pipeline = rd_->render_pipeline_create(shader_, p_framebuffer_format, vertex_format_,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample, depth, blend);
	if (pipeline.is_valid())
		pipelines_.emplace(p_framebuffer_format, pipeline);
	return pipeline;
}

// The tracer pool's distortion pass with slot 2 = the row's 256B target
// (retail CEffectEmitterPool_RenderDistortionPass @ 0x5DCB40 called
// @ 0x583928): every channel of a +0x828 style, one draw per channel in pool
// order; the channels share one material and blend, so the ordered index
// list draws as one call.
bool EffectDistortionDrawer::draw_ribbons(const FrameFxDistortionTarget &p_target,
		std::size_t &r_draws) {
	std::shared_ptr<const RibbonSubmission> ribbons;
	Fog fog;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ribbons = ribbons_;
		fog = fog_;
	}
	drawn_ribbon_draws_.store(0, std::memory_order_release);
	if (!ribbons || ribbons->index_count == 0)
		return true;
	RenderSceneData *scene_data = p_target.render_data != nullptr ?
			p_target.render_data->get_render_scene_data() : nullptr;
	if (scene_data == nullptr || !ensure_ribbon_device(p_target.rd))
		return false;
	const std::uint32_t vertex_bytes = static_cast<std::uint32_t>(ribbons->vertices.size());
	if (!vertex_buffer_.is_valid() || vertex_bytes > vertex_capacity_) {
		release(rd_, vertex_buffer_);
		vertex_capacity_ = std::max<std::uint32_t>(vertex_bytes, 4096u * kRibbonVertexStride);
		vertex_buffer_ = rd_->vertex_buffer_create(vertex_capacity_);
		if (!vertex_buffer_.is_valid())
			return false;
	}
	rd_->buffer_update(vertex_buffer_, 0, vertex_bytes, ribbons->vertices);
	if (!index_buffer_.is_valid() || ribbons->index_count > index_capacity_) {
		release(rd_, index_array_);
		release(rd_, index_buffer_);
		index_capacity_ = std::max<std::uint32_t>(ribbons->index_count, 16384u);
		PackedByteArray zero;
		zero.resize(static_cast<int64_t>(index_capacity_) * 4);
		std::memset(zero.ptrw(), 0, static_cast<std::size_t>(zero.size()));
		index_buffer_ = rd_->index_buffer_create(index_capacity_,
				RenderingDevice::INDEX_BUFFER_FORMAT_UINT32, zero);
		if (!index_buffer_.is_valid())
			return false;
	}
	rd_->buffer_update(index_buffer_, 0, ribbons->index_count * 4u, ribbons->indices);
	release(rd_, index_array_);
	index_array_ = rd_->index_array_create(index_buffer_, 0, ribbons->index_count);
	if (framebuffer_color_ != p_target.color || framebuffer_depth_ != p_target.depth ||
			!framebuffer_.is_valid() || !rd_->framebuffer_is_valid(framebuffer_)) {
		release(rd_, framebuffer_);
		TypedArray<RID> attachments;
		attachments.push_back(p_target.color);
		attachments.push_back(p_target.depth);
		framebuffer_ = rd_->framebuffer_create(attachments);
		framebuffer_color_ = p_target.color;
		framebuffer_depth_ = p_target.depth;
		if (!framebuffer_.is_valid())
			return false;
	}
	if (uniform_texture_ != p_target.screen_texture || uniform_sampler_ != p_target.screen_sampler ||
			!uniform_set_.is_valid() || !rd_->uniform_set_is_valid(uniform_set_)) {
		if (uniform_set_.is_valid() && rd_->uniform_set_is_valid(uniform_set_))
			rd_->free_rid(uniform_set_);
		uniform_set_ = RID();
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(sampled_texture_uniform(0, p_target.screen_sampler,
				p_target.screen_texture));
		uniform_set_ = rd_->uniform_set_create(uniforms, shader_, 0);
		uniform_texture_ = p_target.screen_texture;
		uniform_sampler_ = p_target.screen_sampler;
		if (!uniform_set_.is_valid())
			return false;
	}
	const RID pipeline = ribbon_pipeline_for(rd_->framebuffer_get_format(framebuffer_));
	if (!pipeline.is_valid())
		return false;
	const Ref<RDTextureFormat> color_format = rd_->texture_get_format(p_target.color);
	const Transform3D eye = scene_data->get_cam_transform();
	const Projection view_projection = scene_data->get_view_projection(p_target.view) *
			Projection(eye.affine_inverse());
	for (std::uint32_t column = 0; column < 4; ++column)
		for (std::uint32_t row = 0; row < 4; ++row)
			write_f32(push_constants_, (column * 4u + row) * 4u,
					static_cast<float>(view_projection[column][row]));
	const Vector3 forward = -eye.basis.get_column(2);
	write_f32(push_constants_, 64, static_cast<float>(eye.origin.x));
	write_f32(push_constants_, 68, static_cast<float>(eye.origin.y));
	write_f32(push_constants_, 72, static_cast<float>(eye.origin.z));
	write_f32(push_constants_, 76, fog.end);
	write_f32(push_constants_, 80, static_cast<float>(forward.x));
	write_f32(push_constants_, 84, static_cast<float>(forward.y));
	write_f32(push_constants_, 88, static_cast<float>(forward.z));
	write_f32(push_constants_, 92, fog.start);
	write_f32(push_constants_, 96, fog.color[0]);
	write_f32(push_constants_, 100, fog.color[1]);
	write_f32(push_constants_, 104, fog.color[2]);
	write_f32(push_constants_, 108, static_cast<float>(fog.type));
	write_f32(push_constants_, 112,
			color_format.is_valid() ? static_cast<float>(color_format->get_width()) : 1.0f);
	write_f32(push_constants_, 116,
			color_format.is_valid() ? static_cast<float>(color_format->get_height()) : 1.0f);
	write_f32(push_constants_, 120, 0.0f);
	write_f32(push_constants_, 124, 0.0f);
	const int64_t draw_list = rd_->draw_list_begin(framebuffer_);
	if (draw_list == RenderingDevice::INVALID_ID)
		return false;
	TypedArray<RID> buffers;
	buffers.push_back(vertex_buffer_);
	rd_->draw_list_bind_render_pipeline(draw_list, pipeline);
	rd_->draw_list_bind_uniform_set(draw_list, uniform_set_, 0);
	rd_->draw_list_bind_vertex_buffers_format(draw_list, vertex_format_,
			vertex_bytes / kRibbonVertexStride, buffers);
	rd_->draw_list_bind_index_array(draw_list, index_array_);
	rd_->draw_list_set_push_constant(draw_list, push_constants_, kRibbonPushBytes);
	rd_->draw_list_draw(draw_list, true, 1);
	rd_->draw_list_end();
	++r_draws;
	drawn_ribbon_draws_.store(1, std::memory_order_release);
	return true;
}

void EffectDistortionDrawer::release_device_resources() {
	if (rd_ == nullptr)
		return;
	for (auto &entry : pipelines_) {
		if (entry.second.is_valid() && rd_->render_pipeline_is_valid(entry.second))
			rd_->free_rid(entry.second);
	}
	pipelines_.clear();
	if (uniform_set_.is_valid() && rd_->uniform_set_is_valid(uniform_set_))
		rd_->free_rid(uniform_set_);
	uniform_set_ = RID();
	if (framebuffer_.is_valid() && rd_->framebuffer_is_valid(framebuffer_))
		rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	release(rd_, index_array_);
	release(rd_, index_buffer_);
	release(rd_, vertex_buffer_);
	release(rd_, shader_);
	vertex_format_ = RenderingDevice::INVALID_FORMAT_ID;
	vertex_capacity_ = 0;
	index_capacity_ = 0;
	framebuffer_color_ = RID();
	framebuffer_depth_ = RID();
	uniform_texture_ = RID();
	uniform_sampler_ = RID();
	rd_ = nullptr;
}

} // namespace godot
