#pragma once

#include <cstdint>
#include <string>

namespace opennova::world {

// Listener-relative and interface sounds, distinct from positional SoundSlotEvent.
// [orig: Sound_PlayTriggerSetScaled @0x527B90; NapiNPClientMsg_PlaySoundByName @0x4283A0]
struct ScriptSoundEvent {
    enum class Kind { ListenerRelative, Interface };
    std::string name;
    int32_t distance_q16 = 0;
    int32_t bearing = 0; // raw WAC heading argument; the audio bearing is a byte turn
    Kind kind = Kind::ListenerRelative;
};

} // namespace opennova::world
