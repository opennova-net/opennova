#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/system.h>

namespace opennova::world {

class World;
struct RoundDeath;

// score.ini FIELD records use these one-based IDs and retain their byte-sized
// visibility flag in file order. They are both the match's board schema and the
// exact {field, enabled} pairs serialized in S2C 0x56.
// [orig: ScoreConfig_LoadFile @0x52D8A0; ScoreConfig_LoadScoringTableForGameType
// @0x52D300; Server_BuildEndOfRoundScoreboard @0x508F30]
struct MatchScoreField {
    uint8_t field = 0;
    uint8_t enabled = 0;
};

// The score.ini values advertised in S2C 0x58 are not presentation metadata:
// they are the 39 signed event deltas at scorer slots 74..112. Keeping that
// relationship in the rules object makes the wire configuration and authority
// accounting one value source. [orig: GameEvent_ProcessScoring @0x52F550;
// Server_BuildStatusReport @0x530A60 copies row+300..+452]
struct MatchRules {
    uint32_t game_type = 0;
    uint32_t game_time_minutes =
        0;                    // SET GameTime / g_RespawnTime, despite the old host-field name
    uint32_t score_limit = 0; // SET KillLimit / g_ScoreLimit
    uint32_t hill_limit_minutes = 0; // cfg koth_limit / g_TimeLimitMinutes
    uint32_t hill_delta = 5;         // cfg koth_delta / dword_24D2148
    uint32_t max_score = 0;          // SET MaxScore / g_KillLimit
    uint32_t flag_return_ticks = 210;
    int32_t capture_duration_seconds = 15;  // SET TakeoverTime / g_CaptureDuration
    int32_t capture_speed_setting = 1;       // cfg takeover speed / g_CaptureSpeedSetting
    // Absent means the exact GameType_CreateDefaultSettings row. Present is a
    // fully materialized score.ini overlay and may intentionally contain zero
    // in every slot; absence is therefore not encoded as a magic all-zero row.
    std::optional<std::array<int32_t, 39>> score_values;
    // Empty selects retail's per-game-type defaults. A loaded score.ini section
    // replaces them in file order.
    std::vector<MatchScoreField> score_fields;
    uint8_t team_count = 2;
};

struct MatchLiveTeamScore {
    int32_t primary_score = 0;
    int32_t points = 0;
    uint8_t alive_players = 0;
    uint8_t authored_objectives = 0;
};

// One authoritative projection for S2C 0x16. The match owns every mode
// decision; the wire layer only narrows these values to their wire words.
// Row zero is the retail neutral row and remains zero-filled.
// [orig: Server_BuildAndBroadcastScoreboard @0x50D960]
struct MatchLiveScoreboard {
    bool team_mode = false;
    bool timed_score_mode = false;
    uint8_t team_count = 0;
    std::array<MatchLiveTeamScore, 5> teams{};
};

// The shared retail defaults consumed by Match and the S2C 0x58 session
// report. Keeping the recovered table in one domain source prevents gameplay
// points and advertised points from drifting.
// Indexed by the slot of the shipped VAR name table (retail reads
// `scoringTable[74 + slot]`) [orig: off_830348 @ 0x830348: 38 {name, slot}
// pairs, slots 0..37].
// [orig: GameType_CreateDefaultSettings @0x52DD00]
std::array<int32_t, 39> default_match_score_values(uint32_t game_type);
std::vector<MatchScoreField> default_match_score_fields(uint32_t game_type);

// The direct CPlayerStats dword layout. Index 0 is the associated entity; the
// counters therefore retain their retail indices rather than being repacked
// into a second schema. Only witnessed gameplay fields are named here; the full
// 42-word record remains available for scoreboard serialization.
// [orig: CPlayerStats_RecordEvent @0x52C8E0 — event N in 1..27 increments
// field[N+1] (@0x52c900..0x52caf5), 28 adds to field[29], 29..32 add to
// field[30..33]; CPlayerStats_GetFieldPlusOne (ex CRenderState_GetFieldByIndex)
// @0x52D7D0 returns field[index+1]; ScoreRules_GetPrimaryScoreField (ex
// sub_52C850) @0x52C850]
// Field 11 is the FLAGSAVE counter: GameEvent_ProcessScoring @0x52F550 case 8
// (@0x52f8d1: ++slot dword 29 = field 11 @0x52f8d7, points += scoringTable[83]
// = score.ini VAR slot 9 "FLAGSAVE" in the shipped name table off_830348),
// dispatched by Server_BroadcastEntityDeathEvent @0x517A90 (push 8 @0x517b03)
// from the flag-return interaction in Entity_ProcessWaypointInteraction
// @0x4AD820 (@0x4ad8dc / @0x4ad921). The 0x56 row's sixth i16 reads it as
// CPlayerStats_GetFieldPlusOne(stats, 0xA) @0x50918a. Retail's "ASSISTS" is
// score.ini slot 27 (scoringTable[101]) and has no counter in this record —
// the former kAssists alias of index 11 was the decoder's column-name guess.
struct MatchStats {
    static constexpr size_t kFieldCount = 42;
    // RecordEvent 1: one per accepted round a Player fires (FIELD id 9, and
    // the shots-per-kill words). [orig: GameEvent_ProcessScoring case 1
    // @0x52FB2A; CPlayerStats_RecordEvent case 1 @0x52C8FB]
    static constexpr size_t kShotsFired = 2;
    static constexpr size_t kTeamKills = 4;
    static constexpr size_t kEnemyKills = 5;
    static constexpr size_t kSuicides = 6;
    static constexpr size_t kDeaths = 7;
    // RecordEvent 7: a medic's heal (MEDICHEAL); no FIELD id reads it.
    // [orig: GameEvent_ProcessScoring case 5 @0x52FD3A]
    static constexpr size_t kMedicHeals = 8;
    // RecordEvent 8: a medic's revive (MEDICSAVE, FIELD id 10).
    // [orig: GameEvent_ProcessScoring case 6 @0x52FCD8]
    static constexpr size_t kMedicSaves = 9;
    static constexpr size_t kFlagSaves = 11;
    static constexpr size_t kFlagCaptures = 12;
    static constexpr size_t kFlagPickups = 13;
    static constexpr size_t kTargetsDestroyed = 14;
    // RecordEvent 15: an unnumbered zone captured (PSPTAKEOVER, FIELD id 13).
    // [orig: GameEvent_ProcessScoring case 14 @0x52FDFE]
    static constexpr size_t kPspTakeovers = 16;
    // Kill-cause counters: entity+0x2C bit 0x100 (same-round multi-kill),
    // 0x800 (headshot), 0x400 (knife). [orig: GameEvent_ProcessScoring
    // @0x530076..0x530178 RecordEvent 16/17/18]
    static constexpr size_t kMultipleKills = 17;
    static constexpr size_t kHeadshotKills = 18;
    static constexpr size_t kKnifeKills = 19;
    // RecordEvent 20: an enemy Player killed while carrying a flag
    // (FLAGCARRIERKILL, FIELD id 14). [orig: GameEvent_ProcessScoring
    // @0x530246..0x530277 / @0x5302FE..0x530352]
    static constexpr size_t kFlagCarrierKills = 21;
    static constexpr size_t kVictimNearNeutralObjectiveKills = 22;
    static constexpr size_t kAttackerNearNeutralObjectiveKills = 23;
    static constexpr size_t kVictimNearAttackerObjectiveKills = 24;
    static constexpr size_t kAttackerNearOwnObjectiveKills = 25;
    static constexpr size_t kVictimNearOwnObjectiveKills = 26;
    static constexpr size_t kAttackerNearVictimObjectiveKills = 27;
    static constexpr size_t kSharedPointAwards = 28;
    static constexpr size_t kPoints = 29;
    // RecordEvent 29: the sum of the victims' ItemDef `score` words (scorer
    // event 12, FIELD id 20, the 0x56 row's third word); no points.
    // [orig: GameEvent_ProcessScoring case 12 @0x52FEC2..0x52FF0F;
    // CPlayerStats_RecordEvent case 29 @0x52CBB6]
    static constexpr size_t kUnitScore = 30;
    static constexpr size_t kHillTime = 31;
    static constexpr size_t kHostileZoneTime = 32;
    static constexpr size_t kFriendlyZoneTime = 33;
    static constexpr size_t kRoundMarker = 35;
    static constexpr size_t kZoneTakeovers = 39;
    static constexpr size_t kPeriodicScoreUnits = 40;

