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

// Minimal live-entity state the scripting evaluators read and mutate. This is a
// clean model over the original 172-byte bms record + the pool record's net id;
// the renderer/AI's full entity layout is a separate, deferred concern.
struct Entity {
    uint16_t net_id = 0;      // SSN; the field WAC/BMS address entities by
    int32_t bms_id = 0;       // file entity id (bms::Entity::id); the host keys placed nodes by this
                              // (MissionEntityRegistry), distinct from the runtime net_id/SSN.
    EntityHandle handle;      // self-handle (assigned at spawn)

    EntityKind kind = EntityKind::Item;
    int32_t item_id = 0;      // items.def type id

    Vec3 position;            // mission space (Z-up)
    int16_t yaw = 0;
    int16_t pitch = 0;
    int16_t roll = 0;

    uint8_t team = 0;
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
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ENTITY_H
