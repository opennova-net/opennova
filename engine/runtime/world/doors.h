#pragma once

#include <runtime/world/entity.h>
#include <cstdint>
#include <vector>

namespace opennova::world {
class World;

// Mission-owned monotonic door pool. Indices remain stable until mission reset.
// [orig: HeliLift_ResetAll @0x44E870; FadeEffect_AllocateSlot @0x44E890]
class DoorSystem {
public:
    static constexpr int kCapacity = 10000;
    static constexpr int kMaxDoors = 30;
    struct Slot {
        int32_t state = 0; // 0 closed, 1 opening, 2 open, 3 closing
        int32_t phase = 0;
        int32_t step = 0;
        int32_t max_angle = 0;
        EntityHandle owner;
        uint64_t owner_lifetime = 0;
        uint8_t number = 0; // completion/network numbering is 1-based
    };
    void initialize(Entity &, int32_t step, int32_t max_angle);
    const Slot *slot(const Entity &, int section) const;
    // target event's raw base+section read; unallocated global slots are zero.
    bool target_section_closed(const Entity &, int section) const;
    void tick(World &);
    void command(World &, Entity &, int event, uint32_t touch_mask = 0);
    bool group_open(const World &, int32_t group) const;
    void command_group(World &, int32_t group, bool opening, bool include_pool1);
    // The non-player contact query ignores opening/open building sections.
    uint64_t passable_sections(const Entity &) const;
    // Shared ordinal CTRL writer; caller supplies storage beginning at DOOR_00.
    int write_phases(const Entity &, int32_t *out, int capacity) const;
    // The S2C 0x37 receive leg: row (first + number - 1) takes the host's
    // state; a closed row snaps to phase 0 and an open one to 65536.
    // [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250]
    void apply_wire_row(const Entity &, int number, int32_t state);
    size_t allocated() const { return slots_.size(); }
private:
    std::vector<Slot> slots_;
};
} // namespace opennova::world
