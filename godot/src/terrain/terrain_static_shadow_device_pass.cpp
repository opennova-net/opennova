// Device leg only: the RenderingDevice resources and recording of the static-shadow page pass.
// The job, its order and its state are the engine's (runtime/terrain/
// terrain_static_shadow_device_job.h); the arithmetic the shaders run is the CPU raster's
// (runtime/terrain/terrain_static_shadow_raster.{h,cpp}), its constants spliced in.
#include "terrain/terrain_static_shadow_device_pass.h"

#include "render/rd_glsl.h"
#include "render/rd_uniforms.h"
#include "util/string_convert.h"

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
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>

#include <runtime/renderer/d3d9_raster.h>
#include <runtime/terrain/terrain_static_shadow_raster.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

namespace godot {
namespace {

using opennova::terrain::TerrainStaticShadowBlend;
using opennova::terrain::TerrainStaticShadowDeviceDraw;
using opennova::terrain::TerrainStaticShadowDeviceJob;

// One vertex: the surface corner's model-space position and its authored UV.
constexpr uint32_t kVertexStride = 20u;
constexpr uint32_t kCasterPushBytes = 128u;
constexpr uint32_t kPassPushBytes = 16u;
constexpr uint32_t kComputeGroup = 8u;

// The caster's push block, shared by both stages (128 bytes, std430):
//   0  row0 / row1 / row2: the world matrix rows with the origin in w
//  48  offset_ground: the ROBJ offset, then the ground height under the caster
//  64  uv_a: m00 m01 m10 m11, then uv_b: m20 m21
//  88  alpha_scale, flags (bit 0 textured, 1 alpha test, 2 inverted, 3 UV transform,
//      bits 8..15 the alpha reference byte)
//  96  light: presentation-world surface-to-light
// 108  origin_x, origin_z, inverse_span: the page projection
const char *const kCasterPushBlock = R"GLSL(layout(push_constant, std430) uniform CasterPush {
	vec4 row0;
	vec4 row1;
	vec4 row2;
	vec4 offset_ground;
	vec4 uv_a;
	vec2 uv_b;
	float alpha_scale;
	uint flags;
	vec3 light;
	float origin_x;
	float origin_z;
	float inverse_span;
	float pad0;
	float pad1;
} pc;
)GLSL";

// The vertex stage: the CPU raster's projection (project_terrain_static_shadow_device_vertex,
// project_terrain_static_shadow_vertex) in its order, every step `precise` so no stage
// contracts it; page UV to the temp's clip space with the pixel centre where D3D9 puts it
// (pixel i samples page_u = i / width).
const char *const kCasterVertexTemplate = R"GLSL(#version 450
layout(location = 0) in vec3 a_local;
layout(location = 1) in vec2 a_uv;
@PUSH_BLOCK@
layout(location = 0) out vec2 v_uv;

void main() {
	precise vec3 local = a_local + pc.offset_ground.xyz;
	precise float world_x = pc.row0.x * local.x + pc.row0.y * local.y + pc.row0.z * local.z +
			pc.row0.w;
	precise float world_y = pc.row1.x * local.x + pc.row1.y * local.y + pc.row1.z * local.z +
			pc.row1.w;
	precise float world_z = pc.row2.x * local.x + pc.row2.y * local.y + pc.row2.z * local.z +
			pc.row2.w;
	precise float vertical = max(pc.light.y, @MINIMUM_VERTICAL@);
	precise float height = world_y - pc.offset_ground.w;
	precise float shadow_x = world_x - height * pc.light.x / vertical;
	precise float shadow_z = world_z - height * pc.light.z / vertical;
	precise float page_u = (shadow_x - pc.origin_x) * pc.inverse_span;
	precise float page_v = (shadow_z - pc.origin_z) * pc.inverse_span;
	precise float depth = height * @DEPTH_SCALE@ + @DEPTH_BIAS@;
	vec2 uv = a_uv;
	if ((pc.flags & 8u) != 0u) {
		uv = vec2(a_uv.x * pc.uv_a.x + a_uv.y * pc.uv_a.z + pc.uv_b.x,
				a_uv.x * pc.uv_a.y + a_uv.y * pc.uv_a.w + pc.uv_b.y);
	}
	v_uv = uv;
	gl_Position = vec4(page_u * 2.0 - 1.0 + @NDC_SHIFT@, page_v * 2.0 - 1.0 + @NDC_SHIFT@,
			depth, 1.0);
}
)GLSL";

