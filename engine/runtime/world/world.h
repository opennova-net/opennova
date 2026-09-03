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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/audio/sound_profile.h>
#include <base/io/crt_rand.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/match.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/system.h>
#include <runtime/world/trigger_relations.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/water_cross.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/sound_emitter_mailbox.h>
#include <runtime/world/weather_state.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/terrain_scorch_events.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/world/var_store.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/waypoint_track.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/zone_capture.h>
#include <runtime/world/zone_chain.h>

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

// Packed player-character identity -> the Avatars.def head-sex bit used by
// Entity_GetProfileSlotSound. The shell installs this immutable mission table;
// sound selection remains portable and unknown ids deliberately read male.
// [orig: MinimapSlot_FindByPackedId @0x57a270;
//  Entity_GetProfileSlotSound female select @0x52831c]
struct CharacterTraitsTable {
    struct Row {
        uint16_t character_id = 0;
        bool female = false;
    };
    std::vector<Row> rows;

    void set(uint16_t character_id, bool female) {
        for (Row &row : rows) {
            if (row.character_id != character_id) continue;
            row.female = female;
            return;
        }
        rows.push_back(Row{character_id, female});
    }
    bool is_female(uint16_t character_id) const {
        for (const Row &row : rows)
            if (row.character_id == character_id) return row.female;
        return false;
    }
    void clear() { rows.clear(); }
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
    // The shell-fed posed head-bone world position for the local player
    // (mission units) — the embedder-feeds-back seam the exact eye-offset
    // store consumes (engine/runtime/world carries no skeletal pose). False
    // until the shell samples a skeleton; the capsule formula then stands in,
    // like retail before the first bone build.
    Vec3 local_head;
    bool local_head_valid = false;
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
    // The WAC VM's execution counter, republished here so the script-advance gate
    // can read it without reaching into the VM — retail keeps it in the same
    // global bag as `humans`. [orig: wac_var_ticks]
    int32_t wac_ticks = 0;
    // The posed head as a BODY-RELATIVE delta, paired with local_head above.
    // Lag-free by construction; see the seated eye restamp in infantry.cpp.
    Vec3 local_head_offset;
    bool local_head_offset_valid = false;
};

// Mutable engine values exposed to mission scripts through retail's named-value
// table. This is distinct from V#/G#/M#: named values are direct pointers into
// engine state, so consumers such as infantry AI observe WAC writes immediately.
// [orig: the 24-row table @0x82EEF0; WacScript_ResolveParameter @0x4f2940]
struct WacNamedValues {
    // Seeded 10 at every mission load and teardown [orig: WacScript_FreeAll @0x4f6395
    // `mov wac_var_accuracyspread, 0Ah`, called from GameMode_CreateDefaultDefs @0x4f9061
    // / Game_TeardownMission @0x5226f0].
    static constexpr int32_t kDefaultAccuracySpread = 10;
    // Global multiplier in the infantry sawtooth aim-error formula.
    // [orig: wac_var_accuracyspread @0xC6EAE8; read @0x4bc5ea]
    int32_t accuracy_spread = kDefaultAccuracySpread;
    // The fall-damage tolerance `fallmps` [orig: dword_C6EAE4, the named-value row
    // beside accuracyspread]. Seeded 13 at every mission load and teardown [orig:
    // WacScript_FreeAll @0x4f638b `mov dword_C6EAE4, 0Dh`, called from
    // GameMode_CreateDefaultDefs @0x4f9061 / Game_TeardownMission @0x5226f0]; the
    // authority writes it into the 0x0A sub-block-1 timer state
    // (NetPacket_WritePlayerState @0x4ffa14, connection_fan.cpp state1) and a joiner
    // mirrors it from that packet (NapiNPClientMsg_0x00A @0x4301bc) for its local
    // red-flash only, since the landing damage itself is authority-gated. Damage when
    // landing with vel_z <= -1057*fallmps: health -= excess>>4 [orig: @0x4bf839 /
    // @0x4b7d13]. There is no zero test: 0 damages EVERY landing by
    // (-vel_z)>>4 (a WAC can write it; no shipped script does).
    static constexpr int32_t kDefaultFallmps = 13;
    int32_t fallmps = kDefaultFallmps;
};

