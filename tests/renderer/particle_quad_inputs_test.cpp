// renderer::particle_quad_inputs, the per-particle inputs of retail's billboard
// builders (CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 and its two
// siblings; docs/particles/ptl-format-re.md), pinned on witnessed values: the
// scale blend's weights (D-PTL-35) and the colour curves' and tint's
// truncations (D-PTL-36) each have a vector off their midpoint.

#include <runtime/renderer/particle_quad_inputs.h>

#include <runtime/renderer/particle_color.h>

#include <cmath>
#include <cstdio>

namespace {

namespace r = opennova::renderer;
namespace p = opennova::particle;

int g_failures = 0;

void check(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= 1.0e-6f;
}

constexpr std::array<float, 9> kIdentityView{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

void bake(p::CurveRef &curve, std::uint8_t fill) {
	curve.present = true;
	curve.baked = true;
	curve.baked_lut.fill(fill);
}

p::Particle white_particle() {
	p::Particle particle;
	particle.size = 4.0f;
	particle.color = {255, 255, 255};
	particle.alpha = 255;
	return particle;
}

std::uint8_t byte_at(std::uint32_t argb, int shift) {
	return static_cast<std::uint8_t>((argb >> shift) & 0xffu);
}

// The colour and alpha curves read the raw byte at (int)phase % 256, no lerp,
// and scale the channel by byte / 256: retail `(lut[i] * c) >> 8`
// (BuildBillboardQuads @ 0x5e7012..0x5e70d2). 255 under 128 is 127 both ways.
void colour_luts_read_the_raw_byte_at_the_truncated_phase() {
	p::ParticleDef definition;
	p::GraphicLayer graphic;
	graphic.present = true;
	bake(graphic.alpha_func, 0);
	bake(graphic.red_func, 0);
	graphic.alpha_func.baked_lut[10] = 128;
	graphic.alpha_func.baked_lut[11] = 255;  // the next byte is never lerped in
	graphic.red_func.baked_lut[10] = 128;
	p::Particle particle = white_particle();
	particle.flags = p::particle_runtime_flag::AlphaCurve | p::particle_runtime_flag::RedCurve;
	particle.curve_phase = 10.75f;
	const r::ParticleQuadInputs out =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(byte_at(out.primary_color, 24) == 127, "alpha = (128 * 255) >> 8 at phase 10.75");
	check(byte_at(out.primary_color, 16) == 127, "red = (128 * 255) >> 8");
	check(byte_at(out.primary_color, 8) == 255 && byte_at(out.primary_color, 0) == 255,
			"green and blue carry no curve bit");
	check(out.secondary_color == 0xffffffffu && !out.lit, "a plain quad's SPECULAR is white");

	// The index wraps mod 256 (`and 800000FFh` @ 0x5e6ff9).
	particle.curve_phase = 266.25f;
	const r::ParticleQuadInputs wrapped =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(wrapped.primary_color == out.primary_color, "phase 266 reads index 10");

	// A curve bit with no baked LUT leaves the channel; a graphic without its
	// own LUT reads the definition's.
	p::GraphicLayer bare;
	bare.present = true;
	p::ParticleDef with_def_curve;
	bake(with_def_curve.alpha_func, 64);
	particle.curve_phase = 3.0f;
	const r::ParticleQuadInputs from_def =
			r::particle_quad_inputs(particle, with_def_curve, bare, false, {}, kIdentityView);
	check(byte_at(from_def.primary_color, 24) == 63, "the def's LUT: (64 * 255) >> 8 = 63");
	check(byte_at(from_def.primary_color, 16) == 255, "red's bit with no LUT anywhere is 1.0");
	bake(bare.alpha_func, 128);
	const r::ParticleQuadInputs own =
			r::particle_quad_inputs(particle, with_def_curve, bare, false, {}, kIdentityView);
	check(byte_at(own.primary_color, 24) == 127, "the graphic's own LUT wins over the def's");
}

// The scale curve lerps two bytes by the phase fraction, / 128, then x 0.5
// into the half extent (@ 0x5e70d4..0x5e7110, @ 0x5e7183), the NEXT byte
// weighted by 1 - frac (@ 0x5e7100..0x5e710a, D-PTL-35).
void scale_lut_lerps_into_the_half_extent() {
	p::ParticleDef definition;
	p::GraphicLayer graphic;
	graphic.present = true;
	bake(graphic.scale_func, 0);
	graphic.scale_func.baked_lut[20] = 64;
	graphic.scale_func.baked_lut[21] = 192;
	graphic.scale_func.baked_lut[255] = 200;
	p::Particle particle = white_particle();
	particle.flags = p::particle_runtime_flag::ScaleCurve;
	particle.curve_phase = 20.5f;
	const r::ParticleQuadInputs mid =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(near(mid.half_extent, 2.0f), "(64 + 192) / 2 / 128 x 4 x 0.5 = 2");

	// The last sample: retail reads one byte past the table, the port clamps
	// to lut[255] (D-PTL-11).
	particle.curve_phase = 255.5f;
	const r::ParticleQuadInputs last =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(near(last.half_extent, 4.0f * 200.0f / 128.0f * 0.5f), "lut[255] clamps the i + 1 read");

	// Off the midpoint: lut[i+1] by 1 - frac, lut[i] by frac, 192 x 0.75 +
	// 64 x 0.25 = 160 -> 160 / 128 x 4 x 0.5 = 2.5 (D-PTL-35; the swapped
	// weights gave 96 -> 1.5). At phase 20.0 the blend reads lut[21] whole.
	particle.curve_phase = 20.25f;
	const r::ParticleQuadInputs quarter =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(near(quarter.half_extent, 2.5f), "retail's blend weights at a quarter phase (D-PTL-35)");
	particle.curve_phase = 20.0f;
	const r::ParticleQuadInputs whole =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(near(whole.half_extent, 3.0f), "frac 0 reads the next byte: 192 / 128 x 4 x 0.5");

	particle.flags = 0;
	const r::ParticleQuadInputs unscaled =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(near(unscaled.half_extent, 2.0f), "no ScaleCurve bit: size x 0.5");
}

// The manager tint `(c * tint) >> 7`, saturated, picked by the definition's
// AMBIENTCOLOR bit (AdvanceFrame @ 0x5e65f6..0x5e661c; BuildBillboardQuads
// @ 0x5e7116..0x5e717e): 255 under 64 is 127, under 32 is 63, under 256 is 255.
void the_tint_pick_and_its_saturation() {
	p::ParticleDef definition;
	p::GraphicLayer graphic;
	graphic.present = true;
	const p::Particle particle = white_particle();
	r::ParticleQuadTints tints;
	tints.ambient = {0.5f, 2.0f, 1.0f};
	tints.modulator = {0.25f, 1.0f, 1.0f};
	const r::ParticleQuadInputs ambient =
			r::particle_quad_inputs(particle, definition, graphic, true, tints, kIdentityView);
	check(byte_at(ambient.primary_color, 16) == 127, "ambient red: (255 * 64) >> 7 = 127");
	check(byte_at(ambient.primary_color, 8) == 255, "ambient green saturates at 255");
	check(byte_at(ambient.primary_color, 24) == 255, "alpha takes no tint");
	const r::ParticleQuadInputs modulated =
			r::particle_quad_inputs(particle, definition, graphic, false, tints, kIdentityView);
	check(byte_at(modulated.primary_color, 16) == 63, "modulator red: (255 * 32) >> 7 = 63");

	// A curved channel under a tint truncates once in the port: 1.0 x 0.5 x
	// 1.5 = 0.75 -> 191, where retail truncates twice, ((128 x 255) >> 8) =
	// 127, (127 x 192) >> 7 = 190 (D-PTL-X2, OPEN: porting it updates this).
	p::Particle curved = white_particle();
	curved.flags = p::particle_runtime_flag::RedCurve;
	p::GraphicLayer red_curve;
	red_curve.present = true;
	bake(red_curve.red_func, 128);
	r::ParticleQuadTints one_and_a_half;
	one_and_a_half.modulator = {1.5f, 1.0f, 1.0f};
	const r::ParticleQuadInputs tinted_curve = r::particle_quad_inputs(
			curved, definition, red_curve, false, one_and_a_half, kIdentityView);
	check(byte_at(tinted_curve.primary_color, 16) == 191,
			"the port's one truncation under a 1.5 tint (D-PTL-X2)");

	// Which definitions take the ambient tint (Initialize @ 0x5e6038..0x5e605e).
	p::ParticleDef authored;
	authored.flags = p::particle_flag::AmbientColor;
	check(r::particle_def_ambient_tinted(authored), "AMBIENTCOLOR authored");
	p::ParticleDef additive;
	additive.graphics[0].present = true;
	additive.graphics[0].blend_mode = p::BlendMode::Additive;
	check(!r::particle_def_ambient_tinted(additive), "an additive-only def takes the modulator");
	// A declared Blend graphic forces it; an undeclared slot (blend id 0 by
	// default) does not.
	additive.graphics[2].blend_mode = p::BlendMode::Blend;
	check(!r::particle_def_ambient_tinted(additive), "an undeclared graphic is no graphic");
	additive.graphics[1].present = true;
	additive.graphics[1].blend_mode = p::BlendMode::Blend;
	check(r::particle_def_ambient_tinted(additive), "a declared blend-0 graphic forces AMBIENTCOLOR");
}

// The flipbook clock (256 / phase_rate) x flip_rate x phase / 64 = 4 x
// flip_rate x elapsed seconds, wrapped over the frames (@ 0x5e6f17..0x5e6f45),
// GFXFLIPRAND's start from the serial (D-PTL-10).
void the_flipbook_clock() {
	p::ParticleDef definition;
	p::GraphicLayer graphic;
	graphic.present = true;
	graphic.flip_frames = 5;
	graphic.flip_rate = 2;
	p::Particle particle = white_particle();
	particle.phase_rate = 128.0f;  // a two-second life
	particle.curve_phase = 64.0f;  // 0.5 s: 4 x 2 x 0.5 = 4
	check(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).frame == 4,
			"frame 4 at half a second");
	particle.curve_phase = 80.0f;  // 0.625 s: 5, wrapped to 0
	check(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).frame == 0,
			"the clock wraps over the frame count");
	definition.flags = p::particle_flag::GfxFlipRand;
	particle.serial = 3;  // start (3 x 9) % 5 = 2
	particle.curve_phase = 64.0f;
	check(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).frame == 1,
			"GFXFLIPRAND starts at the serial's frame: (2 + 4) % 5");
	graphic.flip_frames = 1;
	check(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).frame == 0,
			"one frame never flips");
}

