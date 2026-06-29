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
    bool alive = true;
    uint32_t ai_flags = 0;    // BmsiAttributeFlags
    int32_t move_speed_kph = 0;
    int32_t engage_min = 0;
    int32_t engage_max = 0;
    int32_t attack_max = 0;
    int32_t anim_slot = -1;
    bool hidden = false;
    bool held = false;
    bool disabled = false;

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
