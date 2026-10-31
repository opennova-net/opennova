// Runtime world: the single shared substrate both scripting evaluators drive.
//
// Holds the entity registry, the shared variable store, environment + effect
// state, the cached per-tick transient state, the entity-command primitive layer
// (the shared Entity_* operations), and the tick service that drives registered
// systems (WAC VM, BMS event evaluator) at the authoritative
// logic-tick cadence. Editor and runtime drive the SAME World; the editor just
// owns the clock (and can pause/step/snapshot).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <runtime/world/mission_diagnostics.h>
#include <string>
#include <vector>
#include <variant>

#include <runtime/audio/dialog_queue.h>
#include <runtime/audio/oneshot_play.h> // kListenerView*
#include <runtime/audio/sound_profile.h>
#include <base/io/crt_rand.h>
#include <base/io/tick_rate.h> // host_milliseconds_for_logic_tick (authority_tick_count_ms's seam)
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/epilog_cine.h>
#include <runtime/world/doors.h>
#include <runtime/world/facial_animation.h>
#include <runtime/world/teammate_operations.h>
#include <runtime/world/script_events.h>
#include <runtime/world/script_effects.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/script_remote_command.h>
#include <runtime/world/script_sounds.h>
#include <runtime/world/script_squad.h>
#include <runtime/world/script_input.h>
#include <runtime/world/script_voice.h>
#include <runtime/audio/audio_out_meter.h>
#include <runtime/world/match.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/entity_update_transactions.h>
#include <runtime/world/system.h>
#include <runtime/world/title_rules.h> // SessionRules::title_rules
#include <runtime/world/vehicle_system.h>
#include <runtime/world/rotor_wash.h>
#include <runtime/world/zone_system.h>
#include <runtime/world/trigger_relations.h>
#include <runtime/world/body_trace.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/water_cross.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/sound_emitter_mailbox.h>
#include <runtime/world/weather_state.h>
#include <runtime/world/reverb.h>
#include <runtime/world/body_zones.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/spectator_motor.h>
#include <runtime/world/item_sections.h>
#include <runtime/world/terrain_scorch_events.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/world/var_store.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/minefield.h>
#include <runtime/world/powerup.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/waypoint_track.h>
#include <runtime/world/user_waypoints.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/zone_capture.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/world/zone_chain.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::audio { class SoundSetIndex; }
namespace opennova::def { struct DefWeaponsFile; }
namespace opennova::dbf { struct File; }
namespace opennova::lwf { struct File; }
namespace opennova::rtxt { struct File; }

namespace opennova::world {

class CollisionWorld;
class LocalPlayer;
struct LoadoutWeaponNameList;
struct WeaponInventory;

// One sound-profile slot fire (footstep, foley, landing, death scream),
// already resolved to the profile's authored sound-set name. The host present
// layer drains these into full-volume positional one-shots
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 -> Sound_Play3DPositional].
// An empty-name slot is never emitted (the resolved-id-0 no-op).
struct SoundSlotEvent {
    uint16_t source_handle = 0xFFFF; // packed EntityHandle of the body
    int32_t pos[3] = {0, 0, 0};      // mission-frame 16.16 (feet-level for footsteps)
    uint8_t slot = 0;                // audio::SoundProfileSlot, for tests/observability
    char set_name[25] = {}; // LWF Multi has 24 name bytes; reserve a terminator
};

// Packed player-character identity -> the Avatars.def head-sex bit used by
// Entity_GetProfileSlotSound. The shell installs this immutable mission table;
// sound selection remains portable and unknown ids deliberately read male.
// [orig: CAvatarDefs_FindComboByPackedId @0x57a270;
//  Entity_GetProfileSlotSound female select @0x52831c]
struct CharacterTraitsTable {
    struct Row {
        uint16_t character_id = 0;
        bool female = false;
    };
    std::vector<Row> rows;
    // DFX2-53: the sex is read off a player person's avatar blip (+0x3C, its byte +0xC), which a
    // title whose sector render picks the blip again every frame it draws the person
    // (Title::player_avatar_by_viewer_side) points at the first combo of the good side
    // (side_first[0]) while there is no local player or the person is on its team, else of the
    // evil side (side_first[1]): inmatch::install_viewer_side_pick installs the two beside the
    // rows, the pick inmatch::drawn_player_character makes over the registry. Unset
    // (Jointops.exe's): the blip keeps the spawn's pick, the NetId.
    // [orig: Entity_GetProfileSlotSound @0x525490 - the blip @0x52549F, its sex byte @0x5254AC;
    //  Terrain_RenderSectorEntitiesBySide @0x5C6C20 - the Team compare @0x5C6EEE..0x5C6F00, the
    //  blip stored @0x5C6F3D..0x5C6F42; MinimapSlot_InitBlipFromPackedId @0x579500 - the sex byte
    //  @0x579542 (dfx2.exe)]
    bool by_viewer_side = false;
    uint16_t side_first[2] = {0, 0};

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
    // The character a player person's blip names: `net_id`, the spawn's pick (entity+0x15C), or
    // under the viewer-side pick its side's first combo against the viewer.
    uint16_t drawn_character(uint16_t net_id, bool viewer, uint8_t team,
                             uint8_t viewer_team) const {
        if (!by_viewer_side) return net_id;
        return side_first[viewer && team != viewer_team ? 1 : 0];
    }
    void clear() {
        rows.clear();
        by_viewer_side = false;
        side_first[0] = side_first[1] = 0;
    }
};

// ----------------------------------------------------------------------------
// The environment's load-fed words the sim reads (the weather's own state, the
// WAC env commands' home, is World::weather).
// ----------------------------------------------------------------------------
struct EnvState {
    // Mission water plane, 16.16 (0 = no water). Host-fed from the .env at
    // load; the infantry footstep water pick and the landing legs read it
    // sim-side. [orig: g_EnvWaterHeightFixed @ 0x26C6454]
    int32_t water_z = 0;
    // The mission's time-of-day class (env::Config::time_of_day_enum), host-fed from the
    // environment load; the NovaWorld listing's TimeOfDay reads it. [orig: g_EnvTimeOfDayEnum
    // @ 0x24D2364]
    int32_t time_of_day_enum = 0;
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

// Transient player identity and frame data. WAC refreshes local_health only
// at bytecode entry [orig: WacScript_CacheLocalPlayerState @0x4F5780].
struct CachedFrameState {
    // Startup both; camera 0 first person, any other external [orig: @0x43924A].
    uint8_t sound_listener_view_flags = audio::kListenerViewStartup;
    EntityHandle local_player;
    int32_t local_health = 0;
    // The human count — the WAC 'humans' builtin, rebuilt by the host server
    // tick just before the script pre-pass: every live pool-0 row with the
    // Player bit (Flags 0x100) that is not hidden (Flags 1: a player still
    // waiting to deploy is hidden); the item-def test is the allocated-row
    // test (EntityRegistry::count_humans). Doubles in the original as the
    // empty-server world-run gate (entities/WAC advance while humans > 0 ||
    // ticks == 0). [orig: g_WacVarHumans @0xC6EB14 — Server_BuildEntitySlotLists
    // @0x4f97a0: zero @0x4f97c6, the def test @0x4F9809, `test eax,100h`
    // @0x4F9815, `test bl,al` @0x4F9820, +1 @0x4f98b1]
    int32_t humans = 0;
    // Derived view of the VM's mutable clock, published before admission and
    // after execution/restore. Only the VM clock is serialized; this projection
    // lets the world gate read retail's shared word. [orig: g_WacVarTicks @0xC6EAD8]
    int32_t wac_ticks = 0;
    // A host that also plays (the listen host and single player,
    // is_mp_session_peer) whose own player is on the death screen: the
    // entity-update gate skips its empty-world hold. The host role stamps it
    // each frame from its local client's death-screen latch.
    // [orig: Game_ProcessMainFrame -- `cmp is_mp_session_peer` @0x52670B,
    //  `cmp g_DeathScreenActive,0` @0x526713]
    bool peer_death_screen = false;
    // The local client's copy of the pre-round byte (g_ClientPreRoundTimer),
    // which the entity-update gate reads: its one writer is the client's 0x0A
    // handler, so a host that also plays stamps its own client's copy each frame
    // and a dedicated host, which receives no 0x0A, keeps 0.
    // [orig: g_ClientPreRoundTimer @0xA85B64 -- the store NapiNPClientMsg_0x00A
    //  @0x430064, the gate's read Game_ProcessMainFrame @0x52672C]
    uint32_t client_preround_timer = 0;
};

// Mutable engine values exposed to mission scripts through retail's named-value
// table. This is distinct from V#/G#/M#: named values are direct pointers into
// engine state, so consumers such as infantry AI observe WAC writes immediately.
// [orig: the 24-row table @0x82EEF0; WacScript_ResolveParameter @0x4f2920]
struct WacNamedValues {
    // Seeded 10 at every mission load and teardown [orig: WacScript_FreeAll @0x4f6395
    // `mov g_WacVarAccuracySpread, 0Ah`, called from GameMode_CreateDefaultDefs @0x4f9061
    // / Game_TeardownMission @0x5226f0].
    static constexpr int32_t kDefaultAccuracySpread = 10;
    // Global multiplier in the infantry sawtooth aim-error formula.
    // [orig: g_WacVarAccuracySpread @0xC6EAE8; read @0x4bc5ea]
    int32_t accuracy_spread = kDefaultAccuracySpread;
    // The fall-damage tolerance `fallmps` [orig: g_WacVarFallMps, the named-value row
    // beside accuracyspread]. Seeded 13 at every mission load and teardown [orig:
    // WacScript_FreeAll @0x4f638b `mov g_WacVarFallMps, 0Dh`, called from
    // GameMode_CreateDefaultDefs @0x4f9061 / Game_TeardownMission @0x5226f0]; the
    // authority writes it into the 0x0A sub-block-1 timer state
    // (NetPacket_WritePlayerState @0x4ffa14, connection_fan.cpp state1) and a joiner
    // mirrors it from that packet (NapiNPClientMsg_0x00A @0x4301bc, the replica's
    // ClientReplicaState::fallmps); retail's joiner reads it only for its local
    // landing red flash, since the landing damage itself is authority-gated. Damage when
    // landing with vel_z <= -1057*fallmps: health -= excess>>4 [orig: @0x4bf839 /
    // @0x4b7d13]. There is no zero test: 0 damages EVERY landing by
    // (-vel_z)>>4 (a WAC can write it; no shipped script does).
    static constexpr int32_t kDefaultFallmps = 13;
    int32_t fallmps = kDefaultFallmps;
	// USE cannot change the mounted local player's seat while this is nonzero.
	// Forced script detaches still apply. [orig: g_WacVarSeatbelt @0xC6EADC;
	// WacScript_FreeAll @0x4F637B; Entity_ToggleVehicleMount @0x43698B]
	int32_t seatbelt = 0;
	// Two more rows of the named-value table @0x82EEF0. breathtime: the host's
	// drown limit is four samples per second of it (Server_UpdatePlayerBreathTimers
	// @0x50d7e6, GameEvent_PlayerDeath @0x5172f6), the 0x0A player-state wire
	// carries it to the joiners (@0x4ff9db / @0x4301a1), and HUD_DrawBreathBar
	// @0x59d70f reads it (the port's bar is hud::HudFrameCompiler::
	// element_breath_bar). autogain is the iris
	// re-target switch (Environment_ApplyFogAndAmbient @0x57E514, sampled into
	// WeatherState::iris_retarget_enabled). Both seeded by WacScript_FreeAll
	// [orig: @0x4f6381 = 20; @0x4f6371 = 1].
	int32_t breathtime = 20;
	int32_t autogain = 1;
    // location() reads this cached player-body result, not a named-table row.
    // [orig: dword_B763E8, org2 @0x4B634B; WacCmd_Location @0x4ED190]
    int32_t local_location = 0;
    // RND sits below the mission bank, so it lives in the variable store
    // (ScriptVarStore::WacState::rnd).
};

// The def+0x196 unit-class split the SP score block keys on: 3 and 4 are
// vehicles, 9 aircraft, anything else (0 included) infantry. The census and
// the by-player kill tally read the same byte.
// [orig: Score_ClassifyEntityForCounts @0x4fd0c7; Score_TallyKillByLocalPlayer
//  @0x4fd242]
enum class ScoreUnitClass : uint8_t { Infantry, Vehicle, Aircraft };

inline ScoreUnitClass score_unit_class(int32_t item_unit_type) {
    switch (static_cast<uint8_t>(item_unit_type)) {
        case 3:
        case 4: return ScoreUnitClass::Vehicle;
        case 9: return ScoreUnitClass::Aircraft;
        default: return ScoreUnitClass::Infantry;
    }
}

// The SP score block — the 0xC84688..0xC8470B statistics the Show Score panel
// and the SP win epilog draw and the WAC bluekills/greenkills builtins read.
// By-player = kills by the local/host player; by-others = every other killer.
// Only person-class victims (itemdef class 3) tally the blue/green buckets.
// The block's point sums (def+404, difficulty-scaled, the +4 word of every
// kill bucket), the human-victim bucket, the subgoal bonus @0xC846D4 and
// g_SPElapsedUpdateCount @0xC84700 are not modeled: no live code draws them —
// their readers are the dead Cine_ProcessEpilogSequence_Retail @0x575a90,
// Score_ComputeSpTimeWeightedAverage @0x40da30 (called only from it) and
// sub_40D960 @0x40d960, the Show Score panel's dead-store sprintfs and the
// save block. The by-others enemy split folds (only its sum is read); the
// by-player split stays, because its census overflow is visible.
// Zeroed whole by the authority after the PreMission pass and carried by the
// play-start baseline. [orig: Score_TallyKillByLocalPlayer @0x4fd160 /
// Score_TallyKillByOthers @0x4fd300, dispatched per kill by
// Score_ProcessKillEvent @0x4fd400 (SP only); Score_TallySubGoalWon @0x4fd100;
// Server_ResetRoundCounters @0x516c50 — memset(0xC84688, 0, 0x84) @0x516c5e]
struct MissionKillStats {
    int32_t bluekills_by_player = 0;      // team-1 persons [orig: 0xC846F0 — WAC 'bluekills']
    int32_t greenkills_by_player = 0;     // team-0 persons [orig: 0xC846F8 — WAC 'greenkills']
    // team >= 2, any kind, per unit class [orig: 0xC846D8 / 0xC846E0 / 0xC846E8]
    int32_t enemy_infantry_kills_by_player = 0;
    int32_t enemy_vehicle_kills_by_player = 0;
    int32_t enemy_aircraft_kills_by_player = 0;
    int32_t team_kills_by_others = 0;     // [orig: 0xC846C0]
    int32_t friendly_kills_by_others = 0; // [orig: 0xC846C8]
    int32_t enemy_kills_by_others = 0;    // [orig: 0xC846A8/B0/B8 folded]
    // The mission's enemy-unit census, counted at mission start over pools 0
    // and 1: non-player entities with team >= 2 and a non-zero items.def
    // unit-class byte (def+0x196), with its per-class split. A by-player kill
    // that takes a class past its census grows that class and the total by
    // the overflow (Match::process_kill_event). [orig: 0xC84690 total,
    // @0xC84694 vehicle / @0xC84698 infantry / @0xC8469C aircraft —
    // Score_ClassifyEntityForCounts @0x4fd070 from
    // Score_CountMissionSubgoalsAndUnits @0x509dc0, called at Game_StartMission
    // @0x525d5d]
    int32_t enemy_unit_total = 0;
    int32_t enemy_vehicle_total = 0;
    int32_t enemy_infantry_total = 0;
    int32_t enemy_aircraft_total = 0;
    // One per first SubGoalWon of a slot, outside a session.
    // [orig: g_SubGoalsWonCount 0xC846D0 — Score_TallySubGoalWon @0x4fd117]
    int32_t subgoals_won = 0;