// TOPALIGN overwrites the roll with the heading measured in the view (or, for
// a YAWANDPITCH quad, its own yaw/pitch frame): atan2(-x, y)
// (RenderTopAlignedBillboards @ 0x5f587f..0x5f59c7).
void the_topalign_roll() {
	p::ParticleDef definition;
	p::GraphicLayer graphic;
	graphic.present = true;
	p::Particle particle = white_particle();
	particle.rotation = 0.3f;
	check(near(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).roll,
				  0.3f),
			"a plain def keeps the particle's roll");
	definition.flags = p::particle_flag::TopAlign;
	particle.velocity = {1.0f, 0.0f, 0.0f};
	check(near(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).roll,
				  -1.5707964f),
			"heading right: -pi/2");
	particle.velocity = {0.0f, 0.0f, 1.0f};
	check(near(r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView).roll,
				  0.3f),
			"a heading along the view axis keeps the roll");
	// The view basis turns world +y into view +x.
	const std::array<float, 9> turned{0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	particle.velocity = {0.0f, 1.0f, 0.0f};
	check(near(r::particle_quad_inputs(particle, definition, graphic, false, {}, turned).roll,
				  -1.5707964f),
			"the camera-facing heading is measured in the view");

	definition.flags = p::particle_flag::TopAlign | p::particle_flag::YawAndPitch;
	particle.yaw = 1.5707964f;
	particle.pitch = 0.0f;
	particle.velocity = {0.0f, 0.0f, 1.0f};
	const r::ParticleQuadInputs world =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, kIdentityView);
	check(world.alignment == r::ParticleAlignment::WorldOriented, "YAWANDPITCH is world-oriented");
	check(near(world.roll, 1.5707964f), "the world-oriented heading in its own yaw frame");
}