// End-of-round outcome state. `ended` is the double-run latch every round-end
// consumer keys on; `winner_team` is the winning-team value the WAC outcome
// builtins and the presentation layer derive from (0 = none/green, 1 = blue,
// 2 = red, 3/4 = the extra MP teams). It stays 0 until the round ends, exactly
// like the original's scoreboard winner dword (memset 0 at mission start).
// [orig: g_spawn_success_gate @0x24c1928 (latched by Server_ProcessRoundEnd
// @0x5168e4, cleared by Game_StartMission @0x524a1f), g_round_winning_team
// @0x24c1924, the scoreboard winner @0x24c1970 (= S2C 0x1D payload byte 0).]
// The scoring awards for the session's game type, resolved from score.ini's row
// for that type. Retail keeps the whole 452-byte row and indexes it as
// `scoringTable[74 + slot]`; we carry only the awards the ported legs read, each
// looked up BY NAME through engine/formats/score (the row-image layout is not
// witnessed — see that lib's header). Slot numbers below are from the shipped
// name table [orig: off_830348 @ 0x830348, read out of Jointops.exe: 38
// {name, slot} pairs, slots 0..37].
struct ScoreRules {
    bool valid = false;      // false until the host resolves a row; every award is then 0
    int32_t enemy_kill = 0;  // VAR "ENEMYKILL"    slot 3 -> scoringTable[77]
    int32_t friendly_kill = 0; // VAR "FRIENDLYKILL" slot 2 -> scoringTable[76]
    int32_t suicide = 0;     // VAR "SUICIDE"     slot 4 -> scoringTable[78]
    int32_t death = 0;       // VAR "DEATH"       slot 5 -> scoringTable[79]
};

struct RoundEndState {
    bool ended = false;
    int32_t winner_team = 0;
    // The two team totals the S2C 0x1D scoreboard header carries, and the draw
    // flag derived from them. For every WAYPOINT-FAMILY game type (stock and
    // objective Co-op — the `(fieldId & 0xFFFDFFFF) == 0x10020` arm, which is
    // exactly game_type::is_waypoint_family) retail reads dword field 29 of the
    // two per-team stats objects `dword_C87CA8` / `dword_C87DFC`
    // [orig: Server_BuildEndOfRoundScoreboard @0x508f30 -> ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52c850];
    // the draw flag is the plain equality, taken on the team arm because
    // Co-op's g_GameType 0x30020 has bit 0x10000 set
    // [orig: @0x508f30 draw leg, kong 213715-213730].
    //
    // UNPORTED SOURCE (ledger D2): retail's per-slot/per-team SCORE accounting
    // (GameEvent_ProcessScoring @0x52f550) is not ported, so nothing writes
    // these yet and the header ships them as 0. That is a DECLARED unported
    // field, not a witnessed value — D2 lands the accounting and fills them.
    int32_t team_scores[2] = {0, 0};
    bool draw = false;
};

// The epilog/debrief exit timeout: both end screens (WIN score epilog and the
// LOSE debrief) force g_mission_exit_reason = 1 after 18600 ticks (~297.6 s at
// the 62.5 Hz tick) when the player never presses ESC.
// [orig: epilog_cinematic_state_machine_update @0x576240 — the tick compares
//  @0x57621d/@0x5744ea]
inline constexpr int32_t kEpilogExitTimeoutTicks = 18600;