    int32_t enemy_kills_by_player() const {
        return enemy_infantry_kills_by_player + enemy_vehicle_kills_by_player +
                enemy_aircraft_kills_by_player;
    }
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
// vars, named values and input/voice state ride the snapshot; the rest is re-initialised by
// the systems' on_load.
struct ScriptState {
    const IScriptEventQuery *bms_events = nullptr; // non-owning, bound by BMS on_load
    ScriptVarStore vars;       // shared by WAC + BMS (the C6B240/C6BA40 seam)
    // The player's input-action word and the BMS chain mirror [orig:
    // g_InputActionBits @0xB3B738; g_EventInputBitsMirror @0xAE06F8]. The
    // producers are the authority's local-player view actions: view1st (400)
    // |= 0x4000000, viewwithgun (401) |= 0x10000000, viewchase (402)
    // |= 0x8000000, the 412 toggle (third person set -> clear it and set
    // cockpit; else clear cockpit and set third person), orbit yaw 405/406
    // |= 0x10/0x40 (cleared every input frame, `&= ~0x50`), orbit pitch
    // 407/408 |= 0x100/0x4, chase zoom 409/410 |= 0x80/0x200 [orig:
    // Input_HandleActionBinding @0x49c073..0x49c253; Input_ProcessFrame
    // @0x49d52b]; BMS action 28 sub 38 zeroes the word [orig: @0x4535c2].
    // Bits 0x400/0x800/0x1000/0x2000/0x4000/0x8000/0x20000000 have no setter
    // in the image. The BMS evaluator copies the word into the mirror at
    // every chain entry, toggles matched bits in the mirror, and commits the
    // mirror back to the word only when the event fires (event_runtime.cpp).
    uint32_t input_action_bits = 0;
    uint32_t input_action_mirror = 0;
    // The one mission-dialog table (runtime/audio/dialog_queue.h): the BMS
    // PlayWavList action registers a dialog (history and slot), the two
    // PLYRDIALOG trigger subs read it, the round resets clear it, and the
    // shell's dialog device runs its playback tick once a logic tick, which
    // frees a dialog from its slot once its last line has held.
    // [orig: Dialog_PlayByIndex @0x527ae0 -> Dialog_PlayByName @0x44d9f0 ->
    //  Dialog_Register @0x44d980 (history dword_A89600 / dword_A895F8, slots
    //  dword_A8A248 / dword_A8A244); Dialog_UpdatePlayback @0x44e470 ->
    //  Dialog_FreeByName @0x44db40; Dialog_ResetAll @0x44dc90; readers
    //  Dialog_ExistsByIndex @0x44e170 and Dialog_IsFinishedByIndex @0x44e220]
    audio::DialogQueue dialog;
    WacNamedValues wac_values; // writable named engine values (the @0x82EEF0 table)
    ScriptWeaponInput weapon_input;
    ScriptVoiceChannel voice;
    ScriptSquadEvents squad_events;
    int32_t forced_animation = 0; // WAC forceanim; org1 think override [orig: @0xA87058]
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
    // dfx2.exe's co-op checkpoint: the pool-3 slot of the newest player-route
    // marker the authority marked done (BmsEventSystem's checkpoint marking),
    // where a co-op player respawns (spawn_select.cpp) and the fifth objective
    // value S2C 0x0A carries; 0xFFFF before one, and in Jointops.exe, which
    // stores the reset and nothing else (docs/titles/dfx2-vs-jo-re.md DFX2-1,
    // DFX2-9). Each mission's World starts it at 0xFFFF, where every mission
    // start's round-counter reset puts it. [orig: g_CoopCheckpointSlot @0xc8dd54 -
    //  EventTrigger_MarkCoopCheckpoint @0x4534a0 (@0x45359e), Server_ResetRoundCounters
    //  @0x513ea0 (@0x513ffd) (dfx2.exe)] [orig: Server_ResetRoundCounters @0x516C50
    //  (@0x516dad), dword_C947A4]
    uint16_t coop_checkpoint_slot = 0xFFFF;
    // Active teammate heli-lift operations [orig: dword_AC4F40, slots @0xAC4F48,
    // incremented by HeliLift_SpawnPickup @0x45263a]. TeammateOperations owns
    // the slots and publishes this trigger-visible count (0 = none active).
    int32_t heli_lift_active_count = 0;
};

// The class-table row a player body's bound items.def row names by its ai_function, the row
// giving the body its class init (def+0x148, which the spawn calls) and its class event
// callback (def+0x138, copied into entity+0x1C8: the player-body update's think, the damage
// sites): "plyr" the player class, "org0" / "org1" the organic class, every other row an item
// class (the null row also for an empty tag and a name no row carries, the walk's row-0
// default), whose event body is the item row's (PlayerTemplate::item_event_class)
// (world::player_body_class_from_tag).
// [orig: g_EntityClassEventCallbackTable @0x813000 (24-byte rows: the name, the event +0x138,
//  the init +0x148, +0x14C, the serializer +0x164); EntityDef_LookupClassCallbacks @0x407DC0 -
//  the stricmp call @0x407DD8, the row-0 default @0x407DEE..0x407E1E; EntityDef_InitAllCallbacks
//  @0x4A5A70 ("Null" for an empty tag @0x4A5AAE)]
enum class PlayerBodyClass : uint8_t { kPlayer, kOrganic, kItem };

// An item row's class init (def+0x148, its ai_function row's +0xC word) that a body bound to the
// row runs at its spawn (player_body_class_init): the arty, aflr and gflr rows'. The rokt and hlfr
// rows' init (RoktClass_InitFromDef @0x443410, a memset of +0x2B8..+0x30B) zeroes words that are
// all still 0 at a body's spawn, so it is not one. [orig: g_EntityClassEventCallbackTable
//  @0x813000 - the rokt / hlfr rows @0x8132B8 / @0x8132E8 (init @0x8132C4 / @0x8132F4), arty
//  @0x813318 (@0x813324), aflr @0x813330 (@0x81333C), gflr @0x813348 (@0x813354);
//  EntityDef_LookupClassCallbacks @0x407DC0 - @0x407E33..0x407E36]
enum class ItemClassInit : uint8_t { kNone, kArty, kAirFlare, kGroundFlare };

// One items.def row's spawn traits: a player spawn binds its class's camouflage
// row from MissionTables::item_rows (the sweep's copy of g_ItemDefs); a World
// whose sweep never ran (a native harness) spawns from MissionTables::player.
struct PlayerTemplate {
    // The row's type id (items.def id less 100000, def+0x50), the type word the
    // 0x0C / 0x0A records carry for a body bound to it [orig:
    //  NetPacket_SerializeEntityStatesToBuffer `movzx edi, word ptr [eax+50h]` @0x5031DD].
    // The harness seed's is the stock class-8 desert row's, 0x14B9.
    int32_t type_id = 0x14B9;
    // Cached Player ItemDef presence and traits. The default true covers native harnesses that
    // seed the built-in Player directly; resolve_item_traits overwrites it with the database's
    // actual presence so a malformed/missing Player definition is not invented for late spawns.
    // [orig: Entity_InitFromItemDef @0x49e550; D-NET-144]
    bool has_item_def = true;
    // The Player row's items.def ordinal (entity+0x1C ItemTypeIndex); 0 until the
    // sweep resolves it [orig: Entity_SpawnFromBMSRecord @0x40EBFC].
    int32_t item_type_index = 0;
    // Whether the Player row's graphic loaded (its model slot, def+0xF0, which
    // a spawn copies into entity+0x30), the device sweeps' candidate gate; true
    // for native harnesses like has_item_def until the sweep stores the load's
    // result [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 - the store
    // @0x439FDC; Entity_InitFromItemDef @0x49E550 - the +0x30 store @0x49E598;
    // the gate @0x43CC19].
    bool has_graphic_model = true;
    // That graphic's radius word (model+0x14, the GHDR radius in 16.16), 0 when none
    // loaded: a flying round bound to the row clips its model at four times it in the
    // scene collect's RoundPool walk (OcclusionWorld::collect_round_draws).
    int32_t graphic_model_radius_q16 = 0;
    // The Player row's hp word (itemDef+0x17C healthMax), 0 a real word: unset
    // until the traits sweep stores it (or a harness seeds one), and while unset
    // the spawn and the deploy take their fallbacks. A word of 0 makes the
    // class init's model init mark the body indestructible (D-PWR-10)
    // [orig: Entity_InitFromModel @0x40DC85].
    std::optional<int32_t> item_hp;
    int32_t critical_hp = 0;
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
    // The row's class (its ai_function's class-table row); the harness seed's is the stock
    // player rows' "plyr". An item class row's event body (item_death_class_from_tag).
    PlayerBodyClass body_class = PlayerBodyClass::kPlayer;
    ItemDeathClass item_event_class = ItemDeathClass::kNull;
    // Whether the row's network callback (def+0x164, its ai_function row's) is the guided
    // record's serializer: the rokt, stng, hlfr, jvln and arty rows' alone.
    // [orig: g_EntityClassEventCallbackTable @0x813000 - the +0x14 word Entity_SerializeGuidedMissileState
    //  @0x447C50 in the rows @0x8132B8..0x813318; EntityDef_LookupClassCallbacks @0x407DC0]
    bool guided_record = false;
    // The row's item class init a body bound to the row runs (player_body_class_init).
    ItemClassInit class_init = ItemClassInit::kNone;
    // A satchel, claymore or AV mine row's device think, which a body bound to it runs as
    // its class event (ThrowableSim::think_device_row_body); kNone for every other row.
    ThrowClass device_think = ThrowClass::kNone;
    // The physics row the row's move_function binds (def+0x158), the update a body bound
    // to it runs from the entity update's pool walks (world::body_move_row); the harness
    // seed's is the stock player rows' org2.
    // [orig: EntityDef_InitAllCallbacks @0x4A5A70 - EntityDef_LookupPhysicsCallback called
    //  @0x4A5B41; Entity_SpawnFromAnimSlotProperty @0x43C390 - `mov edx, [edi+158h]`
    //  @0x43C440, the +0x1C4 store @0x43C453]
    PhysicsClass move_row = PhysicsClass::Org2;
};

// The per-mission tables the embedder feeds once (the def files, the terrain
// samplers, the Player template) and the mission header facts.
struct MissionTables {
    rtxt::File voice_macros; // vmacros.bin, section macrotext [orig: @0x5B7170]
    // The weapon.def armory table (empty until the host feeds it — Simulation::
    // load_weapon_table). Read by the 0x2F/0x5A loadout service, the extended-uplink
    // equipped-weapon gate, and the player-spawn WPN_M4AUTO default. (D-NET-141/143)
    WeaponTable weapons;
    // The weapon.def parse that table was built from (the kernel's, non-owning; null in a
    // world with no parse): the local player's mounts read its row by name
    // (local_weapon_install_by_name), a switch's commit among them.
    const def::DefWeaponsFile *weapon_defs = nullptr;
    // The ammo.def ballistics/damage table (empty until the host feeds it —
    // Simulation::load_ammo_table, beside the weapon table). [orig: g_AmmoDefTable
    // @0xA2ECE8, AmmoDef_LoadAll @0x40b0b0; §5.60]
    AmmoTable ammo;
    // The powerup.def rows (empty until the mission kernel feeds it; a missing
    // file leaves `loaded` clear, retail's "Unable to load powerup.def" state).
    // [orig: PowerUpDef_LoadFromFile @0x443350 from Game_StartMission @0x5256CD]
    PowerupTable powerups;
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
    // MissionKernel owns this immutable bank catalog for the world's lifetime.
    const audio::SoundSetIndex *sound_sets = nullptr;
    // And the mission's dialog bank, its sounds and its mission text table,
    // which a dialog line's clip and chat lines resolve against (null when the
    // mission has none). [orig: DialogSystem_Init @0x5275E0 ->
    // DialogManager_LoadFromFile @0x44E650 (the .dbf, then its <base>.lwf or
    // .pwf @0x44E7D4..0x44E807 into the dialog bank @0xA8A348); g_TextMission,
    // read by Dialog_LoadAudioClip @0x44DDCD / Dialog_LoadAudioClipLocalized
    // @0x44E054]
    const dbf::File *dialog_bank = nullptr;
    const lwf::File *dialog_sounds = nullptr;
    const rtxt::File *mission_text = nullptr;
    // g_TextOverrideTable: the expansion's text-override table every section
    // lookup searches first (null for none) [orig: TextResource_FindEntryBySectionAndKey
    // @0x75D27B].
    const rtxt::File *text_override = nullptr;
    CharacterTraitsTable character_traits;
    // charattr.def: each CHARACTER row's tokenized ATTRIBUTES dword (AutoScope 1,
    // SpreadBonus 2, KnifeBonus 4, Medic 8, WaterGirl 0x20), indexed by the
    // soldier class as retail indexes g_CharAttr — row (class - 1) & 0xF, the
    // dword at row offset 40. The embedder stamps it from its parsed table
    // (inmatch::charattr_class_attribute_rows over the boot-soft charattr table,
    // re-stamped after every S2C 0x41 and 0x42); zero rows carry no attribute,
    // which is retail's empty-table behaviour, and a disabled ATTRIBUTES zeroes
    // them all. [orig: CharAttr_LoadFromDef @0x412140; the reader
    //  CharAttr_ClassHasAttribute @0x4125e0 — !g_CharAttrPropertyDisabled[0] &&
    //  g_CharAttr row active && dword_A79568[31 * ((slot - 1) & 0xF)] & mask,
    //  with dword_A79568 = g_CharAttr + 0x28]
    std::array<uint32_t, 16> class_attribute_flags{};
    // The authority's charattr disable latches as S2C 0x42 carries them
    // (inmatch::charattr_pack_disabled), stamped with the words above: the body
    // of the join's 0x42 and of every periodic one.
    // [orig: CharAttr_PackDisabledProperties @0x412550, from
    //  NetPacket_WriteCharAttrDisabledProperties @0x505BA0 (Server_OnPlayerJoin
    //  @0x51a802) and Server_SendEntityHandleAndInputState @0x507C02]
    uint16_t charattr_disabled_word = 0;
    static constexpr uint32_t kCharAttrKnifeBonus = 0x4u;
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
    // Host-wired foliage map sampler (the terrain store's remapped masks), which the NPC aim's
    // prone-in-foliage concealment reads. Empty = nothing grows, the sampler's no-map 0.
    // [orig: Foliage_SampleFoliageMapMask @ 0x606620]
    terrain::FoliageMaskMap foliage_map;
    // The seed a World without items rows spawns its players from (a native
    // harness), and the stock class-8 desert row (105305) the joiner-side
    // readers with no body row read.
    PlayerTemplate player;
    // Every items.def row's spawn traits, in load order (g_ItemDefs), stamped by
    // the traits sweep: the rows ItemList_FindIndexByTypeId walks for a player's
    // camouflage item (index 0, the first row, for a type no row carries).
    // [orig: ItemList_FindIndexByTypeId @0x49E100 - the walk @0x49E117..0x49E12F,
    //  0 @0x49E12F / @0x49E131]
    std::vector<PlayerTemplate> item_rows;
    // g_BmsClimate: the mission header's camouflage selector (header +0x84):
    // 1 jungle, 2 arctic, any other desert, by which the player spawn picks its
    // class's camouflage property. [orig: Entity_SpawnFromAnimSlotProperty
    //  @0x43C399..0x43C3BE]
    int32_t bms_climate = 0;
    // CharAttr_GetCammoTypeId's answers per class row ((class - 1) & 0xF) for
    // properties 10 (JUNGLE_CAMMO), 11 (DESERT_CAMMO) and 12 (ARCTIC_CAMMO), 0
    // for an inactive row: the installer's copy of this peer's charattr table
    // (inmatch::install_class_attribute_flags), with whether that table read a
    // charattr.def (a World whose peer read none, a native harness or a
    // synthetic pack, spawns its players from the seed; retail's boot reads a
    // missing file as the zeroed table, see D-NET-348).
    // [orig: CharAttr_GetCammoTypeId @0x4127B0 - inactive @0x4127C5,
    //  10/11/12 @0x4127D2..0x4127DC]
    std::array<std::array<int32_t, 3>, 16> class_cammo_type_ids{};
    bool class_cammo_installed = false;
    // The mission header's attribute flags, stamped by the host at mission load
    // (bms::AttribFlags as a raw dword; 0x40 = SinglePlayerRespawn), the
    // objective Co-op mission start out of a session adding 0x40. Read by the
    // SP auto-lose win-condition leg, the deploy keys and death menu, and by
    // the infantry death scream's night gate (0x100000 EnableNVG -> slot 8
    // SSNightDead @ 0x4b9ca3).
    // [orig: g_BmsAttribFlags @0xa76258; the 0x40 OR Game_StartMission @0x525CF2]
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
    // The items.def "user waypoint" row (type id 6089, items.def 106089)
    // Waypoint_CreateForPlayer seeds its pool-4 rows from: the row's ordinal
    // (0 when the row is missing — ItemList_FindIndexByTypeId's miss) and its
    // type. Stamped by the item-traits sweep.
    // [orig: Waypoint_CreateForPlayer @0x4dfcb0 — ItemList_FindIndexByTypeId(6089)]
    int32_t user_waypoint_type_index = 0;
    uint8_t user_waypoint_item_type = 0;
};

// The session/game-option bits the host stamps at bring-up; the SP defaults
// hold otherwise.
struct SessionRules {
	// The rules of the title the world runs as (title_rules.h; CONTEXT.md "Title"): the knobs the
	// in-match code reads where dfx2.exe and Jointops.exe part, each cited on both binaries
	// (docs/titles/dfx2-vs-jo-re.md §7, the §8 rows). Unset (unset_title_rules) until the embedder
	// sets it from title_rules(code): the kernel's boot (mission::KernelBootOptions::title_rules),
	// the host from its session config (inmatch configure_session_world), the joiner from its
	// title (JoinerRole::set_rules). title() aborts while it is unset.
	const TitleRules *title_rules = unset_title_rules();
	const TitleRules &title() const { return require_title_rules(title_rules, "SessionRules"); }
	// The live rules word. A role stamps it (HostRole: the config word in a
	// session, 0x4204 out of one; a joiner its S2C 0x64 word); a world no
	// role brings up keeps 0, which is no SP default.
	uint32_t mpattrib = 0; // [orig: g_RulesFlags @0x24D1E34, HUD target gates @0x5926DC]
	bool hit_feedback = true; // [orig: Config_SetDefaults @0x54D18B]
    // Session + game-option state the BMS Teammate trigger family reads. Hosts
    // stamp these at bring-up; the SP defaults hold otherwise.
    // [orig: g_NapiNPCtx.is_in_session gate @0x453b53; option dword_24D1E34
    // bit 0x20 = teammates disabled @0x453b67 — the same gate that suppresses
    // type-5305 teammate spawns in Entity_SpawnFromBMSRecord @0x40ea5a]
    bool mp_session = false;
    bool teammates_disabled = false;
    // The retail is_mp_session_peer bit, the is_client half of the session's
    // connection mode: clear only on a HostOnly (dedicated) host, where no
    // dialog plays and no objective line posts. SP, the listen host and a
    // joiner keep it set. [orig: g_NapiNPCtx +0x64 (server_session.cpp
    //  stamps it from the mode's is_client bit); readers EventAction_Dispatch
    //  case 7 @0x45443d and HUD_ShowObjectiveNotification @0x5ba382]
    bool mp_session_peer = true;
    // The per-tick authority role consulted by World&-only callbacks. The
    // suspension role pick and both post-death blast writers read the same
    // g_NapiNPCtx.is_authority bit in retail
    // [orig: @0x46B1B9..0x46B1DB; @0x48F6A0..0x48F71E; @0x4941BE].
    // run_logic_tick stamps it once so every callback sees the tick's role.
    bool logic_authority = true;
    // The outer loop's catch-up flag: true on the last logic tick of a frame's
    // batch (less than one 16 ms tick of backlog left after this quantum),
    // false while catching up. The session stamps it per tick; a bare kernel
    // keeps true. Gates the fire-loop emitter and the local lock tone.
    // [orig: dword_24E0E80, Game_MainLoop @0x52ba21..0x52ba3a; readers
    //  WeaponAction_ProcessFrame @0x5412a7, Entity_UpdateInfantryPlayerBody
    //  @0x4b5229]
    bool last_tick_of_batch = true;
    // The update's head already ran under the pause word: the resuming update,
    // whose HUD counters stepped as paused ahead of its input frame, so
    // begin_tick steps them no more. The session stamps it per tick; a bare
    // kernel keeps false (inmatch::Session::run_updates carries the witness).
    // [orig: Game_ProcessMainFrame @0x5263F0 (Game_TickHudFrameCounters @0x5265D5
    //  ahead of Input_ProcessFrame @0x52661D)]
    bool update_head_stepped = false;
    // The single-player pause word as the session holds it (its Paused state): the
    // action rows' head drops every row whose flags carry bit 0 while it is set
    // (inmatch::Role::action_head_admits). The session stamps it at each state
    // change. [orig: dword_A87050; Input_HandleActionBinding @0x49AD40 (`test ecx,
    //  ecx` @0x49ADDD, `cmp dword_A87050, 0` @0x49ADE1)]
    bool pause_word = false;
    // The local cheat word's 0x100 bit (g_DebugCheatFlags), the infinite-ammo cheat: every
    // ammo pool read returns its class's carry cap (weapon_seat_pool, through the seat
    // entity_ammo_seat builds) and a clipless weapon's per-shot cost test is skipped. The
    // cheat row 114's input action 121 toggles it (inmatch::Role::dispatch_record), out of a
    // session only; a fresh mission's world starts clear, as Game_StartMission's store of the
    // word leaves it.
    // [orig: Cheat_ToggleInfiniteAmmo @0x4DC9C0 - `xor g_DebugCheatFlags, 100h` @0x4DC9C5;
    //  Entity_GetScoreValueBySlotType @0x5406E0 - the test @0x5406E3, g_AmmoClassMaxCarry
    //  @0x5406F4; WeaponSlot_CanFire @0x541BA0 - the test @0x541CCF; Game_StartMission
    //  @0x525B25]
    bool ignore_weapon_ammo_cost = false;
    // game.cfg's farp_reuse_time, in seconds: how long a vehicle's FARP service rests after
    // a service ends, which the FARP pass and the 0x0A rearm byte read; the config
    // defaults' 90 until the host stamps its block.
    // [orig: g_GameConfigState.farpReuseTime_A3C @0x25510F4, read by
    //  Server_UpdateEntityTargetLockAndWeaponOverlays @0x511823 and NetPacket_WritePlayerState
    //  @0x4FF930; Config_SetDefaults @0x54D48B; Config_ParseSettingsLine @0x54FEAB]
    int32_t farp_reuse_seconds = 90;
    // The local cheat word's 0x400 bit, the frame-rate overlay: HUD_DrawGameplayOverlays
    // draws "FR %2i  MN %2i MX %2i CPU %d" while it is up. The Debug
    // row 118's action 74 toggles it, in a session or out of one (its flags are 0); a fresh
    // mission's world starts clear.
    // [orig: Input_HandleActionBinding @0x49AD40 - case 74 `xor g_DebugCheatFlags, 400h`
    //  @0x49B937; HUD_DrawGameplayOverlays - the test @0x5BDEB5, HUD_DrawFrameRateOverlay
    //  @0x5BAE20 called @0x5BDEC2; Game_StartMission @0x525B25]
    bool frame_rate_overlay = false;
    bool cease_fire = false; // g_InCeaseFire @ 0x24C196C
    // The player profile's auto-reload word as the mission start's session copy
    // leaves it (profile::session_input): one process global the weapon action
    // handlers read for EVERY slot they run, the local player's, the AI's and
    // the remote players' an authority pumps alike — an empty magazine's IDLE
    // queues the reload only with it set, and the last round's RECOIL queues it
    // only with it set. Default on, a fresh record's +1524 = 1.
    // [orig: g_AutoReloadEnabled @0x24D2118 <- Game_ApplySessionSettingsToGlobals
    //  @0x551a40 (forced 0 by `/noreload` @0x551a48); readers WeaponAction_Idle
    //  @0x5429a6 (no owner test) and WeaponAction_Recoil @0x543013 (the local
    //  player's arm)]
    bool auto_reload = true;
    // Projectile_UpdatePhysics clamps the radius to 0.1u only for an
    // authoritative multiplayer FatBullets trace owned by a remote player.
    // These explicit host-fed gates keep that option out of ordinary/SP rays.
    bool projectile_authority = true;
    bool fat_bullets = false;
    bool one_shot_kill = false; // MP-only g_OneShotKill; ignored offline
    // The mpattrib rules word's 0x10000 bit: in session the scope-zero -1
    // (auto rangefinder) floor needs it, hence the name. Hosts stamp it from their config's
    // mpattrib value, joiners from the S2C 0x64 fixed block's +44 word
    // [orig: `test g_RulesFlags,10000h` @0x4dbd15 in Player_AdjustWeaponZoomLevel;
    // g_RulesFlags @0x24D1E34 = mpattrib]. Retail's mp_allowsniperscopezoom
    // option never reaches this word (it feeds the 0x08 flags bit 16 that
    // WeaponSlot_InitFromDef @0x53ef17 reads), so the bit is live only when a
    // host cfg carries it in mpattrib.
    bool auto_scope_zero = false;
    // The mpattrib rules word's 0x200 bit (host option NoFriendlyFire): in
    // session it suppresses the self/friendly-hit blackout arm
    // [orig: `test g_RulesFlags,200h` @0x4af752 in Entity_ApplyCollisionForce;
    // g_RulesFlags @0x24D1E34 = mpattrib]. Hosts stamp it from their config's
    // mpattrib value, joiners from the S2C 0x64 fixed block's +44 word.
    bool no_friendly_fire = false;
    // The host's allowSniperScopeZoom option as the SESSION sees it
    // (g_SessionAllowSniperScopeZoom): the class-6 sniper lock on a Primary def's scope zoom (its
    // floor becomes scope_max_mag) reads it at the zoom step and the mount clamp. Retail zeroes it
    // outside a session, stamps the serving host's config value on the authority and, on a joiner,
    // bit 16 of the S2C 0x08 flags dword (the bit server_initial_state.cpp emits from the option);
    // offline it stays 0, which locks snipers at max. Hosts stamp it from
    // GameConfig::allow_sniper_scope_zoom (HostRole::apply_rule_words), joiners from their
    // session-config bitflags after each fold and on the load's fresh world
    // (JoinerRole::mirror_session_zoom_rule); the slot fills read sniper_zoom_fill below.
    // [orig: Game_ApplySessionSettingsToGlobals @0x552284 (the reset) / @0x5522b9
    //  (the authority stamp); NapiNPClientMsg_HandleSessionConfig @0x428392;
    //  readers Player_AdjustWeaponElevation @0x4dbe36, Player_MountWeaponSlot
    //  @0x4dfaf9, WeaponSlot_InitFromDef @0x53ef17]
    bool allow_sniper_scope_zoom = false;
    // The value WeaponSlot_InitFromDef's fill reads for the same class-6 lock: the
    // rule above in a session on a peer, else the config's own allowSniperScopeZoom
    // (a dedicated host's, single player's game.cfg). [orig: WeaponSlot_InitFromDef
    //  @0x53EF07..0x53EF25 - is_in_session @0x53EF07, is_mp_session_peer @0x53EF0F,
    //  g_SessionAllowSniperScopeZoom @0x53EF17,
    //  g_GameConfigState.allowSniperScopeZoom_5EC @0x53EF1F]
    bool sniper_zoom_fill = false;
    // Multiplayer blast damage to Building ItemDefs is disabled unless the
    // host's `destroybuild` rule is nonzero. Offline/SP ignores the option.
    // [orig: g_DestroyBuildings gate in Entity_ApplyWeaponDamage
    // @0x4E682E..0x4E6860]
    bool destroy_buildings = false;
	// A destroyed PlayerControl hull respawns (true) or is removed at its first
	// dead tick (false): the host config's `unlimited_vehicles`, stock 1
	// (inmatch::GameConfig::unlimited_vehicles, stamped by the host bring-up).
	// Retail reads it as the mission-data block's +0x30 word, zero until every
	// Game_StartMission rebuilds the block from the config.
	// [orig: g_RulesUnlimitedVehicles = block unk_24D1E08 +0x30, stored by
	//  Client_BuildMissionDataRequestBlock @0x51E8C5..0x51E8CB from dword_24D2258
	//  = g_GameConfigState.unlimitedVehicles_4D0 (Game_ApplySessionSettingsToGlobals
	//  @0x551D80..0x551D91; Config_SetDefaults @0x54D352); AI_TickState_VehicleDead
	//  @0x467EE9, Entity_UpdateVehicleAIMovement @0x461246]
	bool vehicle_respawns = true;
    // The local debug/cheat word's 0x800 bit (g_DebugCheatFlags): the SM feed's
    // class-0 player leg skips the LOCAL player and the weapon validator
    // rejects every Player target while it is up [orig: `test
    // g_DebugCheatFlags,800h` @0x467141, @0x53A46E]. The cheat row's input
    // action 123 toggles it (@0x4E07A4; inmatch::Role::dispatch_record), out
    // of a session only, and Game_StartMission zeroes it (@0x525B25): a fresh
    // mission's world starts clear. No net wire carries it.
    bool ai_rules_skip_local_player = false;
    // The difficulty word: outside a session the local player's health
    // ceiling doubles at -1 and halves at 1 (max_health_with_difficulty,
    // entity_spawn.h). A fresh SP start's bring-up seeds it from the profile's
    // +0x564, which the player's spawn raise reads (spawn_player); every
    // mission start out of a session then leaves it
    // at mission_start_difficulty's word (MissionKernel::complete_mission_start),
    // where the title's rules set it (TitleRules::mission_start_sets_difficulty;
    // dfx2.exe's keep the profile's);
    // a session's is the config's byte, which no in-session reader of this
    // word needs. [orig: g_SessionDifficulty; Game_ApplySessionSettingsToGlobals
    //  @0x551F75; Game_StartMission @0x525CDD / @0x525CFA;
    //  Entity_GetMaxHealthWithDifficulty @0x43B8C8]
    int32_t difficulty = 0;
};

// The difficulty word a mission start leaves outside a session: 0, and -1
// under the objective Co-op game type (the 0x10020 family with the 0x20000
// bit, 0x30020). [orig: Game_StartMission - `and ecx, 0FFFDFFFFh; cmp ecx,
//  10020h` @0x525CD1..0x525CD7, the 0 @0x525CDD, `test eax, 20000h`
//  @0x525CE5, the -1 @0x525CFA]
constexpr int32_t mission_start_difficulty(uint32_t game_type) {
    return (game_type & 0xFFFDFFFFu) == 0x10020u && (game_type & 0x20000u) != 0 ? -1 : 0;
}

// A HUD relay the authority sends the joiners as S2C 0x3F, in the order the
// sim produced them: kind 0 is an objective notification (slot, is_win,
// is_active, flag), kind 1 a mission-text chat line (team, key). The host
// fan drains these.
// [orig: Server_BroadcastEntityActionPacket @0x5080d0; its producers
//  HUD_ShowObjectiveNotification @0x5ba2e0 (the call @0x5ba3df) and
//  GameMsg_AddChatLineAndRelay @0x5ba170 (the call @0x5ba1c3)]
// A reload the authority's weapon walk began on a row other than the local
// player's: the C2S 0x25 body WeaponAction_Reload queues into the host's own
// dispatcher, which broadcasts it as S2C 0x49. A UseGun gunner addresses its
// gun (whose EWeap def sends 0xFFFF), any other row itself with its slot's
// category * 65 + rank. [orig: WeaponAction_Reload @0x5430B6..0x543108;
//  NetPacket_SendReloadRequest @0x432930 (@0x43296C..0x432974,
//  CNapiNetwork_QueueReliableMessage @0x4329B7)]
struct WeaponReloadRequest {
	uint16_t entity_handle = 0xFFFF;
	uint16_t reload_param = 0;
};

// One CHAT-ring line by its game-text key (WorldOutbox::hud_chat_lines); `system_ring`
// posts it to the SYSTEM ring instead (Chat_AddMessageChannel2's, the cheat toggle's line).
// `text`: with no key, the line as it is (a literal or a line the sim formatted); with a
// key, the key's text with `text` as its $A token (hud::chat_format_player_tokens), as the
// scope-scale toast's [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0x31's
// Chat_FormatPlayerTokens @0x4E05E9].
struct HudChatLine {
    const char *section = "";
    const char *key = "";
    uint32_t argb = 0;
    bool system_ring = false;
    std::string text;
};

struct HudRelay {
    uint8_t kind = 0;
    int32_t slot = 0;
    int32_t is_win = 0;
    int32_t is_active = 0;
    uint8_t flag = 0;
    int32_t team = 0;
    std::string key;
};

// The body state a deferred scope toggle call saw: the entity Flags word and
// the motor's airborne mirror (the 0x2000 bit's other half), read by the
// toggle's 0xA000 gate in place of the live ones.
struct ScopeToggleCallFlags {
    uint32_t flags = 0;
    bool airborne = false;
};

// The local body's water-block scope legs this tick, in call order. Each calls
// Player_ToggleWeaponScope when Player_IsEquippedWeaponScoped or
// Player_IsVehicleGunnerScoped answers; the local view tick runs them after its
// settle promoter, as retail's entity update follows Player_UpdatePerFrame,
// with the Flags word each call saw (world/local_player_view.h).
// [orig: Entity_UpdateInfantryPlayerBody -- the entry @0x4b8316..0x4b8328, the
//  swim @0x4b8342..0x4b8360, the eye @0x4b837b..0x4b83a3]
struct WaterScopeLeg {
    enum class Kind : uint8_t { Entry, Swim, Eye };
    Kind kind = Kind::Entry;
    ScopeToggleCallFlags at_call;
};
struct WaterScopeLegs {
    // The entry or the swim leg, then the eye leg.
    static constexpr int kCapacity = 2;
    std::array<WaterScopeLeg, kCapacity> legs{};
    int count = 0;
    void push(WaterScopeLeg::Kind kind, uint32_t flags, bool airborne) {
        if (count < kCapacity) legs[count++] = {kind, {flags, airborne}};
    }
};

// The HUD's frame counters, each stepped once per frame tick (one Game_ProcessMainFrame, the 62 Hz
// logic tick): the main-frame counter always, the session frame counter unless the menu pause
// holds the game, the HUD frame counter unless the pause or the epilog screen does; the mission
// start zeroes them. The port's menu pause runs no frame tick (inmatch::Session's Paused state),
// so the pause gate holds only for an embedder that steps a paused frame.
// [orig: Game_TickHudFrameCounters @0x434C00 -- ++g_MainFrameCounter (0xA8705C) @0x434C05;
//  `cmp dword_A87050, 0; jnz` @0x434C0B, ++dword_A87060 @0x434C14; `cmp g_EpilogScreenActive,
//  0; jnz` @0x434C1A, ++g_HUDFrameCounter (0xA87064) @0x434C23; called from Game_ProcessMainFrame
//  @0x5265D5, ahead of the frame's input, server tick and entity update; Game_ResetSessionHudState
//  @0x434BD0 from Game_StartMission @0x525BAA]
struct HudFrameCounters {
    uint32_t main_frame = 0;    // g_MainFrameCounter: the chat debounces, the chat cursor blink
    uint32_t session_frame = 0; // dword_A87060: the kill list's page, the map blips' blinks
    uint32_t hud_frame = 0;     // g_HUDFrameCounter: the HUD's blinks and pulses

