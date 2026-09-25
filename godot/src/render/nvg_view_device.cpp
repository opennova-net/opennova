#include "render/nvg_view_device.h"
#include "render/rd_glsl.h"
#include "render/rd_uniforms.h"
#include "util/string_convert.h"

#include <array>
#include <string>

#include <runtime/renderer/frame_fx_effects.h>

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

namespace {

namespace r = opennova::renderer;

constexpr std::uint32_t kPushBytes = 32u;
constexpr std::uint32_t kVertexBytes = 32u;
constexpr int kStrip = r::kNvgLensStripVertices;
// The storage buffer: the eight polar strips, the lens's twenty band strips
// (pass-major), then the ring.
constexpr int kPolarFirst = 0;
constexpr int kLensFirst = r::kNvgPolarPasses * kStrip;
constexpr int kRingFirst = kLensFirst + r::kNvgLensPasses * r::kNvgLensBands * kStrip;
constexpr int kVertexCount = kRingFirst + kStrip;

// The fragment modes (pc.mode).
constexpr int kModePolar = 0;
constexpr int kModeTint = 1;
constexpr int kModeGlow = 2;
constexpr int kModeRing = 3;
constexpr int kModeSights = 4;
constexpr int kModeSightsAlphaTest = 5;

const char *kLensVertexShader = R"GLSL(#version 450
struct LensVertex {
	vec2 position;
	vec2 uv0;
	vec2 uv1;
	uint argb;
	uint pad;
};
layout(set = 1, binding = 0, std430) restrict readonly buffer Vertices {
	LensVertex vertices[];
};
layout(push_constant, std430) uniform LensPush {
	vec2 ndc_scale;
	int mode;
	int first;
	vec4 rect;
} pc;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv0;
layout(location = 2) out vec2 v_uv1;
void main() {
	vec2 position;
	if (pc.mode >= 4) {
		// A card row's quad: corners from the rect, the whole texture.
		vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
		position = mix(pc.rect.xy, pc.rect.zw, corner);
		v_color = vec4(1.0);
		v_uv0 = corner;
		v_uv1 = corner;
	} else {
		LensVertex v = vertices[pc.first + gl_VertexIndex];
		position = v.position;
		vec4 bgra = unpackUnorm4x8(v.argb);
		v_color = vec4(bgra.z, bgra.y, bgra.x, bgra.w);
		v_uv0 = v.uv0;
		v_uv1 = v.uv1;
	}
	// A pre-transformed D3D9 vertex: pixel centres sit on integers.
	gl_Position = vec4((position + vec2(0.5)) * pc.ndc_scale - vec2(1.0), 0.0, 1.0);
}
)GLSL";

// The four stages: the polar ps (4 x luma x diffuse), the NVG tint ps, the
// glow's MODULATE2X(TEXTURE, DIFFUSE), and the ring's two MODULATE2X stages
// (each saturating, as a fixed-function stage does).
const char *kLensFragmentShaderTemplate = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant, std430) uniform LensPush {
	vec2 ndc_scale;
	int mode;
	int first;
	vec4 rect;
} pc;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv0;
layout(location = 2) in vec2 v_uv1;
layout(location = 0) out vec4 frag_color;
const vec3 POLAR_LUMA = @POLAR_LUMA@;
const float POLAR_GAIN = @POLAR_GAIN@;
const vec3 NVG_TINT_DOT = @NVG_TINT_DOT@;
const vec3 NVG_TINT_SCALE = @NVG_TINT_SCALE@;
const vec3 NVG_TINT_BIAS = @NVG_TINT_BIAS@;
void main() {
	if (pc.mode >= 4) {
		// A card row: the texel through its row's blend; the alpha-tested
		// modes pass above 128 (CGfxDevice_SetAlphaTestRef(128), GREATER).
		vec4 row = texture(source, v_uv0);
		if (pc.mode == 5 && row.a <= 128.0 / 255.0)
			discard;
		frag_color = row;
		return;
	}
	vec3 texel = texture(source, v_uv0).rgb;
	if (pc.mode == 0) {
		float l = dot(texel, POLAR_LUMA);
		frag_color = clamp(POLAR_GAIN * l * v_color, 0.0, 1.0);
	} else if (pc.mode == 1) {
		float d = dot(texel, NVG_TINT_DOT);
		frag_color = vec4(clamp(d * NVG_TINT_SCALE + NVG_TINT_BIAS, 0.0, 1.0), 0.0);
	} else if (pc.mode == 2) {
		frag_color = vec4(clamp(2.0 * texel * v_color.rgb, 0.0, 1.0), 1.0);
	} else {
		vec3 stage0 = clamp(2.0 * texel * v_color.rgb, 0.0, 1.0);
		frag_color = vec4(clamp(2.0 * stage0 * texture(source, v_uv1).rgb, 0.0, 1.0), 1.0);
	}
}
)GLSL";

