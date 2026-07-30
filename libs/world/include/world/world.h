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

#include "audio/sound_profile.h"
#include "terrain/surface_type_map.h"
#include "world/destruction.h"
#include "world/entity.h"
#include "world/entity_commands.h"
#include "world/entity_registry.h"
#include "world/net_command_sink.h"
#include "world/system.h"
#include "world/trigger_relations.h"
#include "world/round_ring.h"
#include "world/sound_emitter_mailbox.h"
#include "world/var_store.h"
#include "world/vehicle_mount.h"
#include "world/ammo_table.h"
#include "world/round_sim.h"
#include "world/throwables.h"
#include "world/vehicle_motor.h"
#include "world/waypoint_track.h"
#include "world/weapon_table.h"
#include "world/zone_chain.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class CollisionWorld;

// One sound-profile slot fire (footstep, foley, landing, death scream),
// already resolved to the profile's authored sound-set name. The host present
// layer drains these into full-volume positional one-shots
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 -> Sound_Play3DPositional].
// An empty-name slot is never emitted (the resolved-id-0 no-op).
struct SoundSlotEvent {
    uint16_t source_handle = 0xFFFF; // packed EntityHandle of the body
    int32_t pos[3] = {0, 0, 0};      // mission-frame 16.16 (feet-level for footsteps)
    uint8_t slot = 0;                // audio::SoundProfileSlot, for tests/observability
    char set_name[24] = {};
};

// ----------------------------------------------------------------------------
// Environment / weather state (targets of the WAC env commands: fog/sky/rain/
// tod/sun/...). A clean observable model; the renderer consumes it later.
// ----------------------------------------------------------------------------
struct EnvState {
    int32_t time_of_day = 0;   // 16.16 hours
    int32_t fog_type = 0;
    // Mission water plane, 16.16 (0 = no water). Host-fed from the .env at
    // load; the infantry footstep water pick and the landing legs read it
    // sim-side. [orig: Env_WaterHeightFixed @ 0x26C6454]
    int32_t water_z = 0;
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
    // Active human player slot count — the WAC 'humans' builtin, rebuilt by the host
    // server tick just before the script pre-pass. Doubles in the original as the
    // empty-dedicated-server world-run gate (entities/WAC advance while humans > 0
    // || ticks == 0); an SP host always counts its own player. [orig: wac_var_humans
    // @0xC6EB14 — Server_BuildEntitySlotLists @0x4f97a0: zero @0x4f97c6, +1 per
    // active human slot @0x4f98b1]
    int32_t humans = 0;
};

// Mutable engine values exposed to mission scripts through retail's named-value
// table. This is distinct from V#/G#/M#: named values are direct pointers into
// engine state, so consumers such as infantry AI observe WAC writes immediately.
// [orig: the 24-row table @0x82EEF0; WacScript_ResolveParameter @0x4f2940]
struct WacNamedValues {
    static constexpr int32_t kDefaultAccuracySpread = 1;
    // Global multiplier in the infantry sawtooth aim-error formula.
    // [orig: wac_var_accuracyspread @0xC6EAE8; read @0x4bc5ea]
    int32_t accuracy_spread = kDefaultAccuracySpread;
};

// End-of-round outcome state. `ended` is the double-run latch every round-end
// consumer keys on; `winner_team` is the winning-team value the WAC outcome
// builtins and the presentation layer derive from (0 = none/green, 1 = blue,
// 2 = red, 3/4 = the extra MP teams). It stays 0 until the round ends, exactly
// like the original's scoreboard winner dword (memset 0 at mission start).
// [orig: g_spawn_success_gate @0x24c1928 (latched by Server_ProcessRoundEnd
// @0x5168e4, cleared by Game_StartMission @0x524a1f), g_round_winning_team
// @0x24c1924, the scoreboard winner @0x24c1970 (= S2C 0x1D payload byte 0).]
struct RoundEndState {
    bool ended = false;
    int32_t winner_team = 0;
};