    std::array<int32_t, kFieldCount> fields{};

    int32_t &operator[](size_t index) { return fields[index]; }
    int32_t operator[](size_t index) const { return fields[index]; }
};

// Stable roster identity only. Mutable team/class remain on the authoritative
// Entity and are read at each score event, then frozen into MatchResultPlayer.
// This mirrors retail's player-slot stats + live entity reads without a second
// synchronization path. [orig: GameEvent_ProcessScoring @0x52F550;
// Server_BuildEndOfRoundScoreboard @0x508F30]
struct MatchPlayerIdentity {
    EntityHandle entity;
    uint8_t slot = 0;
    std::string name;
    // The 0x56 row's third string: the NovaWorld clan-list node tag (node+81),
    // keyed by the slot's account netId (+100572) and filled only by a join
    // carrying a nonzero account id at join+0x1A4 (name join+0x1A8, tag
    // join+0x1E8). A LAN join carries none, so the host leaves this empty; the
    // NovaWorld-account join leg is a separate slice. (The row's SECOND string
    // is the literal empty string on every retail host and has no model field.)
    // [orig: Server_BuildEndOfRoundScoreboard @0x5090D3..@0x509116;
    // Server_PlayerAdd clan-list leg @0x51CDF0..@0x51CE2E]
    std::string tag;
};

struct MatchPlayer {
    MatchPlayerIdentity identity;
    MatchStats stats;
    // Player-slot +23595, maintained by the retail capture-proximity pass and
    // supplied as ScoreRules_GetPrimaryScoreField's external score for KOTH/TKOTH.
    int32_t objective_ticks = 0;
    // Player-slot +89868: bit 0 is the neutral-objective/hill bit; bits 1..4
    // identify team-owned objectives. The kill scorer consumes this byte
    // verbatim. [orig: Server_UpdateCaptureZoneProximity @0x5086A0;
    // GameEvent_ProcessScoring @0x52F550]
    uint8_t objective_proximity_mask = 0;
    // Player-slot +23593/+23594. They chase zero while outside a relevant
    // capture source, reset at the score-table capture interval and ten
    // service passes respectively, and remain untouched when the mission has
    // no capture source at all. [orig: @0x50890A..0x508A05]
    int32_t capture_score_ticks = 0;
    int32_t capture_period_ticks = 0;
    // Player-slot +103 (+0x19c): independent live-player service counter for
    // score event 25. It advances for every periodic pass, whether or not the
    // mission contains a capture source. [orig: @0x5087C9..0x5087F1]
    int32_t periodic_score_ticks = 0;
    // Player-slot +0x184: the server ticks this slot spent in state 6 with its
    // entity present and not hidden (Flags bit 0), unsaturated and zero at
    // player-add; WAC onptick reads it in whole seconds.
    // [orig: Server_TickUpdate `add [esi+184h],1` @0x51D977;
    //  WacCmd_OnPlayerTick @0x4F0E65]
    uint32_t play_ticks = 0;
    // WAC pisvar/psetvar address player-slot bytes +392..+408. A new
    // player-add clears them, a team change clears the last one (the dword
    // at +408), death clears none.
    // [orig: WacCmd_PlayerIsVar @0x4F0BD0; WacCmd_PlayerSetVar @0x4F0CB0;
    // Server_PlayerAdd @0x51D51C; Server_ChangeEntityTeam @0x518DE5]
    std::array<uint8_t, 17> script_vars{};
    // Player-slot +100567, the live spectator latch. The scorer refuses
    // every event for a spectator-flagged slot, so the round winner awards
    // skip the row; the proximity pass skips the slot after its mask clear
    // and the TKOTH holder census never counts it. The authority mirrors its
    // connection latch here.
    // [orig: GameEvent_ProcessScoring @0x52F6E5/@0x52F6FA;
    //  Server_UpdateCaptureZoneProximity @0x508795;
    //  Game_CountAlivePlayersPerTeam @0x500214]
    bool spectator = false;
    // Player-slot +89912 bit 0x10, the undeployed (respawn-pending) bit: set
    // at join when the mission offers deploy zones, cleared by the deploy
    // leg. Event 25's counter skips a pending slot. The authority mirrors its
    // connection bit here.
    // [orig: Server_OnPlayerJoin @0x51A6F2; Server_ProcessPlayerDeath
    //  @0x517791; Server_UpdateCaptureZoneProximity @0x5087A2]
    bool respawn_pending = false;
};

// The match requests a disconnect without owning the transport. The authority
// consumes it after the script pass, before its ordinary player-state fan.
// [orig: WacCmd_PlayerPunt @0x4F0DA0; WacCmd_PlayerKillPunt @0x4F0D30]
struct MatchPlayerPunt {
    EntityHandle entity;
    uint64_t spawn_id = 0;
    uint8_t reason = 33;
};

// One outcome latch for every producer: automatic multiplayer rules and the
// WAC/BMS Co-op/SP actions all converge here. Team 0 is a draw/no-team outcome.
// [orig: g_SpawnSuccessGate @0x24c1928 (latched by Server_ProcessRoundEnd
// @0x5164F0 at @0x5168e4, cleared by Game_StartMission @0x524a1f),
// g_RoundWinningTeam @0x24c1924, the scoreboard winner @0x24c1970 (= S2C 0x1D
// payload byte 0; memset 0 at mission start, so it stays 0 until the round ends)]
struct MatchOutcome {
    bool ended = false;
    int32_t winner_team = 0;
};

// Immutable semantic source for the end-round protocol. The network boundary
// chooses byte widths; the match owns when the values freeze and the retail
// player order. [orig: Server_ProcessRoundEnd @0x5164F0 builds the board once
// via Server_BuildEndOfRoundScoreboard @0x508F30]
struct MatchResultPlayer {
    MatchPlayerIdentity identity;
    // Board-time snapshot of entity+354 / entity+660.
    uint8_t team = 0;
    uint8_t player_class = 0;
    MatchStats stats;
    int32_t objective_ticks = 0;
    int32_t primary_score = 0;
};

struct MatchResult {
    bool ready = false;
    uint32_t game_type = 0;
    int32_t winner_team = 0;
    std::array<int32_t, 2> team_scores{}; // teams 1 and 2
    // Team games: team_scores[0] == team_scores[1]. Non-team games: every
    // board row's primary equals the largest sort key, except a lone row
    // with a positive score. [orig: Server_BuildEndOfRoundScoreboard
    // @0x5092AD (team); @0x50909D/@0x50920E/@0x50926A (non-team)]
    bool draw = false;
    std::vector<MatchScoreField> score_fields;
    std::vector<MatchResultPlayer> players;
    std::array<MatchStats, 5> team_stats{};
    // TeamRecord+0x150 (unknown_040[272]) per row: the hill hold timer
    // Game_AccumulateTeamScores drives. The board builder passes it as
    // ScoreRules_GetPrimaryScoreField's third argument for the team scores
    // and reads it directly for matrix column id 5.
    // [orig: Server_BuildEndOfRoundScoreboard @0x508FA7/@0x5092E7;
    // Game_AccumulateTeamScores @0x508DAD/@0x508DC2]
    std::array<int32_t, 5> team_hold_ticks{};
    // 0 for non-team types, 3 for team types, 5 for four-team TDM and for
    // Team KOTH / FlagBall at any team count.
    // [orig: Server_BuildEndOfRoundScoreboard @0x509259..0x50929C]
    uint8_t team_row_count = 0;
};

enum class MatchGameplayEventKind : uint8_t {
    FlagPickup,
    FlagDrop,
    FlagSave,
    FlagCapture,
    FlagReturn,
};

// A semantic objective transition plus the exact entity-state snapshot the
// wire layer needs after the transition. Captured CTF flags can be gone
// from the registry by the time the binding drains this record.
struct MatchGameplayEvent {
    MatchGameplayEventKind kind = MatchGameplayEventKind::FlagPickup;
    EntityHandle actor;
    EntityHandle objective;
    Vec3 position;           // event/feed position before capture/reset
    Vec3 objective_position; // post-transition S2C 0x2F position
    uint8_t objective_flags = 0;
    EntityHandle parent;
    EntityHandle ground;
    bool remove_objective = false;
    // Immutable ItemDef id for event families whose wire code is selected by
    // the objective type after the domain mutation (flag timeout 35/36/37).
    uint16_t objective_item_id = 0;
};

// Resolve one retail scoreboard FIELD ID against the direct CPlayerStats
// layout. This deliberately remains a match-domain operation: the wire layer
// only narrows the frozen value to its historical signed 16-bit width.
// [orig: CPlayerStats_GetFieldByIndex @0x52D630]
int32_t match_score_field_value(const MatchStats &stats, uint8_t field, uint32_t game_type);

// Authoritative match state. Entity simulation stays in World; this record owns
// the retail score events, clock, and game-type decisions that consume it.
class Match {
  public:
    void configure(const MatchRules &rules);

