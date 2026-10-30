// The per-item ITEMS.DEF particle-effect law — slot A ('particlefx <effect>
// <userpoint>'), the always-on emitter the mission-start walk attaches to a
// placed entity: which pools/attribs admit the attach and where on the model
// the emitters go, over the parsed item row's attrib word (formats/def) and the
// model's userpoint table (formats/threedi), and the world's producers on the
// entity's +0x1CC slot (ItemEmitterSystem), which spawn the groups and re-pose
// them from entity state. The watercraft W3/W4 lanes route separately through
// the portable vehicle motor and fixed-tick presenter; fxs/W1/W2 remain
// movement threads (ptl-format-re.md §8).
// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0 (mission
//  start, entity pools 1-3) -> ItemDef_GetBoneMaskByName @ 0x49ea40 (first
//  16, stricmp) -> Entity_SpawnBoneTrailEffect @ 0x43bef0 (one mode-2 attached
//  emitter per masked userpoint: pos = the userpoint, forward = its
//  direction)]
#pragma once

#include <cstdint>
#include <memory>
#include <runtime/particle/effect_scene.h>
#include <runtime/world/geom.h>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::world {

class World;
struct Entity;
struct ItemDeathTraits;
struct ScriptEffectEvent;

// The entity row's one effect group pointer, entity+0x1CC (the IDB's ownerSession), as a scene
// slot (D-PTL-26). Every producer that stores a group there stores it in this slot: the mission
// start's particlefx emitters (the last one spawned), the emit death class, a hit's ammo
// secondary effect and a corpse's decay on a person, and the script effects (fx2ssn, fx2tgt,
// BMS 27 / targetfx). A release detaches what the slot holds, whoever stored it: fx2ssn before
// its spawn, the emit class's toggle, a hit or a decay before theirs, the respawn reset, and the
// row's destroy. A group's death clears the slot only while it still holds that group, the
// scene's StoreOwned rule. One slot per pool row, as the field is the row's. A row our registry
// frees another way, or gives to another entity, releases its slot too: retail frees a row only
// through its destroy, so that release has no witness of its own.
// [orig: Entity_SpawnBoneTrailEffect @ 0x43BF0B..0x43BF22, the stores @ 0x43C074;
//  Entity_SpawnBoneTrailEffect_0 @ 0x43F91A..0x43F949, the store @ 0x43FB14;
//  Projectile_ProcessExplosionQueue @ 0x4EB20A..0x4EB224, the store @ 0x4EB292;
//  WacScript_SpawnEffectAtSsnEntity @ 0x4F2410 (the release ahead of the spawn);
//  EventAction_SpawnParticleEffect @ 0x45418C; Entity_ResetToSpawnState @ 0x4B97CB..0x4B97F5;
//  Entity_Destroy @ 0x43E8EC..0x43E8F5 -> Entity_ReleaseEffectEmitter @ 0x5F75D0;
//  Entity_ClearOwnerSessionIfMatches @ 0x453580]
inline constexpr uint64_t kEntityEffectSlotDomain = 0x4000000000000000ull;
inline particle::EffectSlotToken entity_effect_slot(uint16_t packed_handle) {
    return {packed_handle != 0xFFFF ? kEntityEffectSlotDomain | packed_handle : 0};
}

// The world's effect producers on the entity's +0x1CC slot, and the scene they spawn into. The
// scene is shared with the embedder's particle renderer; its ordinary fixed-tick advance reaps
// groups. Every group here is world-bound: nothing in the scene follows an owner of its own, and
// each producer re-poses the handle it holds from its entity's state at retail's site, as the
// +0x1CC slot's is re-posed by the pool walks (update_emitter_transform).
// [orig: Entity_SpawnBoneTrailEffect @ 0x43BEF0; Entity_SpawnBoneTrailEffect_0 @ 0x43F8F0;
//  CEffect_UpdateEmitterTransform @ 0x5F7410; Entity_ClearOwnerSessionIfMatches @ 0x453580]
class ItemEmitterSystem {
public:
    // What the slot-A ledger holds, for the embedder's read seam (the dev tools, the probes): the
    // entities whose particlefx attach ran, those the particle switch refused (retried once it
    // lifts), the PlayerControl items a controller holds, and the slots a producer stored in.
    struct Counts {
        int attached = 0;
        int pending_hidden = 0;
        int controlled = 0;
        int held_slots = 0;
    };