    void step(bool menu_paused, bool epilog_screen) {
        ++main_frame;
        if (menu_paused) return;
        ++session_frame;
        if (!epilog_screen) ++hud_frame;
    }
};

// A designator round's mark: a DesignateTarget ammo spawns no round, and the
// authority registers its firer's mode-1 designation at the spawn point
// instead, with the firing weapon's life (+0x458) and radius (+0x454).
// [orig: RoundData_SpawnRound @0x4EC249..0x4EC270 -> EntityTracker_RegisterOrUpdate
//  @0x511580 (firer, point, ticks, radius, mode 1)]
struct DesignationMark {
    EntityHandle firer;
    int32_t point[3] = {}; // Q16
    int32_t ticks = 0;
    int32_t radius_units = 0;
};

// A co-op dialog line the presentation plays (a non-authority's S2C 0x28): the dialog by its
// name, the line's index and the local player's class, the clip's locale.
// [orig: NapiNPClientMsg_0x028 @0x425B40 -> Dialog_PlayByNameAndSlot @0x44E3F0, the class
//  @0x425B88..0x425B94]
struct DialogLineOrder {
    std::string dialog_name;
    int32_t line = 0;
    int32_t player_class = 0;
};

// What the sim produced this tick for someone else to drain: the wire (entity
// removals, the round ring, water crossings) and the presentation (effects,
// destruction, scars, scorches, the sound queues). Nothing in the sim reads
// an outbox back; the drains clear them.
struct WorldOutbox {
    // Preserve the callback's send order across 0x21 explosions, 0x26 state,
    // 0x12 removal, the guided motors' 0x44s and the placed devices' 0x59s.
    // The host drains this once, excluding its loopback.
    // [orig: Entity_HandleDeathEvent @ 0x4070F0; Entity_HandleDeathOnAuthority @ 0x407CC0;
    // Server_SendEntityStatePacket @ 0x509D70; Server_RemoveEntityAndNotify @ 0x50A270;
    // Entity_SendEffectPacket @0x445C40; Entity_UpdateSatchelPhysics's 0x59 @0x448BD5]
    using EntityNetworkEvent = std::variant<ItemStateEvent, ItemExplosionEvent, EntityRemoveEvent,
            DoorRowEvent, GuidedUpdateEvent, DeviceSpawnEvent>;
    std::vector<EntityNetworkEvent> entity_events;
    // A client's door section requests (C2S 0x1A, doors.cpp carries the
    // witness); the joiner role drains them into its send queue. The authority
    // never fills it.
    std::vector<DoorRowEvent> door_requests;
    // A client's local body's seat-leave requests (C2S 0x27, the target's wire
    // handle) the plyr class callback raised (infantry_death.cpp carries the
    // witness); the joiner role drains them into its send queue. The authority
    // never fills it.
    std::vector<uint16_t> detach_requests;
    // The local player's C2S 0x1D stance requests the sim raised past the input
    // dispatcher's stance rows, by action id, in raise order: the mount request's crouch
    // (0xA9) for a ForceCrouch def (player_weapon.cpp mount_queue_switch_out carries the
    // witness). The role queues each on its own client connection as the stance key's
    // (Role::flush_stance_requests); a role with no client connection drops them.
    std::vector<uint16_t> stance_requests;
    // A client's C2S 0x0F entity requests the movers raised, in raise order, by World handle
    // (the ewep mover's stale-ground pair, ai_system.cpp carries the witness); the joiner role
    // drains them into its carrier-repair queue, its own body L by its wire handle. The
    // authority never fills it.
    std::vector<EntityHandle> entity_info_requests;
    // HUD relays pending the host's S2C 0x3F fan.
    std::vector<HudRelay> hud_relays;
    // Powerup ammo grants for REMOTE players' connection pools (world/powerup.h);
    // the host tick drains them.
    std::vector<PowerupGrant> powerup_grants;
    // The authority's powerup weapon grants, every picker's (world/powerup.h):
    // the host tick lands a remote picker's on its connection's slot table and
    // fans S2C 0x35 for each.
    std::vector<PowerupWeaponGrant> powerup_weapon_grants;
    // The weapon walk's authority reloads for rows other than the local player's
    // (WeaponReloadRequest above); a listen host relays them through its own
    // client like its player's (D-NET-336).
    std::vector<WeaponReloadRequest> weapon_reloads;
    // Fired-round events pending per-recipient S2C 0x0A tag-2 echo (round_ring.h). Fed by
    // the C2S 0x06 dispatch on accepted fire; drained per connection watermark by the
    // replication emit. [orig: g_RoundRing @0xC8D848 via RoundData_AddRound @0x4fdb40] (D-NET-152)
    RoundRing rounds;
	std::vector<RoundSpawnParams> source_fires; // local source fire awaiting C2S emission
	// Water-surface crossings recorded this tick; the host fan drains them
	// into S2C 0x34 and clears. Presentation only - nothing in the sim reads it.
	WaterCrossQueue water_crossings;
	// The local body's water-block scope legs (WaterScopeLegs above): the
	// body's water block records them, the local view tick runs and clears them.
	WaterScopeLegs water_scope_legs;
	// The local player's tip events (hud/tip_system.h TipEvent) in the order
	// they were raised — boarding and leaving a seat, the scope and NVG
	// toggles, the binocular edge, and a client's spectator begin (the
	// replica's S2C 0x0A / 0x4D legs land through the client effects). The
	// HUD owner drains them into its tip [orig: the CTipSystem_HandleEvent
	// call sites; docs/interface/hud-re.md "The tip"].
	std::vector<uint8_t> tip_events;
	// The S2C 0x0F's death-screen HUD blank pending the HUD owner's apply: the
	// live declutter level goes to the blank level (hud/hud_toggles.h
	// hud_toggles_death_screen) [orig: NapiNPClientMsg_0x00F @0x42e3f5..0x42e41c].
	bool hud_detail_blank = false;
    // The destruction presentation events (world/destruction.h) the host drains.
    DestructionEvents destruction;
	std::vector<VehicleEffectEvent> vehicle_effects; // fixed-tick movement particles
	// The WAC/BMS/sim effect log the presentation drains ("text", "dialog", the
	// WAC fx command names, ...).
	EffectLog effects;
    std::vector<ScriptEffectEvent> script_effects;
    std::vector<ScriptSoundEvent> script_sounds;
    // The CHAT-ring lines the sim posts by their game-text key, in the order raised; the
    // HUD owner resolves each in its section (a miss reads "") and posts it in its colour for
    // the 930 ticks every such site passes (Chat_AddMessageChannel1; the ewep tank-command
    // leg, world::emplaced_command_call, carries the witness).
    std::vector<HudChatLine> hud_chat_lines;
    // The kill-announce banner line a local player's pickup wrote (a powerup.def
    // action's resolved texttoken), with the logic tick it stamped; unset when none is
    // owed. The HUD owner records it into the banner the involved S2C 0x1E lines feed
    // (hud::KillAnnouncement) and clears it; a later write in the frame replaces an
    // earlier one, as the one banner buffer does. [orig: ActionSlot_OnTextTokenCallback
    // @0x4011B0 - g_KillAnnounceText @0x4011D2, g_KillAnnounceTick @0x4011E0; the
    // banner's other writer NetPacket_HandleGameEvent @0x427B86 / @0x427B96]
    std::optional<std::string> kill_announce_text;
    uint32_t kill_announce_tick = 0;
    // An audio channel shutdown is owed: every audio channel falls silent and
    // the ambient channel handles drop, so the mix takes its channels anew. The
    // embedder's audio device takes and clears it. Three setters: the pause
    // row once it has raised the single-player pause word (the session's
    // pausing update; the gate word is the pause word itself, flipped @0x49B52D),
    // the LOSE cine's start (World::process_round_end) and the LOSE epilog's
    // build step (EpilogCine::update_lose). The fourth caller, the in-game
    // options screen's open out of a session, is the embedder's own menu leg
    // (GameWorld::shutdown_audio_channels) [orig: Input_HandleActionBinding
    // @0x49AD40 - case 25, `cmp dword_A87050, 0; jz` @0x49B543..0x49B54A, the
    // Audio_ShutdownChannelsAndDeviceTable call @0x49B550; UI_OptionsScreenInit
    // @0x554800 - the call @0x554DE4 behind !is_in_session;
    // Audio_ShutdownChannelsAndDeviceTable @0x527790 - AudioChannel_InitAll
    // @0x7671B0 (silence on all 26 channels), the handle table zeroed
    // @0x527797..0x5277BA].
    bool audio_channels_shutdown = false;
    // A round end out of a session stops the dialog channel's playing line (the
    // channel the last loaded line holds) ahead of its Dialog_ResetAll; the
    // embedder's audio device takes and clears it [orig: Server_ProcessRoundEnd
    // @0x5164F0 - the session gate @0x516942..0x516948, the 0 argument @0x51694A,
    // the DialogAudio_PlayNextChunkOrStop call @0x51694B; DialogAudio_PlayNextChunkOrStop
    // @0x44E320 - a 0 argument jumps to AudioChannel_ResetByHandle on
    // g_DialogAudioChannelHandle @0x44E32A..0x44E362].
    bool dialog_line_stop = false;
    // The lose cine's edit fade scales the main loop's frame-time smoothing
    // history (g_FrameTimeSmoothedFp4) at its start and its finish, in call
    // order; the in-match session applies each to its bank after the tick
    // (world::TickAccumulator::scale_smoothing_history carries the witness)
    // and clears the list. Neither the presentation nor the wire reads it.
    std::vector<float> frame_time_scales;
    uint64_t next_script_effect_order = 0;
    // The WAC commands the VM replicated this tick (registry flags 0x18) for
    // the server tick to send as S2C 0x23; script_remote_command.h carries the
    // witness. Cleared by the drain; a client-side World never fills it.
    std::vector<ScriptRemoteCommand> script_remote_commands;
    // The designator rounds' marks for the host's designation table and its
    // S2C 0x1E notice; the host tick drains them.
    std::vector<DesignationMark> designation_marks;
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
    // The co-op dialog lines to play, drained by the presentation.
    std::vector<DialogLineOrder> dialog_lines;
    // The weather tick's thunder one-shots (weather_state.h carries the
    // cites); the presentation owner drains them per frame.
    std::vector<WeatherSoundEvent> weather_sounds;
    // The fire-sound propagation-delay queue on the logic clock, seeded inline
    // at round spawn and counted down at the head of run_logic_tick; the
    // presenting host stamps the listener and drains the ready one-shots
    // (world/fire_sound.h witness map). [orig: the pending-sound slots
    // @0x24DF678, Sound_TickPendingSlots @0x529310]
    FireSoundQueue fire_sounds;

