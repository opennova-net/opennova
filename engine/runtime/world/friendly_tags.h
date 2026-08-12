#pragma once

// The friendly-tags gather (D-HUD-20): which entities get an overhead name
// label this frame, with the per-entity facts the HUD compiler's element
// consumes (engine/runtime/hud hud_frame.h HudFriendlyTag). The projection,
// view distance, and fog feed are the presenter's; every selection gate here
// is the witnessed pass. [orig: HUD_DrawFriendlyTagsPass @0x5a4480 +
// the HUD_DrawEntityLabel entry bails @0x5a39eb..0x5a39ff]
// Witness record: docs/interface/hud-re.md (D-HUD-20).

#include "world/entity.h"

#include <string>
#include <vector>

namespace opennova::world {

struct World;

struct FriendlyTagSource {
    EntityHandle entity;
    uint16_t net_id = 0; // the fallback-name index [orig: (pool<<12)|slot]
    Vec3 position;       // raw entity position; the presenter lifts + projects
    std::string name;    // authored display name; empty -> the compiled-in table
    int32_t health_ratio_fp16 = 0x10000;
    // The entity's eye-offset z (entity+0x74, 16.16): the anchor is
    // position.z + this + 0x4000 [orig: HUD_DrawEntityLabel @0x5a3a84..0x5a3a98].
    int32_t eye_offset_z = 0;
    bool player = false;
};

// The pool-0 walk of the tags pass: non-player organics on the local team (or
// neutral), alive with a resolved item def. The slot/player walk (callsign +
// squad/channel legs) and the enemy-visibility grant are the documented
// residues in the record. [orig: HUD_DrawFriendlyTagsPass @0x5a44b0..0x5a4505]
void collect_friendly_tags(World &world, const Entity &local,
                           std::vector<FriendlyTagSource> &out);

} // namespace opennova::world