// The fragment stage: the black PROJSHAD fragment, its alpha the material's scale times the
// alpha texture's sample, tested as the material's alpha test (renderer::alpha_test_passes).
const char *const kCasterFragmentTemplate = R"GLSL(#version 450
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D alpha_texture;
@PUSH_BLOCK@
layout(location = 0) out vec4 frag_color;

void main() {
	float alpha = clamp(pc.alpha_scale, 0.0, 1.0);
	if ((pc.flags & 1u) != 0u) {
		alpha *= texture(alpha_texture, v_uv).r;
	}
	if ((pc.flags & 2u) != 0u) {
		float alpha_byte = alpha * 255.0;
		float reference = float((pc.flags >> 8u) & 255u);
		bool passes = (pc.flags & 4u) != 0u ? alpha_byte <= reference : alpha_byte > reference;
		if (!passes) {
			discard;
		}
	}
	frag_color = vec4(0.0, 0.0, 0.0, alpha);
}
)GLSL";

// The fill: temp texel (x, y) takes the page layer's alpha at (x, y) / scale.
const char *const kFillTemplate = R"GLSL(#version 450
layout(local_size_x = @GROUP@, local_size_y = @GROUP@, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba8) uniform restrict readonly image2DArray page_array;
layout(set = 0, binding = 1, r8) uniform restrict writeonly image2D temp_target;
layout(push_constant, std430) uniform PassPush {
	uint layer;
	uint size;
	uint pad0;
	uint pad1;
} pc;

void main() {
	ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
	if (texel.x >= int(pc.size) || texel.y >= int(pc.size)) {
		return;
	}
	float alpha = imageLoad(page_array, ivec3(texel / @SCALE@, int(pc.layer))).a;
	imageStore(temp_target, texel, vec4(alpha, 0.0, 0.0, 0.0));
}
)GLSL";

// The resolve: the layer's alpha at (x, y) is the rounded mean of the temp's scale x scale
// box at (x, y) * scale, in integers; RGB is stored back as it was.
const char *const kResolveTemplate = R"GLSL(#version 450
layout(local_size_x = @GROUP@, local_size_y = @GROUP@, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba8) uniform restrict image2DArray page_array;
layout(set = 0, binding = 1, r8) uniform restrict readonly image2D temp_target;
layout(push_constant, std430) uniform PassPush {
	uint layer;
	uint size;
	uint pad0;
	uint pad1;
} pc;

void main() {
	ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
	if (texel.x >= int(pc.size) || texel.y >= int(pc.size)) {
		return;
	}
	uint sum = 0u;
	for (int oy = 0; oy < @SCALE@; ++oy) {
		for (int ox = 0; ox < @SCALE@; ++ox) {
			sum += uint(round(imageLoad(temp_target, texel * @SCALE@ + ivec2(ox, oy)).r * 255.0));
		}
	}
	uint alpha = (sum + @HALF_BOX@u) / @BOX@u;
	ivec3 at = ivec3(texel, int(pc.layer));
	vec4 page = imageLoad(page_array, at);
	imageStore(page_array, at, vec4(page.rgb, float(alpha) / 255.0));
}
)GLSL";

