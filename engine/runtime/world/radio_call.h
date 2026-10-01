// Contextual radio keys and localized chat. The misleading retail symbol
// VMacros_BuildShaderPassName formats voice-macro names, not shader names.
#pragma once
#include <cstdint>
#include <string>
#include <runtime/world/entity.h>
namespace opennova::world {
class World;
// flags: 1 body prefix, 2 radio, 8 seven fixed emote slots.
// [orig: VMacros_BuildShaderPassName @0x5BF5D0; sub_5BFB00 @0x5BFB00]
std::string radio_call_key(const World &world, const Entity &speaker, int event,
        uint8_t flags, uint32_t game_type, bool in_active_zone);
std::string radio_call_text(const World &world, const std::string &key,
        const std::string &name, const std::string *location);
// The best percentage (0..100) of a neutral KOTH hill (def type 6006) the
// entity stands in, 0 outside every hill; the radio contexts and the HUD's
// "In the Zone" line both test it for nonzero.
// [orig: CaptureZone_FindMaxProximityCoverage @0x5BF4D0]
int32_t capture_zone_max_coverage(const World &world, const Entity &entity);
} // namespace opennova::world