std::string lens_fragment_source() {
	std::string source(kLensFragmentShaderTemplate);
	splice_token(source, "@POLAR_LUMA@", glsl_vec3(r::kNvgPolarLuma));
	splice_token(source, "@POLAR_GAIN@", glsl_float(r::kNvgPolarGain));
	splice_token(source, "@NVG_TINT_DOT@", glsl_vec3(r::kNvgTintDot));
	splice_token(source, "@NVG_TINT_SCALE@", glsl_vec3(r::kNvgTintScale));
	splice_token(source, "@NVG_TINT_BIAS@", glsl_vec3(r::kNvgTintBias));
	return source;
}

Color color_from_argb(std::uint32_t p_argb) {
	return Color(float((p_argb >> 16) & 0xFFu) / 255.0f, float((p_argb >> 8) & 0xFFu) / 255.0f,
			float(p_argb & 0xFFu) / 255.0f, float((p_argb >> 24) & 0xFFu) / 255.0f);
}

void write_strip(PackedByteArray &p_bytes, int p_first, const r::NvgLensStrip &p_strip) {
	for (std::size_t i = 0; i < p_strip.size(); ++i) {
		const r::NvgLensVertex &v = p_strip[i];
		const std::uint32_t at = static_cast<std::uint32_t>(p_first + static_cast<int>(i)) *
				kVertexBytes;
		write_f32(p_bytes, at + 0, v.x);
		write_f32(p_bytes, at + 4, v.y);
		write_f32(p_bytes, at + 8, v.u0);
		write_f32(p_bytes, at + 12, v.v0);
		write_f32(p_bytes, at + 16, v.u1);
		write_f32(p_bytes, at + 20, v.v1);
		write_u32(p_bytes, at + 24, v.argb);
		write_u32(p_bytes, at + 28, 0u);
	}
}

} // namespace

NvgViewDevice::NvgViewDevice() = default;

NvgViewDevice::~NvgViewDevice() = default;

void NvgViewDevice::release(RenderingDevice *p_rd) {
	auto free_rid = [&](RID &p_rid) {
		if (p_rd != nullptr && p_rid.is_valid())
			p_rd->free_rid(p_rid);
		p_rid = RID();
	};
	for (auto &entry : texture_uniforms_) {
		if (p_rd != nullptr && entry.second.is_valid() &&
				p_rd->uniform_set_is_valid(entry.second))
			p_rd->free_rid(entry.second);
	}
	texture_uniforms_.clear();
	if (p_rd != nullptr && vertex_uniform_.is_valid() &&
			p_rd->uniform_set_is_valid(vertex_uniform_))
		p_rd->free_rid(vertex_uniform_);
	vertex_uniform_ = RID();
	for (auto &entry : pipelines_) {
		if (p_rd != nullptr && entry.second.is_valid() &&
				p_rd->render_pipeline_is_valid(entry.second))
			p_rd->free_rid(entry.second);
	}
	pipelines_.clear();
	if (p_rd != nullptr && polar_framebuffer_.is_valid() &&
			p_rd->framebuffer_is_valid(polar_framebuffer_))
		p_rd->free_rid(polar_framebuffer_);
	polar_framebuffer_ = RID();
	free_rid(polar_);
	free_rid(vertices_);
	free_rid(shader_);
	uploaded_lens_.reset();
}