// The epilog/debrief screens fade in over the cine fade pair: two 48-tick fade
// events back to back (96 ticks, ~1.536 s at the 62.5 Hz tick). The shell
// drives its screen alpha from this, not from a wall-clock stand-in.
// [orig: the 48+48-tick cine fade pair @0x574512]
inline constexpr int32_t kEpilogFadeInTicks = 48 + 48;

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
    // The mission's enemy-unit total, counted ONCE at mission start over the
    // entity pools: non-player entities with team >= 2 and a non-zero
    // items.def unit-class byte (def+0x196). The original also splits the
    // count per class (vehicle 3/4, aircraft 9, else infantry @0xC84694/9C/98)
    // — the Show Score panel consumes only the total, so the split folds like
    // the kill buckets above. [orig: 0xC84690 — Score_ClassifyEntityForCounts
    // @0x4fd070 over both pools from Score_CountMissionSubgoalsAndUnits @0x509dc0, called at
    // Game_StartMission @0x525d5d]
    int32_t enemy_unit_total = 0;
};


// items.def display names keyed by Entity::item_id (the wire type id), the
// ItemDeathTraitsTable shape: filled once per distinct id by the item-traits
// sweep, read by the inspection records (world/inspect.h). Small missions:
// linear is fine.
struct ItemNameTable {
    std::vector<std::pair<int32_t, std::string>> rows;

    const std::string *get(int32_t item_id) const {
        for (const auto &r : rows)
            if (r.first == item_id) return &r.second;
        return nullptr;
    }
    void set(int32_t item_id, std::string name) {
        for (auto &r : rows)
            if (r.first == item_id) { r.second = std::move(name); return; }
        rows.emplace_back(item_id, std::move(name));
    }
    void clear() { rows.clear(); }
};

// ----------------------------------------------------------------------------
// World.
// ----------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// World's public members group by lifetime (ADR 0043 d2): the script's own
// state, the per-mission tables the embedder feeds once, the session rules
// the host stamps at bring-up, and the outbox the presentation/wire drain.
// The sim members (registry, ai, match, the sims, the clocks) stay flat on
// World. Every member keeps the witness comment it carried on World.
// ---------------------------------------------------------------------------

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

// What the mission script (WAC + BMS) reads and writes beyond the entity rows.
// vars and wac_values ride the World snapshot; the rest is re-initialised by
// the systems' on_load.
struct ScriptState {
    ScriptVarStore vars;       // shared by WAC + BMS (the C6B240/C6BA40 seam)
    WacNamedValues wac_values; // writable named engine values (the @0x82EEF0 table)
    // Sticky trigger-relation state (BMS cats 1/2): matrices + group alert/
    // count records + waypoint has-visited. Cleared per mission load by the
    // BMS system's on_load [orig: EventSystem_FreeAll @ 0x453210].
    TriggerRelations relations;
    SubgoalState subgoals;
    // The player waypoint track (built by mission promotion from the blue-route
    // nav channel; empty when the mission authors none). Advanced per logic tick
    // from the local player's position; the BMS event system completes linked
    // entries and ShowWaypoints toggles `show`. See waypoint_track.h for the
    // original anchors. (docs/interface/hud-re.md §Waypoint HUD)
    WaypointTrack waypoints;
    // Active teammate heli-lift operations [orig: dword_AC4F40, slots @0xAC4F48,
    // incremented by HeliLift_SpawnPickup @0x45263a]. The heli-lift subsystem is
    // not ported yet; this counter is its seam so TeammateMedicAssisting /
    // TeammateEvacuating evaluate faithfully once it lands (0 = none active).
    int32_t heli_lift_active_count = 0;
};

// The Player items.def template, cached for host/late-join entities allocated
// after the mission-wide trait sweep.
struct PlayerTemplate {
    // Cached Player ItemDef presence and traits. The default true covers native harnesses that
    // seed the built-in Player directly; resolve_item_traits overwrites it with the database's
    // actual presence so a malformed/missing Player definition is not invented for late spawns.
    // [orig: Entity_InitFromItemDef @0x49e550; D-NET-144]
    bool has_item_def = true;
    int32_t item_hp = 0;
    // The rest of the same Player items.def template, cached for host/late-join
    // entities allocated after the mission-wide trait sweep.
    uint8_t item_type = 3;
    uint32_t item_attrib = 0;
    int32_t armor_impact = 0;
    int32_t armor_kz = 0;
    float damage_reduc_pp = 0.0f;
    float damage_reduc_max = 0.0f;
    // The Player template's radarsig/heatsig — the AI acquisition engage caps a
    // late-joiner spawn seeds (same cache family as the hp above; the sweep
    // stamps live entities directly) [orig: Entity_InitFromModel @0x40e136].
    int32_t radar_sig = 0;
    int32_t heat_sig = 0;
};

