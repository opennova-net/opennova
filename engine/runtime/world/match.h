#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "world/entity.h"

namespace opennova::world {

class World;

// score.ini FIELD records use these one-based IDs and retain their byte-sized
// visibility flag in file order. They are both the match's board schema and the
// exact {field, enabled} pairs serialized in S2C 0x56.
// [orig: ScoreConfig_LoadFile @0x52D8A0; load_scoring_table_for_game_type
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
        0;                    // SET GameTime / g_respawn_time, despite the old host-field name
    uint32_t score_limit = 0; // SET KillLimit / g_score_limit
    uint32_t hill_limit_minutes = 0; // cfg koth_limit / g_time_limit_minutes
    uint32_t hill_delta = 5;         // cfg koth_delta / dword_24D2148
    uint32_t max_score = 0;          // SET MaxScore / g_kill_limit
    uint32_t flag_return_ticks = 210;
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
// decision; the network adapter only narrows these values to their wire words.
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
// [orig: GameType_CreateDefaultSettings @0x52DD00]
std::array<int32_t, 39> default_match_score_values(uint32_t game_type);
std::vector<MatchScoreField> default_match_score_fields(uint32_t game_type);

// The direct CPlayerStats dword layout. Index 0 is the associated entity; the
// counters therefore retain their retail indices rather than being repacked
// into a second schema. Only witnessed gameplay fields are named here; the full
// 42-word record remains available for scoreboard serialization.
// [orig: CPlayerStats_RecordEvent @0x52C8E0; CRenderState_GetFieldByIndex
// @0x52D7D0 returns field[index+1]; sub_52C850 @0x52C850]
struct MatchStats {
    static constexpr size_t kFieldCount = 42;
    static constexpr size_t kTeamKills = 4;
    static constexpr size_t kEnemyKills = 5;
    static constexpr size_t kSuicides = 6;
    static constexpr size_t kDeaths = 7;
    static constexpr size_t kAssists = 11;
    static constexpr size_t kFlagSaves = 11;
    static constexpr size_t kFlagCaptures = 12;
    static constexpr size_t kFlagPickups = 13;
    static constexpr size_t kTargetsDestroyed = 14;
    static constexpr size_t kPoints = 29;
    static constexpr size_t kRoundMarker = 35;
    static constexpr size_t kZoneTakeovers = 39;

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
    std::string clan;
    std::string tag;
};

struct MatchPlayer {
    MatchPlayerIdentity identity;
    MatchStats stats;
    // Player-slot +23595, maintained by the retail capture-proximity pass and
    // supplied as sub_52C850's external score for KOTH/TKOTH.
    int32_t objective_ticks = 0;
};

// One outcome latch for every producer: automatic multiplayer rules and the
// WAC/BMS Co-op/SP actions all converge here. Team 0 is a draw/no-team outcome.
// [orig: g_spawn_success_gate / g_round_winning_team, latched by
// Server_ProcessRoundEnd @0x5164F0]
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
    bool draw = false;
    std::vector<MatchScoreField> score_fields;
    std::vector<MatchResultPlayer> players;
    std::array<MatchStats, 5> team_stats{};
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
// network adapter needs after the transition. Captured CTF flags can be gone
// from the registry by the time the adapter drains this record.
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
    const MatchOutcome &outcome() const { return outcome_; }
    const MatchResult &result() const { return result_; }

    void upsert_player(const MatchPlayerIdentity &identity);
    void remove_player(EntityHandle entity);
    const MatchPlayer *player(EntityHandle entity) const;
    MatchPlayer *player(EntityHandle entity);
    const std::vector<MatchPlayer> &players() const { return players_; }
    const MatchStats &team_stats(uint8_t team) const;

    // Authored objective totals used both by win evaluation and the pre-match
    // status report. The first read freezes the round census, as retail's
    // reset_round_counters does before play.
    int32_t flag_capture_target(const World &world, uint8_t scoring_team);
    int32_t demolition_target(const World &world, uint8_t scoring_team);

    // Retail runs the victim death scorer first, then the killer-victim scorer.
    // An invalid killer records only the victim leg.
    void record_death(World &world, EntityHandle victim,
                      EntityHandle killer = EntityHandle{});

    // Objective scorer cases 9 and 11. The ordinary runtime paths call these
    // from carry contact and death routing; they remain public for script/WAC
    // producers that author the same retail events.
    void record_flag_capture(World &world, EntityHandle player,
                             EntityHandle flag = EntityHandle{});
    void record_target_destroyed(const World &world, EntityHandle target,
                                 EntityHandle attacker);

    // Event 24 for a numbered objective is awarded to every living same-team
    // Player inside the zone, supplied by the capture census in stable slot order.
    void record_numbered_zone_capture(const World &world, const std::vector<EntityHandle> &scorers);

    // Called once per authoritative 62.5 Hz logic tick after the pre-round gate.
    void advance_tick(World &world);

    std::vector<MatchGameplayEvent> drain_gameplay_events();

    // Returns no value while play continues; value 0 is an actual draw decision.
    // The all-zones-owned check precedes the game-type switch exactly as retail.
    std::optional<int32_t> winner_if_finished(const World &world);

    // Shared double-run latch used by World::process_round_end.
    bool finish(int32_t winner_team, const World &world);

    int32_t primary_score(const MatchStats &stats, int32_t objective_ticks = 0) const;
    int32_t primary_score(const MatchPlayer &player) const;
    int32_t team_primary_score(const World &world, uint8_t team) const;
    MatchLiveScoreboard live_scoreboard(World &world);

  private:
    struct CarryObjectiveState {
        EntityHandle objective;
        uint64_t spawn_id = 0;
        Vec3 home;
        int32_t return_ticks = 0;
    };

    int32_t score_value(size_t status_index) const;
    void add_event(MatchPlayer &player, size_t counter, int32_t points);
    void add_team_event(uint8_t team, size_t counter, int32_t points);
    void ensure_objective_census(const World &world);
    CarryObjectiveState *carry_state(World &world, EntityHandle objective);
    void record_flag_pickup(World &world, EntityHandle player, EntityHandle flag);
    void record_flag_save(World &world, EntityHandle player, EntityHandle flag);
    void drop_carried_object(World &world, EntityHandle player);
    void return_flag_home(World &world, EntityHandle flag, MatchGameplayEventKind kind,
                          EntityHandle actor = EntityHandle{});
    void update_hill_presence(const World &world);
    void update_flag_objectives(World &world, bool advance_return_timers);
    int32_t team_objective_ticks(const World &world, uint8_t team) const;

    MatchRules rules_;
    int32_t remaining_ticks_ = -1;
    MatchOutcome outcome_;
    MatchResult result_;
    std::vector<MatchPlayer> players_;
    std::array<MatchStats, 5> teams_{};
    std::array<int32_t, 5> team_hold_ticks_{};
    int32_t periodic_second_timer_ = 0;
    bool objective_census_ready_ = false;
    std::array<int32_t, 5> flag_capture_targets_{};
    std::array<int32_t, 5> demolition_targets_{};
    std::vector<CarryObjectiveState> carry_objectives_;
    std::vector<MatchGameplayEvent> gameplay_events_;
};

} // namespace opennova::world
