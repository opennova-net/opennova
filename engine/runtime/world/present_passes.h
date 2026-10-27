// The presentation-side law the entity present passes share (ADR 0043 d9):
// the husk RENDER-graphic pick, the destroyed entity's presentation identity
// (the dynamic-vs-authored discriminator and the key it spells), and the
// settling batched graft's yaw-only tilt retention. Portable decisions over
// the drained destruction / scar values; the presenting shell
// (godot/src/simulation DestructionPresenter, godot/src/world ScarPresenter)
// grafts the models and resolves the live nodes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

struct Entity;

// The row's husk word (present_rows.h PF_HUSK): 0 intact, 1 husked (Flags & 4),
// 2 husked with the swap stage's final husk promoted into the husk.
float husk_present_value(const Entity &entity);

// The husk RENDER graphic the Flags & 4 swap shows at the death: the def's
// `husk` (entity+0x34), "" when none is authored, which keeps the intact
// graphic drawn. Retail's draw takes the entity's husk model under Flags & 4
// and the graphic when there is none [orig: Render_SectorEntity @ 0x5C4190
// (the Flags & 4 test @ 0x5C41A0)]; the collision picks take the same +0x34
// [orig: Entity_RaycastCollisionModel @ 0x413060 (the pick @ 0x41307B..0x413088);
// Physics_RaycastAgainstEntityPool @ 0x538720 (the pick @ 0x538A15)]. A final
// husk is drawn only through the swap stage (world::update_husk_swap_stage):
// the pending +0x38 inside the fade, then promoted into +0x34. Deliberately
// NOT unified with the death-piece model pick, which is huskFINAL first
// (destruction.h ItemDeathTraits::husk_section_count [orig: @ 0x4934af]):
// the two differ on purpose.
std::string husk_render_graphic(const std::string &husk);

// The final husk's (entity+0x38's) candidate graphics in the load's order: the
// husk name lower-cased with a trailing "xa" made "xb", when it ends so, then
// `huskfinal`; the first that loads is the final husk, none when neither does.
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 - strlwr and the "xa" test
//  @0x43A145..0x43A191, the "xb" load @0x43A1C8, huskfinal while none loaded
//  @0x43A245 / @0x43A26E]
std::vector<std::string> final_husk_graphics(const std::string &husk,
                                             const std::string &huskfinal);

// The death-piece MODEL graphic: the def's `huskfinal` first, `husk` when no
// `huskfinal` is authored, "" when neither is — the piece spawn's pick
// [orig: Entity_SpawnDeathPieces @ 0x4934af..0x4934c3 huskFinalModel ?:
// huskModel, the same pick @ 0x493695..0x4936a3 stored at piece+0]. The
// presenter falls back to `husk` when the `huskfinal` model does not load,
// as the pointer pick does.
std::string death_piece_graphic(const std::string &husk, const std::string &huskfinal);

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
