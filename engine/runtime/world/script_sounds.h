#pragma once

#include <cstdint>
#include <string>

namespace opennova::world {

// The direct listener-space sound descriptor, distinct from positional
// SoundSlotEvent. [orig: Sound_PlayTriggerSetScaled @0x527B90]
struct ScriptSoundEvent {
    std::string name;
    int32_t distance_q16 = 0;
    int32_t bearing = 0; // raw WAC heading argument; the audio bearing is a byte turn
};

} // namespace opennova::world