// SP mission kill tallies — the 0xC846xx stat-bucket family the epilog score
// screen counts from and the WAC bluekills/greenkills builtins read. By-player
// = kills by the local/host player; by-others = every other killer. Only
// person-class victims (itemdef class 3) tally the blue/green buckets; the
// original's per-type enemy split (infantry/vehicle/aircraft) and the point
// values (def+404, difficulty-scaled) fold into plain counts here — the WAC
// predicates and the epilog count columns read counts. [orig:
// Score_TallyKillByLocalPlayer @0x4fd160 / Score_TallyKillByOthers @0x4fd300,
// dispatched per kill by Score_ProcessKillEvent @0x4fd400 (SP only).]
struct MissionKillStats {
    int32_t bluekills_by_player = 0;      // team-1 persons [orig: 0xC846F0 — WAC 'bluekills']
    int32_t greenkills_by_player = 0;     // team-0 persons [orig: 0xC846F8 — WAC 'greenkills']
    int32_t enemy_kills_by_player = 0;    // team >= 2, any kind [orig: 0xC846D8/E0/E8 folded]
    int32_t team_kills_by_others = 0;     // [orig: 0xC846C0]
    int32_t friendly_kills_by_others = 0; // [orig: 0xC846C8]
    int32_t enemy_kills_by_others = 0;    // [orig: 0xC846A8/B0/B8 folded]
};

class AiSystem;  // fwd (lives in world/ai.h; World holds a non-owning pointer so the
                 // shared command layer can reach an entity's AI component in-engine)

// ----------------------------------------------------------------------------
// World.
// ----------------------------------------------------------------------------
class World {
public:
    World() : commands(*this) {}

    EntityRegistry registry;
    ScriptVarStore vars;       // shared by WAC + BMS (the C6B240/C6BA40 seam)
    WacNamedValues wac_values; // writable named engine values (the @0x82EEF0 table)
    EnvState env;
    EffectLog effects;
    CachedFrameState cached;
    LocalSink local_sink;
    INetCommandSink *net = &local_sink;
    EntityCommands commands;
    AiSystem *ai = nullptr;    // non-owning; the host wires this to the AI system driving
                               // this world, so the AI-change command family can reach brains.
    CollisionWorld *collision = nullptr; // non-owning authoritative spatial-query seam;
                                         // the host owns the mission CollisionWorld.
    IMountedPoseProvider *mounted_pose_provider = nullptr; // non-owning live seat-bone seam;
                                                           // null/false keeps static geometry.

    // Session + game-option state the BMS Teammate trigger family reads. Hosts
    // stamp these at bring-up; the SP defaults hold otherwise.
    // [orig: g_napi_np_ctx.is_in_session gate @0x453b53; option dword_24D1E34
    // bit 0x20 = teammates disabled @0x453b67 — the same gate that suppresses
    // type-5305 teammate spawns in Entity_SpawnFromBMSRecord @0x40ea5a]
    bool mp_session = false;
    bool teammates_disabled = false;
    // Projectile_UpdatePhysics clamps the radius to 0.1u only for an
    // authoritative multiplayer FatBullets trace owned by a remote player.
    // These explicit host-fed gates keep that option out of ordinary/SP rays.
    bool projectile_authority = true;
    bool fat_bullets = false;
    bool one_shot_kill = false; // MP-only g_OneShotKill; ignored offline
    // An embedding adapter may own the local player's borrowed UseGun slot so
    // it can supply trigger/reload/scope input and drain presentation events.
    // Standalone World users keep the default global mounted-slot pump.
    bool external_local_mounted_weapon_pump = false;
    // Sticky trigger-relation state (BMS cats 1/2): matrices + group alert/
    // count records + waypoint has-visited. Cleared per mission load by the
    // BMS system's on_load [orig: EventSystem_FreeAll @ 0x453210].
    TriggerRelations relations;
    // 62-tick live-recount divider [orig: the Server_TickUpdate timer word,
    // reload 0x3E @ 0x51db93]. Public like the other tick state; hosts never
    // touch it.
    int group_recount_timer_ = 0;
    // Active teammate heli-lift operations [orig: dword_AC4F40, slots @0xAC4F48,
    // incremented by HeliLift_SpawnPickup @0x45263a]. The heli-lift subsystem is
    // not ported yet; this counter is its seam so TeammateMedicAssisting /
    // TeammateEvacuating evaluate faithfully once it lands (0 = none active).
    int32_t heli_lift_active_count = 0;

    // Cached Player ItemDef presence and traits. The default true covers native harnesses that
    // seed the built-in Player directly; resolve_item_traits overwrites it with the database's
    // actual presence so a malformed/missing Player definition is not invented for late spawns.
    // [orig: Entity_InitFromItemDef @0x49e550; D-NET-144]
    bool player_has_item_def = true;
    int32_t player_item_hp = 0;
    // The rest of the same Player items.def template, cached for host/late-join
    // entities allocated after the mission-wide trait sweep.
    uint8_t player_item_type = 3;
    uint32_t player_item_attrib = 0;
    int32_t player_armor_impact = 0;
    int32_t player_armor_kz = 0;
    float player_damage_reduc_pp = 0.0f;
    float player_damage_reduc_max = 0.0f;

