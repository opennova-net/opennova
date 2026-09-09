// The eight BMS teammate pickup/flyover operations.
// [orig: slots @0xAC4F48, count @0xAC4F40; HeliLift_* @0x451730..0x452A20]
#pragma once

#include <array>
#include <cstddef>
#include <runtime/world/entity_registry.h>

namespace opennova::world {
class World;

// Asset-backed allocation is supplied by MissionKernel. The operation owns
// the exact item, identity and pose; the mission owns DEF/3DI/ADM/AIP loading.
struct TeammateSpawn {
    uint16_t item_type = 0;
    uint16_t ssn = 0;
    int32_t pos[3] = {};
    int32_t heading = 0;
    bool helicopter = false;
};
class ITeammateSpawner {
public:
    virtual ~ITeammateSpawner() = default;
    virtual EntityHandle spawn_teammate(const TeammateSpawn &request) = 0;
};

class TeammateOperations {
public:
    static constexpr size_t kCapacity = 8;
    enum class State : int32_t {
        ApproachPatient, Treat, Return, Boarded, FlyToHover,
        Descend, Land, Ascend, Depart, Finished
    };
    struct Slot {
        EntityLifetime patient, first, second, helicopter;
        std::array<int32_t, 3> goal{};
        int32_t heading = 0;
        State state = State::Finished;
        int32_t assisting = 0, medic = 0, deadline = 0;
    };

    void reset(World &world);
    bool start(World &world, int32_t subtype, int32_t patient_ssn, int32_t marker_number);
    void tick(World &world);
    size_t count() const { return count_; }
    const Slot *at(size_t index) const { return index < count_ ? &slots_[index] : nullptr; }

private:
    void update(World &world, Slot &slot);
    std::array<Slot, kCapacity> slots_{};
    size_t count_ = 0;
};
} // namespace opennova::world
