#include "render/environment_cube_blit.h"
#include "render/rd_fullscreen.h"

#include <cstring>

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
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

constexpr std::uint32_t kPushConstantBytes = 16u;

// One cube layer from one rendered face. The destination texel picks its
// source texel by the face orientation, quantizes the gamma-domain value to
// the retail framebuffer byte (truncation, as the former Image RGBA8 convert
// did) and applies the dim quad's product rounded half up: the integer form
// of opennova::renderer::environment_cube_dim_byte in
// <runtime/renderer/environment_cube.h>, whose byte the push constant
// carries. The output k/255 lands on byte k after the UNORM conversion.
const char *kLayerFragmentShader = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source_face;

layout(push_constant, std430) uniform LayerPush {
	uint orientation;
	uint last_texel;
	uint dim_byte;
	uint padding0;
} pc;

layout(location = 0) out vec4 layer_color;

void main() {
	ivec2 dst = ivec2(gl_FragCoord.xy);
	int last = int(pc.last_texel);
	ivec2 src = dst;
	if (pc.orientation == 1u) {
		src = ivec2(last - dst.x, dst.y);
	} else if (pc.orientation == 2u) {
		src = ivec2(last - dst.y, last - dst.x);
	} else if (pc.orientation == 3u) {
		src = ivec2(dst.y, dst.x);
	}
	vec3 gamma = clamp(texelFetch(source_face, src, 0).rgb, vec3(0.0), vec3(1.0));
	uvec3 bytes = uvec3(gamma * 255.0);
	uvec3 dimmed = (bytes * pc.dim_byte + 127u) / 255u;
	layer_color = vec4(vec3(dimmed) / 255.0, 1.0);
}
)GLSL";

void write_u32(PackedByteArray &bytes, std::uint32_t offset,
		std::uint32_t value) {
	std::memcpy(bytes.ptrw() + offset, &value, sizeof(value));
}

} // namespace

void EnvironmentCubeBlit::set_request(const Request &p_request) {
	request_ = p_request;
}

String EnvironmentCubeBlit::failure() const {
	std::lock_guard<std::mutex> lock(failure_mutex_);
	return String::utf8(failure_.c_str());
}

void EnvironmentCubeBlit::set_failure(const std::string &reason) {
	std::lock_guard<std::mutex> lock(failure_mutex_);
	failure_ = reason;
}

void EnvironmentCubeBlit::finish(bool ok) {
	if (ok) {
		set_failure(std::string());
		published_cubes_.fetch_add(1, std::memory_order_acq_rel);
	}
	last_request_ok_.store(ok, std::memory_order_release);
	completed_requests_.fetch_add(1, std::memory_order_acq_rel);
}

bool EnvironmentCubeBlit::ensure_shader(RenderingDevice *rd) {
	if (shader_.is_valid() && sampler_.is_valid())
		return true;
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kRdFullscreenVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(kLayerFragmentShader));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		set_failure("RenderingDevice returned no SPIR-V for the environment "
				"cube blit");
		return false;
	}
	const String vertex_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error = spirv->get_stage_compile_error(
			RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!vertex_error.is_empty() || !fragment_error.is_empty()) {
		set_failure("environment cube blit shader compilation failed: vertex=" +
				std::string(vertex_error.utf8().get_data()) + "; fragment=" +
				std::string(fragment_error.utf8().get_data()));
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv,
			"OpenNova environment cube blit");
	if (!shader_.is_valid()) {
		set_failure("RenderingDevice rejected the environment cube blit shader");
		return false;
	}
	Ref<RDSamplerState> sampler_state;
	sampler_state.instantiate();
	sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_mip_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_ = rd->sampler_create(sampler_state);
	if (!sampler_.is_valid()) {
		set_failure("RenderingDevice could not create the environment cube "
				"blit sampler");
		return false;
	}
	push_constants_.resize(kPushConstantBytes);
	return true;
}

bool EnvironmentCubeBlit::ensure_cube(RenderingDevice *rd) {
	if (cube_texture_.is_valid())
		return true;
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format->set_width(kFaceSize);
	format->set_height(kFaceSize);
	format->set_depth(1);
	format->set_array_layers(kFaceCount);
	format->set_mipmaps(1);
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_CUBE);
	format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	cube_texture_ = rd->texture_create(format, view);
	if (!cube_texture_.is_valid()) {
		set_failure("RenderingDevice could not create the environment cubemap");
		return false;
	}
	for (int layer = 0; layer < kFaceCount; ++layer) {
		Ref<RDTextureView> layer_view;
		layer_view.instantiate();
		layer_views_[layer] = rd->texture_create_shared_from_slice(layer_view,
				cube_texture_, static_cast<uint32_t>(layer), 0, 1,
				RenderingDevice::TEXTURE_SLICE_2D);
		if (!layer_views_[layer].is_valid()) {
			set_failure("RenderingDevice could not view environment cube layer " +
					std::to_string(layer));
			return false;
		}
		TypedArray<RID> attachments;
		attachments.push_back(layer_views_[layer]);
		layer_framebuffers_[layer] = rd->framebuffer_create(attachments);
		if (!layer_framebuffers_[layer].is_valid()) {
			set_failure("RenderingDevice could not attach environment cube layer " +
					std::to_string(layer));
			return false;
		}
	}
	return true;
}

bool EnvironmentCubeBlit::ensure_pipeline(RenderingDevice *rd) {
	if (pipeline_.is_valid())
		return true;
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
	Ref<RDPipelineColorBlendState> color_blend;
	color_blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	color_blend->set_attachments(attachments);
	const int64_t framebuffer_format =
			rd->framebuffer_get_format(layer_framebuffers_[0]);
	pipeline_ = rd->render_pipeline_create(shader_, framebuffer_format,
			RenderingDevice::INVALID_FORMAT_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample,
			depth, color_blend);
	if (!pipeline_.is_valid()) {
		set_failure("RenderingDevice rejected the environment cube blit pipeline");
		return false;
	}
	return true;
}