    const MatchRules &rules() const { return rules_; }
    int32_t remaining_ticks() const { return remaining_ticks_; }
    // The admin console's in-place writes, mid-round and with no reconfigure: SET KillLimit,
    // MaxScore, KOTHLimit, ChangeTeamDelay and GameTime store the rule globals the round reads,
    // and GameTime restarts the round clock at `3720 * value` (a zero ends a timed round at its
    // next check). [orig: CAdminServer_HandleSetCommand @0x405A60 -- g_ScoreLimit, g_KillLimit,
    // g_TimeLimitMinutes, g_CaptureDuration, g_RespawnTime and g_RoundTimeRemaining]
    MatchRules &live_rules() { return rules_; }
    void set_remaining_ticks(int32_t ticks) { remaining_ticks_ = ticks; }
    const MatchOutcome &outcome() const { return outcome_; }
    const MatchResult &result() const { return result_; }

    void upsert_player(const MatchPlayerIdentity &identity);
    // Drops the leaver's carried objective where the body stood, then forgets
    // the roster row. Retail clears the carried link when the lingering body is
    // finally destroyed by the sweep; the host retires the entity at teardown
    // (D-NET-176 trigger relocation), so the drop lands here.
    // [orig: Entity_Destroy @0x43E8B1..0x43E8B8: a def+0x5C == 3 body calls
    // Entity_DropCarriedObject @0x439DF0 before its fields are wiped]
    void remove_player(World &world, EntityHandle entity);
    // Entity destruction drops a carried objective without removing the
    // player's score/roster record. [orig: Entity_DropCarriedObject @0x439DF0]
    void drop_carried_object(World &world, EntityHandle player);
    // Re-sync a flag to its authored pose without a feed event or scoring: a
    // flag already AT that pose is left alone (no send); one displaced under
    // 2 u keeps its position but re-publishes its 0x2F state; one displaced
    // further snaps home, drops its ground link, and publishes. Returns true
    // only for the snap. The 1 Hz carry-limit break is its caller here.
    // [orig: Entity_SyncPositionFromDefinition @0x43A9B0 — equality return
    //  @0x43a9f8, near path @0x43aa3b..0x43aa7c, snap + ground raycast +
    //  send @0x43aa7d..0x43ab40]
    bool sync_flag_to_authored_pose(World &world, EntityHandle flag);
    // Per-flag class callback (+0x2AC), independent of the server's 1 Hz clock.
    void tick_flag_event(World &world, Entity &flag);
    const MatchPlayer *player(EntityHandle entity) const;
    MatchPlayer *player(EntityHandle entity);
    const std::vector<MatchPlayer> &players() const { return players_; }
    // Mirrors the player-slot spectator latch (+100567) onto the roster row.
    // [orig: Server_PlayerAdd @0x51CD83; Server_KillPlayerAndNotify @0x519E61]
    void set_player_spectator(EntityHandle entity, bool spectator);
    // Mirrors the player-slot undeployed bit (+89912 & 0x10) onto the row.
    // [orig: Server_OnPlayerJoin @0x51A6F2; Server_ProcessPlayerDeath @0x517791]
    void set_player_respawn_pending(EntityHandle entity, bool pending);
    const MatchStats &team_stats(uint8_t team) const;

