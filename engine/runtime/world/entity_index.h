#pragma once

#include <formats/mission/mission.h>
#include <runtime/world/geom.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::world {

// A registered presentation instance. Tokens are opaque host identities;
// zero is absent. Positions are mission-space placement positions.
struct EntityIndexEntry {
    uint64_t token = 0;
    int64_t bms_id = 0;
    int64_t kind = -1;
    int64_t index = -1;
    int64_t group = -1;
    Vec3 position{};
};

// Host lookup for presented models, separate from the simulation's entity
// pools. The host checks token liveness when consuming candidates/results.
class EntityIndex {
public:
    void build(std::vector<EntityIndexEntry> entries, std::vector<mission::AreaTriggerRecord> zones);
    void clear();
    uint64_t generation() const { return generation_; }
    // Try the file identity first, then the origin if the primary is absent
    // or no longer live. The two origins retain all 32 bits of each field.
    std::array<uint64_t, 2> candidates(int64_t bms_id, int64_t kind, int64_t index) const;
    uint64_t by_bms_id(int64_t bms_id) const;
    const std::vector<uint64_t> &group(int64_t group_id) const;
    std::vector<uint64_t> zone(int64_t zone_index) const;
    const std::vector<EntityIndexEntry> &entries() const { return entries_; }

private:
    static uint64_t origin_key(int64_t kind, int64_t index);
    std::unordered_map<int64_t, uint64_t> by_bms_id_;
    std::unordered_map<uint64_t, uint64_t> by_origin_;
    std::unordered_map<int64_t, std::vector<uint64_t>> by_group_;
    std::vector<EntityIndexEntry> entries_;
    std::vector<mission::AreaTriggerRecord> zones_;
    uint64_t generation_ = 0;
};

} // namespace opennova::world
