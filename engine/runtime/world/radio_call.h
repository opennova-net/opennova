// Contextual radio keys and localized chat. The misleading retail symbol
// VMacros_BuildShaderPassName formats voice-macro names, not shader names.
#pragma once
#include <cstdint>
#include <string>
#include <runtime/world/entity.h>
namespace opennova::world {
class World;
// flags: 1 body prefix, 2 radio, 8 seven fixed emote slots.
// [orig: VMacros_BuildShaderPassName @0x5BF5D0; VMacros_BuildRemappedName @0x5BFB00]
std::string radio_call_key(const World &world, const Entity &speaker, int event,
        uint8_t flags, uint32_t game_type, bool in_active_zone);
std::string radio_call_text(const World &world, const std::string &key,
        const std::string &name, const std::string *location);
// The voice-macro table's "macrotext" entry text for `key` (null on a miss),
// searched as every section lookup is: the expansion's override table first
// (world.tables.text_override), then vmacros.bin. The F9 / F10 menus and the
// radio chat line read it.
// [orig: VMacros_GetMacroText @0x5B7170, the call @0x5B7196;
//  HUD_DrawRadioTitleMenu @0x5BFBD5 / @0x5BFC5D; HUD_DrawEmotesMenu
//  @0x5BFF45 / @0x5BFFCD -> TextResource_FindEntryBySectionAndKey @0x75D250]
const std::string *voice_macro_text(const World &world, const std::string &key);
// The best percentage (0..100) of a neutral KOTH hill (def type 6006) the
// entity stands in, 0 outside every hill; the radio contexts and the HUD's
// "In the Zone" line both test it for nonzero.
// [orig: CaptureZone_FindMaxProximityCoverage @0x5BF4D0]
int32_t capture_zone_max_coverage(const World &world, const Entity &entity);
} // namespace opennova::world