    // Script player operations validate the registered slot, not the Player
    // entity flag or health. AddExp alone also requires a resolved ItemDef.
    // [orig: Entity_ValidatePtr @0x500910; WacCmd_AddExp @0x4F2690]
    bool add_experience(const World &world, EntityHandle entity, int32_t amount);
    bool request_player_punt(const World &world, EntityHandle entity, bool kill_punt);
    std::vector<MatchPlayerPunt> drain_player_punts();

    // Authored objective totals used both by win evaluation and the pre-match
    // status report. The first read freezes the round census, as retail's
    // Server_ResetRoundCounters does before play.
    int32_t flag_capture_target(const World &world, uint8_t scoring_team);
    int32_t demolition_target(const World &world, uint8_t scoring_team);

    // A Player victim takes GameEvent_PlayerDeath's two scorer calls (its own
    // death, then the killer-victim call); a non-Player person takes
    // Entity_CheckAndProcessDeath's single killer-victim call. `cause_flags`
    // is the victim's entity+0x2C cause word as the death edge reads it.
    // [orig: GameEvent_PlayerDeath @0x516F06 / @0x516FB0;
    // Entity_CheckAndProcessDeath @0x51B5B3]
    void record_death(World &world, EntityHandle victim,
                      EntityHandle killer = EntityHandle{},
                      uint32_t cause_flags = 0);

