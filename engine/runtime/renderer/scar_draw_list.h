#pragma once

// The impact-scar draw list: the portable compile of the world's scar rings
// into textured quads, per owner ring / section / texture strip, for an
// embedding renderer to upload. Mirrors the particle frame seam
// (renderer/particle_frame.h): no Godot, no simulation dependencies beyond the
// ring cache.
// [orig: Scar_RenderAllCaches @0x5CDF70 (from Terrain_CollectVisibleEntities
//  @0x5c91b7: the shared ring first, then every live entity ring) ->
//  Scar_RenderCache @0x5CD830; the 32 per-texture CDynList24 batches at
//  0x2BDF848; the drawer Scar_DrawBatches @0x5CCD10 (ex
//  Terrain_RenderFoliageBatches), called after the lit sector entities]

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <runtime/world/impact_scar.h>

namespace opennova::world {
struct CollisionMatrix;
}

namespace opennova::renderer {

// No occlusion instance is a host-side all-visible fallback, represented by
// nullopt. Missing owners are rejected before any mask lookup.
//
// `building` is the SLOT's isBuilding byte (world::ScarSlot::building, the
// struck def's type == 5 at the slot write), not the owner's pool: a
// decoration placed in the BMS Building list sits in pool 2 but takes the
// non-building leg, its four containing-building hits
// [orig: Scar_RenderCache @0x5CD92C tests the slot byte +60 that Scar_AddEntry
//  @0x5CCCC6..0x5CCCCE writes from `def+0x5C == 5`].
using ScarSectionMaskLookup = std::function<std::optional<uint32_t>(world::EntityHandle)>;
bool scar_owner_visible(const world::Entity *owner, bool building,
		const ScarSectionMaskLookup &section_mask);


// The witnessed vertex stride {x, y, z, argb, u, v} [orig: the six 24-byte
// vertices Scar_RenderCache writes; FVF 0x142].
struct ScarVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint32_t argb = 0xFF000000u;
	float u = 0.0f;
	float v = 0.0f;
};

// One run of quads sharing an owner ring, a section and a texture strip. Six
// vertices per quad in the witnessed order. A shared-ring batch is in
// mission-space world coordinates; an entity-ring batch is SECTION-LOCAL to
// `section` of the owner's model and draws under that section's node
// [orig: Scar_RenderCache branches on the cache key — bone matrix
//  `bones + bone << 6` for an entity ring, the position as-is for the shared
//  ring]. (The retail Y-negation is the world->D3D fold the presenter replaces.)
//
// An entity-ring batch whose owner's live section matrix resolved also
// carries the same quads in world space (`world_resolved`, the run at
// `world_first_vertex` in ScarDrawList::world_vertices, `vertex_count` long,
// sharing this batch's UVs and colours): the slots taken through the section
// matrix as Scar_RenderCache takes them, for an embedder with no node to
// mount the section-local run under (a statically batched prop).
struct ScarDrawBatch {
	std::uint16_t owner_packed = 0xFFFF; // EntityHandle::packed; 0xFFFF = the shared ring
	std::uint8_t texture = 0;            // strip index (world::scar_texture_strip_name)
	std::uint8_t section = 0;            // entity-local batches: the model section
	bool entity_local = false;
	bool building = false;
	std::uint32_t first_vertex = 0;
	std::uint32_t vertex_count = 0;
	bool world_resolved = false;
	std::uint32_t world_first_vertex = 0;
};

struct ScarDrawList {
	std::vector<ScarVertex> vertices;
	// The world-space form of the entity-ring batches whose section matrix
	// resolved (ScarDrawBatch::world_first_vertex indexes it).
	std::vector<ScarVertex> world_vertices;
	std::vector<ScarDrawBatch> batches;
	// Slots emitted / slots culled this compile (observability for tests + F3).
	std::uint32_t slots_live = 0;
	std::uint32_t slots_culled = 0;
};

