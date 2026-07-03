// Addressable entity model for the OpenNova runtime world.
//
// Faithful to the original engine's entity addressing (EntityPool_FindByNetId
// @0x4f0a20, Jointops.exe): entities live in fixed-capacity pools; each carries a
// 16-bit net id (the "SSN" WAC/BMS scripts reference). A live entity is named by a
// packed handle (pool<<12 | slot) — 4-bit pool index, 12-bit slot.
#ifndef OPENNOVA_WORLD_ENTITY_H
#define OPENNOVA_WORLD_ENTITY_H

#include <cstdint>
#include <string>
#include <vector>

#include "world/geom.h"

namespace opennova::world {

// Mirrors mission::EntityKind / bms::ItemType. Kept independent so libs/world
// has no dependency on libs/mission (promotion adapts between them).
enum class EntityKind : uint8_t {
    Marker = 0,
    Item = 1,
    Building = 2,
    Organic = 3,
};

// Packed addressable handle: (pool_index << 12) | (slot_index & 0xFFF).
// [orig: return value of EntityPool_FindByNetId @0x4f0a20; 0xFFFF == not found.]
struct EntityHandle {
    uint16_t packed = kInvalid;
    static constexpr uint16_t kInvalid = 0xFFFF;

    constexpr int pool() const { return (packed >> 12) & 0xF; }
    constexpr int slot() const { return packed & 0xFFF; }
    constexpr bool valid() const { return packed != kInvalid; }

    static constexpr EntityHandle make(int pool, int slot) {
        EntityHandle h;
        h.packed = static_cast<uint16_t>(((pool & 0xF) << 12) | (slot & 0xFFF));
        return h;
    }

    bool operator==(const EntityHandle &o) const { return packed == o.packed; }
    bool operator!=(const EntityHandle &o) const { return packed != o.packed; }
};

// Seat class for vehicle/emplacement mounting. The enum values are the original
// seatType codes. [orig: Entity_FindBestSeatSlot @0x4351f0 classifies the seat
// bone name: "sitex"->1, "ctrlx"->2, "UseGun"->3, "drvrx"->5.] An emplaced gun
// offers a single Gunner seat.
enum class SeatType : uint8_t {
    None = 0,
    Passenger = 1,  // "sitex"
    Controller = 2, // "ctrlx"
    Gunner = 3,     // "UseGun"
    Driver = 5,     // "drvrx"
};

// One seat a vehicle/emplacement offers. Mirrors the original split: the slot's
// seat-bone type lives at model[605+slot] and its occupant handle at
// vehicle[400+2*slot] (0xFFFF = empty). [orig: Entity_FindBestSeatSlot @0x4351f0 /
// Entity_AttachToVehicleSeat @0x4364a0.]
struct Seat {
    SeatType type = SeatType::None;
    uint8_t bone_index = 0;     // [orig: model[605+slot] seat-bone index]
    uint8_t pose_index = 0;     // `sitexNN`/`ctrlxNN`/`drvrxNN` -> anim_sit + NN
    std::string source_name;     // original seat/userpoint name (`sitex00`, `drvrx01`, `UseGun`)
    Vec3 seat_local;            // seat offset from the vehicle origin (mission space, Z-up)
    int16_t yaw_offset = 0;     // gunner facing offset vs the vehicle yaw [orig: @0x43656c]
    EntityHandle occupant;      // [orig: vehicle[400+2*slot]] kInvalid = empty
};

// Minimal live-entity state the scripting evaluators read and mutate. This is a
// clean model over the original 172-byte bms record + the pool record's net id;
// the renderer/AI's full entity layout is a separate, deferred concern.
struct Entity {
    uint16_t net_id = 0;      // SSN; the field WAC/BMS address entities by
    int32_t bms_id = 0;       // file entity id (bms::Entity::id); the host keys placed nodes by this
                              // (MissionEntityRegistry), distinct from the runtime net_id/SSN.
    EntityHandle handle;      // self-handle (assigned at spawn)

    // The owning connection's ConnectionId/dcb (GamePlayerEntity entity+0x78). The joining client's
    // self-scan matches it against its own ConnectionId; a host/dedicated-server reserves dcb 0. This
    // is the runtime home of what the wire models as OrganicSpawnRecord::entity_flags (the 0x0C
    // entity+0x78 field). [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id;
    // matched in Player_FindLocalPlayerEntity @0x4e0090; net-re §5.2b / D-NET-92/101]
    uint32_t owner_connection_id = 0;