// The per-mission tables the embedder feeds once (the def files, the terrain
// samplers, the Player template) and the mission header facts.
struct MissionTables {
    // The weapon.def armory table (empty until the host feeds it — Simulation::
    // load_weapon_table). Read by the 0x2F/0x5A loadout service, the extended-uplink
    // equipped-weapon gate, and the player-spawn WPN_M4AUTO default. (D-NET-141/143)
    WeaponTable weapons;
    // The ammo.def ballistics/damage table (empty until the host feeds it —
    // Simulation::load_ammo_table, beside the weapon table). [orig: g_ammoDefTable
    // @0xA2ECE8, AmmoDef_LoadAll @0x40b0b0; §5.60]
    AmmoTable ammo;
    // Scoring awards for this session's game type (score.ini row). Populated by the
    // host from the parsed config; zero/!valid until then, which makes every award
    // a no-op rather than a guess.
    ScoreRules score_rules;
    // Per-item vehicle physics traits (empty until the host's item-traits sweep feeds
    // it — Simulation::resolve_item_traits). The AI tick's vehicle pass runs the
    // ground-vehicle motor for pool-1 entities whose traits carry a non-zero `physics`
    // selector. [orig: ItemDef_ParsePhysicsProperty @0x49d870 fields consumed by
    // Entity_UpdateVehiclePhysics @0x48af00; vehicle_motor.h]
    VehicleTraitsTable vehicle_traits;
    // items.def display names per item type (the def row's `name`), filled by
    // the item-traits sweep once per distinct id so the inspection records can
    // name an entity by its item, not only by its BMS label. Tooling only.
    ItemNameTable item_names;
    // items.def sound profiles per ORGANIC item type — the wire body channel's
    // equivalent of AiProfile.sound_profile (audio/sound_profile.h).
    audio::OrganicSoundProfileTable organic_sound_profiles;
    // The mission's SndProf.def profile table (parsed once at load; empty on a
    // headless test world unless a test seeds it) and the per-tick slot-sound
    // emissions the host present layer drains into positional one-shots.
    // [orig: SoundProfile_LoadAll @ 0x527490; the infantry consumers
    // @ 0x4bf15c-0x4bf2b0 (org1) / @ 0x4b76e0-0x4b78a8 (org2)]
    audio::SoundProfileTable sound_profiles;
    CharacterTraitsTable character_traits;
    // charattr.def: each CHARACTER row's tokenized ATTRIBUTES dword (AutoScope 1,
    // SpreadBonus 2, KnifeBonus 4, Medic 8, WaterGirl 0x20), indexed by the
    // soldier class as retail indexes g_CharAttr — row (class - 1) & 0xF, the
    // dword at row offset 40. The embedder stamps it from its parsed table
    // (inmatch::charattr_class_attribute_rows over the boot-soft charattr table,
    // re-stamped after every S2C 0x41 clear); zero rows carry no attribute,
    // which is retail's empty-table behaviour. [orig: CharAttr_LoadFromDef @0x412140;
    //  the reader AnimMap_IsSlotActive @0x4125e0 — dword_A79568[31 *
    //  ((slot - 1) & 0xF)] & mask, with dword_A79568 = g_CharAttr + 0x28]
    std::array<uint32_t, 16> class_attribute_flags{};
    static constexpr uint32_t kCharAttrMedic = 0x8u;
    bool class_has_attribute(uint8_t player_class, uint32_t bit) const {
        const size_t row = static_cast<size_t>((player_class - 1) & 0xF);
        return (class_attribute_flags[row] & bit) != 0;
    }
    // The per-item death traits the item-traits sweep feeds (world/destruction.h).
    ItemDeathTraitsTable item_death_traits;
    // Host-wired terrain sampler for the round sim's ground stop (the AI grounding
    // shares the same field through AiSystem). Null = no terrain impacts.
    const terrain::TerrainHeightField *terrain = nullptr;
    // Host-wired charmap (surface-type) sampler data for the infantry footstep
    // surface pick (surface 3 = the snow slots) and the ammo impact table
    // (surface + 4). Null = surface 1 everywhere, the sampler's no-charmap
    // default. [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]
    terrain::SurfaceTypeMap surface_map;
    PlayerTemplate player;
    // The mission header's attribute flags, stamped by the host at mission load
    // (bms::AttribFlags as a raw dword; 0x40 = SinglePlayerRespawn). Read by the
    // SP auto-lose win-condition leg and by the infantry death scream's night
    // gate (0x100000 EnableNVG -> slot 8 SSNightDead @ 0x4b9ca3).
    // [orig: Bms_AttribFlags @0xa76258]
    // The named bits below mirror bms::AttribFlags (engine/runtime/world stays
    // mission-parser-free; parity pinned by static_asserts in
    // engine/runtime/mission/promote.cpp).
    static constexpr uint32_t kMissionAttribSinglePlayerRespawn = 0x40u;
    static constexpr uint32_t kMissionAttribEnableNVG = 0x100000u;
    uint32_t mission_attrib_flags = 0;
    // The map grid-label origin: the mission's first type-2043 marker
    // ("Map Centerpoint" items.def 102043), Q16 mission x/y.
    // [orig: HUD_InitOverlaySystem @0x5a4999 -> dword_2723EB4]
    bool map_grid_origin_present = false;
    int32_t map_grid_origin_x = 0;
    int32_t map_grid_origin_y = 0;
};

