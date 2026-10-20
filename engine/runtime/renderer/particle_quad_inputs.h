#pragma once

// The per-particle inputs of retail's three billboard builders: what one live
// particle hands its quad before renderer::ParticleFrameCompiler
// (particle_frame.h) sorts and emits it. The camera-facing builder
// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60], the YAWANDPITCH one
// [orig: CParticleEmitter_RenderStaticBillboards @ 0x5f4e10] and the TOPALIGN
// one [orig: CParticleEmitter_RenderTopAlignedBillboards @ 0x5f5640] read the
// same per-particle block: the colour and alpha LUT reads, the scale LUT lerp,
// the manager tint, the flipbook clock and (TOPALIGN) the heading roll.
//
// The embedder keeps what is its own: the atlas lookup of the returned frame
// index among the frames it registered, the view basis it reads off its
// camera, and the snapshot layout and the upload.

#include <array>
#include <cstdint>

#include <runtime/particle/emitter.h>
#include <runtime/renderer/particle_frame.h>

namespace opennova::renderer {

// The manager's two per-frame tints, channel multipliers with 1.0 for the
// retail byte 128 [orig: Render_EmitterEffect @ 0x5f70c0 — the manager's
// +0x3E8 = g_EnvTerrainLightCombined @ 0x5f7163, +0x3F0 = the modulator block
// doubled and saturated @ 0x5f7179..0x5f71c3]. The embedder derives both from
// the frame's environment.
struct ParticleQuadTints {
	std::array<float, 3> ambient{1.0f, 1.0f, 1.0f};
	std::array<float, 3> modulator{1.0f, 1.0f, 1.0f};
};

// Whether a definition's emitters take the ambient tint: AMBIENTCOLOR
// authored, or forced by any declared graphic of blend mode 0 (Blend), which
// Initialize ORs into the def flags [orig: CEffectEmitter_Initialize
// @ 0x5e6038..0x5e605e — `or [def+0x88], 0x4000000` per blend-0 graphic].
// Read once per definition (the embedder's catalog), not per particle.
bool particle_def_ambient_tinted(const particle::ParticleDef &definition);

struct ParticleQuadInputs {
	// The quad's half extent: base size x the scale curve x 0.5.
	float half_extent = 0.0f;
	// DIFFUSE and SPECULAR as D3DCOLOR ARGB. A LitColor (Bump/Bumpadd) quad
	// carries the view-lit light in DIFFUSE and its tinted colour in
	// SPECULAR; every other quad its tinted colour in DIFFUSE.
	std::uint32_t primary_color = 0xffffffffu;
	std::uint32_t secondary_color = 0xffffffffu;
	// LitColor quads only: the alpha byte the lit DIFFUSE carries, so another
	// view can relight the quad (particle_lit_primary_color) for its basis.
	bool lit = false;
	std::uint8_t lit_alpha = 255;
	// The flipbook frame, 0-based; the embedder wraps it over the frames it
	// registered for the graphic.
	int frame = 0;
	// The quad roll, radians: the particle's own, or the TOPALIGN heading.
	float roll = 0.0f;
	ParticleAlignment alignment = ParticleAlignment::CameraFacing;
};

// One particle's quad inputs. `graphic` is the layer the particle drew at
// spawn (its own curves, the definition's being no fallback),
// `ambient_tinted` the definition's particle_def_ambient_tinted, and
// `view_rows` the row-major world-to-view basis of the view the quad is built
// for (the LitColor light and the camera-facing TOPALIGN heading read it).
ParticleQuadInputs particle_quad_inputs(const particle::Particle &particle,
		const particle::ParticleDef &definition, const particle::GraphicLayer &graphic,
		bool ambient_tinted, const ParticleQuadTints &tints,
		const std::array<float, 9> &view_rows);

}  // namespace opennova::renderer