    // The presentation half dropped undrawn, for a host with no presenter
    // (opennova-serve): everything a presenter drains each frame -- the tips,
    // the HUD blank, the audio stops, the destruction, vehicle, script and
    // effect queues, the pending scorches, the sound queues and the fire
    // sounds' ready list -- so nothing accumulates over a long run. The wire half (the entity
    // events, the relays, the grants, the round ring, the water crossings,
    // the replicated WAC commands) stays for the host tick's own drain; the
    // scar rings and the fire-sound countdown slots are fixed-size state, not
    // queues. Next to the fields so the list cannot drift from them.
    void discard_presentation() {
        tip_events.clear();
        hud_detail_blank = false;
        audio_channels_shutdown = false;
        dialog_line_stop = false;
        destruction.clear();
        vehicle_effects.clear();
        effects.clear();
        script_effects.clear();
        script_sounds.clear();
        hud_chat_lines.clear();
        kill_announce_text.reset();
        terrain_scorches.clear_pending();
        slot_sounds.clear();
        sound_emitters.clear();
        dialog_lines.clear();
        weather_sounds.clear();
        (void)fire_sounds.drain();
    }
};

// The player-slot idle timers (the underwater breath samples) live on the host
// session's player slots, so the host session installs this seam for its tick;
// a world without a server session has no idle timers to run.
// [orig: Server_UpdatePlayerBreathTimers @0x50D770]
class IEntityIdleTimers {
public:
    virtual ~IEntityIdleTimers() = default;
    virtual void update_entity_idle_timers(World &world) = 0;
};

// The host session's sends inside the script pass. Retail queues each record
// the pass raises at its call (NapiNPServer_SendFiltered queues on the
// connection there and then), so a record a later call raises follows every
// send an earlier call made, a round end's 0x61 / 0x1D included: the WAC VM's
// 0x23 goes ahead of its own local call, each WAC command's and each BMS
// action's records go out at that call, and a Win's round end announces at its
// latch. Server_TickUpdate installs one around its script pass; a world
// without a host session has none.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 (SendFiltered(0x23) @0x4F5E74 /
//  @0x4F5ED1 ahead of the call switch @0x4F5EF6); EventTrigger_UpdateEntry
//  @0x454C30 (one EventAction_Dispatch per action, the calls @0x454CA6 /
//  @0x454D0E); WacAction_Win @0x4ED4A0 (the Server_ProcessRoundEnd call
//  @0x4ED4A5); NapiNPServer_SendToConn @0x4C4F20 -> CNapiNPConnection_QueueMessage
//  @0x628640]
class IScriptPassSends {
public:
    virtual ~IScriptPassSends() = default;
    // Send every record the pass raised since the last call.
    virtual void send_raised(World &world) = 0;
    // A round end latched inside the pass: what the pass raised before the
    // latch, then the round end's own sends.
    virtual void send_round_end(World &world) = 0;
};

// The authority's per-player weapon tables for a REMOTE player: retail's
// player-slot block behind Entity_ValidatePtr, its 780-slot table at +0x1D0,
// its class pools at +0x15A58 and its shared clips at +0x15C58. The host
// session installs it around its entity pass; with none installed (a joiner,
// a bare world) no remote player has tables, as Entity_ValidatePtr finds no
// slot. [orig: Entity_ValidatePtr @0x500910; WeaponSlot_RecalculateScore
//  @0x542464..0x54247C]
class IRemoteWeaponTables {
public:
    virtual ~IRemoteWeaponTables() = default;
    // A copy of `player`'s tables; false when no session slot holds it.
    virtual bool remote_weapon_tables(const World &world, EntityHandle player,
                                      WeaponInventory &out) const = 0;
    // The two ammo words a slot of `player`'s own table reads: the pool of
    // `ammo_class` (+0x15A58) and the loaded rounds of `bucket` (+0x15C58), each 0
    // outside its table; false when no session slot holds it.
    // [orig: Entity_GetScoreValueBySlotType @0x5406E0 (the validated player's pools
    //  @0x5407B3); WeaponSlot_GetSharedLoadedRounds @0x5405F0 (its buckets @0x540657)]
    virtual bool remote_ammo(const World &world, EntityHandle player, int32_t ammo_class,
                             int32_t bucket, int32_t &pool, int32_t &loaded) const = 0;
    // `player`'s pool of `ammo_class` stepped down by `amount`, floored at 0; false when no
    // session slot holds it. [orig: WeaponSlot_DecrementTimer @0x5454F0 - the validated
    //  player's pools @0x54550F..0x545520]
    virtual bool remote_pool_step_down(EntityHandle player, int32_t ammo_class,
                                       int32_t amount) = 0;
};

// Server_TickUpdate's every-32 legs, inside the one script admission, between
// the WAC tick and the BMS quarter pass: the vehicle spawn markers, then the
// player idle timers. The kernel registers it between the two script systems.
// [orig: Server_TickUpdate — WacScript_AdvanceTick call @0x51D8BF, then
//  `test tick,1Fh` @0x51D8C4, Spawn_AssignOverlaySpawnPoints call @0x51D8D2,
//  Server_UpdatePlayerBreathTimers call @0x51D8D7, then the quarter counter
//  @0x51D8DC]
class ServerIdleLegs final : public ISystem {
public:
    const char *name() const override { return "server_idle_legs"; }
    void tick(World &world, const TickContext &ctx) override;
};

class World {
public:
    World();
    // World has stable identity: commands binds this object and registered
    // systems retain mission-lifetime relationships. Memberwise copy/move
    // would preserve pointers/references into the source World and create a
    // split simulation.
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    World(World &&) = delete;
    World &operator=(World &&) = delete;

