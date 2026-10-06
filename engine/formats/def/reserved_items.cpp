// The items.def ids the engine looks a row up by (reserved_items.h; docs/world/itemdef-re.md, "The ids
// and rows the engine fixes"), each row cited where the engine reads it. The sweep: every immediate
// ItemList_FindIndexByTypeId call, every compare of an ItemDef's id (+0x50) and of a mission record's
// raw type, every immediate of 100001..109999.
#include "reserved_items.h"

#include <formats/def/def.h>

#include <algorithm>
#include <iterator>

namespace opennova::def {
namespace {

using R = ReservedItemRule;
constexpr int kAny = DEF_ITEM_TYPE_UNSET;
constexpr int kMarker = DEF_ITEM_TYPE_MARKER;
constexpr int kObject = DEF_ITEM_TYPE_OBJECT;
constexpr int kPerson = DEF_ITEM_TYPE_PERSON;
constexpr int kVehicle = DEF_ITEM_TYPE_VEHICLE;

constexpr ReservedItem kRows[] = {
	// The canopy a deployed parachute draws [orig: Entity_PreloadSpecialItems @ 0x43c220, the lookup
	// @0x43C22B into g_ParachuteItemIndex; Render_SubmitEntity's draw 1].
	{185, kAny, R::Warn, "Parachute", "Parachute",
	 "The engine draws a deployed parachute's canopy with this item's model."},
	// The fragment a palm-class item's broken sections throw, cloned into pool 2 [orig:
	// Projectile_SpawnFromTile @ 0x53c1c0, the lookup @0x53C1EE; Entity_PreloadPalmFragmentModel @
	// 0x53bd90, @0x53BD95]. Retail ships no row of it.
	{900, kAny, R::Warn, "Palm fragment", "",
	 "The engine throws this item as the fragment of a palm-class item's broken section."},
	// The teammate heli lift's helicopter [orig: Entity_SpawnHelicopter @ 0x4521a0, the lookup @0x452204;
	// Entity_PreloadSpecialItemModels @ 0x4516e0, @0x451705].
	{1281, kVehicle, R::Warn, "Heli lift helicopter", "Blackhawk, MEDIVAC, both doors open",
	 "The engine flies this vehicle for a mission's teammate heli lift."},
	// The binoculars a person holds [orig: Entity_PreloadSpecialItems @ 0x43c220, @0x43C255 into
	// g_BinocItemIndex; Render_SubmitEntity's draw 4].
	{1424, kAny, R::Warn, "Binoculars", "Binoculars", "The engine draws a person's binoculars with this item's model."},
	// The emplacement icon of the minimap: 12 where any other item takes 4 [orig: Entity_ClassifyForMinimap
	// @ 0x50fa70, @0x50FD2E and @0x50FD36].
	{1869, kAny, R::Warn, "Minimap emplacement", "Mounted Grenade Launcher on Tripod",
	 "The minimap draws this item with the emplacement icon."},
	{1886, kAny, R::Warn, "Minimap emplacement", "S&D Destroyable Grenade Launcher",
	 "The minimap draws this item with the emplacement icon."},
	// The goggles a person wears [orig: Entity_PreloadSpecialItems @ 0x43c220, @0x43C240 into
	// g_NightVissionGoggleItemIndex; Render_SubmitEntity's draw 3].
	{1904, kAny, R::Warn, "Night vision goggles", "Night Vision Goggles",
	 "The engine draws a person's night vision goggles with this item's model."},
	// The commander map's grid [orig: HUD_InitOverlaySystem @ 0x5a4620, @0x5A4990].
	{2043, kMarker, R::Refuse, "Map centre", "Map Centerpoint, helps align commander map grid",
	 "A mission's marker of this item aligns the commander map's grid."},
	// A named sector: its Locations name at spawn, keyed on the record's type [orig:
	// Entity_SpawnFromBMSRecord @ 0x40e9f0, @0x40F17C]; the sector names of chat and of sector actions
	// [orig: NapiNPServer_HandleChatMessage @ 0x513760, @0x513979; NapiNPServerMsg_HandleSectorAction @
	// 0x514330, @0x51443B]; the HUD map [orig: HUD_DrawMapOverlay @ 0x5a5f40, @0x5A7598].
	{2044, kMarker, R::Refuse, "Map named location", "Map Named Location",
	 "A mission's marker of this item names a map sector (its Locations string), which chat and the map show."},
	// Preloaded at every mission start, no other use witnessed [orig: Game_ReloadEntityModelsAndCallbacks
	// @ 0x522830, @0x522919]. Retail ships no row of it.
	{2143, kAny, R::Warn, "Preloaded item", "", "The engine loads this item's models at every mission start."},
	// The flags: the capture proximity [orig: Server_UpdateCaptureZoneProximity @ 0x5086a0, @0x508834],
	// the scoring [orig: GameEvent_ProcessScoring @ 0x52f7e1], the waypoint name STRWPNAMEFLAG [orig:
	// get_waypoint_name @ 0x594630, @0x5946B1..0x5946CD], the minimap [orig: Entity_ClassifyForMinimap @
	// 0x50fa70, @0x50FC14..0x50FC44]. 4096 and 4097, which retail ships no row of, at every site beside them.
	{4091, kObject, R::Refuse, "Blue flag", "Flag, blue", "The flag games (Capture the Flag, Flag Ball) carry, capture and score this item as a flag."},
	{4093, kObject, R::Refuse, "Red flag", "Flag, red", "The flag games (Capture the Flag, Flag Ball) carry, capture and score this item as a flag."},
	{4095, kObject, R::Refuse, "Green flag", "Flag, green", "The flag games (Capture the Flag, Flag Ball) carry, capture and score this item as a flag."},
	{4096, kObject, R::Refuse, "Flag", "", "The flag games check this item as a flag (retail ships no row of it)."},
	{4097, kObject, R::Refuse, "Flag", "", "The flag games check this item as a flag (retail ships no row of it)."},
	// The flag bays: STRWPNAMEFLAGBAY [orig: get_waypoint_name @ 0x594630, @0x5946E2..0x5946F0], the enemy
	// capture point [orig: SpawnPoint_FindNearestEnemyCapturePoint @ 0x4dd180, @0x4DD1EC], the minimap
	// [orig: Entity_ClassifyForMinimap @ 0x50fa70, @0x50FC68..0x50FC80], the map's points [orig:
	// Entity_BuildMapPoiLists @ 0x42de40, @0x42DF27]. 4101..4103, which retail ships no row of, beside them.
	{4098, kObject, R::Refuse, "Blue flag bay", "Flag bay, blue", "The flag games take this item as a team's flag bay."},
	{4100, kObject, R::Refuse, "Red flag bay", "Flag bay, red", "The flag games take this item as a team's flag bay."},
	{4101, kObject, R::Refuse, "Flag bay", "", "The flag games check this item as a flag bay (retail ships no row of it)."},
	{4102, kObject, R::Refuse, "Flag bay", "", "The flag games check this item as a flag bay (retail ships no row of it)."},
	{4103, kObject, R::Refuse, "Flag bay", "", "The flag games check this item as a flag bay (retail ships no row of it)."},
	// The heli lift's medics [orig: HeliLift_SpawnPickup @ 0x4525e0, @0x452634; HeliLift_SpawnFlyover @
	// 0x452730, @0x45295B; Entity_PreloadSpecialItemModels @ 0x4516e0, @0x4516E5 and @0x4516F5].
	{4520, kPerson, R::Warn, "Heli lift medic", "Combat Medic01 Soldier w", "The engine spawns this person as a heli lift's medic."},
	{4529, kPerson, R::Warn, "Heli lift medic", "Combat Medic02 Soldier no gun", "The engine spawns this person as a heli lift's medic."},
	// The type a 5305 teammate takes offline, 4999 + the class, class 1 the selector's only value [orig:
	// Entity_SpawnFromBMSRecord @ 0x40e9f0, `add ecx, 1387h` @0x40EA62]. Retail ships no row of it.
	{5000, kPerson, R::Warn, "Teammate", "", "A mission's multiplayer-player record spawns as this item offline (retail ships no row of it)."},
	// A mission record of the type is a teammate: dropped in a session or when the rules turn teammates off,
	// else retyped before its lookup [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0, @0x40EA2F..0x40EA70].
	{5305, kPerson, R::Refuse, "Multiplayer player", "Player #1, Multiplayer",
	 "A mission's record of this item is a teammate slot, never the item itself; OpenNova keys its player on it."},
	// The start families [orig: Server_PositionPlayerForSpawn @ 0x50cf60, the lookups @0x50D202..0x50D36C;
	// Game_StartMission @ 0x524360, registered and deck-localized at every mission start @0x525E92..0x525F14;
	// Spawn_BuildEntityPositionList @ 0x5095e0, @0x509615..0x5096AF].
	{6001, kMarker, R::Refuse, "Co-op fallback start", "start, player",
	 "Co-op (every single-player mission) spawns the player here when the mission has no insertion point; the epilog camera stands here."},
	{6002, kMarker, R::Refuse, "Deathmatch fallback start", "start, dmatch",
	 "The non-team modes spawn at the one farthest from the enemy when the mission has no deathmatch insertion point."},
	{6003, kMarker, R::Refuse, "Blue team fallback start", "start, Blue Team", "Team modes spawn team 1 here when it has no insertion point."},
	{6004, kMarker, R::Refuse, "Red team fallback start", "start, Red Team", "Team modes spawn team 2 here when it has no insertion point."},
	// A waypoint: its radius, its spawn timer and its WPNames name at spawn, keyed on the record's type
	// [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0, @0x40F05A]; the HUD's target line [orig:
	// HUD_GetTargetInfoString @ 0x5bb420, @0x5BB514].
	{6005, kMarker, R::Refuse, "Waypoint", "waypoint",
	 "A mission's marker of this item is a waypoint: its radius, spawn timer and WPNames name are read at spawn."},
	// The KOTH zone: its radius at spawn [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0, @0x40F157], the
	// capture proximity [orig: Server_UpdateCaptureZoneProximity @ 0x5086a0, @0x5089E8], the HUD map
	// [orig: HUD_DrawMapOverlay @ 0x5a5f40, @0x5A7569].
	{6006, kMarker, R::Refuse, "KOTH centre", "KOTH center", "King of the Hill's hill: the zone its teams hold."},
	// The scatter points around a picked spawn zone [orig: Server_PositionPlayerForSpawn @ 0x50cf60, @0x50D049].
	{6007, kMarker, R::Refuse, "Spawn zone scatter point", "LFP remote spawn",
	 "A player spawning at a zone takes one of the first 32 of these within its radius."},
	// The multiplayer map's points [orig: Entity_BuildMapPoiLists @ 0x42de40, @0x42E053..0x42E06F], their
	// names [orig: assign_entity_overlay_names @ 0x523b20, @0x523C87], the HUD map [orig: HUD_DrawMapOverlay @
	// 0x5a5f40, @0x5A7614..0x5A76C5].
	{6026, kMarker, R::Refuse, "Map point (alpha)", "waypoint, mp, alpha", "The multiplayer map shows this marker as a point."},
	{6027, kMarker, R::Refuse, "Map point (red base)", "waypoint, mp, red base", "The multiplayer map shows this marker as the red base."},
	{6028, kMarker, R::Refuse, "Map point (blue base)", "waypoint, mp, blue base", "The multiplayer map shows this marker as the blue base."},
	// A teleport's target [orig: EventAction_TeleportEntityToSpawn @ 0x43dfc0, @0x43DFF4;
	// EventTrigger_FindByEntityRef @ 0x452060, @0x452090; WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0,
	// @0x4F8004]; its name and speed at spawn [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0, @0x40F136, @0x40F24C].
	{6088, kMarker, R::Refuse, "Teleport target", "teleport target", "Teleport actions and script effects find their target among these markers."},
	// The map waypoint a player sets, made in pool 4 [orig: Waypoint_CreateForPlayer @ 0x4dfcb0, @0x4DFCF0].
	{6089, kMarker, R::Refuse, "Player waypoint", "user waypoint", "The engine makes the map waypoint a player sets from this item."},
	{6090, kMarker, R::Refuse, "Yellow team fallback start", "start, Yellow Team", "Four-team modes spawn team 3 here when it has no insertion point."},
	{6091, kMarker, R::Refuse, "Violet team fallback start", "start, Violet Team", "Four-team modes spawn team 4 here when it has no insertion point."},
	{6092, kMarker, R::Refuse, "Map point (yellow base)", "waypoint, mp, yellow base", "The multiplayer map shows this marker as the yellow base."},
	{6093, kMarker, R::Refuse, "Map point (violet base)", "waypoint, mp, violet base", "The multiplayer map shows this marker as the violet base."},
	{6094, kMarker, R::Refuse, "Insertion point", "start, primary, player",
	 "Co-op (every single-player mission) spawns the player at one of these; with none, at a fallback start, else at the map's origin."},
	{6095, kMarker, R::Refuse, "Deathmatch insertion point", "start, primary, dmatch", "The non-team modes spawn the players at these first."},
	{6096, kMarker, R::Refuse, "Blue team insertion point", "start, primary, Blue Team", "Team modes spawn team 1 at these first."},
	{6097, kMarker, R::Refuse, "Red team insertion point", "start, primary, Red Team", "Team modes spawn team 2 at these first."},
	{6098, kMarker, R::Refuse, "Yellow team insertion point", "start, primary, Yellow Team", "Four-team modes spawn team 3 at these first."},
	{6099, kMarker, R::Refuse, "Violet team insertion point", "start, primary, Violet Team", "Four-team modes spawn team 4 at these first."},
};

constexpr bool rows_in_order() {
	for (size_t i = 1; i < std::size(kRows); ++i)
		if (kRows[i - 1].type >= kRows[i].type) return false;
	return true;
}
static_assert(rows_in_order(), "the reserved rows stand in type order, each type once");

} // namespace

const ReservedItem *reserved_items(size_t *count) {
	if (count) *count = std::size(kRows);
	return kRows;
}

const ReservedItem *reserved_item_by_type(int type) {
	const auto found = std::lower_bound(std::begin(kRows), std::end(kRows), type,
	                                    [](const ReservedItem &row, int wanted) { return row.type < wanted; });
	return found != std::end(kRows) && found->type == type ? found : nullptr;
}

const ReservedItem *reserved_item_by_id(int items_def_id) {
	return items_def_id < DEF_ITEM_ID_BASE ? nullptr : reserved_item_by_type(items_def_id - DEF_ITEM_ID_BASE);
}

bool reserved_item_kind_matches(const ReservedItem &row, int item_type) {
	return row.kind == kAny || row.kind == item_type;
}

} // namespace opennova::def