// The session/game-option bits the host stamps at bring-up; the SP defaults
// hold otherwise.
struct SessionRules {
    // Session + game-option state the BMS Teammate trigger family reads. Hosts
    // stamp these at bring-up; the SP defaults hold otherwise.
    // [orig: g_napi_np_ctx.is_in_session gate @0x453b53; option dword_24D1E34
    // bit 0x20 = teammates disabled @0x453b67 — the same gate that suppresses
    // type-5305 teammate spawns in Entity_SpawnFromBMSRecord @0x40ea5a]
    bool mp_session = false;
    bool teammates_disabled = false;
    // The per-tick authority role consulted by World&-only callbacks. The
    // suspension role pick and both post-death blast writers read the same
    // g_napi_np_ctx.is_authority bit in retail
    // [orig: @0x46B1B9..0x46B1DB; @0x48F6A0..0x48F71E; @0x4941BE].
    // run_logic_tick stamps it once so every callback sees the tick's role.
    bool logic_authority = true;
    // Projectile_UpdatePhysics clamps the radius to 0.1u only for an
    // authoritative multiplayer FatBullets trace owned by a remote player.
    // These explicit host-fed gates keep that option out of ordinary/SP rays.
    bool projectile_authority = true;
    bool fat_bullets = false;
    bool one_shot_kill = false; // MP-only g_OneShotKill; ignored offline
    // Multiplayer blast damage to Building ItemDefs is disabled unless the
    // host's `destroybuild` rule is nonzero. Offline/SP ignores the option.
    // [orig: g_destroy_buildings gate in Entity_ApplyWeaponDamage
    // @0x4E682E..0x4E6860]
    bool destroy_buildings = false;
    // An embedder may own the local player's borrowed UseGun slot so
    // it can supply trigger/reload/scope input and drain presentation events.
    // Standalone World users keep the default global mounted-slot pump.
    bool external_local_mounted_weapon_pump = false;
    // MP-rules bit: the AI class-0 player leg skips the LOCAL player when set
    // [orig: dword_24C1930 & 0x800 read @0x467155]. The net wire into it is a
    // tracked D-AI-1 residual; defaults clear (SP).
    bool ai_rules_skip_local_player = false;
};