    EntityRegistry registry;
    ReverbState reverb;
    BodyZones body_zones;
    EnvState env;
    // The retail weather globals, ONE home (weather_state.h): the WAC weather
    // handlers write it through EntityCommands, the weather tick advances it
    // after the logic tick, the wire projection serializes it, a joiner's
    // decoder writes its targets back.
    WeatherState weather;
    CachedFrameState cached;
    // The client-local death-screen spectate state the local player's motor
    // reads (spectator_motor.h); the role stamps it from its replica runtime
    // ahead of every entity update.
    SpectatorMotorState spectator;
    // Non-owning local state, bound by the mission kernel for synchronous
    // deployment resets; bare authoritative worlds need no local view.
    LocalPlayer *local_player_state = nullptr;
    // The process's loadout name list (g_LoadoutWeaponNameList, weapon_inventory.h), bound by
    // the role that runs the world; null reads the image's initializer.
    LoadoutWeaponNameList *loadout_weapon_names = nullptr;
    EntityCommands commands;
    // The AI/motor system: every brain plus the organic bodies. Owned here so
    // the command layer, the sims, the wire and the tools reach brains without
    // a seam; update_all_entities drives it row by row after the script
    // systems, and the kernel wires its collision/terrain/root-motion links.
    AiSystem ai;
    // The server tick's every-32 legs (ServerIdleLegs above), registered by the
    // kernel between WAC and BMS; the host session installs the idle timers.
    ServerIdleLegs server_idle_legs;
    IEntityIdleTimers *entity_idle_timers = nullptr;
    // The script pass's sends (IScriptPassSends above), installed by the host
    // session around run_script_pass.
    IScriptPassSends *script_pass_sends = nullptr;
    IRemoteWeaponTables *remote_weapon_tables = nullptr;
    // The authority's inline transactions (entity_update_transactions.h): armed
    // by the host session around its tick's entity pass (Server_TickUpdate).
    IEntityUpdateTransactions *entity_update_transactions = nullptr;
    // The lifetime groups (declared above): what the script owns, what the
    // embedder feeds once, what the host stamps, what the drains consume.
    ScriptState script;
    // The waypoint/POI walks' view of this world (waypoint_track.h): the live
    // entries' entities and the session facts the KOTH leg reads. The game
    // type is g_GameType: the session's configured match type, or for a world
    // no session configured (the bare local role) the type its mission header
    // implies, the single-player launch's own derivation.
    WaypointCycleContext waypoint_context() const;
    MissionDiagnostics diagnostics; // retained across ticks; restored with the mission baseline
    MissionTables tables;
    SessionRules rules;
    WorldOutbox out;
    // The two systems that own their state and their verbs (vehicle_system.h,
    // zone_system.h); the entity update runs the vehicle motors, the host tick the
    // zone capture transaction.
    VehicleSystem vehicles;
	RotorWashSystem rotor_wash;
	ZoneSystem zones;
	// The command map's placed-waypoint table (world/user_waypoints.h).
	UserWaypointTable user_waypoints;
	// The software mixer's output meter (audio/audio_out_meter.h), which the device advances
	// once a frame: the speaking entity's head kick, TALK register and tag pulse read it
	// (speaking_head_kick, talk_ctrl_register_value).
	audio::AudioOutMeter audio_out_meter;
	// Game_StartMission seeds the one process-global PRNG_Next16 stream after
    // writing it twice; 0x1A10101A is the final retail dword_31BFBB0 value
    // [orig: push 1A10101Ah @ 0x5245F7 -> seed setter PRNG_SetSeed (ex sub_613130) in
    // Game_StartMission @ 0x524360]. AI recoil/engagement, throwable bounce
    // spin, and server control challenges all consume this owner in their
    // actual call order.
    static constexpr uint32_t kMissionPrng16Seed = 0x1A10101Au;
    uint32_t prng16_state = kMissionPrng16Seed;
    uint16_t next_prng16() noexcept;
    static constexpr uint32_t kMissionPrng16BSeed = 0x5ADEADA5u;
    uint32_t prng16_b_state = kMissionPrng16BSeed;
    uint16_t next_prng16_b() noexcept;
	// Stream C. Its presentation consumers (rotor-wash particles, wreck fire
	// crackle, the occlusion focal-wind sampler) draw behind listener,
	// camera-distance or render gates, and the death pieces' probability roll
	// draws for a pool each peer spawns for itself, so those draws diverge per
	// machine. One sim consumer draws it too: the vehicle AI's class init seeds
	// brain word 200 from it at the mission start (promote.cpp) [orig:
	// Entity_InitVehicleAI @0x460200 - the draw @0x46035E], so the stream's position
	// at that init is part of each brained vehicle's start state; nothing compares
	// it across peers. It is
	// snapshotted so an editor play/stop cycle restores the mission-start state.
	// Seeded at every mission start like streams A and B: Game_StartMission
	// pushes 0x10101010 into the stream-C setter right after the A seeds
	// [orig: `push 10101010h; call sub_6131A0` @0x524601/@0x524606 -> dword_31BFBB4;
	//  PRNG_Next16_C @0x6131B0 reads/writes it @0x6131b3/@0x6131dd;
	//  WeatherParticle_UpdateAllEmitters @0x5CB100 draws it after the 0x2200000
	//  camera gate @0x5CB1CD]. (The pre-2026-09-10 "BSS boot state" 0 was wrong.)
	static constexpr uint32_t kMissionPrng16CSeed = 0x10101010u;
	uint32_t prng16_c_state = kMissionPrng16CSeed;
	uint16_t next_prng16_c() noexcept;
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