std::string with_tokens(const char *p_template, uint32_t p_temp_dimension) {
	std::string text(p_template);
	splice_token(text, "@PUSH_BLOCK@", kCasterPushBlock);
	splice_token(text, "@MINIMUM_VERTICAL@",
			glsl_float(opennova::terrain::kTerrainStaticShadowMinimumVerticalLight));
	splice_token(text, "@DEPTH_SCALE@", glsl_float(opennova::terrain::kTerrainStaticShadowDepthScale));
	splice_token(text, "@DEPTH_BIAS@", glsl_float(opennova::terrain::kTerrainStaticShadowDepthBias));
	// Half a temp pixel right and down in clip space: 2 * 0.5 / dimension (y grows down the
	// target, as the page's rows do).
	splice_token(text, "@NDC_SHIFT@",
			glsl_float(2.0f * opennova::renderer::kD3d9PixelCentre /
					static_cast<float>(p_temp_dimension)));
	const uint32_t scale = opennova::terrain::kTerrainStaticShadowTemporaryScale;
	splice_token(text, "@SCALE@", std::to_string(scale));
	splice_token(text, "@BOX@", std::to_string(scale * scale));
	splice_token(text, "@HALF_BOX@", std::to_string(scale * scale / 2));
	splice_token(text, "@GROUP@", std::to_string(kComputeGroup));
	return text;
}

RID compile_compute(RenderingDevice *p_rd, const std::string &p_source, const char *p_name,
		std::string &r_error) {
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, opennova::to_gd(p_source));
	const Ref<RDShaderSPIRV> spirv = p_rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		r_error = "no SPIR-V";
		return RID();
	}
	const String error = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!error.is_empty()) {
		r_error = opennova::to_std(error);
		return RID();
	}
	return p_rd->shader_create_from_spirv(spirv, p_name);
}

Ref<RDUniform> image_uniform(int p_binding, const RID &p_texture) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	uniform->set_binding(p_binding);
	uniform->add_id(p_texture);
	return uniform;
}

// The blend group a draw's PROJSHAD state falls in: opaque and multiply both write the
// black fragment (a black source under DESTCOLOR / SRCCOLOR is black), alpha scales the
// temp by 1 - a, additive leaves it.
std::size_t blend_group(TerrainStaticShadowBlend p_blend) {
	switch (p_blend) {
		case TerrainStaticShadowBlend::Alpha:
			return 1;
		case TerrainStaticShadowBlend::Additive:
			return 2;
		case TerrainStaticShadowBlend::Opaque:
		case TerrainStaticShadowBlend::Multiply:
			break;
	}
	return 0;
}

} // namespace

TerrainStaticShadowDevicePass::~TerrainStaticShadowDevicePass() {
	release();
}

bool TerrainStaticShadowDevicePass::fail(const std::string &p_reason) {
	failure_ = p_reason;
	return false;
}

void TerrainStaticShadowDevicePass::free_rid(RID &r_rid) {
	// Past the server's shutdown the device and its resources are gone with it.
	if (rd_ != nullptr && r_rid.is_valid() && RenderingServer::get_singleton() != nullptr)
		rd_->free_rid(r_rid);
	r_rid = RID();
}

bool TerrainStaticShadowDevicePass::prepare(RenderingDevice *p_rd, const RID &p_page_array,
		uint32_t p_page_dimension) {
	if (prepared_ && rd_ == p_rd && page_array_ == p_page_array &&
			page_dimension_ == p_page_dimension) {
		return true;
	}
	release();
	if (p_rd == nullptr || !p_page_array.is_valid() || p_page_dimension == 0) {
		return fail("no device or page array");
	}
	rd_ = p_rd;
	page_array_ = p_page_array;
	page_dimension_ = p_page_dimension;
	temp_dimension_ = p_page_dimension * opennova::terrain::kTerrainStaticShadowTemporaryScale;
	if (!build_targets() || !build_shaders() || !build_pipelines()) {
		const std::string reason = failure_;
		release();
		failure_ = reason;
		return false;
	}
	caster_push_.resize(kCasterPushBytes);
	pass_push_.resize(kPassPushBytes);
	vertex_buffers_.resize(1);
	vertex_offsets_.resize(1);
	prepared_ = true;
	failure_.clear();
	return true;
}

