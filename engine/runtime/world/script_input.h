// Mission script controls over the local fire binding, shared by WAC and the
// engine input dispatcher. A request is observable even when it is refused.
#pragma once

#include <array>

namespace opennova::world {

class ScriptWeaponInput {
public:
    // [orig: Input_HandleActionBinding_0 @0x4E0968..0x4E097B]
    bool record_fire_request(int category) {
        if (!valid(category)) return true;
        requested_[category] = true;
        return !blocked_[category];
    }

    // [orig: WacCmd_WeaponFired @0x4ED360]
    bool fire_requested(int category) const {
        return valid(category) && requested_[category];
    }

    // [orig: WacCmd_BlockFire @0x4EE140]
    bool set_fire_blocked(int category, bool blocked) {
        if (!valid(category)) return false;
        blocked_[category] = blocked;
        return true;
    }

    // End of each completed script pass, not each logic tick.
    // [orig: WacScript_DecrementTimersAndResetCounters @0x4EE6D0]
    void clear_fire_requests() { requested_.fill(false); }

    // [orig: WacScript_InitAndLoad @0x4F967B..0x4F9681]
    void reset() { requested_.fill(false); blocked_.fill(false); }

private:
    static constexpr int kCategories = 10;
    // Negative script arguments are guarded instead of indexing before the
    // retail arrays. Categories >=10 bypass this gate in the original.
    static bool valid(int category) { return category >= 0 && category < kCategories; }
    std::array<bool, kCategories> requested_{};
    std::array<bool, kCategories> blocked_{};
};

} // namespace opennova::world