    ItemEmitterSystem() = default;
    ~ItemEmitterSystem() { reset(); }
    ItemEmitterSystem(ItemEmitterSystem &&) noexcept = default;
    ItemEmitterSystem &operator=(ItemEmitterSystem &&other) noexcept;
    ItemEmitterSystem(const ItemEmitterSystem &) = delete;
    ItemEmitterSystem &operator=(const ItemEmitterSystem &) = delete;
    void bind_scene(std::shared_ptr<particle::EffectScene> scene, bool owns_clock = false);
    // The scene, open or not (null before a bind), and the one the world's producers spawn into
    // now (null while closed).
    particle::EffectScene *scene() const { return scene_.get(); }
    particle::EffectScene *open_scene() const { return producers_open() ? scene_.get() : nullptr; }
    void event(World &world, Entity &entity, const ItemDeathTraits &traits, int phase);
    // The entity's slot may hold a group a producer stored (the script effects, which the embedder
    // spawns): its row's destroy or reuse releases it (sync_slots).
    void hold_slot(const Entity &entity);
    // Detach what the entity's slot holds, whoever stored it.
    void release_slot(const Entity &entity);
    // Destroy what it holds, now, its particles with it (EffectScene::destroy): the emit class's
    // toggle and a powerup's pickup.
    void destroy_slot(const Entity &entity);
    // The tick's end: the release of every held slot whose row was destroyed or taken by another
    // entity since, and, open, the script effects a restore queued again; then the owned clock.
    void sync_slots(World &world);
    void reset();

    // Slot A, `particlefx <effect> <userpoint>`: what the entity's slot holds is released, then
    // one world-bound emitter per userpoint the name masks in the def's graphic model's first 16,
    // at the point and along its direction through the entity's matrix, tagged with the entity
    // and stored in the slot (the last one stays there); none masked, or no graphic model, takes
    // one emitter at the entity's position, unoriented and tagged, stored nowhere (the store after
    // it writes the stale local). Answers the emitters spawned.
    // [orig: Entity_SpawnBoneTrailEffect @ 0x43BEF0 - the release @ 0x43BF0B..0x43BF22, the
    //  model test @ 0x43BF49, the descriptor (mode 2, the entity tag) @ 0x43BFB8..0x43BFD1, the
    //  point and direction @ 0x43C02E / @ 0x43C053, the spawn and store @ 0x43C05F..0x43C074,
    //  the origin leg Effect_SubmitDescriptor @ 0x43C0A4 (LABEL_18)]
    int spawn_bone_trail(World &world, const Entity &entity);
    // The mission start's walk: every live entity of pools 1-3 whose def names an effect and
    // whose pool's attrib gate admits it (item_effect_pool_allows) takes slot A; a PlayerControl
    // item a controller already holds takes it too (the claim edge's leg, control_started). The
    // open runs it, once the embedder's warm reset is past.
    // [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522EE0 - the pool walks
    //  @ 0x523201..0x5232B2, called from Game_StartMission @ 0x525DE2]
    int attach_mission_effects(World &world);
    // The same gates for one entity a joiner's stream (or a host's runtime attachment) brings in
    // past the open; an entity whose lifetime already attached takes nothing.
    int attach_entity_effects(World &world, const Entity &entity);
    // The PlayerControl claim edges: a claimed item whose pool walk skipped it (its 0x40 bit) takes
    // slot A once a controller claims it; the claimant's leaving releases the slot, whatever it
    // holds (D-PTL-17 runs the spawner for every PlayerControl item, retail's for CHel / cpln).
    // [orig: Entity_UpdateHeloRotorSpin @ 0x48FA70 - the gate @ 0x48FAAD, Entity_SpawnBoneTrailEffect
    //  called @ 0x48FB0C; Entity_DetachFromVehicle @ 0x4355F0 - CEffectEmitter_ReleaseSafe(+0x1CC)
    //  @ 0x435746..0x435759]
    void control_started(World &world, const Entity &vehicle);
    void control_stopped(World &world, const Entity &vehicle);
    // The retail master particle switch lifted: the slot-A attaches it refused, once.
    int retry_hidden(World &world);
    // The +0x1CC re-pose, at the pool walks' visit: the slot's live group goes to the def's first
    // masked graphic userpoint through the entity's matrix, along its direction, else to the
    // entity's position with its aim kept; a watercraft class (def unit_type 5..8) keeps it at or
    // above the water plane. The pool-1 walk runs it every visit, the pool-2 cohort walk on its
    // cohort, each past a non-null update callback; no pool-3 or pool-0 walk runs it.
    // [orig: CEffect_UpdateEmitterTransform @ 0x5F7410 - the first set bit @ 0x5F7467..0x5F7497,
    //  the matrix @ 0x5F74A8..0x5F750B, the point and direction @ 0x5F7518 / @ 0x5F7527, the
    //  position leg @ 0x5F7538..0x5F755B, the water clamp @ 0x5F755F..0x5F757F; called by
    //  Entity_UpdatePool1Slot @ 0x4B8E6C..0x4B8E76 and Entity_UpdateAllEntities @ 0x4C22EC..0x4C22FA]
    void update_emitter_transform(World &world, const Entity &entity);
    Counts counts() const;

