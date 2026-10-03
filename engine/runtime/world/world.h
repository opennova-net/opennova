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
#include <runtime/world/mission_diagnostics.h>
#include <string>
#include <vector>
#include <variant>

#include <runtime/audio/sound_profile.h>
#include <base/io/crt_rand.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
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
#include <runtime/world/match.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/system.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/world/rotor_wash.h>
#include <runtime/world/zone_system.h>
#include <runtime/world/trigger_relations.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/water_cross.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/sound_emitter_mailbox.h>
#include <runtime/world/weather_state.h>
#include <runtime/world/reverb.h>
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
namespace opennova::dbf { struct File; }
namespace opennova::rtxt { struct File; }

namespace opennova::world {

class CollisionWorld;
class LocalPlayer;

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
    // sim-side. [orig: g_EnvWaterHeightFixed @ 0x26C6454]
    int32_t water_z = 0;
    int32_t fog_dist = 0;      // legacy whole-metre mirror; WeatherState owns the Q16 target
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

// Transient player identity and frame data. WAC refreshes local_health only
// at bytecode entry [orig: WacScript_CacheLocalPlayerState @0x4F5780].
struct CachedFrameState {
    uint8_t sound_listener_view_flags = 6; // startup; camera 0 -> 2, other -> 4 [orig: @0x43924A]
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
    int32_t random_result = 0; // RND @0xC6B23C, written by random and named-variable stores
};

// The epilog/debrief exit timeout: both end screens (WIN score epilog and the
// LOSE debrief) force g_MissionExitReason = 1 after 18600 ticks (~297.6 s at
// the 62.5 Hz tick) when the player never presses ESC.
// [orig: Cine_EpilogStateMachineUpdate @0x576240 — the tick compares
//  @0x57621d/@0x5744ea]
inline constexpr int32_t kEpilogExitTimeoutTicks = 18600;

// The epilog/debrief screens fade in over the cine fade pair: two 48-tick fade
// events back to back (96 ticks, ~1.536 s at the 62.5 Hz tick). The shell
// drives its screen alpha from this, not from a wall-clock stand-in.
// [orig: the 48+48-tick cine fade pair @0x574512]
inline constexpr int32_t kEpilogFadeInTicks = 48 + 48;

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

// The mission-dialog registry the two PLYRDIALOG trigger subs read: the
// registered-history list (every Dialog_Register appends the dialog; the
// count saturates at 255, so slot 255 is overwritten and never scanned) and
// the 16-slot active table (inserted at register, removed when the dialog's
// last line finishes, both cleared by Dialog_ResetAll). Keyed by the dialog
// index the "dlg%.3d" name encodes. The producer is the dialog playback
// owner (the shell's mission audio): register on a resolved play, finished
// when that dialog's last line ends, reset at round init.
// [orig: Dialog_PlayByIndex @0x527ae0 -> Dialog_PlayByName @0x44d9f0 ->
//  Dialog_Register @0x44d980 (history @0x44d98d..0x44d9a3, active table
//  @0x44d9b1..0x44d9de); Dialog_UpdatePlayback @0x44e470 -> Dialog_FreeByName
//  @0x44db40 (slot clear @0x44dc07..0x44dc18); Dialog_ResetAll @0x44dc90;
//  readers Dialog_ExistsByIndex @0x44e170 and sub_44E220 @0x44e220]
struct ScriptDialogRegistry {
    static constexpr int kHistoryCapacity = 256; // [orig: dword_A89600]
    static constexpr int kActiveCapacity = 16;   // [orig: dword_A8A248, 16-byte slots]

    // Dialog_Register: the history append (count saturates at 255), then the
    // first free active slot; a full active table skips the insert.
    void register_started(int32_t index) {
        history[history_count] = index;                                    // @0x44d98d
        history_count = history_count == 255 ? 255 : history_count + 1;    // @0x44d99c/@0x44d9a3
        if (active_count < kActiveCapacity) active[active_count++] = index; // @0x44d9b1..0x44d9de
    }
    // Dialog_FreeByName: drop the first active entry naming the dialog and
    // compact the table [orig: @0x44dc07..0x44dc18 -> sub_44DAF0].
    void finished(int32_t index) {
        for (int i = 0; i < active_count; ++i) {
            if (active[i] != index) continue;
            for (int j = i + 1; j < active_count; ++j) active[j - 1] = active[j];
            --active_count;
            return;
        }
    }
    // Dialog_ResetAll @0x44dc90: both tables.
    void reset() {
        history_count = 0;
        active_count = 0;
    }
    // Dialog_ExistsByIndex @0x44e170: the active-table scan.
    bool active_exists(int32_t index) const {
        for (int i = 0; i < active_count; ++i)
            if (active[i] == index) return true;
        return false;
    }
    // sub_44E220's first scan @0x44e253..0x44e28e over [0, history_count).
    bool registered(int32_t index) const {
        for (int i = 0; i < history_count; ++i)
            if (history[i] == index) return true;
        return false;
    }