    // The weapon.def armory table (empty until the host feeds it — NovaSimulation::
    // load_weapon_table). Read by the 0x2F/0x5A loadout service, the extended-uplink
    // equipped-weapon gate, and the player-spawn WPN_M4AUTO default. (D-NET-141/143)
    WeaponTable weapons;

    // Fired-round events pending per-recipient S2C 0x0A tag-2 echo (round_ring.h). Fed by
    // the C2S 0x06 dispatch on accepted fire; drained per connection watermark by the
    // netsim emit. [orig: g_round_ring @0xC8D848 via RoundData_AddRound @0x4fdb40] (D-NET-152)
    RoundRing rounds;

    // The ammo.def ballistics/damage table (empty until the host feeds it —
    // NovaSimulation::load_ammo_table, beside the weapon table). [orig: g_ammoDefTable
    // @0xA2ECE8, AmmoDef_LoadAll @0x40b0b0; §5.60]
    AmmoTable ammo;

    // The live authoritative rounds — spawned synchronously by the accepted C2S 0x06
    // (the same fire that appends `rounds`), stepped inside run_logic_tick, deaths
    // drained by the host session. [orig: RoundData_SpawnRound @0x4ec0d0 inline from
    // RoundData_AddRound; Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    RoundSim round_sim;

    // The explosion queue + AoE damage, the death-piece pool, the per-item death
    // traits, and the destruction presentation events (world/destruction.h;
    // world-wac-ai-re §24). Explosions queued this tick drain inside
    // run_logic_tick right after the round sim [orig: Projectile_ProcessExplosionQueue
    // @0x4ead80 runs once per frame after the projectile update]; the host drains
    // `destruction` (present) and feeds `item_death_traits` (item-traits sweep).
    ExplosionSim explosions;

    // Placed throwable devices + class bindings (world-wac-ai-re §27).
    ThrowableSim throwables;
    DeathPieceSim death_pieces;
    DestructionRng destruction_rng;
    ItemDeathTraitsTable item_death_traits;
    DestructionEvents destruction;

    // End-of-round outcome + the SP kill-stat buckets (see the struct docs above).
    RoundEndState round_end;
    MissionKillStats kill_stats;

    // The mission header's attribute flags, stamped by the host at mission load
    // (bms::AttribFlags as a raw dword; 0x40 = SinglePlayerRespawn). Read by the
    // SP auto-lose win-condition leg and by the infantry death scream's night
    // gate (0x100000 EnableNVG -> slot 8 SSNightDead @ 0x4b9ca3).
    // [orig: Bms_AttribFlags @0xa76258]
    // The named bits below mirror bms::AttribFlags (libs/world stays
    // mission-parser-free; parity pinned by static_asserts in
    // libs/mission/src/promote.cpp).
    static constexpr uint32_t kMissionAttribSinglePlayerRespawn = 0x40u;
    static constexpr uint32_t kMissionAttribEnableNVG = 0x100000u;
    uint32_t mission_attrib_flags = 0;

    // The Advance & Secure zone-slot chain (empty until the host builds it after the
    // item-traits sweep — zone registration needs Entity::is_capture_trigger). Feeds
    // the 0x0F owned-zone mask, the 0x0E deploy gates, and the 0x1E frontier hint.
    // [orig: the inline manager @0x24D1EBC, ZoneSlotChain_BuildFromMission @0x4a2de0
    // from Game_StartMission; net-re §5.61]
    ZoneChain zone_chain;

    // The player waypoint track (built by mission promotion from the blue-route
    // nav channel; empty when the mission authors none). Advanced per logic tick
    // from the local player's position; the BMS event system completes linked
    // entries and ShowWaypoints toggles `show`. See waypoint_track.h for the
    // original anchors. (docs/interface/hud-re.md §Waypoint HUD)
    WaypointTrack waypoints;

    // The mission-objectives (subgoal) state the SP objectives panel reads:
    // per-slot bit masks written by the SubGoal/ShowSubgoal actions — the bit is
    // the RAW 1-based slot (bits 1..8) — plus the mission header's per-slot
    // WinConditions/LoseConditions text-id tables (index 1..8; 0/255 terminate
    // the panel's row walk). [orig: won @0xAC86F4 / lost @0xAC86F0 (readers
    // EventSystem_GetEntityCounts @0x452e10), show-win @0xAC86EC / show-lose
    // @0xAC86E8 (EventSystem_GetTeamCounts @0x452e30); the id tables
    // byte_A7628B/byte_A76293 = the BMS header win_conditions/lose_conditions;
    // panel HUD_DrawWinConditions @0x5ba940]
    struct SubgoalState {
        uint32_t won = 0;
        uint32_t lost = 0;
        uint32_t show_win = 0;
        uint32_t show_lose = 0;
        uint8_t win_text_ids[9] = {};
        uint8_t lose_text_ids[9] = {};
    };
    SubgoalState subgoals;

