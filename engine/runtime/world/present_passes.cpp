#include <runtime/world/present_passes.h>

#include <cmath>

#include <runtime/world/entity.h>

namespace opennova::world {

// The Flags & 4 swap shows the def's `husk`, falling back to `huskfinal`.
// Retail's render pick takes the husk model (+0x34) or the graphic [orig:
// Render_SectorEntity @ 0x5C4190 (the Flags & 4 test @ 0x5C41A0)]; the
// collision picks take the same +0x34 [orig: Entity_RaycastCollisionModel
// @ 0x413060 (the pick @ 0x41307B..0x413088); Physics_RaycastAgainstEntityPool
// @ 0x538720 (the pick @ 0x538A15)]. The `huskfinal` fallback has no such
// witness (present_passes.h).
// The death-piece model is the OTHER way round (huskfinal first,
// destruction.h [orig: @ 0x4934af]); the two picks are kept apart.
std::string husk_render_graphic(const std::string &husk, const std::string &huskfinal) {
    if (!husk.empty()) return husk;
    return huskfinal;
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