// What the sim produced this tick for someone else to drain: the wire (entity
// removals, the round ring, water crossings) and the presentation (effects,
// destruction, scars, scorches, the sound queues). Nothing in the sim reads
// an outbox back; the drains clear them.
struct WorldOutbox {
    // Rows the sim destroyed this tick that the net layer must announce with
    // S2C 0x12 [orig: Server_RemoveEntityAndNotify @0x50A270 writes the handle,
    // send_mask 0x90 (alive + not-host), msgClass 1, then destroys the row]. The
    // world cannot send, so it records the packed handle here and the server tick
    // drains it. Cleared by the drain; a client-side World never fills it.
    std::vector<uint16_t> entity_removals;
    // Fired-round events pending per-recipient S2C 0x0A tag-2 echo (round_ring.h). Fed by
    // the C2S 0x06 dispatch on accepted fire; drained per connection watermark by the
    // netsim emit. [orig: g_round_ring @0xC8D848 via RoundData_AddRound @0x4fdb40] (D-NET-152)
    RoundRing rounds;
    // Water-surface crossings recorded this tick; the host fan drains them
    // into S2C 0x34 and clears. Presentation only - nothing in the sim reads it.
    WaterCrossQueue water_crossings;
    // The destruction presentation events (world/destruction.h) the host drains.
    DestructionEvents destruction;
    // The WAC/BMS/sim effect log the presentation drains ("text", "dialog", the
    // WAC fx command names, ...).
    EffectLog effects;
    // The impact-scar rings (world-wac-ai-re §24.9): 128 per-entity rings + the
    // terrain ring, written by the round stop and cleared on death. Presentation
    // state — the shell compiles it into quads each frame; it is NOT part of the
    // snapshot (retail's caches live beside the renderer, not the entity pools).
    ScarCache scars;
    // Permanent ground scorch insertions share retail's CRT rand stream with
    // animated material noise and keep their 4096-row lifetime across drains.
    TerrainScorchEvents terrain_scorches;
    // The per-tick slot-sound emissions and the sound-emitter mailbox the host
    // present layer drains into positional one-shots.
    std::vector<SoundSlotEvent> slot_sounds;
    SoundEmitterMailbox sound_emitters;
    // The weather tick's thunder one-shots (weather_state.h carries the
    // cites); the presentation owner drains them per frame.
    std::vector<WeatherSoundEvent> weather_sounds;
    // The fire-sound propagation-delay queue on the logic clock, seeded inline
    // at round spawn and counted down at the head of run_logic_tick; the
    // presenting host stamps the listener and drains the ready one-shots
    // (world/fire_sound.h witness map). [orig: the pending-sound slots
    // @0x24DF678, Sound_TickPendingSlots @0x529310]
    FireSoundQueue fire_sounds;
};

class World {
public:
    World() : commands(*this) {}
    // World has stable identity: commands binds this object and registered
    // systems retain mission-lifetime relationships. Memberwise copy/move
    // would preserve pointers/references into the source World and create a
    // split simulation.
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    World(World &&) = delete;
    World &operator=(World &&) = delete;

