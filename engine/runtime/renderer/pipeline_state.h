#pragma once

// The fixed-function device state a draw runs under, as the D3D9 render-state
// words retail sets: the framebuffer blend, the cull mode and the depth test.
// Each renderer module describes its own draws' states with these (the
// particle pipelines, the Q3 bloom-source copies, the slot captures, the
// scene overlay tail); an embedding device makes one pipeline per state and
// translates the words into its own (godot/src/render/rd_pipeline_state).
// No Godot dependencies.
//
// The device takes the cull mode and the depth words per pass: CULLMODE NONE
// for a two-sided pass, else CCW (CW while the mirror flag flips the winding);
// ZWRITEENABLE off for a no-write pass; ZFUNC ALWAYS for a pass that does not
// read depth, else LESSEQUAL
// [orig: CGfxShader_ApplyPass @ 0x683190 - the CULLMODE pick @ 0x6832a8..0x6832be;
//  CRenderBatchQueue_FlushBatches @ 0x5d9f50].

#include <cstdint>

namespace opennova::renderer {

// D3DBLEND: a framebuffer blend factor, by the device's word.
enum class BlendFactor : std::uint8_t {
	Zero = 1,
	One = 2,
	SrcColor = 3,
	InvSrcColor = 4,
	SrcAlpha = 5,
	InvSrcAlpha = 6,
	DestAlpha = 7,
	InvDestAlpha = 8,
	DestColor = 9,
	InvDestColor = 10,
};

// ALPHABLENDENABLE with its SRCBLEND / DESTBLEND pair (the D3D9 defaults:
// blending off, ONE / ZERO). A device with separate colour and alpha slots
// takes the alpha pair here: D3D9's SRCCOLOR / DESTCOLOR factors are
// component-wise, so their alpha component is the source / destination alpha,
// which a colour factor cannot name in an alpha slot (D3D12 rejects one there).
// With blending off the device writes the source and reads no factor.
struct BlendState {
	bool enabled = false;
	BlendFactor src_color = BlendFactor::One;
	BlendFactor dst_color = BlendFactor::Zero;
	BlendFactor src_alpha = BlendFactor::One;
	BlendFactor dst_alpha = BlendFactor::Zero;
};

// D3DCULL: which winding the device drops. CCW drops the faces whose screen
// winding is counter-clockwise, the back faces of D3D's clockwise front.
enum class CullMode : std::uint8_t {
	None = 1,
	CW = 2,
	CCW = 3,
};

// D3DCMP: the depth function (ZFUNC), on retail's forward depth (nearer is
// smaller).
enum class DepthCompare : std::uint8_t {
	LessEqual = 4,
	Always = 8,
};

// One draw's fixed-function state. The D3D9 defaults: CULLMODE CCW, the depth
// test on, ZWRITEENABLE on, ZFUNC LESSEQUAL.
struct FixedFunctionState {
	BlendState blend{};
	CullMode cull = CullMode::CCW;
	bool depth_test = true;
	bool depth_write = true;
	DepthCompare depth_compare = DepthCompare::LessEqual;
};

// One word per distinct state, for a device's pipeline cache.
inline constexpr std::uint32_t fixed_function_state_key(const FixedFunctionState &state) {
	return (state.blend.enabled ? 1u : 0u) |
			static_cast<std::uint32_t>(state.blend.src_color) << 1 |
			static_cast<std::uint32_t>(state.blend.dst_color) << 5 |
			static_cast<std::uint32_t>(state.blend.src_alpha) << 9 |
			static_cast<std::uint32_t>(state.blend.dst_alpha) << 13 |
			static_cast<std::uint32_t>(state.cull) << 17 |
			(state.depth_test ? 1u : 0u) << 19 |
			(state.depth_write ? 1u : 0u) << 20 |
			static_cast<std::uint32_t>(state.depth_compare) << 21;
}

} // namespace opennova::renderer