    // The live authoritative rounds — spawned synchronously by the accepted C2S 0x06
    // (the same fire that appends `rounds`), stepped inside run_logic_tick, deaths
    // drained by the host session. [orig: RoundData_SpawnRound @0x4ec0d0 inline from
    // RoundData_AddRound; Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    RoundSim round_sim;
    // The authority's count of deployed players (g_ServerNumPlayers @0xC8FC54): every
    // joined slot whose body is not hidden, recounted each periodic second by the host's
    // server tick; never written on a client or in single player. The heat seekers' flare
    // roll reads it [orig: Server_CountActivePlayers @0x4FD900 (the store @0x4FD94B), called
    // from Server_TickUpdate @0x51D7E0 (@0x51DF8C); Weapon_GetFlareDeflectChance @0x5382A0].
    int32_t server_num_players = 0;

    // The explosion queue + AoE damage, the death-piece pool, the per-item death
    // traits, and the destruction presentation events (world/destruction.h;
    // world-wac-ai-re §24). Explosions queued this tick drain inside
    // run_logic_tick right after the round sim [orig: Projectile_ProcessExplosionQueue
    // @0x4ead80 runs once per frame after the projectile update]; the host drains
    // `destruction` (present) and feeds `item_death_traits` (item-traits sweep).
    ExplosionSim explosions;

    // The body readout (an instrument, no behaviour; body_trace.h, the body_trace probe):
    // the watched handles and their per-tick, ground-probe, force and blast records. Not
    // part of the snapshot.
    BodyTrace body_trace;

    // Placed throwable devices + class bindings (world-wac-ai-re §27).
    ThrowableSim throwables;
    MinefieldSystem minefields;
    DoorSystem doors;
    FacialSystem facials;
    TeammateOperations teammates;
    int32_t vehicle_ai_spawn_phase = 0; // [orig: dword_B21F80]
    // The record spawn's think-clock stagger: every record spawn, the load's
    // records and the addeweap children alike (a deployable's children too),
    // takes this counter modulo 60 into its +0x2AC clock and steps it by 11.
    // The mission reset each load runs first zeroes it (promote_mission).
    // [orig: g_BmsSpawnPhaseCounter @0xA77638 - Entity_SpawnFromBMSRecord
    //  @0x40EC8C..0x40ECB8; Mission_ResetBmsState @0x40DB80 (the zero @0x40DBD1)]
    uint32_t bms_spawn_phase_counter = 0;
    IItemPieceSpawner *item_piece_spawner = nullptr; // non-owning mission asset factory
    ITeammateSpawner *teammate_spawner = nullptr; // non-owning mission asset factory
    DeathPieceSim death_pieces;
    DestructionRng destruction_rng;
    ItemEmitterSystem item_emitters;