    EntityRegistry registry;
    EnvState env;
    // The retail weather globals, ONE home (weather_state.h): the WAC weather
    // handlers write it through EntityCommands, the weather tick advances it
    // after the logic tick, the wire projection serializes it, a joiner's
    // decoder writes its targets back.
    WeatherState weather;
    CachedFrameState cached;
    EntityCommands commands;
    // The AI/motor system: every brain plus the infantry and vehicle motors.
    // Owned here so the command layer, the sims, the wire and the tools reach
    // brains without a seam; the kernel registers it as the third ISystem
    // (WAC -> BMS -> AI) and wires its collision/terrain/root-motion links.
    AiSystem ai;
    // The lifetime groups (declared above): what the script owns, what the
    // embedder feeds once, what the host stamps, what the drains consume.
    ScriptState script;
    MissionTables tables;
    SessionRules rules;
    WorldOutbox out;
    // Game_StartMission seeds the one process-global PRNG_Next16 stream after
    // writing it twice; 0x1A10101A is the final retail dword_31BFBB0 value
    // [orig: push 1A10101Ah @ 0x5245F7 -> seed setter PRNG_SetSeed (ex sub_613130) in
    // Game_StartMission @ 0x524360]. AI recoil/engagement, throwable bounce
    // spin, and server control challenges all consume this owner in their
    // actual call order.
    static constexpr uint32_t kMissionPrng16Seed = 0x1A10101Au;
    uint32_t prng16_state = kMissionPrng16Seed;
    uint16_t next_prng16() noexcept;
    // The simulation's owner of the CRT rand() recurrence retail draws from
    // (the far-marker spawn scores @0x50CEA2, the 0x100 death-family roll
    // @0x51718A, ...). Retail seeds the process stream from the clock once at
    // host start and never at mission start; the host seeds this owner from
    // its session seed in create_session, so a session's draw sequence is
    // reproducible where retail's is not (D-NET-115). Snapshotted with
    // prng16_state. [orig: CRT rand @0x76B00A; srand @0x51C1AA]
    io::CrtRand crt_rand;
    CollisionWorld *collision = nullptr; // non-owning authoritative spatial-query seam;
                                         // the host owns the mission CollisionWorld.
    // The one tick-profile collector (ADR 0043 d5): non-owning, the kernel's.
    // Null or inactive costs every span one branch; the embedder drains it.
    devtools::TickProfile *profile = nullptr;
    IPoseProvider *pose_provider = nullptr; // non-owning: the embedder's live seat-bone, muzzle
                                            // and userpoint seam; null/false keeps static geometry.

    // 62-tick live-recount divider [orig: the Server_TickUpdate timer word,
    // reload 0x3E @ 0x51db93]. Public like the other tick state; hosts never
    // touch it.
    int group_recount_timer_ = 0;







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

    // The authoritative session rules/stats/outcome + the SP kill-stat buckets.
    // Multiplayer and WAC/BMS outcomes share Match's one double-run latch.
    Match match;
    MissionKillStats kill_stats;



    // The Advance & Secure zone-slot chain (empty until the host builds it after the
    // item-traits sweep — zone registration needs Entity::is_capture_trigger). Feeds
    // the 0x0F owned-zone mask, the 0x0E deploy gates, and the 0x1E frontier hint.
    // [orig: the inline manager @0x24D1EBC, ZoneSlotChain_BuildFromMission @0x4a2de0
    // from Game_StartMission; net-re §5.61]
    ZoneChain zone_chain;
    // The capture request/active transaction is mission state, not host-wire
    // scratch. Keeping it beside the chain prevents a second lifecycle or a
    // static server singleton. [orig: CaptureCtx_Reset @0x53BD00]
    ZoneCaptureState zone_capture_state;
    // Mission-built deploy wave groups. Keeping them beside spawn selection
    // gives immediate picks and timed releases one lifecycle and no host-only
    // shadow table. [orig: SpawnWaveList_BuildFromMission @0x52A920]
    SpawnWaveList spawn_waves;
    // One mission-global round-robin shared by default spawn selection and a
    // picked numbered zone's type-6007 scatter choices.
    // [orig: g_spawn_cycle_counter @0x24C10D0;
    // Server_PositionPlayerForSpawn @0x50CF60]
    uint32_t spawn_cycle_counter = 0;








    // The engine tick counter: one logic tick per host frame at 62 Hz.
    // [orig: current_tick @0x24c1968, ++ once per Game_ProcessMainFrame @0x5263f0.
    //  Per-system cadences divide it: the WAC VM executes every 62nd tick
    //  (WacScript_AdvanceTick @0x4f81b1), the BMS normal-event quarter pass runs every 16th
    //  (Server_TickUpdate @0x51d7e0), the AI motor staggers on 2/8/16 internally.]
    uint32_t logic_tick = 0;

