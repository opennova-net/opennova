#include "render/rd_pipeline_state.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

namespace {

namespace renderer = opennova::renderer;
using renderer::BlendFactor;
using renderer::CullMode;
using renderer::DepthCompare;

RenderingDevice::BlendFactor blend_factor(BlendFactor p_factor) {
	switch (p_factor) {
		case BlendFactor::Zero:
			return RenderingDevice::BLEND_FACTOR_ZERO;
		case BlendFactor::One:
			return RenderingDevice::BLEND_FACTOR_ONE;
		case BlendFactor::SrcColor:
			return RenderingDevice::BLEND_FACTOR_SRC_COLOR;
		case BlendFactor::InvSrcColor:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case BlendFactor::SrcAlpha:
			return RenderingDevice::BLEND_FACTOR_SRC_ALPHA;
		case BlendFactor::InvSrcAlpha:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case BlendFactor::DestAlpha:
			return RenderingDevice::BLEND_FACTOR_DST_ALPHA;
		case BlendFactor::InvDestAlpha:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case BlendFactor::DestColor:
			return RenderingDevice::BLEND_FACTOR_DST_COLOR;
		case BlendFactor::InvDestColor:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	}
	return RenderingDevice::BLEND_FACTOR_ONE;
}

// The stream keeps D3D's winding: Godot's clockwise front faces are retail's,
// so CCW's back-face cull is Godot's back-face cull.
RenderingDevice::PolygonCullMode cull_mode(CullMode p_cull) {
	switch (p_cull) {
		case CullMode::None:
			return RenderingDevice::POLYGON_CULL_DISABLED;
		case CullMode::CW:
			return RenderingDevice::POLYGON_CULL_FRONT;
		case CullMode::CCW:
			return RenderingDevice::POLYGON_CULL_BACK;
	}
	return RenderingDevice::POLYGON_CULL_DISABLED;
}

// LESSEQUAL on retail's forward depth is GREATER_OR_EQUAL on Godot's reversed
// depth (nearer = greater).
RenderingDevice::CompareOperator compare_operator(DepthCompare p_compare) {
	switch (p_compare) {
		case DepthCompare::LessEqual:
			return RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		case DepthCompare::Always:
			return RenderingDevice::COMPARE_OP_ALWAYS;
	}
	return RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
}

} // namespace

RdPipelineState rd_pipeline_state(const opennova::renderer::FixedFunctionState &p_state) {
	RdPipelineState out;
	out.rasterization.instantiate();
	out.rasterization->set_cull_mode(cull_mode(p_state.cull));
	out.depth_stencil.instantiate();
	out.depth_stencil->set_enable_depth_test(p_state.depth_test);
	out.depth_stencil->set_enable_depth_write(p_state.depth_write);
	out.depth_stencil->set_depth_compare_operator(compare_operator(p_state.depth_compare));
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend(p_state.blend.enabled);
	// With blending off the device reads no factor; they keep their defaults.
	if (p_state.blend.enabled) {
		attachment->set_src_color_blend_factor(blend_factor(p_state.blend.src_color));
		attachment->set_dst_color_blend_factor(blend_factor(p_state.blend.dst_color));
		attachment->set_src_alpha_blend_factor(blend_factor(p_state.blend.src_alpha));
		attachment->set_dst_alpha_blend_factor(blend_factor(p_state.blend.dst_alpha));
	}
	attachment->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
	attachment->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
	attachments.push_back(attachment);
	out.color_blend.instantiate();
	out.color_blend->set_attachments(attachments);
	return out;
}

} // namespace godot
