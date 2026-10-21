// The host's own-player spawn — a faithful subset of the §5.2b host player-spawn machine
// (docs/net/novaworld-net-re.md §5.2b/§5.38), in engine/runtime/world so the runtime/replication/Phase-2 listen
// server can spawn the player without a engine/runtime/mission dependency. The player is an
// authoritative pool-0 World entity (ADR 0012), indistinguishable from any other simulated
// entity, driven by the SAME infantry motor as an NPC but ordered from input, not AI think.
#pragma once

#include <cstdint>
#include <optional>

#include <runtime/world/angle.h>  // placed_heading_bam
#include <runtime/world/entity.h> // EntityHandle, Vec3

namespace opennova::world {

class World;
struct PlayerTemplate;
enum class PlayerBodyClass : uint8_t;

// The stock class-8 desert camouflage row's type id (items.def 105305, "Player #1,
// Multiplayer"), the seed a World with no items rows spawns its players as
// [orig: net-re §5.2b — type_id 0x14B9]. A player's type is its class's
// camouflage row's (player_cammo_type_id), not this id.
inline constexpr int32_t kPlayerInfantryTypeId = 0x14B9;

// The first pool-0 slot a player entity may occupy. Retail assigns players the LOWEST free pool-0
// slots — a same-map retail↔retail ASH_I5A capture (2026-07-01) shows the listen host's own player at
// slot 0 and the joiner at slot 1 (roster slot N -> entity slot N when the mission has no pool-0 AI).
// The prior value 4 (a mistaken "low slots reserved" assumption) offset every player by +4 vs retail.
// Canonical home for both the inmatch host (inmatch::kRetailPlayerMinEntitySlot re-exports this) and the
// Godot listen host (simulation.cpp). [orig: §5.2b spawn placement; Server_PlayerAdd @0x51cbc0]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = 0;

// Spawn parameters for the host's own player. `net_id` is the SSN the caller assigns (default a
// reserved high value unlikely to collide with mission entities).
struct PlayerSpawn {
    EntityPosition position; // the source pose's words (D-NET-507)
    // The Yaw word (+0x10) the body and the motor start from: the placement's
    // copied word (SpawnPointResult::heading_bam) or a joiner's spawn record's
    // full orientation, verbatim (D-NET-376); a spawn with no source pose (a
    // test's, a tool's) passes a placed row's word (placed_heading_bam). The
    // default is mission yaw 0's word (D-NET-NEW-L6-2).
    // [orig: Entity_FindBestSpawnPoint @0x50CF38;
    //  Server_PositionPlayerForSpawn @0x50D3F7]
    int32_t heading = placed_heading_bam(0);
    // The Pitch / Roll words the spawn stores (D-NET-NEW-L6-3).
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
    uint8_t team = 0;
    uint16_t net_id = 0xFFF0;
    // Item-less FALLBACK only: when the Player def is resolved (world.tables.player, the
    // items.def Player hp, 150) the spawn's ceiling is THAT def word. (D-NET-144)
    int16_t health = 100;
    // A joiner's own body, built from its 0x0C record: the client's item init stores the def word
    // as its health, which the class init's raise (in a session, to that word) keeps, where the
    // authority's spawn zeroes the row first. [orig: NapiNPClientMsg_0x00C @0x42E730 - the memset
    //  @0x42E7F1, the Entity_InitFromItemDef call @0x42E84D; Entity_InitFromItemDef @0x49E550 -
    //  Health @0x49E5B9..0x49E5C0, the +0x148 class init @0x49E5D5..0x49E5EA]
    bool from_wire_record = false;
    uint16_t min_entity_slot = 0;
    // The owning connection's ConnectionId/dcb -> Entity::owner_connection_id (entity+0x78). The host's
    // own player carries the host dcb (the loopback connection_id); a joiner carries its 0x48-ack dcb.
    // [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id]
    uint32_t owner_connection_id = 0;
    // The soldier class (5..9 MP personas; entity+0x294 playerClass). Default 8 (rifleman) — the class
    // the client re-resolves the soldier model from at round-load [CharAttr_GetCammoTypeId(class,lod)].
    // The spawn seed MUST carry it, or the World entity keeps player_class 0 (which build_pool0 masks to 8
    // on the wire, but the host's own logic then reads 0). [net-re §5.2b; host-diag 2026-07-01]
    uint8_t player_class = 8;
    // The items.def type the body binds: -1 (the authority's spawn) resolves the
    // camouflage item of `item_class` (0: of player_class) by the mission's
    // climate (player_cammo_type_id); a joiner's own body from its 0x0C record
    // passes the record's type word. A joiner's add resolves by its class word
    // before its class stores and clamp (a zero word reads 8 in a session), so
    // the two classes can differ.
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C399..0x43C3CA; Server_PlayerAdd
    //  @0x51D02B..0x51D056, the stores @0x51D09A / @0x51D0A8, the clamp
    //  @0x51D0FA..0x51D10E; NapiNPClientMsg_0x00C @0x42E730]
    int32_t item_type_id = -1;
    uint8_t item_class = 0;
    // Equipped-weapon AdmDef index (entity+0x2B0) — the 0x0A off-16 echo default. The inmatch
    // spawn resolves the process's loadout name list's first name when the armory is fed
    // (player_spawn_equipped_default) [orig: PlayerClass_InitEntity @0x4B1116 resolves by
    // name]; 0xFF = none (table-less hosts). (D-NET-143)
    uint8_t equipped_adm_index = 0xFF;
    // GamePlayerEntity.animSlot (entity+0x374): the character-model/avatar selector — the joiner's
    // per-side VCA/VCB 0x42 join var picked by ASSIGNED team, or the host's own avatar (retail
    // default 1 when the profile carries none). 0 = "tag absent" (retail sends the raw 0).
    // [orig: Server_PlayerAdd @0x51cbc0 (@0x51d0b1); Avatars_ResolveSelectionIndex (ex sub_57AE60) default-return 1; D-NET-146]
    uint8_t anim_slot = 0;
    // The wire NetId (entity+0x15C): the minimap/character-slot id picked per assigned team from
    // the joiner's CI0/CI1 join vars (low u16). 0 = unassigned -> the encoder's D-NET-137 shim.
    // [orig: Server_PlayerAdd @0x51cbc0 slot+440 -> entity+0x15C]
    uint16_t minimap_net_id = 0;
    // The BERSERK behavior bit (AiSlot[1] 0x200) the authority spawn sets on every player of a
    // game type without the team bit (DM, KOTH, Flag Me): a solo-mode player is everyone's enemy,
    // so the friendly-fire gate and the kill classifier treat a same-team pair as hostile.
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C53C `test g_GameType, 10000h` ->
    //  `or [entity+0x68 slot +4], 200h` @0x43C54F; its callers are Server_PlayerAdd @0x51D056
    //  and Server_InitAllPlayerEntitiesForRound @0x516B67]
    bool berserk = false;
};

// PlayerClass_InitEntity's equipped weapon for every player body the process spawns: the
// first name of the process's loadout name list (World::loadout_weapon_names; the image's
// initializer, the title's default kit, when none is bound or nothing wrote it) resolved by
// name; 0xFF when the written list is empty or its first name is no weapon (the lookup's
// -1 stored as a byte). (D-NET-143)
// [orig: PlayerClass_InitEntity @0x4B1060 - `push offset g_LoadoutWeaponNameList`
//  @0x4B1100, AdmDef_FindIndexByName @0x4B110E, `mov [esi+2B0h], al` @0x4B1116]
uint8_t player_spawn_equipped_default(const World &world);

// The class's camouflage item type id for the mission's climate: g_BmsClimate 1
// JUNGLE_CAMMO, 2 ARCTIC_CAMMO, any other DESERT_CAMMO, of the class row
// (class - 1) & 0xF the installer copied (0 for an inactive row).
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C399..0x43C3BE ->
//  CharAttr_GetCammoTypeId @0x4127B0]
int32_t player_cammo_type_id(const World &world, uint8_t player_class);

// The class-table row an items.def row's ai_function names, as a person body takes it
// (PlayerBodyClass, world.h): the whole name ignoring case; an empty tag reads "Null" and a
// name no row carries the walk's row-0 default, both the null row, an item class.
// [orig: Entity_LookupRenderCallbacks @0x407DC0 - the stricmp call @0x407DD8, the row-0 default
//  @0x407DEE..0x407E1E; EntityDef_InitAllCallbacks @0x4A5AAE]
PlayerBodyClass player_body_class_from_tag(const char *ai_function);

// ItemList_FindIndexByTypeId over the World's items rows: the first row of the
// type, 0 (the first row) when none [orig: @0x49E100].
int32_t item_index_by_type_id(const World &world, int32_t type_id);

// A player body's bound row, by its ItemTypeIndex (the def the game reads the
// body's healthMax and armor through); the seed in a World with no rows.
// [orig: entity+0x20 itemDef = g_ItemDefs[entity+0x1C]]
const PlayerTemplate &player_item_row(const World &world, const Entity &body);

// The loadout accept's re-resolve of a player body's row by its (just stored)
// class: the camouflage item by the climate, its ItemTypeIndex, the def (its type
// word and the traits the game reads through the def: the type byte, the attribute
// word, the armor words, the damage reduction, the model slot) and nothing the
// spawn copied into the body (Health, Armor, the signatures, the callbacks, the
// bound radius). A World whose spawns take the seed keeps the body as it is.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 - the entity test
//  @0x515AEB..0x515AED, the climate pick @0x515AF3..0x515B42, ItemList_FindIndexByTypeId
//  @0x515B47, ItemTypeIndex / itemDef @0x515B5A / @0x515B5D, the model slots
//  @0x515B60..0x515B78]
void rebind_player_camouflage_row(World &world, Entity &body);

// Faithful §5.2b sequence: (1) alloc a pool-0 player-infantry entity of its class's
// camouflage row (player_spawn.cpp player_spawn_row); (2/3)
// item-template health init; (4) place Position/Yaw/Team; (5) entity_reset_to_spawn_state
// (clear the entity+36 movement gate). Then mount the infantry motor as the LOCAL PLAYER
// (inf.active + inf.is_local_player; input-ordered, no AI route) and publish
// World::cached.local_player. Returns the spawn handle, or an invalid handle if pool 0 is
// full or the World has no AiSystem wired. [orig: net-re §5.2b/§5.38; ADR 0012]
EntityHandle spawn_player(World &world, const PlayerSpawn &spawn);

// Spawn a REMOTE PEER's player entity on the host (a joiner). The SAME faithful §5.2b sequence
// as spawn_player, EXCEPT it is NOT the host's own player: inf.is_local_player stays false and
// World::cached.local_player is NOT republished (the host keeps its own player as the local
// one). The peer is a full pool-0 player entity the host chases from the joiner's C2S 0x0C
// uplinks (replication EntityWireBridge::apply_player_intent) and the motor skips once the entity is
// net-snapped. Pass a distinct net_id per joiner (the default 0xFFF0 is the host's own player).
// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0 registers a
// joined player's entity without assigning g_LocalPlayerEntity; net-re §5.2a/§5.2b.]
EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn);

} // namespace opennova::world