    // May the mission script advance this tick? Retail wraps its WAC tick, the
    // idle-timer sweep and the BMS event pump in ONE condition, and the half that
    // matters here is `wac_var_humans || !wac_var_ticks`: once the VM has run at
    // all, the whole script HOLDS until a human player is in the world. An empty
    // host does not burn through its mission.
    //
    // That gate is the difference between a scripted kill reaching a client and
    // firing into an empty session: 05TRcoop kills ten AI a fifth of a second in,
    // and without this an unattended host runs that before anyone can join.
    //
    // Unmodeled halves of the same condition: retail also requires
    // `!g_preround_delay_timer` (the pre-round countdown) and
    // `!g_epilog_screen_active` (the end-of-round screen); we have neither
    // concept yet, and both only ever ADD holds, so omitting them cannot make the
    // script run where retail would not.
    // [orig: the wrapper @0x51b8xx region — `if (!g_preround_delay_timer &&
    //  (wac_var_humans || !wac_var_ticks) && !g_epilog_screen_active)` around
    //  WacScript_AdvanceTick + Server_UpdateEntityIdleTimers +
    //  EventTrigger_UpdateQuarterRoundRobin @0x454d50]
    bool script_may_advance() const {
        return cached.humans > 0 || cached.wac_ticks == 0;
    }
    // Authoritative whole-second pre-round phase. Networking and the frame
    // clock remain live while World gameplay systems are frozen; phase-0 0x0A
    // projects its low byte to each client. Joiners retain the same field from
    // that wire projection, giving host and client one phase predicate.
    // [orig: g_preround_delay_timer @0xC8D824; seed @0x516C8D;
    // decrement @0x51DC20..0x51DC33; writer @0x4FF82D]
    uint32_t preround_delay_seconds = 0;

    void add_system(ISystem *sys);
    void load_systems();       // calls on_load for each

    // One frame-clock tick. Gameplay runs every system; PreMission runs only
    // the authored BMS pre-pass; PreRound advances shared clocks but freezes
    // WAC/entities/projectiles. The explicit phase replaces the old boolean
    // pre-mission seam so no caller can mistake a pre-round freeze for a script
    // initialization pass.
    void run_logic_tick(bool is_authority = true,
                        TickPhase phase = TickPhase::Gameplay);

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
    // registry + script vars + named WAC values + env + clock + match + stable
    // local-player ownership; per-tick health/proximity/human-count caches reset
    // and systems re-init on restore.
    struct Snapshot {
        EntityRegistry registry;
        ScriptVarStore vars;
        WacNamedValues wac_values;
        EnvState env;
        WeatherState weather;
        Match match;
        SpawnWaveList spawn_waves;
        ZoneCaptureState zone_capture_state;
        uint32_t spawn_cycle_counter = 0;
        uint32_t logic_tick = 0;
        uint32_t preround_delay_seconds = 0;
        uint32_t prng16_state = kMissionPrng16Seed;
        uint32_t crt_rand_state = 1;
        EntityHandle local_player;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot &s);

private:
    std::vector<ISystem *> systems_;
};

// The mission-start unit scan feeding MissionKillStats::enemy_unit_total —
// the Show Score panel's enemy-units denominator. Runs once at the
// Game_StartMission-equivalent moment, after entity placement and the
// item-traits sweep stamped Entity::item_unit_type.
// [orig: Score_CountMissionSubgoalsAndUnits (ex sub_509DC0) @0x509dc0 -> Score_ClassifyEntityForCounts @0x4fd070,
//  called at Game_StartMission @0x525d5d]
void count_mission_units(World &world);

// The mission's defined-subgoal count: the leading run of authored win
// conditions (slots 1..8) before the first 0 or 0xFF entry.
// [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning byte_A7628C — the header
//  win-condition text ids; dword_C8468C]
int32_t count_defined_subgoals(const World &world);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WORLD_H