bool EnvironmentCubeBlit::ensure_layer_uniform(RenderingDevice *rd, int layer,
		const RID &face_texture) {
	RID &uniform = layer_uniforms_[layer];
	if (uniform.is_valid() && layer_uniform_sources_[layer] == face_texture &&
			rd->uniform_set_is_valid(uniform))
		return true;
	if (uniform.is_valid() && rd->uniform_set_is_valid(uniform))
		rd->free_rid(uniform);
	uniform = RID();
	Ref<RDUniform> sampled;
	sampled.instantiate();
	sampled->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	sampled->set_binding(0);
	sampled->add_id(sampler_);
	sampled->add_id(face_texture);
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled);
	uniform = rd->uniform_set_create(uniforms, shader_, 0);
	layer_uniform_sources_[layer] = face_texture;
	if (!uniform.is_valid()) {
		set_failure("RenderingDevice could not bind environment face " +
				std::to_string(layer));
		return false;
	}
	return true;
}

bool EnvironmentCubeBlit::draw_layer(RenderingDevice *rd, int layer,
		Orientation orientation) {
	write_u32(push_constants_, 0, static_cast<std::uint32_t>(orientation));
	write_u32(push_constants_, 4, static_cast<std::uint32_t>(kFaceSize - 1));
	write_u32(push_constants_, 8, opennova::renderer::kEnvironmentCubeDimByte);
	write_u32(push_constants_, 12, 0);
	const int64_t draw_list = rd->draw_list_begin(layer_framebuffers_[layer],
			RenderingDevice::DRAW_IGNORE_COLOR_ALL);
	if (draw_list == RenderingDevice::INVALID_ID) {
		set_failure("RenderingDevice could not begin the environment cube layer " +
				std::to_string(layer) + " draw list");
		return false;
	}
	rd->draw_list_bind_render_pipeline(draw_list, pipeline_);
	rd->draw_list_bind_uniform_set(draw_list, layer_uniforms_[layer], 0);
	rd->draw_list_set_push_constant(draw_list, push_constants_,
			kPushConstantBytes);
	rd->draw_list_draw(draw_list, false, 1, 3);
	rd->draw_list_end();
	return true;
}

void EnvironmentCubeBlit::publish() {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs != nullptr ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		set_failure("no RenderingDevice for the environment cube blit");
		finish(false);
		return;
	}
	if (device_failed_.load(std::memory_order_acquire)) {
		finish(false);
		return;
	}
	rd_ = rd;
	if (!ensure_shader(rd) || !ensure_cube(rd) || !ensure_pipeline(rd)) {
		device_failed_.store(true, std::memory_order_release);
		finish(false);
		return;
	}
	for (int layer = 0; layer < kFaceCount; ++layer) {
		const FaceSource &source = request_[layer];
		const RID face = rs->texture_get_rd_texture(source.viewport_texture);
		if (!face.is_valid() || !rd->texture_is_valid(face)) {
			set_failure("environment face " + std::to_string(layer) +
					" has no device texture yet");
			finish(false);
			return;
		}
		const Ref<RDTextureFormat> format = rd->texture_get_format(face);
		if (format.is_null() || format->get_width() != kFaceSize ||
				format->get_height() != kFaceSize) {
			set_failure("environment face " + std::to_string(layer) +
					" is not a " + std::to_string(kFaceSize) + "-square target");
			finish(false);
			return;
		}
		if (!ensure_layer_uniform(rd, layer, face) ||
				!draw_layer(rd, layer, source.orientation)) {
			finish(false);
			return;
		}
	}
	finish(true);
}

void EnvironmentCubeBlit::release() {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs != nullptr ? rs->get_rendering_device() : rd_;
	if (rd != nullptr) {
		// Validity-checked: a consumer that owned the cube through the server
		// may have freed it (and, through the device's dependency tracking,
		// the layer views and framebuffers) ahead of this release.
		for (RID &uniform : layer_uniforms_) {
			if (uniform.is_valid() && rd->uniform_set_is_valid(uniform))
				rd->free_rid(uniform);
			uniform = RID();
		}
		for (RID &framebuffer : layer_framebuffers_) {
			if (framebuffer.is_valid() && rd->framebuffer_is_valid(framebuffer))
				rd->free_rid(framebuffer);
			framebuffer = RID();
		}
		for (RID &view : layer_views_) {
			if (view.is_valid() && rd->texture_is_valid(view))
				rd->free_rid(view);
			view = RID();
		}
		if (cube_texture_.is_valid() && rd->texture_is_valid(cube_texture_))
			rd->free_rid(cube_texture_);
		if (pipeline_.is_valid() && rd->render_pipeline_is_valid(pipeline_))
			rd->free_rid(pipeline_);
		if (sampler_.is_valid())
			rd->free_rid(sampler_);
		if (shader_.is_valid())
			rd->free_rid(shader_);
	}
	cube_texture_ = RID();
	pipeline_ = RID();
	sampler_ = RID();
	shader_ = RID();
	for (RID &source : layer_uniform_sources_)
		source = RID();
	rd_ = nullptr;
	set_failure(std::string());
	device_failed_.store(false, std::memory_order_release);
	last_request_ok_.store(false, std::memory_order_release);
	published_cubes_.store(0, std::memory_order_release);
	completed_requests_.store(0, std::memory_order_release);
}