bool TerrainStaticShadowDevicePass::build_targets() {
	// The temp target: the blue channel the composite reads, as one R8 channel the casters
	// draw into and the fill and the resolve read and write in place.
	const BitField<RenderingDevice::TextureUsageBits> color_usage =
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT;
	if (!rd_->texture_is_format_supported_for_usage(RenderingDevice::DATA_FORMAT_R8_UNORM,
				color_usage) ||
			!rd_->texture_is_format_supported_for_usage(RenderingDevice::DATA_FORMAT_D32_SFLOAT,
					RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		return fail("the device cannot attach and store an R8 temp with a D32F depth");
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	format->set_width(temp_dimension_);
	format->set_height(temp_dimension_);
	format->set_depth(1);
	format->set_array_layers(1);
	format->set_mipmaps(1);
	format->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	format->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	format->set_usage_bits(color_usage);
	Ref<RDTextureView> view;
	view.instantiate();
	temp_color_ = rd_->texture_create(format, view);
	format->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
	format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	temp_depth_ = rd_->texture_create(format, view);
	if (!temp_color_.is_valid() || !temp_depth_.is_valid()) {
		return fail("the device could not create the temp target");
	}
	TypedArray<RID> attachments;
	attachments.push_back(temp_color_);
	attachments.push_back(temp_depth_);
	framebuffer_ = rd_->framebuffer_create(attachments);
	if (!framebuffer_.is_valid()) return fail("the device could not attach the temp target");
	return true;
}

bool TerrainStaticShadowDevicePass::build_shaders() {
	Ref<RDShaderSPIRV> spirv;
	const std::string caster_errors = compile_rd_spirv(rd_,
			with_tokens(kCasterVertexTemplate, temp_dimension_),
			with_tokens(kCasterFragmentTemplate, temp_dimension_), spirv);
	if (!caster_errors.empty()) {
		return fail("the caster shader did not compile: " + caster_errors);
	}
	caster_shader_ = rd_->shader_create_from_spirv(spirv, "OpenNova terrain static shadow casters");
	if (!caster_shader_.is_valid()) return fail("the device refused the caster shader");
	std::string error;
	fill_shader_ = compile_compute(rd_, with_tokens(kFillTemplate, temp_dimension_),
			"OpenNova terrain static shadow fill", error);
	if (!fill_shader_.is_valid()) return fail("the fill shader did not compile: " + error);
	resolve_shader_ = compile_compute(rd_, with_tokens(kResolveTemplate, temp_dimension_),
			"OpenNova terrain static shadow resolve", error);
	if (!resolve_shader_.is_valid()) return fail("the resolve shader did not compile: " + error);
	return true;
}

bool TerrainStaticShadowDevicePass::build_pipelines() {
	TypedArray<Ref<RDVertexAttribute>> attributes;
	const auto attribute = [&](uint32_t p_location, RenderingDevice::DataFormat p_format,
								   uint32_t p_offset) {
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
	attribute(0, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0u);
	attribute(1, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 12u);
	vertex_format_ = rd_->vertex_format_create(attributes);
	if (vertex_format_ == RenderingDevice::INVALID_FORMAT_ID) {
		return fail("the device refused the caster vertex format");
	}
	const int64_t framebuffer_format = rd_->framebuffer_get_format(framebuffer_);
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);
	// Every shipped PROJSHAD declaration has the normal z mode: LESSEQUAL, z writes on,
	// whatever the blend; the clip volume's near face is the CPU raster's depth >= 0 clip.
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test(true);
	depth->set_enable_depth_write(true);
	depth->set_depth_compare_operator(RenderingDevice::COMPARE_OP_LESS_OR_EQUAL);
	for (std::size_t group = 0; group < caster_pipelines_.size(); ++group) {
		Ref<RDPipelineColorBlendStateAttachment> attachment;
		attachment.instantiate();
		if (group == 0) {
			attachment->set_enable_blend(false);
		} else {
			attachment->set_enable_blend(true);
			const bool alpha = group == 1;
			attachment->set_src_color_blend_factor(alpha ? RenderingDevice::BLEND_FACTOR_SRC_ALPHA
														  : RenderingDevice::BLEND_FACTOR_ONE);
			attachment->set_dst_color_blend_factor(alpha
							? RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
							: RenderingDevice::BLEND_FACTOR_ONE);
			attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
			attachment->set_src_alpha_blend_factor(attachment->get_src_color_blend_factor());
			attachment->set_dst_alpha_blend_factor(attachment->get_dst_color_blend_factor());
			attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
		}
		Ref<RDPipelineColorBlendState> blend;
		blend.instantiate();
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> blend_attachments;
		blend_attachments.push_back(attachment);
		blend->set_attachments(blend_attachments);
		for (std::size_t cull = 0; cull < 2; ++cull) {
			// The ordinary pass culls CCW: the CPU raster keeps a positive signed area in
			// screen space (y down), which in this target's framebuffer space is the
			// clockwise front face; a two-sided material culls nothing (the device job's
			// witness).
			Ref<RDPipelineRasterizationState> raster;
			raster.instantiate();
			raster->set_enable_depth_clamp(false);
			raster->set_front_face(RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE);
			raster->set_cull_mode(cull == 0 ? RenderingDevice::POLYGON_CULL_BACK
											: RenderingDevice::POLYGON_CULL_DISABLED);
			caster_pipelines_[group][cull] = rd_->render_pipeline_create(caster_shader_,
					framebuffer_format, vertex_format_, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
					raster, multisample, depth, blend);
			if (!caster_pipelines_[group][cull].is_valid()) {
				return fail("the device refused a caster pipeline");
			}
		}
	}
	fill_pipeline_ = rd_->compute_pipeline_create(fill_shader_);
	resolve_pipeline_ = rd_->compute_pipeline_create(resolve_shader_);
	if (!fill_pipeline_.is_valid() || !resolve_pipeline_.is_valid()) {
		return fail("the device refused a compute pipeline");
	}
	TypedArray<Ref<RDUniform>> images;
	images.push_back(image_uniform(0, page_array_));
	images.push_back(image_uniform(1, temp_color_));
	fill_set_ = rd_->uniform_set_create(images, fill_shader_, 0);
	resolve_set_ = rd_->uniform_set_create(images, resolve_shader_, 0);
	if (!fill_set_.is_valid() || !resolve_set_.is_valid()) {
		return fail("the device could not bind the page array and the temp");
	}
	// The alpha textures' sampler: MINFILTER / MAGFILTER LINEAR, a LINEAR mip filter (the
	// planner's TerrainStaticShadowMipFilter::Linear) and WRAP addressing.
	Ref<RDSamplerState> sampler;
	sampler.instantiate();
	sampler->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	sampler_ = rd_->sampler_create(sampler);
	if (!sampler_.is_valid()) return fail("the device refused the alpha sampler");
	// An untextured draw binds a one-texel texture it never samples.
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	format->set_width(1);
	format->set_height(1);
	format->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray white;
	white.resize(1);
	white.set(0, 255);
	TypedArray<PackedByteArray> data;
	data.push_back(white);
	blank_texture_ = rd_->texture_create(format, view, data);
	if (!blank_texture_.is_valid()) return fail("the device refused the blank alpha texture");
	TypedArray<Ref<RDUniform>> sampled;
	sampled.push_back(sampled_texture_uniform(0, sampler_, blank_texture_));
	blank_set_ = rd_->uniform_set_create(sampled, caster_shader_, 0);
	if (!blank_set_.is_valid()) return fail("the device could not bind the blank alpha texture");
	return true;
}

const TerrainStaticShadowDevicePass::GeometryEntry *TerrainStaticShadowDevicePass::geometry_for(
		const TerrainStaticShadowDeviceDraw &p_draw) {
	const auto found = geometries_.find(p_draw.geometry.get());
	if (found != geometries_.end()) return &found->second;
	const opennova::terrain::TerrainStaticShadowResolvedGeometry &geometry = *p_draw.geometry;
	GeometryEntry entry;
	entry.geometry = p_draw.geometry;
	std::vector<float> vertices;
	uint32_t vertex_count = 0;
	for (std::size_t lod = 0; lod < geometry.surfaces.size(); ++lod) {
		for (const opennova::terrain::TerrainStaticShadowResolvedSurface &surface :
				geometry.surfaces[lod]) {
			entry.first_vertex[lod].push_back(vertex_count);
			const std::size_t corners = surface.indices.size() / 3 * 3;
			for (std::size_t corner = 0; corner < corners; ++corner) {
				const int32_t index = surface.indices[corner];
				if (index < 0 || static_cast<std::size_t>(index) >= surface.vertices.size() ||
						static_cast<std::size_t>(index) >= surface.uvs.size()) {
					fail("a caster surface names a vertex it does not hold");
					return nullptr;
				}
				const std::array<float, 3> &position = surface.vertices[static_cast<std::size_t>(index)];
				const std::array<float, 2> &uv = surface.uvs[static_cast<std::size_t>(index)];
				vertices.insert(vertices.end(), { position[0], position[1], position[2], uv[0], uv[1] });
				++vertex_count;
			}
		}
	}
	if (vertex_count > 0) {
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(vertices.size() * sizeof(float)));
		std::memcpy(bytes.ptrw(), vertices.data(), vertices.size() * sizeof(float));
		entry.vertices = rd_->vertex_buffer_create(static_cast<uint32_t>(bytes.size()), bytes);
		if (!entry.vertices.is_valid()) {
			fail("the device could not hold a caster's vertices");
			return nullptr;
		}
	}
	++geometry_uploads_;
	return &geometries_.emplace(p_draw.geometry.get(), std::move(entry)).first->second;
}

RID TerrainStaticShadowDevicePass::texture_set_for(const TerrainStaticShadowDeviceDraw &p_draw) {
	if (p_draw.alpha_texture == nullptr) return blank_set_;
	const auto found = textures_.find(p_draw.alpha_texture);
	if (found != textures_.end()) return found->second.uniform_set;
	const opennova::terrain::TerrainStaticShadowAlphaPyramid &pyramid = *p_draw.alpha_texture;
	if (pyramid.mips.empty() || pyramid.mips[0].width == 0 || pyramid.mips[0].height == 0) {
		fail("a caster's alpha texture has no level");
		return RID();
	}
	// The levels the standard chain under mip 0 matches, packed level after level.
	const uint32_t width = pyramid.mips[0].width;
	const uint32_t height = pyramid.mips[0].height;
	PackedByteArray bytes;
	uint32_t levels = 0;
	for (const opennova::terrain::TerrainStaticShadowAlphaMipView &mip : pyramid.mips) {
		const uint32_t level_width = std::max(1u, width >> levels);
		const uint32_t level_height = std::max(1u, height >> levels);
		if (mip.alpha == nullptr || mip.width != level_width || mip.height != level_height ||
				mip.row_stride < mip.width) {
			break;
		}
		const int64_t at = bytes.size();
		bytes.resize(at + static_cast<int64_t>(level_width) * level_height);
		for (uint32_t row = 0; row < level_height; ++row) {
			std::memcpy(bytes.ptrw() + at + static_cast<int64_t>(row) * level_width,
					mip.alpha + static_cast<std::size_t>(row) * mip.row_stride, level_width);
		}
		++levels;
		if (level_width == 1 && level_height == 1) break;
	}
	if (levels == 0) {
		fail("a caster's alpha texture has no usable level");
		return RID();
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	format->set_width(width);
	format->set_height(height);
	format->set_mipmaps(levels);
	format->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	TypedArray<PackedByteArray> data;
	data.push_back(bytes);
	TextureEntry entry;
	entry.owner = p_draw.geometry;
	entry.texture = rd_->texture_create(format, view, data);
	if (!entry.texture.is_valid()) {
		fail("the device refused a caster's alpha texture");
		return RID();
	}
	TypedArray<Ref<RDUniform>> sampled;
	sampled.push_back(sampled_texture_uniform(0, sampler_, entry.texture));
	entry.uniform_set = rd_->uniform_set_create(sampled, caster_shader_, 0);
	if (!entry.uniform_set.is_valid()) {
		free_rid(entry.texture);
		fail("the device could not bind a caster's alpha texture");
		return RID();
	}
	++texture_uploads_;
	const RID uniform_set = entry.uniform_set;
	textures_.emplace(p_draw.alpha_texture, std::move(entry));
	return uniform_set;
}

bool TerrainStaticShadowDevicePass::dispatch(const RID &p_pipeline, const RID &p_set,
		uint32_t p_layer, uint32_t p_size) {
	write_u32(pass_push_, 0, p_layer);
	write_u32(pass_push_, 4, p_size);
	write_u32(pass_push_, 8, 0u);
	write_u32(pass_push_, 12, 0u);
	const int64_t list = rd_->compute_list_begin();
	if (list == RenderingDevice::INVALID_ID) return fail("the device could not begin a compute list");
	rd_->compute_list_bind_compute_pipeline(list, p_pipeline);
	rd_->compute_list_bind_uniform_set(list, p_set, 0);
	rd_->compute_list_set_push_constant(list, pass_push_, kPassPushBytes);
	const uint32_t groups = (p_size + kComputeGroup - 1) / kComputeGroup;
	rd_->compute_list_dispatch(list, groups, groups, 1);
	rd_->compute_list_end();
	return true;
}

bool TerrainStaticShadowDevicePass::record(const TerrainStaticShadowDeviceJob &p_job,
		uint32_t p_layer) {
	if (!prepared_) return fail("the pass is not prepared");
	if (p_job.draws.empty()) return true;
	const std::optional<opennova::TerrainTilePageProjection> projection =
			opennova::TerrainTileCompositionCache::page_projection(p_job.page);
	if (!projection.has_value()) return fail("the job's page has no projection");
	// The temp starts as the layer's DOT3 light alpha (retail's light quad into the temp).
	if (!dispatch(fill_pipeline_, fill_set_, p_layer, temp_dimension_)) return false;
	// The casters into the temp over its depth clear (the light quad writes no depth), in the
	// job's order: one submit and flush per caster.
	PackedColorArray no_clear;
	const int64_t list = rd_->draw_list_begin(framebuffer_, RenderingDevice::DRAW_CLEAR_DEPTH,
			no_clear, p_job.clear_depth);
	if (list == RenderingDevice::INVALID_ID) return fail("the device could not begin the caster draw list");
	write_f32(caster_push_, 96, p_job.surface_to_light.x);
	write_f32(caster_push_, 100, p_job.surface_to_light.y);
	write_f32(caster_push_, 104, p_job.surface_to_light.z);
	write_f32(caster_push_, 108, projection->world_origin_x);
	write_f32(caster_push_, 112, projection->world_origin_z);
	write_f32(caster_push_, 116, projection->inverse_world_span);
	write_f32(caster_push_, 120, 0.0f);
	write_f32(caster_push_, 124, 0.0f);
	bool ok = true;
	for (const TerrainStaticShadowDeviceDraw &draw : p_job.draws) {
		if (draw.geometry == nullptr || draw.lod_index > 1 || draw.triangle_count == 0) continue;
		const GeometryEntry *geometry = geometry_for(draw);
		const RID texture_set = texture_set_for(draw);
		if (geometry == nullptr || !texture_set.is_valid() ||
				draw.surface_index >= geometry->first_vertex[draw.lod_index].size()) {
			ok = false;
			break;
		}
		const std::array<float, 12> &t = draw.world_transform;
		for (int row = 0; row < 3; ++row) {
			const uint32_t base = static_cast<uint32_t>(row) * 16u;
			write_f32(caster_push_, base, t[static_cast<std::size_t>(row) * 3]);
			write_f32(caster_push_, base + 4, t[static_cast<std::size_t>(row) * 3 + 1]);
			write_f32(caster_push_, base + 8, t[static_cast<std::size_t>(row) * 3 + 2]);
			write_f32(caster_push_, base + 12, t[9 + static_cast<std::size_t>(row)]);
		}
		const opennova::terrain::TerrainStaticShadowResolvedSurface &surface =
				draw.geometry->surfaces[draw.lod_index][draw.surface_index];
		write_f32(caster_push_, 48, surface.render_object_offset[0]);
		write_f32(caster_push_, 52, surface.render_object_offset[1]);
		write_f32(caster_push_, 56, surface.render_object_offset[2]);
		write_f32(caster_push_, 60, draw.ground_y);
		write_f32(caster_push_, 64, draw.uv.m00);
		write_f32(caster_push_, 68, draw.uv.m01);
		write_f32(caster_push_, 72, draw.uv.m10);
		write_f32(caster_push_, 76, draw.uv.m11);
		write_f32(caster_push_, 80, draw.uv.m20);
		write_f32(caster_push_, 84, draw.uv.m21);
		write_f32(caster_push_, 88, draw.alpha_scale);
		uint32_t flags = static_cast<uint32_t>(draw.alpha_ref) << 8u;
		if (draw.alpha_texture != nullptr) flags |= 1u;
		if (draw.alpha_test_enabled) flags |= 2u;
		if (draw.alpha_test_inverted) flags |= 4u;
		if (draw.transforms_uv) flags |= 8u;
		write_u32(caster_push_, 92, flags);
		const uint32_t vertex_count = draw.triangle_count * 3u;
		vertex_offsets_[0] = static_cast<int64_t>(
									 geometry->first_vertex[draw.lod_index][draw.surface_index]) *
				kVertexStride;
		vertex_buffers_[0] = geometry->vertices;
		rd_->draw_list_bind_render_pipeline(list,
				caster_pipelines_[blend_group(draw.blend)][draw.two_sided ? 1 : 0]);
		rd_->draw_list_bind_uniform_set(list, texture_set, 0);
		rd_->draw_list_bind_vertex_buffers_format(list, vertex_format_, vertex_count,
				vertex_buffers_, vertex_offsets_);
		rd_->draw_list_set_push_constant(list, caster_push_, kCasterPushBytes);
		rd_->draw_list_draw(list, false, 1);
		++recorded_draws_;
		recorded_triangles_ += draw.triangle_count;
	}
	rd_->draw_list_end();
	if (!ok) return false;
	// The layer's alpha from the temp's boxes; its RGB as uploaded.
	if (!dispatch(resolve_pipeline_, resolve_set_, p_layer, page_dimension_)) return false;
	++recorded_jobs_;
	return true;
}

void TerrainStaticShadowDevicePass::prune() {
	// A geometry no caster holds any more is held only by this cache (its entry and the
	// texture entries its materials own).
	std::unordered_map<const opennova::terrain::TerrainStaticShadowResolvedGeometry *, long> holds;
	for (const auto &[key, entry] : geometries_) holds[key] = 1;
	for (const auto &[key, entry] : textures_) ++holds[entry.owner.get()];
	for (auto it = textures_.begin(); it != textures_.end();) {
		if (it->second.owner.use_count() <= holds[it->second.owner.get()]) {
			free_rid(it->second.uniform_set);
			free_rid(it->second.texture);
			it = textures_.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = geometries_.begin(); it != geometries_.end();) {
		if (it->second.geometry.use_count() <= 1) {
			free_rid(it->second.vertices);
			it = geometries_.erase(it);
		} else {
			++it;
		}
	}
}

void TerrainStaticShadowDevicePass::release() {
	for (auto &[key, entry] : textures_) {
		free_rid(entry.uniform_set);
		free_rid(entry.texture);
	}
	textures_.clear();
	for (auto &[key, entry] : geometries_) free_rid(entry.vertices);
	geometries_.clear();
	free_rid(blank_set_);
	free_rid(blank_texture_);
	free_rid(sampler_);
	free_rid(fill_set_);
	free_rid(resolve_set_);
	free_rid(fill_pipeline_);
	free_rid(resolve_pipeline_);
	for (auto &group : caster_pipelines_) {
		for (RID &pipeline : group) free_rid(pipeline);
	}
	free_rid(fill_shader_);
	free_rid(resolve_shader_);
	free_rid(caster_shader_);
	free_rid(framebuffer_);
	free_rid(temp_depth_);
	free_rid(temp_color_);
	vertex_format_ = -1;
	page_array_ = RID();
	page_dimension_ = 0;
	temp_dimension_ = 0;
	prepared_ = false;
	rd_ = nullptr;
}

} // namespace godot