    std::array<int32_t, kHistoryCapacity> history{};
    int history_count = 0; // [orig: dword_A895F8]
    std::array<int32_t, kActiveCapacity> active{};
    int active_count = 0;  // [orig: dword_A8A244]
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
    ScriptDialogRegistry dialog;
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
    // Active teammate heli-lift operations [orig: dword_AC4F40, slots @0xAC4F48,
    // incremented by HeliLift_SpawnPickup @0x45263a]. TeammateOperations owns
    // the slots and publishes this trigger-visible count (0 = none active).
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
    // The Player row's items.def ordinal (entity+0x1C ItemTypeIndex); 0 until the
    // sweep resolves it [orig: Entity_SpawnFromBMSRecord @0x40EBFC].
    int32_t item_type_index = 0;
    int32_t item_hp = 0;
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
};

// The per-mission tables the embedder feeds once (the def files, the terrain
// samplers, the Player template) and the mission header facts.
struct MissionTables {
    rtxt::File voice_macros; // vmacros.bin, section macrotext [orig: @0x5B7170]
    // The weapon.def armory table (empty until the host feeds it — Simulation::
    // load_weapon_table). Read by the 0x2F/0x5A loadout service, the extended-uplink
    // equipped-weapon gate, and the player-spawn WPN_M4AUTO default. (D-NET-141/143)
    WeaponTable weapons;
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
    // And the mission's co-named dialog bank and its mission text table, which
    // a dialog line's clip and chat lines resolve against (null when the
    // mission has none). [orig: DialogSystem_Init @0x5275E0 ->
    // DialogManager_LoadFromFile @0x44E650 (the .dbf); g_TextMission, read by
    // Dialog_LoadAudioClip @0x44DDCD / Dialog_LoadAudioClipLocalized @0x44E054]
    const dbf::File *dialog_bank = nullptr;
    const rtxt::File *mission_text = nullptr;
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
    PlayerTemplate player;
    // The mission header's attribute flags, stamped by the host at mission load
    // (bms::AttribFlags as a raw dword; 0x40 = SinglePlayerRespawn). Read by the
    // SP auto-lose win-condition leg and by the infantry death scream's night
    // gate (0x100000 EnableNVG -> slot 8 SSNightDead @ 0x4b9ca3).
    // [orig: g_BmsAttribFlags @0xa76258]
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
    bool ignore_weapon_ammo_cost = false; // dword_24C1930 bit 0x100
    bool cease_fire = false; // g_InCeaseFire @ 0x24C196C
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
    // The host's allowSniperScopeZoom option as the SESSION sees it (byte_A821F0):
    // the class-6 sniper lock on a Primary def's scope zoom (its floor becomes
    // scope_max_mag) reads it at the zoom step and the mount clamp. Retail zeroes
    // it outside a session, stamps the serving host's config value on the
    // authority and, on a joiner, bit 16 of the S2C 0x08 flags dword (the bit
    // server_initial_state.cpp emits from the option); offline it stays 0, which
    // locks snipers at max. Hosts stamp it from GameConfig::allow_sniper_scope_zoom,
    // joiners from their session-config bitflags.
    // [orig: Game_ApplySessionSettingsToGlobals @0x552284 (the reset) / @0x5522b9
    //  (the authority stamp); NapiNPClientMsg_HandleSessionConfig @0x428392;
    //  readers Player_AdjustWeaponElevation @0x4dbe36, Player_MountWeaponSlot
    //  @0x4dfaf9, WeaponSlot_InitFromDef @0x53ef17]
    bool allow_sniper_scope_zoom = false;
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
    // The local debug/cheat word's 0x800 bit (dword_24C1930): the SM feed's
    // class-0 player leg skips the LOCAL player and the weapon validator
    // rejects every Player target while it is up [orig: `test
    // dword_24C1930,800h` @0x467141, @0x53A46E]. Input action 123 toggles it
    // (@0x4E07A4) and Game_StartMission zeroes it (@0x525B25); no net wire
    // carries it. Defaults clear.
    bool ai_rules_skip_local_player = false;
};

// A HUD relay the authority sends the joiners as S2C 0x3F, in the order the
// sim produced them: kind 0 is an objective notification (slot, is_win,
// is_active, flag), kind 1 a mission-text chat line (team, key). The host
// fan drains these.
// [orig: Server_BroadcastEntityActionPacket @0x5080d0; its producers
//  HUD_ShowObjectiveNotification @0x5ba2e0 (the call @0x5ba3df) and
//  GameMsg_AddChatLineAndRelay @0x5ba170 (the call @0x5ba1c3)]
struct HudRelay {
    uint8_t kind = 0;
    int32_t slot = 0;
    int32_t is_win = 0;
    int32_t is_active = 0;
    uint8_t flag = 0;
    int32_t team = 0;
    std::string key;
};

// What the sim produced this tick for someone else to drain: the wire (entity
// removals, the round ring, water crossings) and the presentation (effects,
// destruction, scars, scorches, the sound queues). Nothing in the sim reads
// an outbox back; the drains clear them.
struct WorldOutbox {
    // Preserve the callback's send order across 0x21 explosions, 0x26 state,
    // and 0x12 removal. The host drains this once, excluding its loopback.
    // [orig: Entity_HandleDeathEvent @ 0x4070F0; Entity_HandleDeathOnAuthority @ 0x407CC0;
    // Server_SendEntityStatePacket @ 0x509D70; Server_RemoveEntityAndNotify @ 0x50A270]
    using EntityNetworkEvent = std::variant<ItemStateEvent, ItemExplosionEvent, EntityRemoveEvent,
            DoorRowEvent>;
    std::vector<EntityNetworkEvent> entity_events;
    // A client's door section requests (C2S 0x1A, doors.cpp carries the
    // witness); the joiner role drains them into its send queue. The authority
    // never fills it.
    std::vector<DoorRowEvent> door_requests;
    // HUD relays pending the host's S2C 0x3F fan.
    std::vector<HudRelay> hud_relays;
    // Powerup ammo grants for REMOTE players' connection pools (world/powerup.h);
    // the host tick drains them.
    std::vector<PowerupGrant> powerup_grants;
    // Fired-round events pending per-recipient S2C 0x0A tag-2 echo (round_ring.h). Fed by
    // the C2S 0x06 dispatch on accepted fire; drained per connection watermark by the
    // replication emit. [orig: g_RoundRing @0xC8D848 via RoundData_AddRound @0x4fdb40] (D-NET-152)
    RoundRing rounds;
	std::vector<RoundSpawnParams> source_fires; // local source fire awaiting C2S emission
	// Water-surface crossings recorded this tick; the host fan drains them
	// into S2C 0x34 and clears. Presentation only - nothing in the sim reads it.
	WaterCrossQueue water_crossings;
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
    uint64_t next_script_effect_order = 0;
    // The WAC commands the VM replicated this tick (registry flags 0x18) for
    // the server tick to send as S2C 0x23; script_remote_command.h carries the
    // witness. Cleared by the drain; a client-side World never fills it.
    std::vector<ScriptRemoteCommand> script_remote_commands;
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

// The player-slot idle timers (the underwater breath samples) live on the host
// session's player slots, so the host session installs this seam for its tick;
// a world without a server session has no idle timers to run.
// [orig: Server_UpdatePlayerBreathTimers @0x50D770]
class IEntityIdleTimers {
public:
    virtual ~IEntityIdleTimers() = default;
    virtual void update_entity_idle_timers(World &world) = 0;
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
	// PRESENTATION-ONLY stream: every consumer (rotor-wash particles, wreck
	// fire crackle, the occlusion focal-wind sampler) draws behind listener,
	// camera-distance or render gates, so its value diverges per machine and
	// no authoritative system may read it or compare it across peers. It is
	// snapshotted only so an editor play/stop cycle restores the mission-start
	// state. Seeded at every mission start like streams A and B: Game_StartMission
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

