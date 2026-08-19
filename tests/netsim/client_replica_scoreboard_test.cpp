// The Tab board's folded lanes: S2C 0x16 (the scoreboard) and S2C 0x46 (the
// connection-slot roster it joins names from). This pins the witnessed
// retention shape [orig: NapiNPClientMsg_PlayerList @0x42FAE0;
// NapiNPClientMsg_PlayerSync @0x431370]:
//   * every well-formed 0x16 applies unconditionally — an empty update yields
//     an empty board [orig: the row-count zero @0x42fb46];
//   * rows for roster-unknown slots DROP [orig: @0x42fc05];
//   * name/clan join into the row at APPLY time [orig: @0x42fd4c..0x42fd8f],
//     so a later removal does not blank rows already on the board;
//   * a removal deactivates and wipes the slot [orig: PlayerSlot_ClearAndUnlink
//     @0x434730; the re-bind re-init @0x4346c0];
// plus the row/team field semantics that were mislabelled before the
// 2026-07-28 / 2026-08-06 / 2026-08-19 refutations.
#include <cstdio>
#include <cstdint>
#include <vector>

#include <netsim/client_replica_pipeline.h>
#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::netsim;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

void u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(static_cast<uint8_t>(v & 0xFF));
	b.push_back(static_cast<uint8_t>(v >> 8));
}

// [u8 flags][u8 rowCount] rows{slot,status,score1,score2,flags} [u8 teamCount]
// (teamCount+1) teams{score1,score2,kothHold,ctfFlag} [u8 inGame][u8 spectators]
std::vector<uint8_t> make_list_with_teams(uint8_t flags,
		const std::vector<std::vector<int>> &rows,
		const std::vector<std::vector<int>> &teams, uint8_t in_game,
		uint8_t spectators) {
	std::vector<uint8_t> b;
	b.push_back(flags);
	b.push_back(static_cast<uint8_t>(rows.size()));
	for (const auto &r : rows) {
		b.push_back(static_cast<uint8_t>(r[0]));
		u16(b, static_cast<uint16_t>(r[1]));
		u16(b, static_cast<uint16_t>(r[2]));
		u16(b, static_cast<uint16_t>(r[3]));
		b.push_back(static_cast<uint8_t>(r[4]));
	}
	b.push_back(static_cast<uint8_t>(teams.size() - 1)); // team_count (rows = count + 1)
	for (const auto &t : teams) {
		u16(b, static_cast<uint16_t>(t[0]));
		u16(b, static_cast<uint16_t>(t[1]));
		b.push_back(static_cast<uint8_t>(t[2]));
		b.push_back(static_cast<uint8_t>(t[3]));
	}
	b.push_back(in_game);
	b.push_back(spectators);
	return b;
}

std::vector<uint8_t> make_list(uint8_t flags,
		const std::vector<std::vector<int>> &rows, uint8_t in_game,
		uint8_t spectators) {
	return make_list_with_teams(flags, rows,
			{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, in_game, spectators);
}

// [u8 slot][u16 bitmask][u8 entity_slot] then the present fields in bit order.
std::vector<uint8_t> make_sync(uint8_t slot, uint8_t entity_slot,
		uint16_t bits, const char *name = nullptr, const char *clan = nullptr,
		int team = -1, int quality = -1) {
	std::vector<uint8_t> b;
	b.push_back(slot);
	u16(b, bits);
	b.push_back(entity_slot);
	if (bits & kPlayerSyncHasName) {
		for (const char *p = name; *p != '\0'; ++p) b.push_back(static_cast<uint8_t>(*p));
		b.push_back(0);
	}
	if (bits & kPlayerSyncHasTeamString) {
		for (const char *p = clan; *p != '\0'; ++p) b.push_back(static_cast<uint8_t>(*p));
		b.push_back(0);
	}
	if (bits & kPlayerSyncHasTeamByte) b.push_back(static_cast<uint8_t>(team));
	if (bits & kPlayerSyncHasQuality) b.push_back(static_cast<uint8_t>(quality));
	return b;
}

std::vector<uint8_t> make_sync_name(uint8_t slot, const char *name,
		uint8_t entity_slot = 0xFF) {
	return make_sync(slot, entity_slot, kPlayerSyncHasName, name);
}

std::vector<uint8_t> make_sync_removal(uint8_t slot) {
	std::vector<uint8_t> b;
	b.push_back(slot);
	u16(b, 0x8000);   // removal — no body follows
	return b;
}

// The row fields carry the REFUTED-name semantics: the second u16 is a status
// bitfield (not a ping) and the fourth is accumulated points (not deaths).
// Name/clan join into the row from the roster at apply time.
void test_row_fields_and_flags() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC,
			make_sync(7, 0xFF, kPlayerSyncHasName | kPlayerSyncHasTeamString,
					"SPAGHETTI", "TAG"));
	// slot 7, status 0x0401, score1 12, score2 900, flags 0x04 => team 2, not spectator
	view.apply(s2c::PLAYER_LIST, make_list(0x03, {{7, 0x0401, 12, 900, 0x04}}, 3, 1));
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.known);
	CHECK(sb.team_mode);            // flags bit0
	CHECK(sb.timed);                // flags bit1
	CHECK(sb.in_game_count == 3);
	CHECK(sb.spectator_count == 1);
	CHECK(sb.rows.size() == 1);
	if (sb.rows.size() == 1) {
		CHECK(sb.rows[0].slot_id == 7);
		CHECK(sb.rows[0].status_flags == 0x0401);
		CHECK(sb.rows[0].score1 == 12);
		CHECK(sb.rows[0].score2 == 900);
		CHECK(sb.rows[0].team == 2);      // flags >> 1
		CHECK(!sb.rows[0].spectator);     // flags bit0 clear
		CHECK(sb.rows[0].name == "SPAGHETTI"); // the parse-time join
		CHECK(sb.rows[0].clan == "TAG");
	}
	CHECK(sb.teams.size() == 3);          // team_count + 1 (T0 neutral)
	// The accepted row refreshes the roster's team byte [orig: @0x42fc7c].
	CHECK(view.state().roster[7].team == 2);
}

