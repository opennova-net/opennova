// The per-item ITEMS.DEF particle-effect law — slot A ('particlefx <effect>
// <userpoint>'), the always-on emitter the mission-start walk attaches to a
// placed entity: which pools/attribs admit the attach, which identity aliases
// a controller lifecycle event names, and where on the model the emitters go.
// Portable decisions over the parsed item row's attrib word (formats/def) and
// the model's userpoint table (formats/threedi); the presenting shell
// (godot/src/world/item_effect_director) spawns the groups and follows the
// nodes. The watercraft W3/W4 lanes route separately through the portable
// vehicle motor and fixed-tick presenter; fxs/W1/W2 and the death/fire/other
// family remain movement/damage-state threads (ptl-format-re.md §8).
// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0 (mission
//  start, entity pools 1-3) -> ItemDef_GetBoneMaskByName @ 0x49ea40 (first
//  16, stricmp) -> Entity_SpawnBoneTrailEffect @ 0x43bef0 (one mode-2 attached
//  emitter per masked userpoint: pos = the userpoint, forward = its
//  direction)]
#pragma once

#include <cstdint>
#include <memory>
#include <runtime/particle/effect_scene.h>
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

// The emit class's groups and the entity slots (entity_effect_slot) the world stored a group in.
// The scene is shared with the embedder's particle renderer; its ordinary fixed-tick advance reaps
// groups.
// [orig: Entity_SpawnBoneTrailEffect_0 @ 0x43F8F0;
// Entity_ClearOwnerSessionIfMatches @ 0x453580]
class ItemEmitterSystem {
public:
    ItemEmitterSystem() = default;
    ~ItemEmitterSystem() { reset(); }
    ItemEmitterSystem(ItemEmitterSystem &&) noexcept = default;
    ItemEmitterSystem &operator=(ItemEmitterSystem &&other) noexcept;
    ItemEmitterSystem(const ItemEmitterSystem &) = delete;
    ItemEmitterSystem &operator=(const ItemEmitterSystem &) = delete;
    void bind_scene(std::shared_ptr<particle::EffectScene> scene, bool owns_clock = false);
    void event(World &world, Entity &entity, const ItemDeathTraits &traits, int phase);
    // The entity's slot may hold a group a producer stored (the script effects, which the embedder
    // spawns): its row's destroy or reuse releases it (sync_owners).
    void hold_slot(const Entity &entity);
    // Detach what the entity's slot holds, whoever stored it.
    void release_slot(const Entity &entity);
    // The emit owners' poses, and the release of every held slot whose row was destroyed or
    // taken by another entity since; open, the script effects a restore queued again spawn here.
    void sync_owners(World &world);
    void reset();

    // The producers the world spawns at their event time, into the scene and in the tick's order
    // with the emit class's toggle and every release of the slot (D-PTL-26): the script effects
    // (fx2ssn, fx2tgt, fxrain, the marker action) and the +0x1CC positioned emitters (a hit's, a
    // corpse's decay). They spawn once the embedder opens its scene to them. Closed (a headless
    // kernel, a dedicated host, a mission start until the embedder's warm reset), a script effect
    // queues on World::out.script_effects, which the open spawns in order or a dedicated host
    // drains unshown, and a positioned emitter spawns nowhere, as a device with no effect system's
    // spawn returns none. `effect_names` are the script catalog's names, interned into the scene
    // first so a BMS by-name lookup finds the stock clones the compile registered.
    void open_producers(World &world, const std::vector<std::string> &effect_names);
    // A mission start closes them until its embedder's open.
    void close_producers() { producers_open_ = false; }
    bool producers_open() const { return producers_open_ && scene_ != nullptr; }
    // A script effect at its event time (open), or queued (closed). Its slot store is held either
    // way: the row's destroy or reuse releases it.
    void submit_script_effect(World &world, const ScriptEffectEvent &event);
    // The entity's +0x1CC positioned emitter (a hit's ammo secondary effect, a corpse's decay):
    // what the slot holds is released, whoever stored it, then `effect` spawns at the entity's
    // position, oriented (0, 0, -0.5) and tagged with the entity, and is stored in the slot, where
    // it stays put (no walk re-poses a person's +0x1CC emitter).
    void store_positioned_emitter(World &world, const Entity &entity, const std::string &effect);
private:
    void spawn_script_effect(World &world, const ScriptEffectEvent &event);
    void spawn_queued_script_effects(World &world);
    struct Binding {
        uint16_t handle = 0xFFFF;
        uint64_t lifetime = 0;
        particle::EffectGroupId group;
        particle::EffectOwnerToken owner;
    };
    struct HeldSlot {
        uint16_t handle = 0xFFFF;
        uint64_t lifetime = 0;
    };
    std::shared_ptr<particle::EffectScene> scene_;
    std::vector<Binding> bindings_;
    std::vector<HeldSlot> held_;
    uint64_t next_owner_ = 1;
    bool owns_clock_ = false;
    bool producers_open_ = false;
};

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

// The identity aliases one entity answers to across the seams a controller
// lifecycle event may name it by: "wire:<handle>" (a 16-bit wire handle;
// 0xffff = none), "net:<id>" (a positive simulation net id), "bms:<id>" (a
// positive BMS record id) and "origin:<packed>" (a positive spawn-origin
// word, entity.h spawn_origin_pack). Synthetic items.def attachments all
// carry the same sentinel origin (-1 or kSpawnOriginNone) and no authored
// BMS/net identity: their packed runtime handle is therefore the ONLY alias
// that distinguishes siblings on the same carrier, so a wire-identified
// entity with bms 0 and the sentinel origin answers to the wire alias alone.
void item_effect_identity_aliases(int32_t net_id, int32_t bms_id,
		int64_t spawn_origin, int32_t wire_handle,
		std::vector<std::string> &out);

// True when the two alias lists share a member (the event <-> node match).
bool item_effect_aliases_intersect(const std::vector<std::string> &left,
		const std::vector<std::string> &right);

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