    // The explosion queue + AoE damage, the death-piece pool, the per-item death
    // traits, and the destruction presentation events (world/destruction.h;
    // world-wac-ai-re §24). Explosions queued this tick drain inside
    // run_logic_tick right after the round sim [orig: Projectile_ProcessExplosionQueue
    // @0x4ead80 runs once per frame after the projectile update]; the host drains
    // `destruction` (present) and feeds `item_death_traits` (item-traits sweep).
    ExplosionSim explosions;

    // Placed throwable devices + class bindings (world-wac-ai-re §27).
    ThrowableSim throwables;
    MinefieldSystem minefields;
    DoorSystem doors;
    FacialSystem facials;
    TeammateOperations teammates;
    int32_t vehicle_ai_spawn_phase = 0; // [orig: dword_B21F80]
    IItemPieceSpawner *item_piece_spawner = nullptr; // non-owning mission asset factory
    ITeammateSpawner *teammate_spawner = nullptr; // non-owning mission asset factory
    DeathPieceSim death_pieces;
    DestructionRng destruction_rng;
    ItemEmitterSystem item_emitters;

    // The authoritative session rules/stats/outcome + the SP kill-stat buckets.
    // Multiplayer and WAC/BMS outcomes share Match's one double-run latch.
    Match match;
    MissionKillStats kill_stats;