    // The producers the world spawns at their event time, into the scene and in the tick's order
    // with the emit class's toggle and every release of the slot (D-PTL-26): the script effects
    // (fx2ssn, fx2tgt, fxrain, the marker action), the +0x1CC positioned emitters (a hit's, a
    // corpse's decay), slot A, and the destruction and round effects (DestructionEmitters,
    // RoundEmitters). They spawn once the embedder opens its scene to them. Closed (a headless
    // kernel, a dedicated host, a mission start until the embedder's warm reset), a script effect
    // queues on World::out.script_effects, which the open spawns in order or a dedicated host
    // drains unshown, and every other producer spawns nowhere, as a device with no effect
    // system's spawn returns none. `effect_names` are the script catalog's names, interned into
    // the scene first so a BMS by-name lookup finds the stock clones the compile registered. The
    // open runs the mission start's slot-A walk (attach_mission_effects) first.
    void open_producers(World &world, const std::vector<std::string> &effect_names);
    // A mission start closes them until its embedder's open.
    void close_producers() { producers_open_ = false; }
    bool producers_open() const { return producers_open_ && scene_ != nullptr; }
    // A script effect at its event time (open), or queued (closed). Its slot store is held either
    // way: the row's destroy or reuse releases it.
    void submit_script_effect(World &world, const ScriptEffectEvent &event);
    // The entity's +0x1CC positioned emitter (a hit's ammo secondary effect, a corpse's decay):
    // what the slot holds is released, whoever stored it, then `effect` spawns at the entity's
    // position, oriented (0, 0, -0.5) and tagged with the entity, and is stored in the slot,
    // where it stays put (no walk re-poses a person's +0x1CC emitter).
    void store_positioned_emitter(World &world, const Entity &entity, const std::string &effect);
    // The vehicle's heavy-damage smoke, which the mover stores in the same slot only while it is
    // empty: an unoriented descriptor at `position`, tagged with the vehicle.
    // [orig: Entity_UpdateVehiclePhysics @ 0x48AF00 - the empty test @ 0x48B0DE..0x48B0E5, the
    //  submit @ 0x48B0F1 (Effect_SubmitDescriptor, the entity tag @ 0x48B0F0), the store
    //  @ 0x48B0F6]
    void store_damage_smoke(World &world, const Entity &entity, const std::string &effect,
            const Vec3 &position);
private:
    void spawn_script_effect(World &world, const ScriptEffectEvent &event);
    void spawn_queued_script_effects(World &world);
    // The slot's held entry dropped, as a release or a destroy leaves it.
    void forget_slot(const Entity &entity);
    struct Lifetime {
        uint16_t handle = 0xFFFF;
        uint64_t lifetime = 0;
    };
    static bool holds(const std::vector<Lifetime> &rows, const Entity &entity);
    static void put(std::vector<Lifetime> &rows, const Entity &entity);
    static void drop(std::vector<Lifetime> &rows, uint16_t handle);
    std::shared_ptr<particle::EffectScene> scene_;
    std::vector<Lifetime> held_;
    // Slot A's ledger: the lifetimes whose attach ran, the attaches the particle switch refused,
    // and the PlayerControl items a controller holds.
    std::vector<Lifetime> attached_;
    std::vector<Lifetime> pending_hidden_;
    std::vector<Lifetime> controlled_;
    bool owns_clock_ = false;
    bool producers_open_ = false;
};

