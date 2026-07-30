#pragma once

// The runtime UV-animation transform — the time-driven MatTexCoord1 matrix
// built per material each draw for #UV (TEX_UVXFORM) effects.
// Structural port of [orig: compute_uv_transform_matrix @ 0x5b1990], the sole
// producer of the MatTexCoord1 effect parameter (bound in
// [orig: apply_shader_parameters @ 0x58db80] with
// time_units = (tick_ms << 8) / 1000 — 1/256-second units in a wrapping
// uint16). The waveform bands come from the SAME 2816-byte table the PANM
// part-animation sampler uses (libs/threedi threedi_panm_wave_table()); the
// lookup indexing here is the render-side dialect
// [orig: wave_lookup @ 0x5de6b0].
//
// Channel blocks live at runtime matdef+524 (U) / +532 (V) — copied from the
// .3di material by convert_material_definition @ 0x5b03c0; which MTRL file
// fields feed them is an open threedi mapping question
// (docs/render/render-material-re.md).
//
// Pure C++, deterministic: the controlled-animation table value and the
// noise-waveform random are caller inputs.

#include <cstdint>

namespace renderer {

// One animation channel (8 bytes at matdef+524 / +532):
// type = waveform/mode byte, phase = start phase (high byte), speed = phase
// step per time unit, base/range = signed 8.8 value window (waveform and
// controlled modes only).
struct UvAnimChannel {
	uint8_t type = 0;
	uint8_t phase = 0;
	int16_t speed = 0;
	int16_t base = 0;   // 8.8 signed
	int16_t range = 0;  // 8.8 signed (window END; range = end - base)
};

// [u v 1] * M (the D3D COUNT2 texture transform / CalcAnimatedUV form):
//   u' = u*m00 + v*m10 + m20
//   v' = u*m01 + v*m11 + m21
struct UvAnimTransform {
	float m00 = 1.0f, m01 = 0.0f;
	float m10 = 0.0f, m11 = 1.0f;
	float m20 = 0.0f, m21 = 0.0f;
};

// The render-side waveform lookup [orig: wave_lookup @ 0x5de6b0]: band by
// type & 0xF over the shared 2816-byte table, value returned in 8.8
// (byte << 8; types 7/10 lerp by the phase low byte; type 6 is noise —
// 16 * (rand16 & 0xFFF), rand16 supplied by the caller).
int32_t uv_anim_wave_lookup(uint8_t type, uint16_t phase16, uint16_t rand16);

// Build the transform for one channel pair at a time point.
// time_units16: low 16 bits of (tick_ms << 8) / 1000 [orig: @ 0x58dd49].
// controlled_u/_v: the controlled-animation table value for each channel's
// slot (dword_83FCE8[2*phase] — driven by scripted material animation).
// rand16_u/_v: independent noise samples for waveform type 6. Retail calls
// CRT rand() from wave_lookup once per channel lookup, in U-then-V order.
UvAnimTransform uv_anim_transform(const UvAnimChannel &u_channel,
                                  const UvAnimChannel &v_channel,
                                  uint16_t time_units16,
                                  int32_t controlled_u,
                                  int32_t controlled_v,
                                  uint16_t rand16_u,
                                  uint16_t rand16_v);

} // namespace renderer