bool NvgViewDevice::initialize(RenderingDevice *p_rd, std::string &r_failure) {
	if (shader_.is_valid() && vertices_.is_valid() && polar_framebuffer_.is_valid() &&
			vertex_uniform_.is_valid())
		return true;
	release(p_rd);
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			String::utf8(kLensVertexShader));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			String::utf8(lens_fragment_source().c_str()));
	Ref<RDShaderSPIRV> spirv = p_rd->shader_compile_spirv_from_source(source);
	if (spirv.is_null() ||
			!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX).is_empty() ||
			!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
		r_failure = "NVG lens shader compilation failed: " +
				(spirv.is_null() ? std::string("no SPIR-V") :
						opennova::to_std(spirv->get_stage_compile_error(
								RenderingDevice::SHADER_STAGE_VERTEX)) + "; " +
								opennova::to_std(spirv->get_stage_compile_error(
										RenderingDevice::SHADER_STAGE_FRAGMENT)));
		return false;
	}
	shader_ = p_rd->shader_create_from_spirv(spirv, "OpenNova NVG lens");
	if (!shader_.is_valid()) {
		r_failure = "RenderingDevice rejected the NVG lens shader";
		return false;
	}
	vertices_ = p_rd->storage_buffer_create(kVertexCount * kVertexBytes);
	if (!vertices_.is_valid()) {
		r_failure = "RenderingDevice could not allocate the NVG lens vertices";
		return false;
	}
	// The polar strips never change: upload them once.
	PackedByteArray polar_bytes;
	polar_bytes.resize(static_cast<int64_t>(kLensFirst) * kVertexBytes);
	const auto polar_strips = r::nvg_polar_unwrap_passes();
	for (int p = 0; p < r::kNvgPolarPasses; ++p)
		write_strip(polar_bytes, kPolarFirst + p * kStrip, polar_strips[static_cast<std::size_t>(p)]);
	if (p_rd->buffer_update(vertices_, 0, static_cast<std::uint32_t>(polar_bytes.size()),
				polar_bytes) != OK) {
		r_failure = "RenderingDevice rejected the NVG polar vertices";
		return false;
	}
	Ref<RDUniform> storage;
	storage.instantiate();
	storage->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	storage->set_binding(0);
	storage->add_id(vertices_);
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(storage);
	vertex_uniform_ = p_rd->uniform_set_create(uniforms, shader_, 1);
	// The polar target: 64 x 16 (retail ViewFx_CreateRenderTargets
	// @0x5cf6c1..0x5cf6de).
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format->set_width(r::kNvgPolarWidth);
	format->set_height(r::kNvgPolarHeight);
	format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT));
	Ref<RDTextureView> view;
	view.instantiate();
	polar_ = p_rd->texture_create(format, view);
	if (polar_.is_valid()) {
		TypedArray<RID> attachments;
		attachments.push_back(polar_);
		polar_framebuffer_ = p_rd->framebuffer_create(attachments);
	}
	push_.resize(kPushBytes);
	if (!vertex_uniform_.is_valid() || !polar_.is_valid() || !polar_framebuffer_.is_valid()) {
		r_failure = "RenderingDevice could not allocate the NVG lens targets";
		return false;
	}
	return true;
}

