// The D-COL-5 ladder legs shared between the motor TU (the jump/bottom tails
// inside tick_infantry) and the ladder TU (the org2 override + org1 block).
// Witness record: docs/world/world-wac-ai-re.md §30.
#ifndef OPENNOVA_WORLD_INFANTRY_LADDER_H
#define OPENNOVA_WORLD_INFANTRY_LADDER_H

#include <cstdint>

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

namespace opennova::world {

struct AiEntity;

// The 0.5u push off the ladder face along -yaw, shared by the back/side
// dismounts, the on-ladder jump, and the grounded bottom exit. Full-precision
// x87-style sincos at 2^22, truncated, then the (<<15)>>22 pair.
// [orig: @ 0x4b75b4-0x4b75c7 / 0x4b7666-0x4b7679 / 0x4b76b3-0x4b76c6 /
//  0x4b7f49-0x4b7f68 / 0x4b7ff3-0x4b8016]
void ladder_push_back(int32_t pos[3], int32_t yaw_bam);

// The shared dismount tail. [orig: Flags &= ~0x100000 @ 0x4b75ca/0x4b767c/
//  0x4b76c9/0x4b8019]
void ladder_unlatch(Entity *ent);

// While latched, the view yaw is clamped to ±120° (0x55555500) of the body
// heading — the ladder does not let the mouse spin the aim behind the climber;
// the clamped write reaches the mouse accumulator through the embedder
// write-back. The parachute/carried legs of the same 0x100060 gate and its
// carried-parent exemptions ride their slices.
// [orig: gate @ 0x4b4978; clamp ±0x55555500 @ 0x4b4b04-0x4b4b42;
//  g_LocalPlayerLookYaw mirror @ 0x4b4b2a]
void infantry_ladder_view_clamp(InfantryState &inf, uint32_t entity_flags);

// The climb-motor channels the resolver's CL legs read and write, built over
// this entity's InfantryState. The returned struct holds POINTERS into
// e/e.inf — build it at the resolve call site and consume it immediately.
// [orig: the resolver reads the same entity fields inline
//  @ 0x4b3245-0x4b3495 / @ 0x4b3c5c-0x4b3d55]
LadderResolveIO make_ladder_resolve_io(AiEntity &e, int32_t tick_start_z);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_INFANTRY_LADDER_H
