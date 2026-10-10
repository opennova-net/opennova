// Spawn-state init for runtime entities — the field-init half of the host player-spawn
// machine (docs/net/novaworld-net-re.md §5.2b). Kept separate from promotion
// (engine/runtime/mission) so the runtime/replication/Phase-2 in-process listen-server player spawn can reuse it
// without pulling in a mission dependency (engine/runtime/world stays Godot- and mission-agnostic).
#pragma once

#include <cstdint>

#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

namespace opennova::world {

class World;
class AiSystem;

// [orig: Entity_ResetToSpawnState @0x4B9610] Entity-only seeding is for a
// fresh row before its motor is attached. Live rows use the World overload:
// it also resets the motor, clears references/mounts and refreshes collision.
void entity_reset_to_spawn_state(Entity &e);
void entity_reset_to_spawn_state(World &world, Entity &e);
void entity_reset_to_spawn_state(World &world, AiSystem &ai, Entity &e);

// Fresh NPC initialization after its ADM, definition and collision bindings.
// [orig: Entity_InitOrganicAI @0x4BFCC0; warmup @0x4B8B20]
void initialize_organic_ai(World &world, Entity &e);

// What the organic init reads to choose the body state a person starts in: its definition
// callback, which both person classes run (the org0 and org1 rows of the event-callback table
// name it as their second function) [orig: g_EntityClassEventCallbackTable @0x813018 / @0x813030
// -> Entity_InitOrganicAI @0x4BFCC0]. The world fills it from the live entity; the editor's
// mission view fills it from a placed record and its definition (docs/world/world-wac-ai-re.md
// section 41).
struct OrganicSpawnFacts {
    // The AI slot's has-route word (+0x8C) and its route channel (+0x94): set from the record's
    // waypoint_id when it is nonzero [orig: Entity_SpawnFromBMSRecord, slot+140/+148]. A body
    // with no AI slot (a definition without `aidata`) has neither.
    bool route = false;
    int32_t route_channel = 0;
    // The entity Flags (+0x24) as the init reads them: 0x200 the queued Co-op mount (a player
    // body's alone), 0x40 the guard (the record's Guarding attribute on an `aidata` definition
    // [orig: Entity_SpawnFromBMSRecord @0x40ED9F], or a mount).
    uint32_t flags = 0;
    // A rotor-wash zone within 15 units [orig: the query @0x4C001B -> @0x5CBE30].
    bool rotor_wash = false;
    // A parent (+0x16C), and its definition's phrase_set (+0x86C, 0 for none or no definition).
    bool parented = false;
    int parent_phrase_set = 0;
};

// Whether an item's ai_function class runs that init: the event-callback table's org0 and org1
// rows (found by the table walk's whole-tag stricmp) name Entity_InitOrganicAI as their second
// function, and no other row does [orig: g_EntityClassEventCallbackTable @0x813018 / @0x813030;
// the lookup Entity_LookupRenderCallbacks @0x407dc0, stricmp @0x407de2].
bool organic_init_class(const char *ai_function);

// The facts a placed record hands the init through its spawn, before any mount, parent or rotor's
// wash: on a definition with `aidata` (attrib 0x100000, the AI slot) the record's waypoint_id is
// the slot's has-route word and route channel, and its Guarding attribute (2) folds to Flags 0x40
// (kEntityFlagMounted); a body with no AI slot has neither [orig: Entity_SpawnFromBMSRecord
// `test [eax+54h],100000h` @0x40ED4E; the fold 2 -> Flags 0x40 @0x40ED9F; slot+140/+148 from the
// record's waypoint_id]. The world's spawn reaches the same facts through the slot and the
// entity's flags (promote's init_ai_slot / fold_ai_entity_flags).
OrganicSpawnFacts organic_spawn_facts_from_record(bool ai_slot, int32_t waypoint_id,
                                                  uint32_t bmsi_attributes);

// The warmup's final ground solve sets a body down when its feet stand under one unit (16.16) over
// what lies under them, a positive clearance included [orig: Entity_WarmUpOrganicAnimation
// @0x4B8BD8..0x4B8BF5, the cmp 10000h @0x4B8BE7].
inline constexpr int32_t kOrganicWarmupSettleQ16 = 0x10000;

// The primary state the init requests, against `source`'s clips for `adm_id` (a state is there
// when its .adm authors its row): 43 idle, or walk 1 on a route; sit 76 under Flags 0x200 and
// guard 140 under Flags 0x40 where authored; route 126 idle_2 44, route 127 idle 43; near a
// rotor's wash the wash rows 28 / 29 / 27 in place of a walk, a jog or run, an idle; a parented
// body 67, or 68..75 by its parent's phrase_set where authored. The secondary starts at 43.
// [orig: Entity_InitOrganicAI @0x4BFF8F..0x4C01BD]
int organic_spawn_state(const OrganicSpawnFacts &facts, const IRootMotionSource *source, int adm_id);

// How many dual updates the warmup runs before its final step, by the entity's id (+0x7C, the
// record's id [orig: Entity_SpawnFromBMSRecord @0x40EBBE..0x40EBC1]): 8 * ((id & 12) + 8 *
// ((id & 2) + 4 * (id & 1))) + 10, 10 to 490. An unparented body runs one more after them.
// [orig: Entity_WarmUpOrganicAnimation @0x4B8B3F..0x4B8BA3, the final update @0x4B8BC1]
uint32_t organic_warmup_updates(uint32_t net_id);

// A placed person's body as the game's init and its warmup leave it, before its first AI tick:
// both channels started on the .adm's reset row and requesting their states (the secondary 43,
// the primary organic_spawn_state's), then the warmup's dual updates run. `rings` is the
// variant-ring heads the spawn shares with every body of the .adm spawned before it, so a caller
// posing a mission's people passes one through them in their pool order. With no clips (`source`
// null or `adm_id` < 0) the body stands on its requests at phase 0, as the world's does.
// [orig: Entity_InitOrganicAI @0x4BFCC0; AnimMap_RegisterEntity @0x40BB60;
//  Entity_WarmUpOrganicAnimation @0x4B8B20]
struct OrganicSpawnBody {
    InfantryBodyPose pose;
    // The height the warmup's vertical root motion adds to an unparented body (16.16; 0 for a
    // parented one), and the last frame's capsule, which the ground solve after it reads: the
    // capsule bottom is the origin's (the hips') height over the ground, so a record placed on the
    // ground spawns its origin lifted by about that much.
    int32_t rise = 0;
    int32_t capsule_bottom = 0, capsule_top = 0;
    // The body's channels as the init and the warmup leave them, for a caller that ticks it on
    // (infantry_org1_motor_head).
    InfantryState channels;
};
OrganicSpawnBody organic_spawn_pose(const OrganicSpawnFacts &facts, uint32_t net_id, IRootMotionSource *source,
                                    AnimVariantRings &rings, int adm_id);

enum class NpcCorpseStep { Kept, Respawned, Removed };
bool npc_respawn_unhide(World &world, const AiSystem &ai, Entity &e);
NpcCorpseStep step_npc_corpse(World &world, AiSystem &ai, Entity &e);

} // namespace opennova::world