RID NvgViewDevice::pipeline_for(RenderingDevice *p_rd, int64_t p_format, Blend p_blend,
		bool p_write_alpha) {
	const auto key = std::make_tuple(p_format, p_blend, p_write_alpha);
	const auto found = pipelines_.find(key);
	if (found != pipelines_.end() && p_rd->render_pipeline_is_valid(found->second))
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
	attachment->set_enable_blend(p_blend != Blend::Replace);
	// D3D9 applies the colour factors to alpha by their alpha counterparts.
	auto factors = [&](RenderingDevice::BlendFactor p_src, RenderingDevice::BlendFactor p_dst,
			RenderingDevice::BlendFactor p_src_alpha, RenderingDevice::BlendFactor p_dst_alpha) {
		attachment->set_src_color_blend_factor(p_src);
		attachment->set_dst_color_blend_factor(p_dst);
		attachment->set_src_alpha_blend_factor(p_src_alpha);
		attachment->set_dst_alpha_blend_factor(p_dst_alpha);
	};
	switch (p_blend) {
		case Blend::Replace:
		case Blend::Add:
			factors(RenderingDevice::BLEND_FACTOR_ONE, RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_ONE, RenderingDevice::BLEND_FACTOR_ONE);
			break;
		case Blend::SourceAlphaBlend:
			factors(RenderingDevice::BLEND_FACTOR_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA,
					RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			break;
		case Blend::SourceAlphaAdd:
			factors(RenderingDevice::BLEND_FACTOR_SRC_ALPHA, RenderingDevice::BLEND_FACTOR_ONE,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA, RenderingDevice::BLEND_FACTOR_ONE);
			break;
		case Blend::DestColorSourceColor:
			factors(RenderingDevice::BLEND_FACTOR_DST_COLOR,
					RenderingDevice::BLEND_FACTOR_SRC_COLOR,
					RenderingDevice::BLEND_FACTOR_DST_ALPHA,
					RenderingDevice::BLEND_FACTOR_SRC_ALPHA);
			break;
	}
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_write_a(p_write_alpha);
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	blend->set_attachments(attachments);
	const RID pipeline = p_rd->render_pipeline_create(shader_, p_format,
			RenderingDevice::INVALID_FORMAT_ID, RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
			raster, multisample, depth, blend);
	if (pipeline.is_valid())
		pipelines_[key] = pipeline;
	return pipeline;
}

RID NvgViewDevice::texture_uniform(RenderingDevice *p_rd, const RID &p_texture,
		const RID &p_sampler) {
	const auto key = std::make_tuple(p_texture, p_sampler);
	const auto found = texture_uniforms_.find(key);
	if (found != texture_uniforms_.end() && p_rd->uniform_set_is_valid(found->second))
		return found->second;
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back(sampled_texture_uniform(0, p_sampler, p_texture));
	const RID uniform = p_rd->uniform_set_create(uniforms, shader_, 0);
	texture_uniforms_[key] = uniform;
	return uniform;
}

bool NvgViewDevice::upload_lens(RenderingDevice *p_rd,
		const std::shared_ptr<const opennova::renderer::NvgScopeLens> &p_lens,
		std::string &r_failure) {
	if (p_lens == uploaded_lens_)
		return true;
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(kVertexCount - kLensFirst) * kVertexBytes);
	for (int pass = 0; pass < r::kNvgLensPasses; ++pass) {
		for (int band = 0; band < r::kNvgLensBands; ++band) {
			write_strip(bytes, (pass * r::kNvgLensBands + band) * kStrip,
					p_lens->passes[static_cast<std::size_t>(pass)][static_cast<std::size_t>(band)]);
		}
	}
	write_strip(bytes, kRingFirst - kLensFirst, p_lens->ring);
	if (p_rd->buffer_update(vertices_, static_cast<std::uint32_t>(kLensFirst) * kVertexBytes,
				static_cast<std::uint32_t>(bytes.size()), bytes) != OK) {
		r_failure = "RenderingDevice rejected the NVG lens vertices";
		return false;
	}
	uploaded_lens_ = p_lens;
	return true;
}

