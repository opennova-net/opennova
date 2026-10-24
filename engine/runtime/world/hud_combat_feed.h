#pragma once
#include <array>
#include <runtime/hud/hud_combat.h>
#include <runtime/world/entity_registry.h>
#include <string>
namespace opennova::world {
class World;
struct AiEntity;
struct LocalPlayerWeapon;
struct LocalPlayerViewFrame;
struct LocalPlayerViewTracker;

// THE HUD ENTITY-INFO RECORD'S TARGET CURSOR (g_HUDTargetCursorEntity, record +0x168) as the HUD
// pass last built it: the lock word (AiSlot +0xC, an entity or a round), else, under a zero word
// only, the head-look target, which on a joiner may be the aim ray's decoded remote person (its
// wire row). The pass builds the record at its head, past the round-over, white-flash and
// no-player returns and ahead of the level-3 one (inmatch::build_hud_entity_info, from the
// session's HUD step), so the readers outside the pass read the last build: the joiner uplink's
// interest list in the frame's update, and the weapon inset in the scene frame ahead of that
// frame's pass (LocalPlayerViewTracker::hud_cursor_at_scene); the crosshair inside the pass reads
// this pass's build (LocalPlayerViewTracker::hud_cursor). Entity_Destroy zeroes the word for its
// entity; the cursor holds the named entity's lifetime, so a destroyed or replaced entity
// resolves to none (hud_target_cursor_entity). A mission start's HUD init zeroes the record:
// each kernel's LocalPlayer starts with none.
// [orig: HUD_RenderAllOverlays @0x5A8070 - the HUD_BuildEntityInfo call @0x5A80BC;
//  HUD_BuildEntityInfo @0x4B8440 (@0x4B87ED..0x4B882D); Entity_Destroy @0x43E810
//  (@0x43E89C..0x43E8A4); HUD_InitOverlaySystem @0x5A4620 (the memset @0x5A4941)]
struct HudTargetCursor {
	TargetRef lock;                 // the lock word: an entity or a round
	EntityLifetime entity;          // the named entity: the lock's, else the head-look target
	uint16_t wire_person = 0xFFFF;  // the aim ray's decoded remote person, under neither
};
// The record's build over the local body's slot word, its head-look target and the aim ray's
// decoded remote person (LocalPlayer::build_hud_target_cursor; no body builds none).
HudTargetCursor hud_target_cursor_build(const World &world, const AiEntity *body,
		uint16_t aim_wire_person);
// The cursor's entity as a reader resolves it now: none for a round lock, for no word, or for
// a named entity destroyed (or its slot reused) since the build.
const Entity *hud_target_cursor_entity(const World &world, const HudTargetCursor &cursor);
// A decoded remote person's row went through the client's destroy: a cursor naming it names
// none from then on, as the destroy zeroes the word for its entity (a named registry entity
// carries its own lifetime instead, hud_target_cursor_entity). A row that a later spawn page
// rebuilds in place is no destroy: the word keeps naming its slot.
// [orig: Entity_Destroy @0x43E810 - `cmp g_HUDTargetCursorEntity, esi` @0x43E89C, the clear
//  @0x43E8A4; NapiNPClientMsg_0x00C @0x42E730 - the slot's memset calls @0x42E7F1 / @0x42E7FE]
inline void hud_target_cursor_note_destroyed(HudTargetCursor &cursor, uint16_t wire_handle) {
	if (cursor.wire_person == wire_handle) cursor.wire_person = 0xFFFF;
}
// World-space inputs are projected by the device using the actual play camera.
// [orig: HUD_BuildEntityInfo @0x4B8440; HUD_DrawCrosshair @0x592640]
struct HudCombatView {
	hud::HudCombatState state;
	std::array<int32_t, 3> target{}, aim{}, commander{}, impact{}, vehicle_fixed{}, vehicle_lag{};
	bool target_valid = false, aim_valid = false, commander_valid = false, impact_valid = false,
		 vehicle_fixed_valid = false, vehicle_lag_valid = false;
	std::string custom_texture, commander_texture, weapon_texture, vehicle_texture, cargo_texture;
	// The HUD loader's mode for cargo_texture: an item def's HUD image loads in
	// alpha mode, the stock flag and document art in colour mode
	// [orig: HUD_LoadAllTextures @0x59E26A (an item's HUD image, mode 1);
	//  H_flag.tga @0x59DE53, H_docmnt.tga @0x59DE64 (mode 0)].
	bool cargo_texture_alpha = true;
	// The local player's position and team, the vehicle-bay logo walk's
	// viewer (hud_bay_logos.h) [orig: HUD_DrawVehicleBayLogos @0x5a2c00 --
	//  g_LocalPlayerEntity+0x162 @0x5a2c8e, +4/+8 @0x5a2cdf..0x5a2ce2].
	std::array<int32_t, 3> local_position{};
	uint8_t local_team = 0;
	bool local_valid = false;
};
void fill_hud_combat_view(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view,
		LocalPlayerViewTracker &tracker, bool can_fire);
void fill_hud_vehicle_sights(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view);
} // namespace opennova::world
