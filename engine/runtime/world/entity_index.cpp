// Presentation registration and identity lookup, moved from the host's
// EntityIndex. This bookkeeping does not own simulation or network state.
#include "entity_index.h"

#include <utility>

namespace opennova::world {

uint64_t EntityIndex::origin_key(int64_t kind, int64_t index) {
    return (static_cast<uint64_t>(kind) << 32) | static_cast<uint32_t>(index);
}

void EntityIndex::build(std::vector<EntityIndexEntry> entries,
                       std::vector<mission::AreaTriggerRecord> zones) {
    clear();
    entries_ = std::move(entries);
    zones_ = std::move(zones);
    for (const auto &entry : entries_) {
        if (entry.bms_id != 0) by_bms_id_[entry.bms_id] = entry.token;
        if (entry.kind >= 0 && entry.index >= 0)
            by_origin_[origin_key(entry.kind, entry.index)] = entry.token;
        if (entry.group >= 0) by_group_[entry.group].push_back(entry.token);
    }
}

void EntityIndex::clear() {
    ++generation_;
    by_bms_id_.clear();
    by_origin_.clear();
    by_group_.clear();
    entries_.clear();
    zones_.clear();
}

uint64_t EntityIndex::by_bms_id(int64_t bms_id) const {
    if (bms_id == 0) return 0;
    const auto found = by_bms_id_.find(bms_id);
    return found == by_bms_id_.end() ? 0 : found->second;
}

std::array<uint64_t, 2> EntityIndex::candidates(int64_t bms_id, int64_t kind, int64_t index) const {
    std::array<uint64_t, 2> result{by_bms_id(bms_id), 0};
    if (kind >= 0 && index >= 0) {
        const auto found = by_origin_.find(origin_key(kind, index));
        if (found != by_origin_.end()) result[1] = found->second;
    }
    return result;
}

const std::vector<uint64_t> &EntityIndex::group(int64_t group_id) const {
    static const std::vector<uint64_t> empty;
    if (group_id < 0) return empty;
    const auto found = by_group_.find(group_id);
    return found == by_group_.end() ? empty : found->second;
}

std::vector<uint64_t> EntityIndex::zone(int64_t zone_index) const {
    std::vector<uint64_t> out;
    if (zone_index < 0 || static_cast<size_t>(zone_index) >= zones_.size()) return out;
    const auto &zone = zones_[static_cast<size_t>(zone_index)];
    const Vec3 lo{zone.min_x < zone.max_x ? zone.min_x : zone.max_x,
                  zone.min_y < zone.max_y ? zone.min_y : zone.max_y,
                  zone.min_z < zone.max_z ? zone.min_z : zone.max_z};
    const Vec3 hi{zone.min_x > zone.max_x ? zone.min_x : zone.max_x,
                  zone.min_y > zone.max_y ? zone.min_y : zone.max_y,
                  zone.min_z > zone.max_z ? zone.min_z : zone.max_z};
    // Selection uses inclusive bounds and the authored Z constraint. Zone
    // activity is not a gate for this presentation lookup.
    for (const auto &entry : entries_) {
        const auto &p = entry.position;
        if (p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y) continue;
        if (zone.constrain_z && (p.z < lo.z || p.z > hi.z)) continue;
        out.push_back(entry.token);
    }
    return out;
}

} // namespace opennova::world
