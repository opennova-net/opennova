// Runtime world: the single shared substrate both scripting evaluators drive.
//
// Holds the entity registry, the shared variable store, environment + effect
// state, the cached per-tick transient state, the entity-command primitive layer
// (the shared Entity_* operations), and the tick service that drives registered
// systems (WAC VM, BMS event evaluator, future GDScript) at the authoritative
// logic-tick cadence. Editor and runtime drive the SAME World; the editor just
// owns the clock (and can pause/step/snapshot).
#ifndef OPENNOVA_WORLD_WORLD_H
#define OPENNOVA_WORLD_WORLD_H

#include <cstdint>
#include <string>
#include <vector>

#include "world/entity.h"
#include "world/entity_registry.h"
#include "world/var_store.h"

namespace opennova::world {

// ----------------------------------------------------------------------------
// Environment / weather state (targets of the WAC env commands: fog/sky/rain/
// tod/sun/...). A clean observable model; the renderer consumes it later.
// ----------------------------------------------------------------------------
struct EnvState {
    int32_t time_of_day = 0;   // 16.16 hours
    int32_t fog_type = 0;
    int32_t fog_dist = 0;      // 16.16 meters
    int32_t sky_speed = 0;
    int32_t rain = 0;
    int32_t snow = 0;
    int32_t overcast = 0;
    uint32_t sun_rgb = 0;
    uint32_t sky_rgb = 0;
    uint32_t fog_rgb = 0;
    uint32_t generation = 0;   // bumped on any change, for change detection
};

// Non-entity side effects (text/sound/fx/objective) recorded for observability
// and host consumption. Behavior tests assert against this log.
struct Effect {
    std::string kind;          // "text", "fx2tgt", "sound", "win", "lose", ...
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
    int32_t d = 0;
    std::string str;
};

class EffectLog {
public:
    void push(Effect e) { entries_.push_back(std::move(e)); }
    void clear() { entries_.clear(); }
    const std::vector<Effect> &entries() const { return entries_; }
    size_t count(const std::string &kind) const {
        size_t n = 0;
        for (const Effect &e : entries_) if (e.kind == kind) ++n;
        return n;
    }
private:
    std::vector<Effect> entries_;
};

// Once-per-tick transient snapshot (local player handle/health, near-enemy data).
// [orig: WacScript_CacheLocalPlayerState @0x4f5780.]
struct CachedFrameState {
    EntityHandle local_player;
    int32_t local_health = 0;
    int32_t near_type = 0;
    int32_t near_dist = 0;
    int32_t near_id = 0;
};

// ----------------------------------------------------------------------------
// Replication seam. [orig: entity-targeted commands serialize to a NAPI payload
// and NapiNPServer_SendFiltered(..., 0x23, ...) to the owner when the target is
// not local.] Single-player uses LocalSink (always authoritative, run locally).
// ----------------------------------------------------------------------------
struct INetCommandSink {
    virtual ~INetCommandSink() = default;
    virtual bool is_authority(EntityHandle target) = 0;
    virtual void send_command(EntityHandle owner, uint16_t command_id,
                              const int32_t *args, int argc) = 0;
};

struct LocalSink : INetCommandSink {
    bool is_authority(EntityHandle) override { return true; }
    void send_command(EntityHandle, uint16_t, const int32_t *, int) override {}
};

class World;     // fwd
class AiSystem;  // fwd (lives in world/ai.h; World holds a non-owning pointer so the
                 // shared command layer can reach an entity's AI component in-engine)

// ----------------------------------------------------------------------------
// Shared entity-command primitive layer. Models the original Entity_* mutation
// functions (Entity_KillByNetId, Entity_SetWaypointByTeam, Entity_SetAlertByNetId,
// Entity_SetMoveSpeedKPH, ...) that BOTH EventAction_Dispatch and the WacScript_*
// handlers funnel through. WAC handlers and BMS actions both call these.
// ----------------------------------------------------------------------------
class EntityCommands {
public:
    explicit EntityCommands(World &world) : world_(world) {}

    // --- entity (by net id) ---
    bool kill_ssn(uint16_t ssn);
    bool remove_ssn(uint16_t ssn);
    bool set_ssn_hp(uint16_t ssn, int32_t hp);
    bool add_ssn_hp(uint16_t ssn, int32_t delta);
    bool set_ssn_waypoint(uint16_t ssn, int32_t wp);
    bool set_ssn_alert(uint16_t ssn, int32_t state);
    bool set_ssn_target(uint16_t ssn, uint16_t target);
    bool set_ssn_move_speed(uint16_t ssn, int32_t kph);
    bool set_ssn_engage_min(uint16_t ssn, int32_t v);
    bool set_ssn_engage_max(uint16_t ssn, int32_t v);
    bool set_ssn_attack_max(uint16_t ssn, int32_t v);
    bool set_ssn_anim(uint16_t ssn, int32_t anim_slot);
    bool set_ssn_hidden(uint16_t ssn, bool hidden);
    bool set_ssn_held(uint16_t ssn, bool held);
    bool set_ssn_disabled(uint16_t ssn, bool disabled);

