// The death-piece draw: which live pieces of the pool a view draws this
// frame, at which level of their model, which section each shows and the
// matrix that places it. Retail collects the pieces with the frame's visible
// entities, then draws each as its model's level mesh with every bone matrix
// set to the piece matrix and every other section's matrix collapsed.
//
// [orig: DeathPiece_CollectVisible @ 0x57b560 (the collect) from
//  Terrain_CollectVisibleEntities @ 0x5c91bc, DeathPiece_RenderVisible
//  @ 0x57b830 (the level walk) from Terrain_RenderWorldScene @ 0x5c9575,
//  and DeathPiece_RenderSection @ 0x57b690 (the section draw); the
//  256 x 180-B pool
//  g_death_piece_pool @ 0x26bac58]
#pragma once

#include <array>
#include <cstdint>

#include <runtime/world/destruction.h>
#include <runtime/world/geom.h>

namespace opennova::world {

// One piece as its draw submits it. Positions are mission space.
struct DeathPieceDraw {
	int32_t slot = -1;         // the pool slot (the present rows' key)
	uint64_t generation = 0;   // the slot's allocation incarnation
	int32_t item_id = 0;       // the wreck's item (its piece model's owner)
	int32_t lod_level = 0;     // the level mesh drawn
	// The sections the draw collapses (piece+0x7C): every set bit below the
	// level's section count takes a bone matrix whose w is zero.
	uint32_t hidden_mask = 0;
	// The section drawn: the first clear bit of hidden_mask below the level's
	// section count (0 when none is clear).
	int32_t section = 0;
	// The matrix pivots on the drawn section's COBJ centre (only when
	// hidden_mask is nonzero); zero otherwise.
	bool pivoted = false;
	std::array<int32_t, 3> pivot_q16{};
	Vec3 pos;                  // piece+4..+0xC
	float heading = 0.0f;      // piece+0x10 (degrees, the BAM heading)
	float pitch = 0.0f;        // piece+0x14
	float roll = 0.0f;         // piece+0x18
	float scale = 1.0f;        // piece+0x88
	// The submit's 0x20: transparents to the below-water queue, while the
	// piece is at or under the water plane.
	bool below_water = false;
	int32_t projected_radius_q16 = 0; // the collect's recorded radius
};

// The section a piece's draw shows: the first index below `section_count`
// whose bit (index & 31) is clear in `hidden_mask`, else 0.
// [orig: DeathPiece_RenderSection @ 0x57b6c5..0x57b6f4]
int32_t death_piece_render_section(uint32_t hidden_mask, int32_t section_count);

// One collected piece through the level walk and the section draw: false when
// the draw returns without submitting (the sub-pixel floor, an empty level
// slot). `projected_radius_q16` is the collect's recorded radius, `lod_scale`
// renderer::death_piece_lod_scale, `water_z_q16` the mission water plane.
// [orig: DeathPiece_RenderVisible @ 0x57b830 ->
//  DeathPiece_RenderSection @ 0x57b690]
bool death_piece_draw(const DeathPiece &piece, const DeathPieceModel &model,
		int32_t projected_radius_q16, float lod_scale, int32_t water_z_q16,
		DeathPieceDraw &out);

} // namespace opennova::world
