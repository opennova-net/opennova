#include <runtime/renderer/particle_quad_inputs.h>

#include <runtime/renderer/particle_color.h>

#include <algorithm>
#include <cmath>

namespace opennova::renderer {
namespace {

using particle::CurveRef;

// The LUT a channel reads: the graphic's own when it baked one, else the
// definition's (the graphic's LUT pointers as CEffectDef_ResolveAllReferences
// @ 0x5e9d70 resolves them, docs/particles/ptl-format-re.md §2.4).
const CurveRef &resolved_curve(const CurveRef &graphic_curve, const CurveRef &definition_curve) {
	return graphic_curve.baked ? graphic_curve : definition_curve;
}

// Godot's YXZ Euler basis R = Ry(yaw) * Rx(pitch) * Rz(0), evaluated in the
// order and grouping its Basis::from_euler and operator* use, so the
// world-oriented TOPALIGN frame lands on the same floats the presentation
// basis does (the YAWANDPITCH quad's frame, renderer::ParticleFrameCompiler).
using Rows = std::array<float, 9>;

Rows multiply(const Rows &a, const Rows &b) {
	Rows out{};
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 3; ++col) {
			out[row * 3 + col] = b[col] * a[row * 3] + b[3 + col] * a[row * 3 + 1] +
					b[6 + col] * a[row * 3 + 2];
		}
	}
	return out;
}

Rows yaw_pitch_frame(float pitch, float yaw) {
	const float cx = std::cos(pitch), sx = std::sin(pitch);
	const Rows xmat{1.0f, 0.0f, 0.0f, 0.0f, cx, -sx, 0.0f, sx, cx};
	const float cy = std::cos(yaw), sy = std::sin(yaw);
	const Rows ymat{cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
	const float cz = std::cos(0.0f), sz = std::sin(0.0f);
	const Rows zmat{cz, -sz, 0.0f, sz, cz, 0.0f, 0.0f, 0.0f, 1.0f};
	return multiply(multiply(ymat, xmat), zmat);
}

}  // namespace

bool particle_def_ambient_tinted(const particle::ParticleDef &definition) {
	// [orig: CEffectEmitter_Initialize @ 0x5e6038..0x5e605e — every declared
	// graphic whose blend id is 0 ORs AMBIENTCOLOR (0x4000000) into the def
	// flags; CParticleEmitter_AdvanceFrame @ 0x5e65f6 tests that bit].
	if ((definition.flags & particle::particle_flag::AmbientColor) != 0)
		return true;
	for (const particle::GraphicLayer &graphic : definition.graphics) {
		if (graphic.present && graphic.blend_mode == particle::BlendMode::Blend)
			return true;
	}
	return false;
}