void NvgViewDevice::set_push(const Vector2i &p_target, int p_mode, int p_first, float p_x1,
		float p_y1, float p_x2, float p_y2) {
	write_f32(push_, 0, 2.0f / static_cast<float>(p_target.x));
	write_f32(push_, 4, 2.0f / static_cast<float>(p_target.y));
	write_u32(push_, 8, static_cast<std::uint32_t>(p_mode));
	write_u32(push_, 12, static_cast<std::uint32_t>(p_first));
	write_f32(push_, 16, p_x1);
	write_f32(push_, 20, p_y1);
	write_f32(push_, 24, p_x2);
	write_f32(push_, 28, p_y2);
}

bool NvgViewDevice::draw_sights(RenderingDevice *p_rd, const RID &p_scene_framebuffer,
		const Vector2i &p_scene_size, const RID &p_sampler, const std::vector<SightsRow> &p_rows,
		std::size_t &r_draws, std::string &r_failure) {
	if (p_rows.empty())
		return true;
	if (p_rd == nullptr || !initialize(p_rd, r_failure))
		return false;
	const int64_t format = p_rd->framebuffer_get_format(p_scene_framebuffer);
	const int64_t list = p_rd->draw_list_begin(p_scene_framebuffer,
			BitField<RenderingDevice::DrawFlags>(0), PackedColorArray());
	if (list == RenderingDevice::INVALID_ID) {
		r_failure = "RenderingDevice could not begin the NVG card draw list";
		return false;
	}
	p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
	bool ok = true;
	for (const SightsRow &row : p_rows) {
		if (!row.texture.is_valid() || !p_rd->texture_is_valid(row.texture))
			continue;
		// The six modes: BLEND, ADD, BLEND_AT, MULTIPLY (the doubled-source
		// multiply DESTCOLOR x SRC + SRCCOLOR x DST), ADD_AT, MULTIPLY_AT.
		Blend blend = Blend::SourceAlphaBlend;
		switch (row.blend) {
			case 1:
			case 4:
				blend = Blend::SourceAlphaAdd;
				break;
			case 3:
			case 5:
				blend = Blend::DestColorSourceColor;
				break;
			default:
				break;
		}
		const bool alpha_test = row.blend == 2 || row.blend == 4 || row.blend == 5;
		const RID pipeline = pipeline_for(p_rd, format, blend, true);
		const RID uniform = texture_uniform(p_rd, row.texture, p_sampler);
		if (!pipeline.is_valid() || !uniform.is_valid()) {
			ok = false;
			break;
		}
		p_rd->draw_list_bind_render_pipeline(list, pipeline);
		p_rd->draw_list_bind_uniform_set(list, uniform, 0);
		p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
		set_push(p_scene_size, alpha_test ? kModeSightsAlphaTest : kModeSights, 0, row.x1,
				row.y1, row.x2, row.y2);
		p_rd->draw_list_set_push_constant(list, push_, kPushBytes);
		p_rd->draw_list_draw(list, false, 1, 4);
		++r_draws;
	}
	p_rd->draw_list_end();
	if (!ok)
		r_failure = "RenderingDevice rejected an NVG card pipeline or uniform set";
	return ok;
}