    // The authoritative session rules/stats/outcome + the SP kill-stat buckets.
    // Multiplayer and WAC/BMS outcomes share Match's one double-run latch.
    Match match;
    MissionKillStats kill_stats;
    // The SP end-of-round cine (world/epilog_cine.h): the round end's SP tail
    // starts it, the entity update steps it.
    EpilogCine epilog;
    // Why the mission loop ends, as the world-side writers store it: the end
    // screens' timeouts (1), the round-over RESTART key and the in-game
    // RESTART command (4, the single-player restart), the round-over ESC (1).
    // The session reports a nonzero reason as its mission exit; the net
    // writers (the NovaWorld exit, a disconnect record) keep theirs on the
    // server context and the client runtime, the values of
    // inmatch/mission_exit.h. [orig: g_MissionExitReason @0x24C1918]
    int32_t mission_exit_reason = 0;
    // The mission a Mission box (type 3) sends the local player to, with the exit reason 8:
    // the box's eight characters and ".bms", which the Game Loop's start loads (loose when the
    // file stands in the game folder) [orig: g_MapFileName, written
    // Entity_UpdateInfantryPlayerBody @0x4B60BE..0x4B60F3; g_MissionSourceIsLoose
    // @0x4B60FE..0x4B6114].
    std::string mission_exit_map;











    // The engine tick counter: one logic tick per host frame at 62 Hz.
    // [orig: g_CurrentTick @0x24c1968, ++ once per Game_ProcessMainFrame @0x5263f0.
    //  Per-system cadences divide it: the WAC VM executes every 62nd tick
    //  (WacScript_AdvanceTick @0x4f81b1), the BMS normal-event quarter pass runs every 16th
    //  (Server_TickUpdate @0x51d7e0), the AI motor staggers on 2/8/16 internally.]
    uint32_t logic_tick = 0;
    // The HUD's frame counters, stepped at the head of every frame tick (begin_tick).
    HudFrameCounters hud_frames;
    // The entity-update counter: the number of completed entity updates. Its
    // one writer is the tail of a non-epilog update_all_entities, and nothing
    // resets it, so it runs one behind logic_tick through the process's first
    // mission, holds still on a frame whose entity update is skipped, and
    // keeps counting across restores (it is not in the Snapshot) and mission
    // loads (the embedder carries it into the next kernel). The ground
    // vehicles' ground-link cadence, the vehicle avoid-brake factor, the
    // movement resolver's full-update cadence and the water decal scroll read
    // it. [orig: g_EntityUpdateCounter, `add g_EntityUpdateCounter,esi` in
    //  Entity_UpdateAllEntities @0x4C2639]
    uint32_t entity_update_counter = 0;
    // The embedder's leg at the entity update's proximity build, ahead of the World's
    // own tables: a joiner rebuilds its decoded rows' collision proxies there, after the
    // pool-1 walk and the projectile pass, so those read the table the previous update
    // built, as retail's do (D-NET-196). Empty on every other peer. [orig:
    // Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
    // Weapon_UpdateAllProjectiles call @0x4C223A, the Entity_BuildProximityLists_Pool01
    // call @0x4C240A]
    std::function<void()> before_proximity_build;
    // The embedder's leg between the entity update's proximity build and its pool-0
    // walk: a joiner's replica pool-0 rows through its own body's slot take their
    // updates there, so the own body's update re-reads them moved (D-NET-196). Empty
    // on every other peer. [orig: Entity_UpdateAllEntities @0x4C2100 - the proximity
    // build @0x4C240A, the pool-0 walk @0x4C2426..0x4C2472]
    std::function<void()> before_pool0_walk;
    // The embedder's leg at each pool-1 slot of the walk, its handle the slot's: a
    // joiner's decoded pool-1 row takes its visit there, at its own slot in slot order
    // (D-NET-196). update_pool1_slot calls it as the slot's +0x1C4 legs end (the vehicle
    // mover, which returns at once for a traitless row, the ewep class update, the item
    // callbacks, the drop motion), so a chased hull's chase runs at its mover's position
    // and a World mover's pose, or an ewep or genx row's class update, reaches its row
    // right after it; a slot the registry holds no row at calls it in the walk. Empty on
    // every other peer. [orig:
    // Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
    // Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 / @0x4C21E0 / @0x4C21E9;
    // Entity_UpdatePool1Slot @0x4B8DD0 - the +0x1C4 mover @0x4B8E41..0x4B8E53]
    std::function<void(EntityHandle)> pool1_slot_leg;
    // A slot's destroyed mark (+0x219) by its wire handle: retail's shared destroy zeroes a
    // slot and marks it, and a record that rewrites the slot clears the mark with its memset.
    // A joiner's decoded rows keep the marks (replication::ClientState::destroyed_marks) and
    // its role binds this to read them; empty on every other peer, where no reader runs (the
    // readers run off the authority only). [orig: Entity_Destroy @0x43E810 - the memset
    // @0x43EA70, the mark @0x43EA7F; read by Entity_UpdateTransformAndTurret @0x440D2A]
    std::function<bool(uint16_t wire_handle)> destroyed_mark;
    // A decoded remote person's entity words, by its wire handle: retail's client built the
    // person as its own pool-0 entity, and its readers take these words of it. The HUD target
    // cursor's (HudTargetCursor::wire_person): the team byte the crosshair's and the inset's team
    // compares read, the def type the crosshair's bracket test reads. The heat query's pool-0
    // leg (guided_heat_target), which walks the client's pool-0 slots and so these persons: the
    // walk's words and the fire origin's. The def words come from the items row the record's
    // type word names (item_index_by_type_id: no row is index 0, no def). A joiner's role binds
    // both hooks over its replica rows (false: no row holds the handle); both are empty on
    // every other peer, whose pool-0 persons are registry entities.
    // [orig: NapiNPClientMsg_0x00C @0x42E730 - ItemList_FindIndexByTypeId's index @0x42E84A,
    //  the team @0x42E979; HUD_DrawCrosshair @0x592640 - the team read @0x592692, the def type
    //  @0x592CE8..0x592CF3; Render_WeaponInsetScene @0x5C9740 - the team read @0x5C9F84;
    //  Entity_FindTargets @0x53A610 - the pool-0 walk @0x53A6E7..0x53A8A3]
    struct WirePerson {
        uint16_t type_id = 0;  // the record's type word (+0x1C's ItemList_FindIndexByTypeId key)
        uint8_t team = 0;      // +0x162, 0 while none decoded
        uint8_t byte_154 = 0;  // +0x154, the record's slot byte
        uint32_t flags = 0;    // +0x24: the spawn's word and the client's own latches on it
        bool health_zero = false; // Health (+0x11E) zeroed by a death record
        // g_CurrentTick less +0x1AC, the death edge's stamp (0 until an edge): a dead body
        // stays a target for 16 ticks [orig: Entity_UpdateInfantryPlayerBody @0x4B4CC1 /
        //  @0x4B4CCC; Entity_UpdateInfantryAI @0x4B9D24 / @0x4B9D2F].
        int32_t death_age = 0;
        int32_t pos[3] = {};   // Position (+4..+0xC)
        int32_t eye[3] = {};   // CameraOffset (+0x6C..+0x74)
        // The seat mount (+0x16C), a pool-1 handle (0xFFFF none): the LOS walk's target parent.
        uint16_t seat_parent = 0xFFFF;
    };
    std::function<bool(uint16_t wire_handle, WirePerson &out)> wire_person;
    // The decoded pool-0 rows other than the local player's own, by handle (pool-0 slot)
    // ascending: the slots the client's pool-0 walks visit.
    std::function<void(std::vector<uint16_t> &handles)> wire_person_handles;
    // The authority's GetTickCount: the process clock its host binds (HostConfig::clock_ms, at
    // the session start and at each next mission's bind); empty is the deterministic logic-tick
    // seam (authority_tick_count_ms). The 0x26 sender's entity+560 stamp reads it, and so do
    // the S2C 0x19 / 0x1A / 0x0F stamps a joiner echoes as its C2S 0x28 kill-page window, so a
    // load or a held start counts on both sides of the window (D-NET-1040).
    // [orig: Server_SendEntityStatePacket @0x509D70 - GetTickCount, stored to entity+0x230
    //  @0x509D7A]
    std::function<uint32_t()> tick_count_ms;

    // May the mission script advance this tick? Retail wraps its WAC tick, the
    // idle-timer sweep and the BMS event pump in ONE condition, and the half that
    // matters here is `g_WacVarHumans || !g_WacVarTicks`: a nonzero mutable WAC
    // clock holds the script until a human player is in the world. Scripts may
    // write ticks back to zero; completed-execution diagnostics do not gate it.
    //
    // That gate is the difference between a scripted kill reaching a client and
    // firing into an empty session: 05TRcoop kills ten AI a fifth of a second in,
    // and without this an unattended host runs that before anyone can join.
    //
    // The pre-round half of the same condition (`!g_PreRoundDelayTimer`) is
    // the PreRound tick phase (WAC frozen with the entities). The epilog half
    // is epilog_screen_active() below.
    // [orig: Server_TickUpdate @0x51d7e0, the gate @0x51d8bd — `if
    //  (!g_PreRoundDelayTimer && (g_WacVarHumans || !g_WacVarTicks) &&
    //  !g_EpilogScreenActive)` around the WacScript_AdvanceTick call @0x51d8bf +
    //  Server_UpdatePlayerBreathTimers + EventTrigger_UpdateQuarterRoundRobin
    //  @0x454d50]
    bool script_may_advance() const {
        return (cached.humans > 0 || cached.wac_ticks == 0) && !epilog_screen_active();
    }
    // Game_ProcessMainFrame's gate over the whole entity update. On the
    // authority an empty world (no human, a started WAC clock) holds still,
    // unless a playing host's own player is on the death screen; the local
    // client's copy of the pre-round byte holds every peer (the authority's own
    // pre-round timer is no part of it); in a session the ended round holds
    // every peer still (the round-over latch the round end raises). A joiner's
    // PreRound tick phase is its copy of the byte (run_entity_pass).
    // [orig: Game_ProcessMainFrame @0x526703..0x526742 -- `cmp is_authority`
    //  @0x526703, the humans/ticks tests @0x52671C..0x52672A, `cmp
    //  g_ClientPreRoundTimer` @0x52672C, `cmp is_in_session` @0x526734, `cmp
    //  g_RoundOver` @0x52673C; the latch writer Server_ProcessRoundEnd
    //  @0x5168E4]
    bool entity_update_admitted(bool is_authority) const {
        if (is_authority && !cached.peer_death_screen && cached.humans == 0 &&
                cached.wac_ticks != 0)
            return false;
        if (cached.client_preround_timer != 0) return false;
        return !(rules.mp_session && match.outcome().ended);
    }
    // The SP end screen is up: the round end's SP tail started the cine, and
    // its stage machine built the MISSION FAILED screen (the lose cine's
    // first dispatch after a rendered cine frame) or the win's score screen
    // (95 timeline frames in). From then on the WAC tick, the BMS quarter pass
    // and most of the entity update hold, so a `Lose` line reaches the chat
    // exactly once (world/epilog_cine.h carries the stage machines).
    // [orig: g_EpilogScreenActive @0xA87054 — the writers @0x57450C (lose) /
    //  @0x5764EC (win); the gate read @0x51D8B7]
    bool epilog_screen_active() const { return epilog.screen_active; }
    // Authoritative whole-second pre-round phase. Networking and the frame
    // clock remain live while World gameplay systems are frozen; phase-0 0x0A
    // projects its low byte to each client. Joiners retain the same field from
    // that wire projection, giving host and client one phase predicate.
    // [orig: g_PreRoundDelayTimer @0xC8D824; seed @0x516C8D;
    // decrement @0x51DC20..0x51DC33; writer @0x4FF82D]
    uint32_t preround_delay_seconds = 0;

