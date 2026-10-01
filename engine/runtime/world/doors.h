#pragma once

#include <runtime/world/entity.h>
#include <cstdint>
#include <vector>

namespace opennova::world {
class World;

// One door-row packet, S2C 0x37 `[u16 handle][i16 state][u8 number]`: the
// authority queues it on the world's entity-event outbox, the host fans it to
// every in-match remote (mask 0x90). `state` is the record at (first + number
// - 1), read when the packet is built. [orig: Server_SendWeaponSlotActionPacket
// @0x50F9A0]
struct DoorRowEvent {
    uint16_t handle = 0xFFFF;
    int16_t state = 0;
    uint8_t number = 0;
};

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
    // The state word a door-row packet for `number` carries: record
    // (first + number - 1) of the global table, 0 past the door count or
    // outside the allocated records. [orig: Server_SendWeaponSlotActionPacket
    // @0x50f9c0..0x50f9d7 — dword_A8A400[6 * (number + first)], the record
    // before the one at A8A418 + 24 * (number + first)]
    int32_t wire_row_state(const Entity &, int number) const;
    // The authority's C2S 0x1A leg: record (first + number - 1) of `entity`
    // under the request's gates and switch; false when a gate refuses (no
    // reply), else the record's resulting state in `out_state`.
    // [orig: NapiNPServerMsg_HandleVoteUpdate @0x514B20]
    bool apply_request(const Entity &, int number, int32_t value, int32_t &out_state);
private:
    std::vector<Slot> slots_;
};
} // namespace opennova::world