// A spectator row sets bit0 and reports team 0 from the remaining bits.
void test_spectator_bit() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(2, "Belsman"));
	view.apply(s2c::PLAYER_LIST, make_list(0x00, {{2, 0, 0, 0, 0x01}}, 1, 1));
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.rows.size() == 1 && sb.rows[0].spectator);
	CHECK(!sb.team_mode && !sb.timed);
}

// AN EMPTY UPDATE CLEARS THE BOARD. Retail zeroes its row count before the
// row loop and applies the team table and trailer even for a zero-row list
// [orig: @0x42fb46]; the drawer then draws nothing [orig: @0x423c46]. (The
// earlier "an empty update never clobbers a populated board" rule was an
// invention — refuted 2026-08-19.)
void test_empty_update_clears_board() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(1, "A"));
	view.apply(s2c::PLAYER_SYNC, make_sync_name(2, "B"));
	view.apply(s2c::PLAYER_LIST,
			make_list(0x01, {{1, 0, 5, 50, 0x02}, {2, 0, 3, 30, 0x04}}, 2, 0));
	CHECK(view.state().scoreboard.rows.size() == 2);
	const std::uint64_t rev = view.state().scoreboard.revision;
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {}, 0, 0));
	CHECK(view.state().scoreboard.rows.empty());        // cleared, like retail
	CHECK(view.state().scoreboard.revision == rev + 1); // a real update
	CHECK(view.state().scoreboard.in_game_count == 0);  // trailer applied too
}

// A row whose connection slot has no roster binding is DROPPED (retail skips
// inactive slots and queues the C2S 0x22 retry [orig: @0x42fc05..0x42fc3a]).
void test_unknown_slot_rows_drop() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(1, "KNOWN"));
	view.apply(s2c::PLAYER_LIST,
			make_list(0x01, {{1, 0, 5, 50, 0x02}, {9, 0, 3, 30, 0x04}}, 2, 0));
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.rows.size() == 1);
	if (sb.rows.size() == 1) CHECK(sb.rows[0].slot_id == 1);
	CHECK(sb.rows_dropped_unknown_slot == 1);
}

// Rows keep the names copied into them at apply time: a roster removal does
// not blank the board, and the SLOT stops producing rows only on the next
// 0x16 (retail's records hold their strings; the removal deactivates the
// slot so later rows drop) [orig: @0x42fd4c records vs @0x434730 unlink].
void test_row_names_survive_removal() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(5, "SPAGHETTI"));
	view.apply(s2c::PLAYER_LIST, make_list(0x00, {{5, 0, 1, 0, 0x02}}, 1, 0));
	view.apply(s2c::PLAYER_SYNC, make_sync_removal(5));
	CHECK(!view.state().roster[5].bound);              // slot deactivated
	CHECK(view.state().roster[5].name.empty());        // and wiped
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.rows.size() == 1);
	if (sb.rows.size() == 1)
		CHECK(sb.rows[0].name == "SPAGHETTI");         // the board still names it
	// The next update carries the same slot: now unknown -> dropped.
	view.apply(s2c::PLAYER_LIST, make_list(0x00, {{5, 0, 1, 0, 0x02}}, 1, 0));
	CHECK(view.state().scoreboard.rows.empty());
	CHECK(view.state().scoreboard.rows_dropped_unknown_slot == 1);
}