    // Three more Server_TickUpdate globals, kept beside the pre-round timer
    // because the mission owns their reset exactly as it owns that one; the
    // host tick is their only reader/writer.
    // The 744-tick priority-target sweep countdown: zero-armed, so the first
    // authority tick of every mission sweeps entity Flags 0x4000 off pools 0/1
    // and reloads 744. [orig: g_DirtyFlagClearTimer @0xC8D810; zeroed per
    // mission by Nbstat_StartupInit @0x4fde30 <- Game_StartMission @0x526108;
    // Server_TickUpdate @0x51d82b..0x51d840]
    uint32_t priority_target_clear_countdown = 0;
    // The 1 Hz round-robin S2C 0x2F flag-state refresh cursor: the pool-1 flag
    // index the next periodic second re-broadcasts; a walk that sends nothing
    // resets it. [orig: dword_24C10D4, read/written only by Server_RefreshNextFlagState @0x517B20]
    uint32_t flag_refresh_cursor = 0;
    // The team-mode 62-tick countdown behind the per-second S2C 0x46 field-0x0008
    // (downed state) resend to a dead teammate's side. [orig: g_WeaponResendTimer
    // @0xC947A0; Server_TickUpdate @0x51e2db..0x51e307]
    uint32_t team_downed_resend_countdown = 0;

    void add_system(ISystem *sys);
    void load_systems();       // calls on_load for each, then the AI's

    // One frame-clock tick. Gameplay runs every system, then the entity
    // update; PreMission runs only the authored BMS pre-pass; PreRound
    // advances shared clocks but freezes WAC/entities/projectiles. The
    // explicit phase replaces the old boolean pre-mission seam so no caller
    // can mistake a pre-round freeze for a script initialization pass.
    // run_logic_tick is the script and entity half of the frame; the host's
    // server tick splits it along retail's frame: begin_tick, the script pass
    // (Server_TickUpdate's WAC tick, every-32 legs and BMS quarter pass), its
    // own maintenance and 0x0A, then the entity pass (Game_ProcessMainFrame's
    // gated entity update and the tail that advances logic_tick). The weapon
    // actions follow later in the frame (pump_weapon_actions).
    // [orig: Game_ProcessMainFrame @0x5263f0]
    void run_logic_tick(bool is_authority = true,
                        TickPhase phase = TickPhase::Gameplay);
    // The frame's pending-sound pass, ahead of the script and entity halves:
    // the fire-sound countdown, then the incoming-lock tone drain.
    // [orig: Sound_TickPendingSlots @0x529310 (the slot walk, then the
    //  g_LockOnMeToneTicks drain @0x5293a5..0x5293af), from Game_ProcessMainFrame
    //  @0x526697]
    void tick_pending_sound_slots();
    TickContext begin_tick(bool is_authority, TickPhase phase);
    void run_script_pass(const TickContext &ctx);
    void run_entity_pass(const TickContext &ctx);
    // The frame's one weapon-action walk, after the weather tick and the
    // camera compose: every pool-0 row in slot order (the local player's slot
    // through the installed LocalPlayer's pump, a UseGun gunner's borrowed
    // parent slot through the AI pump), then each unoccupied EWEAP pool-1 row
    // whose slot is still hot. Every host and the bare local role run it once
    // per frame after their weather and view legs, whatever the phase; a
    // joiner walks its replica slots instead. It reads the frame's own tick,
    // one behind logic_tick once the entity pass's tail has run.
    // [orig: Game_ProcessMainFrame @0x5263F0 -- the Environment_UpdateWeatherTick
    //  call @0x526774, the Camera_ComputeThirdPersonView call @0x526781, then the
    //  WeaponAction_ProcessAllEntities call @0x526786;
    //  WeaponAction_ProcessAllEntities @0x542690..0x542724]
    void pump_weapon_actions();

    // One gameplay tick's entity update, in the retail phase order: the
    // pool-1 walk (every live row once, in slot order, its ground-entity
    // chain first), the attachment poses, HeliLift, the faces, the
    // precipitation fall, the death pieces, the AI timed events, the weather
    // particles, the projectiles, the explosion queue (then the damage
    // reactions they stamped), the pool-2 cohort walk, the doors, the pool-3
    // cohort walk, the proximity tables, then the pool-0 walk in slot order.
    // On the SP epilog screen only the pool-1 rows a player drives are
    // visited (every pool-1 row's pose is saved), HeliLift through the pool-3
    // walk is skipped, and the update is not counted.
    // [orig: Entity_UpdateAllEntities @0x4C2100]
    void update_all_entities(const TickContext &ctx);
    // One pool-1 visit: mark the row visited; while its +0x2AC clock is
    // expired, refresh its blink state and run its class think (the brain
    // machine, the minefield or item damage callback; a placed device carries
    // its own legs); then its +0x1C4 motor legs; then the clock decrement.
    // [orig: Entity_UpdatePool1Slot @0x4B8DD0]
    void update_pool1_slot(Entity &row, const TickContext &ctx);

    // End the round: the double-run latch, the winning team, and the SP presentation
    // tail surfaced as the "round_end" host effect. Callers are the witnessed
    // producers — the WAC win/lose handlers, the BMS Blue/Red/GreenWin actions, and
    // the server win-condition check. [orig: Server_ProcessRoundEnd @0x5164f0]
    void process_round_end(int32_t winning_team);
    // A script call's records go out at the call while the host session's
    // script pass runs (IScriptPassSends); a no-op otherwise.
    void send_script_pass_records() {
        if (script_pass_sends != nullptr) script_pass_sends->send_raised(*this);
    }

    // The round-over RESTART key's world half (hud::hud_round_over_key took
    // it): the end screen comes down and the mission exits for the SP
    // restart. The splash re-run before it and the queued input event 12 are
    // the embedder's. [orig: Input_HandleSpecialKeys — g_EpilogScreenActive = 0
    //  @0x49c879, g_MissionExitReason = 4 @0x49c8ad]
    void round_over_restart();
    // The round-over ESC: the mission exits to the Post Menu.
    // [orig: Input_HandleSpecialKeys @0x49c8e2 — g_MissionExitReason = 1]
    void round_over_exit();
    // The in-game menu's RESTART command: out of a session the mission exits
    // for the SP restart; in a session nothing happens. The in-game screens'
    // close, the unpause and the action-12 dispatch it runs first are the
    // embedder's. Returns whether it took.
    // [orig: UI_IngameRestartCommand @0x555410 — the is_in_session gate
    //  @0x555417, Game_CloseInGameScreens @0x555419, Input_HandleActionBinding(12)
    //  @0x555428, dword_A87050 &= ~1 @0x555430, g_MissionExitReason = 4
    //  @0x555437; registered on INGAME/RESTART by UI_RegisterIngameCallbacks
    //  @0x555529]
    bool ingame_restart_command();

    // An objective shown or hidden by BMS actions 35/36, or relayed by S2C 0x3F
    // on a joiner: an active notice posts the two chat lines (the "objective"
    // presentation effect: a = slot, b = win, c = the header text id) on a
    // client or outside a session, and the authority relays it to the
    // joiners. [orig: HUD_ShowObjectiveNotification @0x5ba2e0]
    void show_objective_notification(int32_t slot, int32_t is_win, int32_t is_active,
                                     uint8_t flag);
    // The relay half of a mission-text chat line (the SubGoalWon/SubGoalLost
    // announcements): the authority in a session sends the joiners the key
    // and team (S2C 0x3F kind 1) and each resolves the key in its own
    // mission text. The local line is the caller's presentation effect.
    // [orig: GameMsg_AddChatLineAndRelay @0x5ba170 — the relay gate
    //  @0x5ba19f..0x5ba1af]
    void relay_mission_text_chat(int32_t team, const std::string &key);

    // Group population counts for the trigger records over pools 2, 0, 1.
    // Initial: once per mission start, right after the pre pass -- EVERY used
    // row tallied by its group id with no dead/health test, group 0 forced to
    // zero, then live copied from it for every group [orig:
    // EntityPool_RecountByType @ 0x40e7e0, sole call Game_StartMission
    // @ 0x525b8b]. Live: a full rescan counting only rows that are not dead
    // (Flags & 2) and hold health > 0, run by the server tick's periodic
    // second and after group reassignment [orig: EntityPool_RecountLiveByGroup
    // @ 0x40e8d0; timer @ 0x51db6d, reload 0x3E @ 0x51db93, call @ 0x51dc02].
    void recount_group_initials();
    void recount_group_live();

    // Editor "play" support: snapshot/restore of mutable world state so a
    // simulate/stop cycle doesn't dirty the authored mission. Value copies of the
    // registry + script vars + named WAC values + env + clock + match + stable
    // local-player ownership; per-tick health/proximity/human-count caches reset
    // and systems re-init on restore.
    struct Snapshot {
        EntityRegistry registry;
        MissionDiagnostics diagnostics;
        ScriptVarStore vars;
        WacNamedValues wac_values;
        ScriptSquadEvents squad_events;
        ScriptWeaponInput weapon_input;
        ScriptVoiceChannel::State voice;
        // Play-start particle descriptors have not been presented when the
        // baseline seals. Replay them after the presenter's scene reset.
        std::vector<ScriptEffectEvent> initial_script_effects;
        std::vector<ScriptSoundEvent> initial_script_sounds;
        std::vector<SoundSlotEvent> initial_slot_sounds;
        uint64_t next_script_effect_order = 0;
        int32_t forced_animation = 0;
        ReverbState reverb;
    EnvState env;
        WeatherState weather;
        DoorSystem doors;
        FacialSystem facials;
        TeammateOperations teammates;
        int32_t vehicle_ai_spawn_phase = 0;
        uint32_t bms_spawn_phase_counter = 0;
        Match match;
        // The SP score block and the subgoal masks at play start: a restart
        // lands on the post-census, post-memset block and the post-PreMission
        // masks retail's re-run of Game_StartMission rebuilds (the masks
        // zeroed by EventSystem_FreeAll, then the PreMission pass).
        // [orig: Game_RestartRoundSP @0x5263a0 -> Game_DestroyAllEntitiesAndReset
        //  -> Mission_ResetBmsState -> EventSystem_FreeAll @0x453356..0x453368,
        //  then Game_StartMission @0x5263db]
        MissionKillStats kill_stats;
        // The idle play-start cine and no exit (restore keeps the
        // process-lifetime EpilogCine::frame_drawn).
        EpilogCine epilog;
        int32_t mission_exit_reason = 0;
        std::string mission_exit_map;
        SubgoalState subgoals;
        SpawnWaveList spawn_waves;
        ZoneCaptureState zone_capture_state;
        uint32_t spawn_cycle_counter = 0;
        uint32_t logic_tick = 0;
        uint32_t preround_delay_seconds = 0;
        uint32_t prng16_state = kMissionPrng16Seed;
        uint32_t prng16_b_state = kMissionPrng16BSeed;
		uint32_t prng16_c_state = 0;
		bool cease_fire = false;
		uint32_t crt_rand_state = 1;
        EntityHandle local_player;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot &s);

private:
    std::vector<ISystem *> systems_;
};

// The mission-start unit scan feeding MissionKillStats's census (the total
// and its per-class split) — the Show Score panel's and the win epilog's
// enemy-units max. Runs once at the Game_StartMission-equivalent moment, after
// entity placement and the item-traits sweep stamped Entity::item_unit_type,
// over pools 0 and 1 only.
// [orig: Score_CountMissionSubgoalsAndUnits (ex sub_509DC0) @0x509dc0 -> Score_ClassifyEntityForCounts @0x4fd070,
//  the pool walks @0x509e13..0x509e4a, called at Game_StartMission @0x525d5d]
void count_mission_units(World &world);

// One row's census leg: a non-player row with team >= 2 and a non-zero unit-class byte adds
// one enemy unit to MissionKillStats's total and its class. The mission start's walk runs it
// over pools 0 and 1, and an org1 respawn on the authority runs it on the respawned row, so
// every respawn of an enemy grows the census.
// [orig: Score_ClassifyEntityForCounts @0x4fd070, called by
//  Score_CountMissionSubgoalsAndUnits @0x509e1d / @0x509e4a and Entity_UpdateInfantryAI
//  @0x4ba055]
void classify_entity_for_counts(World &world, const Entity &e);

// The mission's defined-subgoal count: the leading run of authored win
// conditions (slots 1..8) before the first 0 or 0xFF entry.
// [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning byte_A7628C — the header
//  win-condition text ids; dword_C8468C]
int32_t count_defined_subgoals(const World &world);

// The authority's GetTickCount: the bound process clock (World::tick_count_ms), else the
// deterministic seam over the logic tick a clockless test runs on (D-NET-1040).
inline uint32_t authority_tick_count_ms(const World &world) {
    return world.tick_count_ms ? world.tick_count_ms()
                               : io::host_milliseconds_for_logic_tick(world.logic_tick);
}

// The same on a path that carries the host's own tick (the session dispatch): the bound
// process clock, else the seam over `host_tick` (D-NET-1041).
inline uint32_t authority_tick_count_ms(const World *world, uint32_t host_tick) {
    return world != nullptr && world->tick_count_ms ? world->tick_count_ms()
                                                    : io::host_milliseconds_for_logic_tick(host_tick);
}

} // namespace opennova::world