    // --- queries ---
    bool ssn_exists(uint16_t ssn) const;
    bool ssn_alive(uint16_t ssn) const;
    bool ssn_dead(uint16_t ssn) const;
    bool ssn_in_area(uint16_t ssn, int area_id) const;

    // --- group (by group id) ---
    int kill_group(int group);          // returns members affected
    int group_to_waypoint(int group, int32_t wp);
    int set_group_hp(int group, int32_t hp);
    int set_group_engage_min(int group, int32_t v);
    int set_group_engage_max(int group, int32_t v);
    int set_group_attack_max(int group, int32_t v);
    bool group_dead(int group) const;   // true if all members dead/absent
    bool group_alive(int group) const;  // true if any member alive

    // --- AI command (the AI-change action family) ---
    // [orig: Entity_ApplyCommand @0x43ab60, reached from EventAction_Dispatch @0x4542e0
    // via Entity_HandleAlertStateEvent @0x43dee0.] Apply an AI sub-type command to the
    // target's AI component, reached through World::ai. p2/p3/p4 are the sub-type's slots
    // (e.g. PLAYPARTANIM: p2=channel, p3=play_type, p4=time). No-op (returns false / 0)
    // when there is no AI system or no brain for the target.
    bool apply_ai_command(uint16_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    int apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    int apply_area_ai_command(int zone_area_id, int team, int sub_type,
                              int32_t p2, int32_t p3, int32_t p4);

    World &world() { return world_; }

private:
    World &world_;
};

// ----------------------------------------------------------------------------
// Systems plugged into the tick service (WAC VM, BMS evaluator, ...).
// ----------------------------------------------------------------------------
struct TickContext {
    World *world = nullptr;
    uint32_t logic_tick = 0;
    bool is_authority = true;
    bool pre_mission = false; // BMS PreMission pass (EventFlags PreMission=2)
};

struct ISystem {
    virtual ~ISystem() = default;
    virtual const char *name() const = 0;
    virtual void on_load(World &) {}
    virtual void tick(World &, const TickContext &) = 0;
};

// ----------------------------------------------------------------------------
// World.
// ----------------------------------------------------------------------------
class World {
public:
    World() : commands(*this) {}

    EntityRegistry registry;
    ScriptVarStore vars;       // shared by WAC + BMS (the C6B240/C6BA40 seam)
    EnvState env;
    EffectLog effects;
    CachedFrameState cached;
    LocalSink local_sink;
    INetCommandSink *net = &local_sink;
    EntityCommands commands;
    AiSystem *ai = nullptr;    // non-owning; the host wires this to the AI system driving
                               // this world, so the AI-change command family can reach brains.

    uint32_t logic_tick = 0;   // [orig: dword_C6EAD8]

    void add_system(ISystem *sys);
    void load_systems();       // calls on_load for each

    // One authoritative logic tick: cache transient state, tick all systems,
    // advance the tick counter (post-execution, faithful to sub_4F81A0 @0x4f81d3).
    void run_logic_tick(bool is_authority = true, bool pre_mission = false);

    // Editor "play" support: snapshot/restore of mutable world state so a
    // simulate/stop cycle doesn't dirty the authored mission. Value copies of the
    // registry + vars + env + clock; systems re-init from on_load on restore.
    struct Snapshot {
        EntityRegistry registry;
        ScriptVarStore vars;
        EnvState env;
        uint32_t logic_tick = 0;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot &s);

private:
    std::vector<ISystem *> systems_;
};

// ----------------------------------------------------------------------------
// Host-frame -> logic-tick reducer. [orig: sub_4F81A0 runs the WAC program once
// per 62 render frames (0x3E divider).] The editor drives the same service.
// ----------------------------------------------------------------------------
class TickService {
public:
    static constexpr int kFramesPerLogicTick = 0x3E; // 62

    bool paused = false; // [orig: dword_C6EB28 script-disable gate]

    // Advances the frame counter; runs a logic tick every kFramesPerLogicTick.
    // Returns true if a logic tick fired this frame.
    bool advance_frame(World &world, bool is_authority = true);

    // Force a single logic tick (tests / editor Step).
    void step(World &world, bool is_authority = true) { world.run_logic_tick(is_authority); }

private:
    int frame_accum_ = 0; // [orig: dword_C6EAD4]
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WORLD_H
