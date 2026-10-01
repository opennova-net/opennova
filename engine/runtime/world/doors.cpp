#include <runtime/world/doors.h>
#include <runtime/world/world.h>
#include <runtime/world/pose_provider.h>

#include <algorithm>
#include <cstdio>

namespace opennova::world {

// [orig: Entity_SpawnFromBMSRecord @0x40F25D..0x40F2DA — the door-count loop
//  over FadeEffect_AllocateSlot @0x40F288, the first slot to the entity
//  @0x40F292, the def +0x8A0 / +0x89C words into the slot
//  @0x40F2BB..0x40F2CC]
void DoorSystem::initialize(Entity &entity, int32_t step, int32_t max_angle) {
    if (entity.door_initialized) return;
    entity.door_initialized = true;
    for (int i = 0; i < entity.door_count; ++i) {
        const int index = slots_.size() < kCapacity ? static_cast<int>(slots_.size()) : -1;
        if (i == 0) entity.door_slot = static_cast<int16_t>(index);
        if (index < 0) continue;
        Slot state;
        state.step = step;
        state.max_angle = max_angle;
        state.owner = entity.handle;
        state.owner_lifetime = entity.registry_spawn_id;
        state.number = static_cast<uint8_t>(i + 1);
        slots_.push_back(state);
    }
}

const DoorSystem::Slot *DoorSystem::slot(const Entity &entity, int section) const {
    if (entity.door_slot < 0 || section < 0 || section >= entity.door_count) return nullptr;
    const size_t index = static_cast<size_t>(entity.door_slot) + section;
    if (index >= slots_.size()) return nullptr;
    const Slot &value = slots_[index];
    // Contain retail's exhausted-pool/stale-pointer reads without aliasing a
    // newly spawned entity that reused the same packed handle.
    if (value.owner != entity.handle || value.owner_lifetime != entity.registry_spawn_id)
        return nullptr;
    return &value;
}

// [orig: target @0x43F89A..0x43F8B2; unlike ordinary door lookup, no first-bone subtraction]
bool DoorSystem::target_section_closed(const Entity &entity, int section) const {
    const int index = int(entity.door_slot) + section;
    if (index < 0 || index >= kCapacity) return false;
    return size_t(index) >= slots_.size() || slots_[size_t(index)].state == 0;
}

// [orig: FadeEffect_UpdateAll @0x44E920]
void DoorSystem::tick(World &world) {
    for (Slot &value : slots_) {
        const Entity *entity = world.registry.get(value.owner);
        if (entity == nullptr || entity->registry_spawn_id != value.owner_lifetime) continue;
        if (value.state == 1) {
            value.phase = static_cast<int32_t>(static_cast<uint32_t>(value.phase) +
                    static_cast<uint32_t>(value.step));
            if (value.phase >= 65536) {
                value.phase = 65536;
                value.state = 2;
            }
        } else if (value.state == 3) {
            value.phase = static_cast<int32_t>(static_cast<uint32_t>(value.phase) -
                    static_cast<uint32_t>(value.step));
            if (value.phase <= 0) {
                value.phase = 0;
                value.state = 0;
            }
        }
    }
}

// [orig: Entity_ProcessSectionDamageTransition @0x43F370]
void DoorSystem::command(World &world, Entity &entity, int event, uint32_t touch_mask) {
    if (!entity.door_event) return;
    if (event == 6) entity.door_touch_mask = touch_mask;
    if (event == 6 || event == 7 || event == 8) {
        const uint32_t selected = event == 6 ? entity.door_touch_mask : 0xFFFFFFFFu;
        for (int i = 0; i < entity.door_count; ++i) {
            if ((selected & (1u << (i & 31))) == 0 || slot(entity, i) == nullptr) continue;
            Slot &value = slots_[static_cast<size_t>(entity.door_slot) + i];
            const char *sound = nullptr;
            if ((value.state == 0 && event != 8) || (value.state == 3 && event == 7)) {
                value.state = 1;
                sound = entity.door_open_sound;
            } else if ((value.state == 1 && event == 8) ||
                    (value.state == 2 && event != 7)) {
                value.state = 3;
                sound = entity.door_close_sound;
            }
            if (sound != nullptr && sound[0] != '\0') {
                SoundSlotEvent output;
                output.source_handle = entity.handle.packed;
                output.pos[0] = static_cast<int32_t>(entity.position.x * 65536.0f);
                output.pos[1] = static_cast<int32_t>(entity.position.y * 65536.0f);
                output.pos[2] = static_cast<int32_t>(entity.position.z * 65536.0f);
                if (world.pose_provider != nullptr)
                    world.pose_provider->resolve_section_pivot(world, entity.handle,
                            std::max(0, entity.door_first_bone + i), output.pos);
                std::snprintf(output.set_name, sizeof(output.set_name), "%s", sound);
                world.out.slot_sounds.push_back(output);
            }
        }
    }
    entity.class_think_ticks = 1920;
}

// A door row's state from the host. The row is the entity's first record plus
// number - 1, gated on a def, a non-negative index, a non-zero number and
// number <= the def's door count; the state is stored raw (sign-extended),
// then a closed row's phase is 0 and an open row's 65536, while an opening or
// closing row keeps its phase for the local tick to carry on. No authority
// gate. Retail indexes the global record table; a row past this pool (never
// allocated) is left alone.
// [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250 — the reads
//  @0x431263..0x43128c, the def/index/number gates @0x4312d4..0x4312f8, the
//  store @0x431307, phase 0 @0x43131f, phase 0x10000 @0x431316]
void DoorSystem::apply_wire_row(const Entity &entity, int number, int32_t state) {
    if (!entity.has_item_def || entity.door_slot < 0) return;
    const int index = int(entity.door_slot) + number - 1;
    if (index <= -1 || number == 0 || number > entity.door_count) return;
    if (size_t(index) >= slots_.size()) return;
    Slot &row = slots_[size_t(index)];
    row.state = state;
    if (state == 0)
        row.phase = 0;
    else if (state == 2)
        row.phase = 65536;
}

// [orig: WacCmd_DoorOpen @0x4F70A0 -> @0x43F340]
bool DoorSystem::group_open(const World &world, int32_t group) const {
    bool found = false;
    bool open = false;
    world.registry.for_each_in_pool(2, [&](const Entity &entity) {
        if (found || static_cast<int16_t>(entity.group_id) != group || !entity.door_event) return;
        found = true;
        const Slot *first = slot(entity, 0);
        open = first != nullptr && first->state == 2;
    });
    return open;
}

// WAC visits only pool 2; BMS actions 30/31 visit pool 2 then pool 1.
// [orig: @0x4F7D40/@0x4F7DA0; @0x4541A0/@0x454240]
void DoorSystem::command_group(World &world, int32_t group, bool opening, bool include_pool1) {
    for (int pool_index = 2; pool_index >= (include_pool1 ? 1 : 2); --pool_index) {
        world.registry.for_each_in_pool(pool_index, [&](const Entity &row) {
            if (static_cast<int16_t>(row.group_id) == group && row.door_event)
                command(world, *world.registry.get(row.handle), opening ? 7 : 8);
        });
    }
}

uint64_t DoorSystem::passable_sections(const Entity &entity) const {
    if (entity.item_type != 5) return 0;
    uint64_t mask = 0;
    for (int i = 0; i < entity.door_count; ++i) {
        const Slot *value = slot(entity, i);
        if (value != nullptr && (value->state == 1 || value->state == 2)) {
            const int section = entity.door_first_bone + i;
            if (section >= 0 && section < 64) mask |= uint64_t{1} << section;
        }
    }
    return mask;
}

// [orig: BoneCallback_BuildBoneTransforms @0x4E3070; BoneCallback_AnimatedBones_World @0x4E3180]
int DoorSystem::write_phases(const Entity &entity, int32_t *out, int capacity) const {
    if (!entity.door_motion || out == nullptr) return 0;
    const int count = std::max(0, std::min<int>(entity.door_count, capacity));
    for (int i = 0; i < count; ++i) {
        const Slot *value = slot(entity, i);
        out[i] = value != nullptr ? value->phase : 0;
    }
    return count;
}
} // namespace opennova::world