    // The kill accounting a damage-pass lethal edge runs (RoundDeath::
    // kill_event): with a killer and a victim whose ItemDef `score` word is
    // nonzero, scorer event 12 in every session, then outside a network
    // session the single-player tallies world.kill_stats.
    // [orig: Score_ProcessKillEvent @0x4FD400]
    void process_kill_event(World &world, const RoundDeath &death);
    // Scorer event 12: the victim's `score` word adds to the killer's field 30
    // (and its team row in team modes).
    // [orig: Score_ProcessKillEvent @0x4FD400 (the event-12 call @0x4FD438)]
    void record_kill_event(const World &world, EntityHandle killer,
                           EntityHandle victim);
    // Scorer event 1: an accepted non-alt round fired by a Player.
    // [orig: Server_ClientFiredRound @0x50BAA0 (the event-1 call @0x50C727)]
    void record_shot(const World &world, EntityHandle shooter);
    // Scorer event 6: a medic revived a downed teammate.
    // [orig: GameEvent_RevivePlayer @0x517CD0 (the event-6 call @0x517DC5)]
    void record_revive(const World &world, EntityHandle medic);
    // Scorer event 5: a medic healed a hurt teammate.
    // [orig: GameEvent_HealPlayer @0x50DE30 (the event-5 call @0x50DEA4)]
    void record_heal(const World &world, EntityHandle medic, EntityHandle patient);