bool NvgViewDevice::draw(RenderingDevice *p_rd, const Inputs &p_inputs, std::size_t &r_draws,
		std::string &r_failure) {
	if (p_rd == nullptr || !p_inputs.lens || p_inputs.screen_size.x <= 0 ||
			p_inputs.screen_size.y <= 0) {
		r_failure = "The NVG lens has no geometry for this frame";
		return false;
	}
	if (!initialize(p_rd, r_failure) || !upload_lens(p_rd, p_inputs.lens, r_failure))
		return false;
	const RID scene = texture_uniform(p_rd, p_inputs.scene, p_inputs.clamp_sampler);
	const RID glow = texture_uniform(p_rd, p_inputs.glow, p_inputs.clamp_sampler);
	const RID polar = texture_uniform(p_rd, polar_, p_inputs.repeat_sampler);
	const int64_t polar_format = p_rd->framebuffer_get_format(polar_framebuffer_);
	const int64_t frame_format = p_rd->framebuffer_get_format(p_inputs.frame_framebuffer);
	const RID polar_pipeline = pipeline_for(p_rd, polar_format, Blend::Replace, true);
	const RID replace = pipeline_for(p_rd, frame_format, Blend::Replace, false);
	const RID add = pipeline_for(p_rd, frame_format, Blend::Add, false);
	if (!scene.is_valid() || !glow.is_valid() || !polar.is_valid() ||
			!polar_pipeline.is_valid() || !replace.is_valid() || !add.is_valid()) {
		r_failure = "RenderingDevice rejected an NVG lens pipeline or uniform set";
		return false;
	}

	// The polar unwrap: the target cleared to 0xFFFF0000, then the eight
	// passes, blending off (retail NVG_RenderSceneToTarget @0x5d0a1e..0x5d0e7f).
	PackedColorArray polar_clear;
	polar_clear.push_back(color_from_argb(r::kNvgPolarClearColor));
	const Vector2i polar_size(r::kNvgPolarWidth, r::kNvgPolarHeight);
	int64_t list = p_rd->draw_list_begin(polar_framebuffer_,
			RenderingDevice::DRAW_CLEAR_COLOR_0, polar_clear);
	if (list == RenderingDevice::INVALID_ID) {
		r_failure = "RenderingDevice could not begin the NVG polar draw list";
		return false;
	}
	p_rd->draw_list_bind_render_pipeline(list, polar_pipeline);
	p_rd->draw_list_bind_uniform_set(list, scene, 0);
	p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
	for (int p = 0; p < r::kNvgPolarPasses; ++p) {
		set_push(polar_size, kModePolar, kPolarFirst + p * kStrip);
		p_rd->draw_list_set_push_constant(list, push_, kPushBytes);
		p_rd->draw_list_draw(list, false, 1, kStrip);
		++r_draws;
	}
	p_rd->draw_list_end();

	// The frame: cleared black, the tint pass, the four glow passes, the ring
	// (retail Render_ProcessMainSceneFrame @0x5ca6c1..0x5ca6eb, then
	// NVG_DrawScopedLens @0x5d1f22..0x5d2793).
	PackedColorArray black;
	black.push_back(Color(0.0f, 0.0f, 0.0f, 1.0f));
	list = p_rd->draw_list_begin(p_inputs.frame_framebuffer,
			RenderingDevice::DRAW_CLEAR_COLOR_0, black);
	if (list == RenderingDevice::INVALID_ID) {
		r_failure = "RenderingDevice could not begin the NVG lens draw list";
		return false;
	}
	p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
	for (int pass = 0; pass < r::kNvgLensPasses; ++pass) {
		if (pass <= 1) {
			p_rd->draw_list_bind_render_pipeline(list, pass == 0 ? replace : add);
			p_rd->draw_list_bind_uniform_set(list, pass == 0 ? scene : glow, 0);
			p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
		}
		for (int band = 0; band < r::kNvgLensBands; ++band) {
			set_push(p_inputs.screen_size, pass == 0 ? kModeTint : kModeGlow,
					kLensFirst + (pass * r::kNvgLensBands + band) * kStrip);
			p_rd->draw_list_set_push_constant(list, push_, kPushBytes);
			p_rd->draw_list_draw(list, false, 1, kStrip);
			++r_draws;
		}
	}
	p_rd->draw_list_bind_render_pipeline(list, replace);
	p_rd->draw_list_bind_uniform_set(list, polar, 0);
	p_rd->draw_list_bind_uniform_set(list, vertex_uniform_, 1);
	set_push(p_inputs.screen_size, kModeRing, kRingFirst);
	p_rd->draw_list_set_push_constant(list, push_, kPushBytes);
	p_rd->draw_list_draw(list, false, 1, kStrip);
	++r_draws;
	p_rd->draw_list_end();
	return true;
}

} // namespace godot