// The per-frame inputs Scar_RenderCache reads: the camera ground position and
// the fog distance for the pre-transform box cull of world-space slots
// [orig: `|p.x - camX| <= fog + r && |p.y - camY| <= fog + r` before the view
// transform; entity-local slots are culled by the presenter with their
// owner's node], the terrain light colour folded onto every vertex [orig:
// g_EnvTerrainLightCombined | 0xFF000000], and the owner visibility predicate,
// asked per slot with the slot's own isBuilding byte [orig: a building slot
// draws when its owner's `g_BuildingSectionVisMask[idx] & 0xFFFFFFF` is
// nonzero; another slot when any of its owner's four containing buildings
// (+464..+476) is visible, or outright when +464 == 0]. A null predicate
// treats every owner as visible. `section_matrix` answers an entity-ring
// owner's live section matrix (Q22 rotation, 16.16 translation: the bone
// matrix `bones + bone << 6` the model callback returns), from which the
// compile also emits each entity-ring batch in world space; null (or a false
// answer) leaves the batch section-local only. The drawer's state is the
// presenter's: blend mode 0 (CD3DDevice_SetFogAndBlendMode), alpha test ref
// 128, the strip's one-texture mode effect with clamp wrap.
struct ScarViewContext {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float fog_distance = 0.0f;
	std::uint32_t terrain_light_argb = 0xFFFFFFFFu;
	bool (*owner_visible)(std::uint16_t owner_packed, bool building, void *user) = nullptr;
	bool (*section_matrix)(std::uint16_t owner_packed, int section,
			world::CollisionMatrix &out, void *user) = nullptr;
	void *user = nullptr;
};

// Compile every live slot of every ring into `out` (cleared first): the shared
// ring first, then the entity rings in cache order, every slot gated on its
// own owner's visibility, each ring grouped per section and texture strip
// into one batch.
void compile_scar_draws(const opennova::world::ScarCache &cache,
		const ScarViewContext &ctx, ScarDrawList &out);

// The device state a strip's GfxShader mode word selects
// (world::scar_texture_strip_mode_word), decoded from the witnessed layout
// [orig: CGfxShader_ApplyPass @0x683190 — `combined = modeWord | passFlags`,
//  the drawer passes 0x10000000 (LIGHTING off @ CGfxShader_ApplyPassRenderStates (ex sub_6808A0)): bit 0x40000 ->
//  ALPHATESTENABLE, 0x20000 -> FOGENABLE, 0x100000 -> z-write OFF, 0x400000 ->
//  CULLMODE NONE (else CCW); RenderState_DecodeBlendModeToD3DStates @0x680F00 —
//  bits 0-3, nibble 1 = SRCALPHA/INVSRCALPHA; RenderState_DecodeModeAlphaStage
//  @0x680B00 — bits 4-7, nibble 5 = MODULATE(TEXTURE, DIFFUSE);
//  RenderState_DecodeModeColorStage @0x681080 — bits 8-13, family 0x600 =
//  MODULATE2X(TEXTURE, DIFFUSE) under GfxDevice_Modulate2XEnabled, which
//  CGfxDevice_CreateDevice @0x67EB5F sets to 1 unconditionally]. The alpha
// test compares GREATER against the drawer's latched ref
// [orig: CGfxDevice_SetAlphaTestRef(128) @0x5CCDAE — ALPHAFUNC + ALPHAREF
//  only; without the 0x40000 bit the latch is inert].
struct ScarStripState {
	bool src_alpha_blend = false;            // SRCBLEND SRCALPHA / DESTBLEND INVSRCALPHA
	bool alpha_modulate_texture_diffuse = false; // ALPHAOP MODULATE(TEXTURE, DIFFUSE)
	bool color_modulate2x_texture_diffuse = false; // COLOROP MODULATE2X(TEXTURE, DIFFUSE)
	bool alpha_test = false;                 // ALPHATESTENABLE (GREATER kScarAlphaTestRef)
	bool fog = false;                        // FOGENABLE
	bool depth_write = false;                // ZWRITEENABLE
	bool cull_none = false;                  // CULLMODE NONE; else the CCW back-face cull
};
inline constexpr int kScarAlphaTestRef = 128;
ScarStripState decode_scar_strip_mode(std::uint32_t mode_word);

}  // namespace opennova::renderer