// The scene-frame point of a mission point (the scene is the effect frame, (x, z, -y)).
inline particle::Vec3 effect_point(const Vec3 &mission) {
    return {mission.x, mission.z, -mission.y};
}
// A spawn's section stamp: tagged with the entity it spawns at, and the building blink volumes
// holding its mission point (16.16).
// [orig: CEffectWorld_SpawnEmitterAtPosition @ 0x5F6EFB (the tag), @ 0x5F6F5A
//  (Entity_QueryBlinkBoxesAtPoint at the descriptor position)]
particle::EffectSectionGate effect_section_gate(World &world, const int32_t position[3]);
particle::EffectSectionGate effect_section_gate(World &world, const Vec3 &position);
// The mission water plane as the scene's kill-plane height (env.water_z is 16.16, 0 = none).
float effect_water_height(const World &world);

// The original walks entity pools 1-3 with distinct attrib masks; `kind` is
// the imported mission kind (EntityKind: Marker=0, Item=1, Building=2,
// Organic=3, any other value admits nothing). Pool 0 organics are excluded;
// pool 1 items skip attrib 0x42 (POWERUP | PLAYERCONTROL); pools 2/3 —
// buildings and markers — skip only the powerup bit 0x2.
// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0 — the
//  three per-pool attrib gates of its pool walk]
bool item_effect_pool_allows(int kind, uint32_t attrib);

// The occupied-controller pass bypasses only PlayerControl: an Item whose
// attrib carries PLAYERCONTROL and not POWERUP attaches once a controlling
// occupant arrives (the vehicle_control_* lifecycle edges) and detaches when
// the last one leaves. The independent powerup exclusion remains intact.
bool item_effect_controller_allows(int kind, uint32_t attrib);

// The attach plan for one model: every userpoint the authored name masks in
// the model's FIRST 16 (exact case-insensitive match, duplicate names all
// match — the 16-bit mask of the original), else the spawn_count==0 leg —
// ONE emitter at the entity origin (no matched point, or no authored point
// name). The shell composes each userpoint's position + direction with the
// entity pose; the origin leg keeps the entity basis.
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40 (formats/threedi
//  threedi_3di3_user_point_mask carries the scan); the spawn_count==0 leg
//  Entity_SpawnBoneTrailEffect @ 0x43c097 -> Effect_SubmitDescriptor
//  @ 0x43c0a4 at entity->Position]
struct ItemEffectAttachPlan {
	uint16_t mask = 0;             // the first-16 match mask
	std::vector<int> user_points;  // one attach per set bit, ascending
	bool origin_fallback = false;  // no match: one emitter at the entity origin
};

ItemEffectAttachPlan item_effect_attach_plan(const opennova::threedi::Threedi3di3 &model,
		const char *userpoint_name);

} // namespace opennova::world