    EntityKind kind = EntityKind::Item;
    int32_t item_id = 0;      // items.def type id
    bool is_ai_capable = false; // items.def ItemDefAttrib & 0x100000 (AIData / §5.6 AI class). Gates the
                                // 0x0D AI-trailer (D-NET-97). Distinct from ai_flags (BMS). [docs/world/itemdef-re.md]
    // The §5.10b wire replication class, resolved from the item's items.def *_function class
    // tag (ai_function, else move_function -> ItemDef+356 serialize callback) and stamped by
    // the host's post-promotion item-traits sweep. Stored as an OPAQUE code (the novaworld
    // EntityClass value; libs/world stays net-agnostic) — 0xFF = unresolved, netsim falls back
    // to its minimal heuristic. Load-bearing: a pool-1 item that is NOT a vehicle class (e.g.
    // ai_function ewep emplacements) must NOT be serialized with the vehicle compact record or
    // the client desyncs mid-frame (retail-join v13, 2026-07-02).
    uint8_t net_class_code = 0xFF;

    Vec3 position;            // mission space (Z-up)
    int16_t yaw = 0;
    int16_t pitch = 0;
    int16_t roll = 0;

    // Runtime entity flags — the GamePlayerEntity `Flags` at entity+36. Bit 1 is the
    // movement gate cleared at spawn and checked before the C2S 0x0C input uplink
    // [orig: Entity_ResetToSpawnState @0x4B9610 / Player_BuildTag0CInputBody @0x42A550;
    // docs/net/novaworld-net-re.md §5.2b/§5.6]. Distinct from ai_flags (BMS attributes).
    uint32_t flags = 0;
    // Spawn-point backup of position, written by world::entity_reset_to_spawn_state
    // [orig: Entity_ResetToSpawnState backs Position into pad9[124/128/132]].
    Vec3 spawn_position;

    uint8_t team = 0;
    // GamePlayerEntity.playerClass (entity+0x294) — the soldier class 5..9. The joiner's client
    // resolves its body-anim model from THIS at round-load [orig: Game_ReloadEntityModelsAndCallbacks
    // @0x522830 -> AnimMap_GetSlotPropertyInt(playerClass) @0x4127b0 -> ADM -> AnimMap_RegisterEntity
    // @0x40bb60 writes animChannelB(+0x188)]. The MP branch preloads classes 5..9 only; class 0 maps
    // to slot 15 -> empty ADM -> no anim channel -> Entity_UpdateInfantryPlayerBody @0x4b40e0 bails,
    // so the player cannot move/crouch/prone. Set from the player's loadout at spawn (default a valid
    // class for players); 0 = unset / non-player. [orig: re-grill 2026-06-28; net-re §5.2b/§5.23]
    uint8_t player_class = 0;
    uint8_t group_id = 0;     // named-group membership
    uint8_t waypoint_id = 0;  // wplist / route this entity follows
    int32_t wp_number = 0;    // position along that route

