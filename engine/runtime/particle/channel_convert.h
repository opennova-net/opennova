#pragma once

// The atlas-time per-type texture channel conversions. Retail converts the
// authored textures as it bakes the atlas pages: the additive class (type 1)
// ships its alpha cleared to zero, and the normal-mapped classes read the
// blue byte as height and store the wrapped gradient as the normal —
// Bump/Bumpadd (types 3/6) at scale 1/8, Distort (type 7) at 1/32 with the
// output blue byte forced to 255 (alpha left untouched).

#include <cstdint>

namespace opennova::particle {

constexpr float kBumpHeightToNormalScale = 0.125f;
constexpr float kDistortHeightToNormalScale = 0.03125f;

// [orig: Texture_GenerateNormalMapFromHeight @ 0x5e2df0; the type-3/6 (1/8)
// and type-7 (1/32, forced blue) selections in
// CParticleManager_BuildTextureAtlases @ 0x5e8db0]. rgba is width*height*4
// bytes; neighbor reads wrap with the retail power-of-two masks.
void convert_height_to_normal_map(std::uint8_t *rgba, int width, int height,
		float scale, bool force_blue);

// The type-1 additive alpha clear [orig: the atlas blit alpha clear in
// CParticleManager_BuildTextureAtlases @ 0x5e9116]: additive fragments carry
// zero alpha, so the witnessed ONE/INVSRCALPHA pair becomes a pure add and
// the authored alpha curve never affects an additive layer
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380].
void clear_alpha_channel(std::uint8_t *rgba, int width, int height);

} // namespace opennova::particle