// A re-bind after a removal starts from a fresh slot: nothing from the old
// binding leaks (retail re-inits every field, clan and quality included
// [orig: PlayerSlotTable_GetOrInitSlot @0x4346c0]).
void test_removal_resets_slot() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC,
			make_sync(3, 4,
					kPlayerSyncHasName | kPlayerSyncHasTeamString | kPlayerSyncHasQuality,
					"OLD", "CLAN", -1, 3));
	CHECK(view.state().roster[3].quality == 3);
	CHECK(view.state().roster[3].entity_slot == 4);
	view.apply(s2c::PLAYER_SYNC, make_sync_removal(3));
	CHECK(!view.state().roster[3].bound);
	CHECK(view.state().roster[3].entity_slot == -1);
	view.apply(s2c::PLAYER_SYNC,
			make_sync(3, 8, kPlayerSyncHasQuality, nullptr, nullptr, -1, 9));
	CHECK(view.state().roster[3].bound);
	CHECK(view.state().roster[3].name.empty());        // no leak from "OLD"
	CHECK(view.state().roster[3].clan.empty());        // no leak from "CLAN"
	CHECK(view.state().roster[3].quality == 4);        // clamp 4 [orig: @0x43170d]
	CHECK(view.state().roster[3].entity_slot == 8);    // restamped unconditionally
}

// Per-FIELD last-write-wins: a sync that omits a bit leaves that field alone
// rather than clearing it. The entity binding is NOT bit-gated — every
// non-removal sync restamps it [orig: @0x431477/@0x431480].
void test_roster_fields_are_last_write_wins_per_bit() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(3, "Belsman", 6));
	CHECK(view.state().roster[3].entity_slot == 6);
	view.apply(s2c::PLAYER_SYNC,
			make_sync(3, 0xFF, kPlayerSyncHasQuality, nullptr, nullptr, -1, 2));
	CHECK(view.state().roster[3].name == "Belsman");
	CHECK(view.state().roster[3].quality == 2);
	CHECK(view.state().roster[3].entity_slot == -1);   // restamped to none
}

// The team-table u8 pair is kothHold + ctfFlag (the old player/alive-count
// names were decode-era guesses) [orig: @0x50dc62/@0x50dd30].
void test_team_table_fields() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_LIST, make_list_with_teams(0x01, {},
			{{0, 0, 0, 0}, {10, 200, 7, 1}, {8, 150, 0, 2}}, 0, 0));
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.teams.size() == 3);
	if (sb.teams.size() == 3) {
		CHECK(sb.teams[1].score1 == 10);
		CHECK(sb.teams[1].score2 == 200);
		CHECK(sb.teams[1].koth_hold == 7);
		CHECK(sb.teams[1].ctf_flag == 1);
		CHECK(sb.teams[2].ctf_flag == 2);
	}
}

// Accepted rows write the row team through the slot's entity binding, like
// retail's entity+354 store [orig: @0x42fc88]; the 0x46 team byte does the
// same [orig: @0x4315fc].
void test_row_team_refreshes_entity() {
	ClientReplicaPipeline view;
	ClientEntityState entity;
	entity.handle = 5; // pool 0, slot 5
	view.state().entities.push_back(entity);
	view.apply(s2c::PLAYER_SYNC, make_sync_name(3, "DRIVER", 5));
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {{3, 0, 0, 0, 0x04}}, 1, 0));
	CHECK(view.state().entities[0].team == 2);
	CHECK(view.state().entities[0].team_known);
	// Entity rows are ClientState.revision-covered presenter state: the write
	// bumps on change (the apply_team_assign form) and holds on a same-team
	// re-apply, so row-plan invalidation stays edge-triggered.
	const std::uint64_t rev_after_list = view.state().revision;
	CHECK(rev_after_list > 0);
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {{3, 0, 0, 0, 0x04}}, 1, 0));
	CHECK(view.state().revision == rev_after_list);
	view.apply(s2c::PLAYER_SYNC,
			make_sync(3, 5, kPlayerSyncHasTeamByte, nullptr, nullptr, 1));
	CHECK(view.state().entities[0].team == 1);
	CHECK(view.state().revision == rev_after_list + 1);
}

// A malformed body is counted, not folded.
void test_malformed_body_is_rejected() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_LIST, {0x00});   // truncated: no row count
	CHECK(!view.state().scoreboard.known);
}

} // namespace

int main() {
	test_row_fields_and_flags();
	test_spectator_bit();
	test_empty_update_clears_board();
	test_unknown_slot_rows_drop();
	test_row_names_survive_removal();
	test_removal_resets_slot();
	test_roster_fields_are_last_write_wins_per_bit();
	test_team_table_fields();
	test_row_team_refreshes_entity();
	test_malformed_body_is_rejected();
	if (failures == 0) std::printf("client_replica_scoreboard_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