    uint8_t alert_state = 0;  // green/yellow/red
    int32_t ai_state = 0;     // AI component state
    int32_t ai_target = -1;   // net id of current AI target, -1 = none
    int32_t health = 100;     // 0 -> dead
    // items.def hp (itemDef+0x17C healthMax), stamped by the host's item-traits sweep
    // (0 = unresolved). The original spawns entities at Health = healthMax
    // [orig: Entity_InitFromItemDef @0x49e550]; the sweep mirrors that by lifting health
    // to hp for entities still at their spawn default. Feeds the §5.10 field-17 tier
    // denominator and the §5.13 vehicle health word.
    int32_t health_max = 0;
    bool alive = true;
    uint32_t ai_flags = 0;    // BmsiAttributeFlags
    int32_t move_speed_kph = 0;
    int32_t engage_min = 0;
    int32_t engage_max = 0;
    int32_t attack_max = 0;
    // The body-anim CLIP the present pass plays (kBodyAnim*, body_anim.h), selected by the
    // infantry motor / AI brain each tick; -1 = no clip / hold rest. RENAMED from `anim_slot`:
    // this is presentation state, NOT the retail entity+0x374 `animSlot` below — echoing it
    // onto the wire was the D-NET-146 DBuggy-shadow bug.
    int32_t body_anim_slot = -1;
    // GamePlayerEntity.animSlot (entity+0x374) — the character-model/anim-set selector: the
    // BMS AnimSlot spawn property, or a player's per-side avatar (the joiner's VCA/VCB 0x42
    // join vars picked by ASSIGNED team, host default 1). Serialized raw as field 13 of the
    // 0x0C organic / 0x18 full-entity spawn records. [orig: Entity_SpawnFromAnimSlotProperty
    // @0x43c390 (+0x374 write @0x43c522); Server_PlayerAdd @0x51cbc0 (@0x51d0b1);
    // Server_InitAllPlayerEntitiesForRound @0x516aa0 (@0x516b8e); net-re §5.23 D-NET-146]
    uint8_t anim_slot = 0;
    // Players only: the wire NetId (entity+0x15C) = the minimap/character-slot id, picked per
    // assigned team from the joiner's CI0/CI1 join vars (low u16 of the atol). 0 = unassigned
    // (the encoder falls back to its D-NET-137 shim). Non-players serialize Entity::net_id
    // (the WAC SSN space) there instead. [orig: Server_PlayerAdd @0x51cbc0 slot+440 ->
    // entity+0x15C; NapiNetConfig_LoadFromConnTags @0x4c7260 jsp[56]/jsp[58]]
    uint16_t minimap_net_id = 0;
    // The wire movement-INPUT byte (entity+0x12C low): the owning client uplinks it every frame
    // (§5.10 extended C2S 0x0C) and the host echoes it in that player's 0x0A compact record —
    // remote players are motor-driven from replicated input, NOT from an anim slot [orig: case-2
    // apply @0x4c11ec; consumers Entity_UpdatePlayerInfantryMovement @0x48496d,
    // check_bone_ground_contact @0x441ba4 (stance bits 8-9)]. Written by apply_player_intent for
    // remote peers; stays 0 (no input / idle) for entities without an uplink source.
    uint8_t net_move_input = 0;
    // Equipped-weapon AdmDef index (entity+0x2B0), echoed at this player's 0x0A off-16
    // (anim_def_index). 0xFF = none — the apply-skip sentinel the client honors (0 is a VALID
    // index: the "null" def). Ingested from the owner's extended C2S 0x0C uplink gated
    // AdmDefs[idx].category < 11 [orig: case-4 store @0x4C20A3]; host-spawned players default
    // to the WPN_M4AUTO table index [orig: PlayerClass_InitEntity @0x4B1116 resolves by name].
    // (D-NET-143)
    uint8_t equipped_adm_index = 0xFF;
    bool hidden = false;
    bool held = false;
    bool disabled = false;

    // The retail entity Flags dword (entity+36) as composed at spawn — the 0x10 static record
    // streams it RAW as its flag-0x20 i32 (the field the early RE misread as "parentSlot",
    // D-NET-147/150). Composed from mission attributes + item-def traits:
    //   BMS Indestructible(1<<21) -> 0x4000000, Reflective(1<<23) -> 0x400,
    //   NoShadow(1<<24) -> 0x1000000            [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    //   kind Building                -> 0x20000  [orig: Entity_InitFromModel @0x40e105]
    //   items.def hp == 0            -> 0x4000000 (+ sub_type 0xFF) [orig: @0x40dc8e]
    // Dynamic runtime bits (movement gate 0x2, mounted 0x40, ...) are NOT modeled here.
    uint32_t engine_flags = 0;
    // entity+290 low byte <- BMS record byte 81; always-present byte of the 0x10 static record
    // (golden buildings carry 0xFF). [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    uint8_t ammo_count = 0;
    // entity+532 subType — 0xFF when the item def is indestructible (hp 0) [orig:
    // Entity_InitFromModel @0x40dc85]; the 0x10 record's flag-0x80 byte.
    uint8_t sub_type = 0;
    // entity+533 refNum <- BMS record byte 153; the 0x10 record's flag-0x40 byte (D-NET-94).
    uint8_t ref_num = 0;

    uint32_t spawn_origin = 0; // back-ref to the BMS (kind,index) it was promoted from
    std::string name;          // named markers/areas

    // --- vehicle/emplacement mounting (AttachToEmplaced) ---
    // Seats this entity OFFERS as a vehicle/emplacement (mirrors vehicle[400..] + model[605..]).
    // Empty for plain entities; an emplaced gun seeds one Gunner seat.
    std::vector<Seat> seats;
    uint8_t emplaced_pose_variant = 0; // model config 1..8 -> anim_emplaced_2..9 when available
    // Occupant side: this entity is RIDING mount_target's seat mount_seat. [orig: occupant+364
    // vehicle ptr / +360 seat index / +36 & 0x40 mounted flag, written by
    // Entity_AttachToVehicleSlot @0x4946d0.] mounted == false => the rest are unset.
    EntityHandle mount_target;          // kInvalid = not mounted
    int8_t mount_seat = -1;
    SeatType mount_type = SeatType::None;
    bool mounted = false;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ENTITY_H