    // Objective scorer cases 9 and 11. The ordinary runtime paths call these
    // from carry contact and death routing; they remain public for script/WAC
    // producers that author the same retail events.
    void record_flag_capture(World &world, EntityHandle player,
                             EntityHandle flag = EntityHandle{});
    void record_target_destroyed(const World &world, EntityHandle target,
                                 EntityHandle attacker);

    // Capture event 24 (LFPTAKEOVER): a numbered zone's flip supplies every
    // living same-team Player in radius.
    // [orig: CaptureZone_CheckProximityScoring @0x500C50, the event-24 call
    // @0x500D84]
    void record_zone_capture(const World &world,
                             const std::vector<EntityHandle> &scorers);
    // Capture event 14 (PSPTAKEOVER): an unnumbered zone's flip or timed
    // completion scores its capturer.
    // [orig: CaptureZone_CheckProximityScoring @0x500C50, the event-14 call
    // @0x500DC5]
    void record_psp_takeover(const World &world, EntityHandle capturer);

    // Called once per authoritative 62.5 Hz logic tick after the pre-round gate.
    // Advance the server-owned match services for one frame. The periodic
    // proximity/score pass remains live during PreRound, as it does after the
    // retail countdown block; carried-objective motion and round time do not.
    void advance_tick(World &world,
                      TickPhase phase = TickPhase::Gameplay);

