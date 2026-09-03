// The presentation-side law the entity present passes share (ADR 0043 d9):
// the husk RENDER-graphic pick, the destroyed entity's presentation identity
// (the dynamic-vs-authored discriminator and the key it spells), and the
// settling batched graft's yaw-only tilt retention. Portable decisions over
// the drained destruction / scar values; the presenting shell
// (godot/src/simulation DestructionPresenter, godot/src/world ScarPresenter)
// grafts the models and resolves the live nodes.
#ifndef OPENNOVA_WORLD_PRESENT_PASSES_H
#define OPENNOVA_WORLD_PRESENT_PASSES_H

#include <cstdint>
#include <string>

namespace opennova::world {

// The husk RENDER graphic the Flags & 4 swap shows: the def's `husk` first,
// `huskfinal` when no `husk` is authored, "" when neither is — the witnessed
// render + collision pick [orig: Entity_RaycastCollisionModel @ 0x413086; the
// collision-model pick @ 0x538720 shares the husk-first order]. Deliberately
// NOT unified with the death-piece model pick, which is huskFINAL first
// (destruction.h ItemDeathTraits::husk_section_count [orig: @ 0x4934af]):
// the two differ on purpose.
std::string husk_render_graphic(const std::string &husk, const std::string &huskfinal);

// Authored destruction rows retain the mission-present value identity: file
// BMS id plus packed (kind, index), with the origin as the zero-id leg.
// Synthetic runtime entities instead use their packed wire handle because
// siblings share both zero BMS id and the non-BMS origin sentinel. A real
// authored origin remains canonical even when its BMS id is zero;
// runtime-only entities carry either no origin or the promotion sentinel.
// `spawn_origin` is the packed word as the drains carry it (a widened
// uint32_t: kSpawnOriginNone compares as its positive value, never as -1);
// `wire_handle` < 0 or EntityHandle::kInvalid means no wire identity.
bool husk_identity_is_dynamic(int32_t bms_id, int64_t spawn_origin, int32_t wire_handle);

// The identity key the present passes cache by: "wire:<handle>" for a
// dynamic row, else "<bms_id>:<origin kind>:<origin index>".
std::string husk_identity_key(int32_t bms_id, int64_t spawn_origin, int32_t wire_handle);

// Compact peer poses carry yaw only. Static death motion changes position but
// not orientation, so a settling node-less husk graft retains the exact
// authored basis carved from the batch when pitch/roll are unavailable
// (both zero); host/listen poses carry the full Euler angles and take the
// live basis. The zero test is the shell's is_zero_approx (|v| < 1e-5).
bool husk_settle_keeps_carved_tilt(float pitch_deg, float roll_deg);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PRESENT_PASSES_H
