// Contextual radio keys and localized chat. The misleading retail symbol
// build_shader_pass_name formats voice-macro names, not shader names.
#pragma once
#include <cstdint>
#include <string>
#include <runtime/world/entity.h>
namespace opennova::world {
class World;
// flags: 1 body prefix, 2 radio, 8 seven fixed emote slots.
// [orig: build_shader_pass_name @0x5BF5D0; sub_5BFB00 @0x5BFB00]
std::string radio_call_key(const World &world, const Entity &speaker, int event,
        uint8_t flags, uint32_t game_type, bool in_active_zone);
std::string radio_call_text(const World &world, const std::string &key,
        const std::string &name, const std::string *location);
} // namespace opennova::world
