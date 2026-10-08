// The host's own-player spawn — a faithful subset of the §5.2b host player-spawn machine
// (docs/net/novaworld-net-re.md §5.2b/§5.38), in engine/runtime/world so the runtime/replication/Phase-2 listen
// server can spawn the player without a engine/runtime/mission dependency. The player is an
// authoritative pool-0 World entity (ADR 0012), indistinguishable from any other simulated
// entity, driven by the SAME infantry motor as an NPC but ordered from input, not AI think.
#pragma once

#include <cstdint>
#include <optional>

#include <runtime/world/angle.h>  // bam_heading_from_mission_yaw_deg
#include <runtime/world/entity.h> // EntityHandle, Vec3

namespace opennova::world {

class World;

// The player infantry template id [orig: net-re §5.2b — type_id 0x14B9].
inline constexpr int32_t kPlayerInfantryTypeId = 0x14B9;

// The first pool-0 slot a player entity may occupy. Retail assigns players the LOWEST free pool-0
// slots — a same-map retail↔retail ASH_I5A capture (2026-07-01) shows the listen host's own player at
// slot 0 and the joiner at slot 1 (roster slot N -> entity slot N when the mission has no pool-0 AI).
// The prior value 4 (a mistaken "low slots reserved" assumption) offset every player by +4 vs retail.
// Canonical home for both the inmatch host (inmatch::kRetailPlayerMinEntitySlot re-exports this) and the
// Godot listen host (simulation.cpp). [orig: §5.2b spawn placement; Server_PlayerAdd @0x51cbc0]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = 0;

// Spawn parameters for the host's own player. `yaw` is the mission yaw in degrees (the same
// convention as a BMS heading). `net_id` is the SSN the caller assigns (default a reserved
// high value unlikely to collide with mission entities).
struct PlayerSpawn {
    Vec3 position;
    int16_t yaw = 0;
    // The heading word (+0x10) the motor starts from, when the spawn copies a
    // source pose: the placement's (SpawnPointResult::heading_bam) or a
    // joiner's spawn record's full orientation. Retail copies it verbatim,
    // never through the whole-degree `yaw` (D-NET-376). A spawn with no source
    // pose (a test's, a tool's) turns `yaw` into its heading.
    // [orig: Entity_FindBestSpawnPoint @0x50CF38;
    //  Server_PositionPlayerForSpawn @0x50D3F7]
    std::optional<int32_t> heading_bam;
    int16_t pitch = 0;
    int16_t roll = 0;
    uint8_t team = 0;
    uint16_t net_id = 0xFFF0;
    // Item-less FALLBACK only: when World::player_item_hp is resolved (the items.def Player hp,
    // 150), the spawn seeds THAT at full [orig: Entity_InitFromItemDef @0x49e550]. (D-NET-144)
    int16_t health = 100;
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
    // Equipped-weapon AdmDef index (entity+0x2B0) — the 0x0A off-16 echo default. The inmatch
    // spawn resolves the WPN_M4AUTO table index when the armory is fed [orig: PlayerClass_InitEntity
    // @0x4B1116 resolves by name]; 0xFF = none (table-less hosts). (D-NET-143)
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

// The heading word a spawn starts the motor and the local look yaw from: the
// copied source word, else `yaw` turned to the (90 - yaw) heading.
// [orig: Entity_FindBestSpawnPoint @0x50CF38 (Yaw), @0x50CF4D (the look yaw)]
inline int32_t player_spawn_heading(const PlayerSpawn &spawn) {
    return spawn.heading_bam.has_value() ? *spawn.heading_bam
                                         : bam_heading_from_mission_yaw_deg(spawn.yaw);
}

// Faithful §5.2b sequence: (1) alloc a pool-0 player-infantry (0x14B9) entity; (2/3)
// item-template health init; (4) place Position/Yaw/Team; (5) entity_reset_to_spawn_state
// (clear the entity+36 movement gate). Then mount the infantry motor as the LOCAL PLAYER
// (inf.active + inf.is_local_player; input-ordered, no AI route) and publish
// World::cached.local_player. Returns the spawn handle, or an invalid handle if pool 0 is
// full or the World has no AiSystem wired. [orig: net-re §5.2b/§5.38; ADR 0012]
EntityHandle spawn_player(World &world, const PlayerSpawn &spawn);

// Spawn a REMOTE PEER's player entity on the host (a joiner). The SAME faithful §5.2b sequence
// as spawn_player, EXCEPT it is NOT the host's own player: inf.is_local_player stays false and
// World::cached.local_player is NOT republished (the host keeps its own player as the local
// one). The peer is a full pool-0 0x14B9 entity the host SNAPs from the joiner's C2S 0x0C
// uplinks (replication EntityWireBridge::apply_player_intent) and the motor skips once the entity is
// net-snapped. Pass a distinct net_id per joiner (the default 0xFFF0 is the host's own player).
// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0 registers a
// joined player's entity without assigning g_LocalPlayerEntity; net-re §5.2a/§5.2b.]
EntityHandle spawn_remote_player(World &world, const PlayerSpawn &spawn);

} // namespace opennova::world