    // The engine tick counter: one logic tick per host frame at 62 Hz.
    // [orig: g_CurrentTick @0x24c1968, ++ once per Game_ProcessMainFrame @0x5263f0.
    //  Per-system cadences divide it: the WAC VM executes every 62nd tick
    //  (WacScript_AdvanceTick @0x4f81b1), the BMS normal-event quarter pass runs every 16th
    //  (Server_TickUpdate @0x51d7e0), the AI motor staggers on 2/8/16 internally.]
    uint32_t logic_tick = 0;
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
    // The tick process_round_end ran on (the SP epilog gate's reference).
    uint32_t round_end_tick = 0;

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
    // unless a playing host's own player is on the death screen; in a session
    // the ended round holds every peer still (the round-over latch the round
    // end raises). The pre-round byte half is the PreRound tick phase.
    // [orig: Game_ProcessMainFrame @0x526703..0x526742 -- `cmp is_authority`
    //  @0x526703, the humans/ticks tests @0x52671C..0x52672A, `cmp
    //  is_in_session` @0x526734, `cmp g_SpawnSuccessGate` @0x52673C;
    //  the latch writer Server_ProcessRoundEnd @0x5168E4]
    bool entity_update_admitted(bool is_authority) const {
        if (is_authority && !cached.peer_death_screen && cached.humans == 0 &&
                cached.wac_ticks != 0)
            return false;
        return !(rules.mp_session && match.outcome().ended);
    }
    // The SP end-of-round screen's gate over the script tick. A single-player
    // round that ends against the player (`Server_ProcessRoundEnd` with any
    // winner but 1) starts the LOSE cine on the spot (`Cine_StartPlayback
    // @0x577840`, the SP tail @0x51691d..0x51698f); the next frame's cine
    // dispatch enters lose state 1 and the frame after builds the MISSION
    // FAILED screen, raising `g_EpilogScreenActive @0xA87054`
    // (`Cinematic_EpilogUpdate @0x577950`, the mode-2 leg @0x5744fd..0x57450c).
    // From then on the WAC and the BMS quarter pass never run again, so a
    // `Lose` line reaches the chat exactly once. The WIN epilog raises the flag
    // only after its flyaway (state 4 @0x5764ec) — that flow is unported
    // (D-AI-10), so a won SP round keeps the script running as before.
    // [orig: g_EpilogScreenActive writers @0x57450c (lose) / @0x5764ec (win);
    //  the gate read @0x51d8b7]
    bool epilog_screen_active() const {
        return !rules.mp_session && match.outcome().ended && match.outcome().winner_team != 1 &&
               logic_tick > round_end_tick;
    }
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
    // resets it. [orig: dword_24C10D4, read/written only by sub_517B20 @0x517B20]
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
    //  dword_B764C0 drain @0x5293a5..0x5293af), from Game_ProcessMainFrame
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
        Match match;
        // The SP score block and the subgoal masks at play start: a restart
        // lands on the post-census, post-memset block and the post-PreMission
        // masks retail's re-run of Game_StartMission rebuilds (the masks
        // zeroed by EventSystem_FreeAll, then the PreMission pass).
        // [orig: Game_RestartRoundSP @0x5263a0 -> Game_DestroyAllEntitiesAndReset
        //  -> Mission_ResetBmsState -> EventSystem_FreeAll @0x453356..0x453368,
        //  then Game_StartMission @0x5263db]
        MissionKillStats kill_stats;
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

// The mission's defined-subgoal count: the leading run of authored win
// conditions (slots 1..8) before the first 0 or 0xFF entry.
// [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning byte_A7628C — the header
//  win-condition text ids; dword_C8468C]
int32_t count_defined_subgoals(const World &world);

} // namespace opennova::world