    // Per-item vehicle physics traits (empty until the host's item-traits sweep feeds
    // it — NovaSimulation::resolve_item_traits). The AI tick's vehicle pass runs the
    // ground-vehicle motor for pool-1 entities whose traits carry a non-zero `physics`
    // selector. [orig: ItemDef_ParsePhysicsProperty @0x49d870 fields consumed by
    // Entity_UpdateVehiclePhysics @0x48af00; vehicle_motor.h]
    VehicleTraitsTable vehicle_traits;

    // Host-wired terrain sampler for the round sim's ground stop (the AI grounding
    // shares the same field through AiSystem). Null = no terrain impacts.
    const terrain::TerrainHeightField *terrain = nullptr;

    // Host-wired charmap (surface-type) sampler data for the infantry footstep
    // surface pick (surface 3 = the snow slots) and, later, the ammo impact
    // table. Null = surface 1 everywhere, the sampler's no-charmap default.
    // [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]
    terrain::SurfaceTypeMap surface_map;

    // The mission's SndProf.def profile table (parsed once at load; empty on a
    // headless test world unless a test seeds it) and the per-tick slot-sound
    // emissions the host present layer drains into positional one-shots.
    // [orig: SoundProfile_LoadAll @ 0x527490; the infantry consumers
    // @ 0x4bf15c-0x4bf2b0 (org1) / @ 0x4b76e0-0x4b78a8 (org2)]
    audio::SoundProfileTable sound_profiles;
    std::vector<SoundSlotEvent> slot_sounds;
    SoundEmitterMailbox sound_emitters;

    // The engine tick counter: one logic tick per host frame at 62 Hz.
    // [orig: current_tick @0x24c1968, ++ once per Game_ProcessMainFrame @0x5263f0.
    //  Per-system cadences divide it: the WAC VM executes every 62nd tick
    //  (WacScript_AdvanceTick @0x4f81b1), the BMS normal-event quarter pass runs every 16th
    //  (Server_TickUpdate @0x51d7e0), the AI motor staggers on 2/8/16 internally.]
    uint32_t logic_tick = 0;

    void add_system(ISystem *sys);
    void load_systems();       // calls on_load for each

    // One authoritative logic tick: cache transient state, tick all systems,
    // advance the tick counter (post-execution, faithful to WacScript_AdvanceTick @0x4f81d3).
    void run_logic_tick(bool is_authority = true, bool pre_mission = false);

    // End the round: the double-run latch, the winning team, and the SP presentation
    // tail surfaced as the "round_end" host effect. Callers are the witnessed
    // producers — the WAC win/lose handlers, the BMS Blue/Red/GreenWin actions, and
    // the server win-condition check. [orig: Server_ProcessRoundEnd @0x5164f0]
    void process_round_end(int32_t winning_team);

    // Group population counts for the trigger records. Initial: once per
    // mission start, right after the pre pass, live copied from it and group 0
    // forced to zero [orig: EntityPool_RecountByType @ 0x40e7e0, sole call
    // Game_StartMission @ 0x525b8b]. Live: a full alive-member rescan, run on
    // a 62-tick cadence inside the logic tick and after group reassignment
    // [orig: EntityPool_RecountLiveByGroup @ 0x40e8d0; timer @ 0x51db6d,
    // reload 0x3E @ 0x51db93, call @ 0x51dc02].
    void recount_group_initials();
    void recount_group_live();

    // Editor "play" support: snapshot/restore of mutable world state so a
    // simulate/stop cycle doesn't dirty the authored mission. Value copies of the
    // registry + vars + named WAC values + env + clock + stable local-player
    // ownership; per-tick health/proximity/human-count caches reset and systems
    // re-init on restore.
    struct Snapshot {
        EntityRegistry registry;
        ScriptVarStore vars;
        WacNamedValues wac_values;
        EnvState env;
        uint32_t logic_tick = 0;
        EntityHandle local_player;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot &s);

private:
    std::vector<ISystem *> systems_;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WORLD_H