// LitColor (Bump/Bumpadd): DIFFUSE the view-lit light under the colour's
// alpha, SPECULAR the colour with alpha 255 (@ 0x5e737c, @ 0x5e7487..0x5e74a3).
void lit_quads_carry_the_light_and_the_colour() {
	p::ParticleDef definition;
	definition.bump_scale = 1.5f;
	p::GraphicLayer graphic;
	graphic.present = true;
	bake(graphic.alpha_func, 128);
	p::Particle particle = white_particle();
	particle.flags = p::particle_runtime_flag::LitColor | p::particle_runtime_flag::AlphaCurve;
	particle.rotation = 0.25f;
	const std::array<float, 9> view{0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f};
	const r::ParticleQuadInputs out =
			r::particle_quad_inputs(particle, definition, graphic, false, {}, view);
	check(out.lit && out.lit_alpha == 127, "the lit DIFFUSE keeps the curved alpha");
	check(out.primary_color == r::particle_lit_primary_color(1.5f, 0.25f, 127, view),
			"DIFFUSE is the view-lit light");
	check(out.secondary_color == 0xffffffffu, "SPECULAR: the white colour, alpha forced");
}

}  // namespace

int main() {
	colour_luts_read_the_raw_byte_at_the_truncated_phase();
	scale_lut_lerps_into_the_half_extent();
	the_tint_pick_and_its_saturation();
	the_flipbook_clock();
	the_topalign_roll();
	lit_quads_carry_the_light_and_the_colour();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("particle_quad_inputs: ok");
	return 0;
}