    // The movement callbacks the frame's entity update left in the collision
    // stream (a player touching a flag or a bay): retail runs them inline in
    // the movement resolver, so the entity update's tail consumes them, ahead
    // of the next server tick. [orig: Entity_ProcessWaypointInteraction
    // @0x4AD820, its sole caller @0x4B2FF5 in the movement resolver]
    void process_movement_contacts(World &world);

    // True for the frame on which the shared one-second service fired. The
    // host's Server_TickUpdate consumes this same countdown for its own 1 Hz
    // legs (StartDelay, win conditions, waves, the capture transaction), so
    // the world and the wire can never sit a frame apart.
    // [orig: g_PeriodicSecondTimer @0xC8D83C; reload 62 @0x51DB93]
    bool periodic_second() const { return periodic_second_fired_; }

    std::vector<MatchGameplayEvent> drain_gameplay_events();

    // Returns no value while play continues; value 0 is an actual draw decision.
    // The all-zones-owned check (A&S and C&C only) precedes the game-type
    // switch exactly as retail.
    std::optional<int32_t> winner_if_finished(const World &world);

    // Shared double-run latch used by World::process_round_end.
    bool finish(int32_t winner_team, const World &world);
    // A client's round-over latch: S2C 0x1D raises the gate the host's round
    // end raises (the entity update and the target filters read it), with no
    // scoring pass and no board; the next mission start's fresh Match clears it.
    // [orig: NapiNPClientMsg_0x01D @0x430840 -- `mov g_SpawnSuccessGate,1`
    //  @0x430858 under !is_authority; cleared by Game_StartMission @0x524A1F]
    void latch_round_over() { outcome_.ended = true; }

    int32_t primary_score(const MatchStats &stats, int32_t objective_ticks = 0) const;
    int32_t primary_score(const MatchPlayer &player) const;
    // The team row's primary: ScoreRules_GetPrimaryScoreField over the team
    // stats with the team's hill hold timer (TeamRecord+0x150) as the KOTH
    // family's external score, on both the live and the end-round board.
    // [orig: Server_BuildAndBroadcastScoreboard @0x50DB92..@0x50DCCE;
    // Server_BuildEndOfRoundScoreboard @0x508FA7/@0x508FB8]
    int32_t team_primary_score(uint8_t team) const;
    // The team row's hill hold timer, whole seconds (TeamRecord+0x150).
    // [orig: Game_AccumulateTeamScores @0x508DAD/@0x508DC2; read by
    // Server_DrawStatusScreen @0x50ae3c / @0x50aeaa]
    int32_t team_hold_ticks(uint8_t team) const {
        return team < team_hold_ticks_.size() ? team_hold_ticks_[team] : 0;
    }
    MatchLiveScoreboard live_scoreboard(World &world);

  private:
    struct CarryObjectiveState {
        EntityHandle objective;
        uint64_t spawn_id = 0;
        Vec3 home;
        int32_t return_ticks = 0;
        int32_t previous_x_q16 = 0;
        int32_t previous_y_q16 = 0;
        // The authored heading as its whole-degree placement yaw: the
        // definition pose's yaw word is its spawn angle, spawn_angle_bam(90 -
        // yaw) (aiRuntime f0_7[7]).
        int16_t home_yaw = 0;
    };

    void share_experience(const World &world, MatchPlayer &recipient, int32_t amount);
    int32_t score_value(size_t status_index) const;
    // RecordEvent(counter) then the RecordEvent 28 points award, which shares
    // with the +0x170 links unless `share` is false (scorer event 25 alone).
    void add_event(const World &world, MatchPlayer &player, size_t counter,
                   int32_t points, int32_t raw_delta = 1, bool share = true);
    // A bare RecordEvent 28 award (no counter), on a Player and its team row.
    void add_points(const World &world, MatchPlayer &player, int32_t points);
    void add_team_points(uint8_t team, int32_t points);
    // The scorer's team leg: TeamRecords[team] exists only in team modes.
    void add_team_event(uint8_t team, size_t counter, int32_t points,
                        int32_t raw_delta = 1);
    // A direct TeamRecords row award, ungated by the team bit (the flag
    // capture's hard-routed rows).
    void add_team_record_event(uint8_t team, size_t counter, int32_t points,
                               int32_t raw_delta = 1);
    // Scorer event 3 without a victim (a Player's own death) and with one.
    void score_death(World &world, EntityHandle victim);
    void score_kill(World &world, EntityHandle killer, EntityHandle victim,
                    uint32_t cause_flags, bool victim_carried_flag);
    void ensure_objective_census(const World &world);
    CarryObjectiveState *carry_state(World &world, EntityHandle objective);
    void record_flag_pickup(World &world, EntityHandle player, EntityHandle flag);
    void record_flag_save(World &world, EntityHandle player, EntityHandle flag);
    void return_flag_home(World &world, EntityHandle flag, MatchGameplayEventKind kind,
                          EntityHandle actor = EntityHandle{});
    void update_objective_proximity(const World &world);
    void accumulate_team_scores(const World &world);
    void update_flag_objectives(World &world);
    int32_t team_objective_ticks(const World &world, uint8_t team) const;

