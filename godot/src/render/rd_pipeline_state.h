#pragma once

// The RenderingDevice states of an engine fixed-function state
// (runtime/renderer/pipeline_state.h): the device leg every RD render
// pipeline of an engine draw list makes its rasterization, depth-stencil and
// colour-blend states through. The sample count and the shader stay the
// caller's.

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>

#include <runtime/renderer/pipeline_state.h>

namespace godot {

struct RdPipelineState {
	Ref<RDPipelineRasterizationState> rasterization;
	Ref<RDPipelineDepthStencilState> depth_stencil;
	Ref<RDPipelineColorBlendState> color_blend; // one attachment
};

RdPipelineState rd_pipeline_state(const opennova::renderer::FixedFunctionState &p_state);

} // namespace godot
