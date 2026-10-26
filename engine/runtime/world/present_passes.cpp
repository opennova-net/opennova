#include <runtime/world/present_passes.h>

#include <cctype>
#include <cmath>

#include <runtime/world/entity.h>

namespace opennova::world {

float husk_present_value(const Entity &entity) {
    if ((entity.engine_flags & kEntityFlagHusk) == 0) return 0.0f;
    return entity.husk_promoted ? 2.0f : 1.0f;
}

// The Flags & 4 swap shows the def's `husk` (present_passes.h).
// The death-piece model is the OTHER way round (huskfinal first,
// destruction.h [orig: @ 0x4934af]); the two picks are kept apart.
std::string husk_render_graphic(const std::string &husk) {
    return husk;
}

// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 (@0x43A145..0x43A26E)]
std::vector<std::string> final_husk_graphics(const std::string &husk,
                                             const std::string &huskfinal) {
    std::vector<std::string> out;
    if (!husk.empty()) {
        std::string variant = husk;
        for (char &c : variant) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (variant.size() >= 2 && variant[variant.size() - 2] == 'x' && variant.back() == 'a') {
            variant.back() = 'b';
            out.push_back(std::move(variant));
        }
    }
    if (!huskfinal.empty()) out.push_back(huskfinal);
    return out;
}

std::string death_piece_graphic(const std::string &husk, const std::string &huskfinal) {
    if (!huskfinal.empty()) return huskfinal;
    return husk;
}

bool husk_identity_is_dynamic(int32_t bms_id, int64_t spawn_origin, int32_t wire_handle) {
    if (wire_handle < 0 || wire_handle == static_cast<int32_t>(EntityHandle::kInvalid) ||
        bms_id != 0) {
        return false;
    }
    // A real authored origin remains canonical even when its BMS id is zero.
    // Runtime-only entities carry either no origin or the promotion sentinel.
    return spawn_origin == static_cast<int64_t>(kSpawnOriginNone);
}

std::string husk_identity_key(int32_t bms_id, int64_t spawn_origin, int32_t wire_handle) {
    if (husk_identity_is_dynamic(bms_id, spawn_origin, wire_handle)) {
        return "wire:" + std::to_string(wire_handle);
    }
    const uint32_t origin = static_cast<uint32_t>(spawn_origin);
    return std::to_string(bms_id) + ":" + std::to_string(spawn_origin_kind(origin)) + ":" +
           std::to_string(spawn_origin_index(origin));
}

bool husk_settle_keeps_carved_tilt(float pitch_deg, float roll_deg) {
    // The shell's is_zero_approx: |v| < CMP_EPSILON (1e-5), compared in
    // double as the script runtime did.
    constexpr double kZeroEpsilon = 0.00001;
    return std::fabs(static_cast<double>(pitch_deg)) < kZeroEpsilon &&
           std::fabs(static_cast<double>(roll_deg)) < kZeroEpsilon;
}

} // namespace opennova::world