    MatchRules rules_;
    int32_t remaining_ticks_ = -1;
    MatchOutcome outcome_;
    MatchResult result_;
    std::vector<MatchPlayer> players_;
    std::vector<MatchPlayerPunt> player_punts_;
    std::array<MatchStats, 5> teams_{};
    std::array<int32_t, 5> team_hold_ticks_{};
    int32_t periodic_second_timer_ = 0;
    bool periodic_second_fired_ = false;
    bool objective_census_ready_ = false;
    std::array<int32_t, 5> flag_capture_targets_{};
    std::array<int32_t, 5> demolition_targets_{};
    std::vector<CarryObjectiveState> carry_objectives_;
    std::vector<MatchGameplayEvent> gameplay_events_;
};


// The carrier's words a drop reads: its position, its +0x10/+0x14 heading
// and pitch, its Flags (the indoors bit), its blink quad, and its identity as
// the drop sound's source (none for a replica-only carrier).
struct DropCarrierPose {
    Vec3 position;
    int32_t heading_bam = 0;
    int32_t pitch_bam = 0;
    uint32_t flags = 0;
    uint32_t blink_hits[4] = {};
    int32_t bms_id = 0;
    uint16_t handle = 0xFFFF;
};

// A native carrier's drop words: a person's live heading and pitch from its
// body record, any other carrier's from its live euler.
DropCarrierPose drop_carrier_pose(const World &world, const Entity &carrier);

// The dropped object's own legs of a drop, over its carrier's words: the
// carried and indoors bits, the motion, the installed fall, the pose off the
// carrier (Z + 0x4000, a quarter turn) and the blink quad with its proximity
// refresh. The host's Match::drop_carried_object runs it, and so does a
// joiner whose 0x2F state takes the flag off its carrier.
// [orig: Entity_DropCarriedObject @0x439df0; its joiner callers
//  NapiNPClientMsg_0x02F @0x43105c / @0x4310dc]
void drop_object_from_carrier(World &world, Entity &object, const DropCarrierPose &carrier);

// One pool-1 visit of a dropped object's installed callback (Entity::
// drop_motion): the fall until the clamped ground stops it, then the ride on
// the entity it landed on, which falls again when that entity dies.
// [orig: Entity_UpdatePositionAndTransform @0x4adef0;
//  Entity_UpdateParentTransform @0x4a88b0; Entity_InterpolateFromParentDelta
//  @0x4a8d60]
void update_dropped_object(World &world, Entity &object);

// The sim-side end-of-round state plus the SP kill-stat buckets the epilog
// score screen and the WAC bluekills/greenkills builtins read, as one value
// the embedder fills (World::match.outcome(), World::kill_stats,
// World::cached.humans, World::rules.mp_session); its Godot record wraps it
// by value (ADR 0043 d10).
struct RoundOutcomeView {
    bool ended = false;
    int32_t winner_team = 0;
    int32_t bluekills = 0;
    int32_t greenkills = 0;
    int32_t enemy_kills = 0;
    int32_t team_kills_by_others = 0;
    int32_t friendly_kills_by_others = 0;
    int32_t enemy_kills_by_others = 0;
    int32_t humans = 0;
    bool mp_session = false;
};

} // namespace opennova::world
