#include "radio_call.h"
#include "world.h"
#include <runtime/audio/sound_profile.h>
#include <base/io/strutil.h>
#include <cmath>
#include <utility>

namespace opennova::world {
namespace {
const char *vehicle_prefix(int type, const char *fallback) {
    switch (type) {
    case 1: return "WL_";
    case 2: return "TN_";
    case 3: case 4: return "HO_";
    case 5: case 6: case 7: case 8: return "BT_";
    case 12: return "DB_";
    default: return fallback;
    }
}
// The 6006 coverage probe has a strict interior test after integer percentage
// quantization. The boundary and the outermost <1% ring both return zero.
// [orig: find_max_proximity_coverage @0x5BF4D0]
bool has_proximity_coverage(const World &world, const Entity &speaker) {
    for (size_t i = 0; i < world.registry.pool_capacity(3); ++i) {
        const Entity *e = world.registry.get(EntityHandle::make(3, static_cast<int>(i)));
        if (!e || !e->has_item_def || e->item_id != 6006 || e->team != 0) continue;
        const int32_t radius = to_fixed(e->bound_radius);
        if (radius <= 0) continue;
        const auto abs_delta = [](int32_t a, int32_t b) {
            const int32_t delta = static_cast<int32_t>(uint32_t(a) - uint32_t(b));
            return delta < 0 ? static_cast<int32_t>(0u - uint32_t(delta)) : delta;
        };
        const int64_t x = abs_delta(to_fixed(e->position.x), to_fixed(speaker.position.x));
        const int64_t y = abs_delta(to_fixed(e->position.y), to_fixed(speaker.position.y));
        if (x > radius || y > radius) continue;
        const int32_t squared = static_cast<int32_t>(((x*x + 0x8000) >> 16) + ((y*y + 0x8000) >> 16));
        // x87's invalid sqrt -> integer-indefinite -> SHL 8 is zero. Preserve
        // that result without a C++ NaN-to-integer conversion. IMUL 100 also
        // wraps before the signed division; widening it changes large zones.
        const int32_t distance = squared < 0 ? 0 : static_cast<int32_t>(std::sqrt(double(squared))) * 256;
        const int32_t numerator = static_cast<int32_t>(100u * (uint32_t(radius) - uint32_t(distance)));
        if (distance <= radius && numerator / radius > 0) return true;
    }
    return false;
}
}

std::string radio_call_key(const World &world, const Entity &speaker, int event,
        uint8_t flags, uint32_t game_type, bool in_active_zone) {
    std::string prefix;
    if (flags & 1) prefix = std::string(audio::body_model_prefix(speaker.anim_slot)) + "_";
    prefix += (flags & 2) ? "RAD_" : "EMO_";
    if (((flags & 2) && event < 9) || ((flags & 8) && event < 8))
        return prefix + std::to_string(event);
    if ((flags & 2) && event > 8) event -= 8;
    else if ((flags & 8) && event > 7) event -= 7;
    std::string context;
    const Entity *parent = world.registry.get(speaker.mount_target);
    const int type = parent && parent->has_item_def ? parent->item_unit_type : 0;
    switch (speaker.mount_type) {
    case SeatType::Passenger:
        context = (type == 3 || type == 4) ? "HO_PASS" :
            (type >= 5 && type <= 8) ? "BT_PASS" : type == 12 ? "DB_PASS" : "WL_PASS";
        break;
    case SeatType::Controller: case SeatType::Driver:
        context = vehicle_prefix(type, "WL_");
        break;
    case SeatType::Gunner: {
        const Entity *hull = parent ? world.registry.get(parent->emplacement_parent) : nullptr;
        if (!hull && parent) hull = world.registry.get(parent->ground_target);
        context = vehicle_prefix(hull && hull->has_item_def ? hull->item_unit_type : 0, "");
        const auto *weapon = world.tables.weapons.by_index(speaker.equipped_adm_index);
        if (weapon) context += weapon->voice_macro_token;
        break;
    }
    default:
        if (game_type < 2) context = "MP_DM";
        else if (game_type == 0x10000) context = "MP_TDM";
        else if (game_type == 8 || game_type == 0x10008) context = "MP_FB";
        else if (game_type == 0x10001 && has_proximity_coverage(world, speaker)) context = "MP_TKTH";
        else if (game_type == 0x10004) context = "MP_CTF";
        else if (game_type == 0x90002 && has_proximity_coverage(world, speaker)) context = "MP_S&D";
        else if (game_type == 0x10002) context = "MP_AD";
        else if (game_type == 0x10010 && in_active_zone) context = "MP_A&S";
        else switch (speaker.player_class) {
        case 5: context = "MEDIC"; break;
        case 6: context = "SNIPER"; break;
        case 7: context = "GUNNER"; break;
        case 8: context = "RIFLE"; break;
        default: context = "ENG"; break;
        }
    }
    std::string key = prefix + context + std::to_string(event);
    static constexpr std::pair<const char *, const char *> remaps[] = {
        {"EMO_DB_3", "EMO_BT_1"}, {"EMO_DB_PASS1", "EMO_WL_PASS3"},
        {"EMO_DB_PASS2", "EMO_WL_PASS1"}, {"EMO_TN_1", "EMO_BT_2"},
        {"EMO_TN_2", "EMO_BT_1"}, {"EMO_TN_50CAL1", "EMO_TN_COMM1"},
        {"EMO_TN_50CAL2", "EMO_TN_COMM2"}, {"EMO_TN_50CAL3", "EMO_TN_COMM3"},
        {"EMO_TN_GUNN1", "EMO_WL_50CAL2"}, {"EMO_TN_GUNN2", "EMO_ENG2"},
        {"EMO_TN_GUNN3", "EMO_WL_50CAL3"}, {"RAD_DB_1", "RAD_WL_1"},
        {"RAD_DB_2", "RAD_WL_2"}, {"RAD_DB_PASS1", "RAD_WL_1"},
        {"RAD_DB_PASS2", "RAD_WL_PASS2"}, {"RAD_TN_2", "RAD_WL_2"},
        {"RAD_TN_50CAL1", "RAD_TN_COMM1"}, {"RAD_TN_50CAL2", "RAD_TN_COMM2"},
    };
    for (const auto &remap : remaps) {
        const std::string from = remap.first;
        if (key.size() >= from.size() && strutil::iequals(key.substr(key.size()-from.size()), from))
            key.replace(key.size()-from.size(), from.size(), remap.second);
    }
    return key;
}

// [orig: NapiNPClientMsg_HandleEntityDeath @0x430C50; VMacros_GetMacroText @0x5B7170]
std::string radio_call_text(const World &world, const std::string &key,
        const std::string &name, const std::string *location) {
    const auto *entry = world.tables.voice_macros.find_in_section("macrotext", key);
    return name + (location ? ":[" + *location + "]: " : ": ") + (entry ? entry->text : key);
}
} // namespace opennova::world
