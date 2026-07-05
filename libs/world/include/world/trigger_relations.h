// Sticky trigger-relation state backing the BMS Group/Single condition
// categories (cats 1/2): twelve relation bitmatrices, the per-group alert +
// count records, and the waypoint has-visited matrices. Zeroed per mission
// load; written by the AI/combat/damage paths; read by the trigger evaluator.
// Full witness: docs/mission/bms-event-runtime-re.md §3a (2026-07-05 grill).
//
// [orig: matrices @ 0xAC50E8..0xAC86F8 + 0xAD86F8, zeroed by
//  EventSystem_FreeAll @ 0x453210; group records 48 B x 64 @ 0xA33FA4;
//  single key = the authored SSN (DcbId, entity +0x7C), group key =
//  commandGroup (entity +0x11C)]
//
// Retail's setters bound-check rows (< 0x80) while the trigger-side tests
// are UNGUARDED reads of whatever memory follows; reproducing an OOB read
// would be manufacturing garbage (ADR 0003 class), so every read here is
// sanitized to false out of range — recorded in the RE record §3a.
#ifndef OPENNOVA_WORLD_TRIGGER_RELATIONS_H
#define OPENNOVA_WORLD_TRIGGER_RELATIONS_H

#include <cstdint>
#include <cstring>

namespace opennova::world {

class TriggerRelations {
public:
    static constexpr int kGroups = 64;   // commandGroup rows
    static constexpr int kSingles = 128; // authored-SSN rows (guard < 0x80)
    static constexpr int kWaypointLists = 128;
    static constexpr int kWaypointBits = 32;

    // Alert values [orig: 0=green 1=yellow 2=red; setters
    // TriggerGroup_SetAlertGreen/Yellow/Red @ 0x40d5f0/0x40d610/0x40d630].
    enum Alert : int32_t { kAlertGreen = 0, kAlertYellow = 1, kAlertRed = 2 };

    enum Relation : int { kSees = 0, kTargeted = 1, kShot = 2 };

    struct GroupState {
        int32_t alert = kAlertGreen;  // [orig: 0xA33FA4 + 48*g]
        int32_t initial_count = 0;    // [orig: 0xA33FA8] set once at mission start
        int32_t live_count = 0;       // [orig: 0xA33FAC] 62-tick rescan
    };

    // [orig: EventSystem_FreeAll @ 0x453210] — one memset per mission load.
    void clear() { std::memset(this, 0, sizeof(*this)); }

    GroupState &group(int g) { return groups_[g & (kGroups - 1)]; }
    const GroupState *group_or_null(int g) const {
        return (g >= 0 && g < kGroups) ? &groups_[g] : nullptr;
    }

    // --- relation matrices ---------------------------------------------------
    // Setters mirror the retail write guards; group rows use the low 6 bits of
    // the commandGroup the way the 64-entry table indexes, single rows are the
    // raw authored SSN with the < 0x80 guard [orig: e.g. TeamMatrix_SetEnemy
    // @ 0x452bf0, EntityMatrix_SetProximityBit @ 0x452b60].

    void set_group_group(Relation r, int a, int b) {
        if (!group_ok(a) || !group_ok(b)) return;
        gg_[r][a][b >> 5] |= bit(b);
    }
    void set_single_group(Relation r, int ssn, int g) {
        if (!single_ok(ssn) || !group_ok(g)) return;
        sg_[r][ssn][g >> 5] |= bit(g);
    }
    // The G->S table is stored transposed: row = the SEEN/TARGETED/SHOT single
    // [orig: index [2b + (a >> 5)] @ the GS bases].
    void set_group_single(Relation r, int g, int ssn) {
        if (!group_ok(g) || !single_ok(ssn)) return;
        gs_[r][ssn][g >> 5] |= bit(g);
    }
    void set_single_single(Relation r, int a, int b) {
        if (!single_ok(a) || !single_ok(b)) return;
        ss_[r][a][b >> 5] |= bit(b);
    }

    bool group_group(Relation r, int a, int b) const {
        return group_ok(a) && group_ok(b) && (gg_[r][a][b >> 5] & bit(b)) != 0;
    }
    bool single_group(Relation r, int ssn, int g) const {
        return single_ok(ssn) && group_ok(g) && (sg_[r][ssn][g >> 5] & bit(g)) != 0;
    }
    bool group_single(Relation r, int g, int ssn) const {
        return group_ok(g) && single_ok(ssn) && (gs_[r][ssn][g >> 5] & bit(g)) != 0;
    }
    bool single_single(Relation r, int a, int b) const {
        return single_ok(a) && single_ok(b) && (ss_[r][a][b >> 5] & bit(b)) != 0;
    }

    // --- waypoint has-visited ------------------------------------------------
    // Cell = dword[row][waypointList], bit = waypointNumber (< 32). Set when an
    // AI advances past a waypoint [orig: AI_UpdateWaypointMovement @ 0x457c77/
    // 0x457c88 — SetBitB(group,...) + SetBitA(ssn,...)].
    void mark_waypoint_visited(int ssn, int group_id, int list, int number) {
        if (list < 0 || list >= kWaypointLists) return;
        if (number < 0 || number >= kWaypointBits) return;
        if (single_ok(ssn)) visited_single_[ssn][list] |= 1u << number;
        if (group_ok(group_id)) visited_group_[group_id][list] |= 1u << number;
    }
    bool single_visited(int ssn, int list, int number) const {
        return single_ok(ssn) && list >= 0 && list < kWaypointLists &&
               number >= 0 && number < kWaypointBits &&
               (visited_single_[ssn][list] & (1u << number)) != 0;
    }
    bool group_visited(int g, int list, int number) const {
        return group_ok(g) && list >= 0 && list < kWaypointLists &&
               number >= 0 && number < kWaypointBits &&
               (visited_group_[g][list] & (1u << number)) != 0;
    }
    // Actions 32/33 clear ONE row — and only waypoint lists 0..31 of it, the
    // witnessed 0x80-byte memset quirk, replicated
    // [orig: EventTrigger_ClearSlotA/B @ 0x453600/@ 0x4535e0].
    void clear_single_visited_row(int ssn) {
        if (single_ok(ssn)) std::memset(visited_single_[ssn], 0, 0x80);
    }
    void clear_group_visited_row(int g) {
        if (group_ok(g)) std::memset(visited_group_[g], 0, 0x80);
    }

private:
    static bool group_ok(int g) { return g >= 0 && g < kGroups; }
    static bool single_ok(int s) { return s >= 0 && s < kSingles; }
    static uint32_t bit(int i) { return 1u << (i & 31); }

    GroupState groups_[kGroups]{};
    uint32_t gg_[3][kGroups][2]{};
    uint32_t sg_[3][kSingles][2]{};
    uint32_t gs_[3][kSingles][2]{}; // transposed: row = the single
    uint32_t ss_[3][kSingles][4]{};
    uint32_t visited_single_[kSingles][kWaypointLists]{};
    uint32_t visited_group_[kGroups][kWaypointLists]{};
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_TRIGGER_RELATIONS_H
