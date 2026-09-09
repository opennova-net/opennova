// Authored target policy, shared by acquisition and weapon validation.
#pragma once

#include <cstdint>

namespace opennova::world {

enum class AiTargetSelector { ExclusiveSsn, PreferredSsn, ExclusiveGroup, PreferredGroup };

// Four independent entity words. Zero disables an individual selector.
// [orig: Entity_FindTargets @0x53A610; Entity_ValidateWeaponTarget @0x53A400]
struct AiTargetSelectors {
    uint16_t exclusive_ssn = 0;    // entity+332
    uint16_t preferred_ssn = 0;    // entity+334
    uint16_t exclusive_group = 0;  // entity+336
    uint16_t preferred_group = 0;  // entity+338

    void set(AiTargetSelector field, int32_t value) {
        const auto word = static_cast<uint16_t>(value);
        switch (field) {
            case AiTargetSelector::ExclusiveSsn: exclusive_ssn = word; break;
            case AiTargetSelector::PreferredSsn: preferred_ssn = word; break;
            case AiTargetSelector::ExclusiveGroup: exclusive_group = word; break;
            case AiTargetSelector::PreferredGroup: preferred_group = word; break;
        }
    }

    bool admits_team(int32_t ssn, int32_t group) const {
        return (exclusive_ssn != 0 && exclusive_ssn == ssn) ||
                (preferred_ssn != 0 && preferred_ssn == ssn) ||
                (exclusive_group != 0 && exclusive_group == group) ||
                (preferred_group != 0 && preferred_group == group);
    }

    bool allows(int32_t ssn, int32_t group) const {
        return (exclusive_ssn == 0 || exclusive_ssn == ssn) &&
                (exclusive_group == 0 || exclusive_group == group);
    }

    int32_t adjust_score(int32_t score, int32_t ssn, int32_t group) const {
        // Literal signed shifts, also for negative nearest-threat scores.
        // [orig: Weapon_CalcDamageByType @0x53943C..0x53946E]
        if (preferred_ssn != 0 && preferred_ssn != ssn) score >>= 2;
        if (preferred_group != 0 && preferred_group != group) score >>= 2;
        return score;
    }
};

} // namespace opennova::world