ParticleQuadInputs particle_quad_inputs(const particle::Particle &particle,
		const particle::ParticleDef &definition, const particle::GraphicLayer &graphic,
		bool ambient_tinted, const ParticleQuadTints &tints,
		const std::array<float, 9> &view_rows) {
	using namespace particle::particle_runtime_flag;
	ParticleQuadInputs out;

	// The LUT index is the curve phase truncated, mod 256; the scale lerp
	// takes its fraction [orig: CParticleEmitter_BuildBillboardQuads
	// @ 0x5e6fe6..0x5e7006 — ftol of particle+0x30, `and 800000FFh` with the
	// signed fix-up; the same block @ 0x5f50b9..0x5f50d9 and @ 0x5f5a6b..0x5f5a8b
	// in the other two builders]. A phase never runs negative here (it starts
	// at 0 or a positive spawn offset), which the floor at 0 states.
	const float phase = std::max(0.0f, particle.curve_phase);
	const int lut_index = static_cast<int>(phase) & 0xff;
	const float lut_fraction = phase - std::floor(phase);

	// The colour and alpha curves read the RAW byte at the index, no lerp,
	// and scale their channel by byte / 256: retail's channel is the integer
	// `(lut[i] * c) >> 8` on the particle's colour dword, one LUT per bit of
	// the particle's curve flags (alpha +0x1E0, red +0x228, green +0x270, blue
	// +0x2B8) [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e7012..0x5e70d2;
	// CParticleEmitter_RenderStaticBillboards @ 0x5f50e5..0x5f51a5;
	// CParticleEmitter_RenderTopAlignedBillboards @ 0x5f5a97..0x5f5b57]. The
	// port carries the channel in float to the one truncating pack below, so
	// a curved channel under a non-neutral tint can land up to 2 below or
	// above retail's two integer truncations (D-PTL-X2).
	auto channel_multiplier = [&](std::uint32_t flag, const CurveRef &graphic_curve,
										const CurveRef &definition_curve) -> float {
		if ((particle.flags & flag) == 0)
			return 1.0f;
		const CurveRef &curve = resolved_curve(graphic_curve, definition_curve);
		if (!curve.baked)
			return 1.0f;
		return static_cast<float>(curve.baked_lut[static_cast<std::size_t>(lut_index)]) /
				256.0f;
	};
	const float alpha_multiplier =
			channel_multiplier(AlphaCurve, graphic.alpha_func, definition.alpha_func);
	const float red_multiplier =
			channel_multiplier(RedCurve, graphic.red_func, definition.red_func);
	const float green_multiplier =
			channel_multiplier(GreenCurve, graphic.green_func, definition.green_func);
	const float blue_multiplier =
			channel_multiplier(BlueCurve, graphic.blue_func, definition.blue_func);

	// The scale curve lerps two LUT bytes by the phase fraction and scales the
	// base size by the result / 128, then by 0.5 into the half extent
	// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e70d4..0x5e7110 (flt_7C3DD4
	// = 1/128) and @ 0x5e7183 (flt_7C3B94 = 0.5); RenderStaticBillboards
	// @ 0x5f51a7..0x5f51e1; RenderTopAlignedBillboards @ 0x5f5b59..0x5f5b93].
	// Retail reads lut[i + 1] one byte past the table at i = 255; the port
	// clamps to lut[255] (D-PTL-11). Retail weights lut[i + 1] by (1 - frac)
	// and lut[i] by frac (`fld1; fsub` multiplied into the lut[i + 1] load
	// @ 0x5e7100..0x5e7104, frac into lut[i] @ 0x5e710a); the port weights
	// lut[i] by (1 - frac) (D-PTL-X1).
	float scale_multiplier = 1.0f;
	if ((particle.flags & ScaleCurve) != 0) {
		const CurveRef &curve = resolved_curve(graphic.scale_func, definition.scale_func);
		if (curve.baked) {
			const std::uint8_t first_sample = curve.baked_lut[static_cast<std::size_t>(lut_index)];
			const std::uint8_t second_sample =
					curve.baked_lut[static_cast<std::size_t>(std::min(lut_index + 1, 255))];
			const float interpolated = static_cast<float>(first_sample) +
					(static_cast<float>(second_sample) - static_cast<float>(first_sample)) *
							lut_fraction;
			scale_multiplier = interpolated / 128.0f;
		}
	}
	const float size = particle.size * scale_multiplier;
	out.half_extent = size * 0.5f;

	// The manager tint `(c * tint) >> 7`, saturated at 255, on the three
	// colour channels (alpha untinted) [orig: BuildBillboardQuads
	// @ 0x5e7116..0x5e717e over the emitter's +0xC8..+0xCA bytes]: an
	// ambient-tinted definition's emitter holds the terrain light, every other
	// one the doubled modulator [orig: CParticleEmitter_AdvanceFrame
	// @ 0x5e65f6..0x5e661c — the AMBIENTCOLOR test picks the manager's +0x3E8
	// or +0x3F0 into emitter+0xC8].
	const std::array<float, 3> &tint = ambient_tinted ? tints.ambient : tints.modulator;
	constexpr float byte_to_unit = 1.0f / 255.0f;
	float red = static_cast<float>(particle.color.r) * byte_to_unit;
	float green = static_cast<float>(particle.color.g) * byte_to_unit;
	float blue = static_cast<float>(particle.color.b) * byte_to_unit;
	red = std::clamp(red * red_multiplier * tint[0], 0.0f, 1.0f);
	green = std::clamp(green * green_multiplier * tint[1], 0.0f, 1.0f);
	blue = std::clamp(blue * blue_multiplier * tint[2], 0.0f, 1.0f);
	const float alpha = std::clamp(
			static_cast<float>(particle.alpha) * byte_to_unit * alpha_multiplier, 0.0f, 1.0f);

	// The flipbook clock: frame = (start + trunc(4 * flip_rate * elapsed
	// seconds)) mod frames, retail's `(start - ftol((256 / phase_rate) *
	// flip_rate * phase * dbl_7DC730)) % frames` with dbl_7DC730 = -1/64
	// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6f17..0x5e6f45;
	// RenderStaticBillboards @ 0x5f4f84..0x5f4faf; RenderTopAlignedBillboards
	// @ 0x5f57ea..0x5f5815]. GFXFLIPRAND's start is the particle slot pointer's
	// `(p + (p >> 3)) % frames` in retail [orig: @ 0x5e6ef9..0x5e6f15]; the
	// port derives it from the serial (D-PTL-10). The frame count carries
	// D-PTL-19's 1..256 bound. Retail runs the clock whenever the particle's
	// Flipbook bit (frames > 1) is set [orig: @ 0x5e6eec]; the port also skips
	// it at a rate of 0 or below, which no shipped graphic with frames > 1
	// authors (JO:CA's 85 effect files).
	const int flip_frames =
			std::clamp(graphic.flip_frames, 1, particle::kMaxParticleFlipFrames);
	const int flip_rate = std::max(0, graphic.flip_rate);
	if (flip_frames > 1 && flip_rate > 0) {
		int random_offset = 0;
		if ((definition.flags & particle::particle_flag::GfxFlipRand) != 0)
			random_offset = static_cast<int>(particle.serial * 9u) % flip_frames;
		const float elapsed = particle.phase_rate > 0.0f ? phase / particle.phase_rate : 0.0f;
		out.frame = (random_offset +
							static_cast<int>(4.0f * static_cast<float>(flip_rate) * elapsed)) %
				flip_frames;
	}

	// YAWANDPITCH (def flags 0x100) routes the emitter to the world-oriented
	// builder; every other def builds camera-facing quads [orig:
	// CParticleEmitter_RenderStaticBillboards @ 0x5f4e1c..0x5f4e3d — without
	// the bit it tail-calls BuildBillboardQuads].
	out.alignment = (definition.flags & particle::particle_flag::YawAndPitch) != 0 ?
			ParticleAlignment::WorldOriented :
			ParticleAlignment::CameraFacing;
	out.roll = particle.rotation;
	if ((definition.flags & particle::particle_flag::TopAlign) != 0) {
		// TOPALIGN defs run through the rot-head builder, which overwrites the
		// roll every frame so the quad's top follows the particle's heading:
		// the velocity is taken into the view frame, normalized, and the roll
		// is `±acos(dot(dir, up))`, negative when the direction points right
		// [orig: CParticleEmitter_RenderTopAlignedBillboards @ 0x5f5640 — head =
		// pos + vel @ 0x5f587f, normalize @ 0x5f597b, acos and sign
		// @ 0x5f59a0..0x5f59c7]. A world-oriented quad measures against its own
		// yaw/pitch frame.
		float hx = particle.velocity.x, hy = particle.velocity.y, hz = particle.velocity.z;
		if (out.alignment == ParticleAlignment::WorldOriented) {
			const Rows frame = yaw_pitch_frame(particle.pitch, particle.yaw);
			const float x = (frame[0] * hx) + (frame[3] * hy) + (frame[6] * hz);
			const float y = (frame[1] * hx) + (frame[4] * hy) + (frame[7] * hz);
			hx = x;
			hy = y;
		} else {
			const float x = view_rows[0] * hx + view_rows[1] * hy + view_rows[2] * hz;
			const float y = view_rows[3] * hx + view_rows[4] * hy + view_rows[5] * hz;
			hx = x;
			hy = y;
		}
		const float planar = hx * hx + hy * hy;
		if (planar > 1.0e-12f)
			out.roll = std::atan2(-hx, hy);
	}

	// A LitColor (Bump/Bumpadd) quad's DIFFUSE is the view-lit light under the
	// colour's alpha and its SPECULAR the colour with alpha forced to 255; any
	// other quad carries the colour in DIFFUSE [orig: BuildBillboardQuads
	// @ 0x5e737c — the flag-0x80 test; @ 0x5e7487..0x5e74a3 the lit pair;
	// @ 0x5e74b1..0x5e74ba the plain colour] (particle_lit_primary_color
	// carries the light).
	if ((particle.flags & LitColor) != 0) {
		out.lit = true;
		out.lit_alpha = particle_unit_byte(alpha);
		out.primary_color = particle_lit_primary_color(
				definition.bump_scale, particle.rotation, out.lit_alpha, view_rows);
		out.secondary_color = particle_pack_argb(red, green, blue, 1.0f);
	} else {
		out.primary_color = particle_pack_argb(red, green, blue, alpha);
		out.secondary_color = 0xffffffffu;
	}
	return out;
}

}  // namespace opennova::renderer
