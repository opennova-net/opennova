// The Tab board's folded lanes: S2C 0x16 (the scoreboard) and S2C 0x46 (the
// connection-slot roster it joins names from). This pins the two witnessed
// RETENTION rules a naive "latest message wins" fold gets wrong, plus the row
// field semantics that were mislabelled before the 2026-07-28 / 2026-08-06
// refutations [orig: NapiNPClientMsg_PlayerList @0x42FAE0;
// NapiNPClientMsg_PlayerSync @0x431370].
#include <cstdio>
#include <cstdint>
#include <vector>

#include <netsim/client_replica_pipeline.h>
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
// (teamCount+1) teams{score1,score2,players,alive} [u8 inGame][u8 spectators]
std::vector<uint8_t> make_list(uint8_t flags,
		const std::vector<std::vector<int>> &rows, uint8_t in_game,
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
	b.push_back(2);                    // team_count -> 3 rows (T0 + T1 + T2)
	for (int t = 0; t < 3; ++t) {
		u16(b, static_cast<uint16_t>(t));
		u16(b, 0);
		b.push_back(0);
		b.push_back(0);
	}
	b.push_back(in_game);
	b.push_back(spectators);
	return b;
}

// [u8 slot][u16 bitmask] then the present fields in bit order.
std::vector<uint8_t> make_sync_name(uint8_t slot, const char *name) {
	std::vector<uint8_t> b;
	b.push_back(slot);
	u16(b, kPlayerSyncHasName);
	b.push_back(0);   // entity_slot_id (present when !removal)
	for (const char *p = name; *p != '\0'; ++p) b.push_back(static_cast<uint8_t>(*p));
	b.push_back(0);
	return b;
}

std::vector<uint8_t> make_sync_removal(uint8_t slot) {
	std::vector<uint8_t> b;
	b.push_back(slot);
	u16(b, 0x8000);   // removal — no body follows
	return b;
}

// The row fields carry the REFUTED-name semantics: the second u16 is a status
// bitfield (not a ping) and the fourth is accumulated points (not deaths).
void test_row_fields_and_flags() {
	ClientReplicaPipeline view;
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
	}
	CHECK(sb.teams.size() == 3);          // team_count + 1 (T0 neutral)
}

// A spectator row sets bit0 and reports team 0 from the remaining bits.
void test_spectator_bit() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_LIST, make_list(0x00, {{2, 0, 0, 0, 0x01}}, 1, 1));
	const ClientScoreboard &sb = view.state().scoreboard;
	CHECK(sb.rows.size() == 1 && sb.rows[0].spectator);
	CHECK(!sb.team_mode && !sb.timed);
}

// AN EMPTY UPDATE MUST NOT CLOBBER A POPULATED BOARD. Round-end and next-map
// 0x16s frequently carry zero rows; retail keeps showing the last populated
// list.
void test_empty_update_does_not_clobber() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {{1, 0, 5, 50, 0x02}, {2, 0, 3, 30, 0x04}}, 2, 0));
	CHECK(view.state().scoreboard.rows.size() == 2);
	const std::uint64_t rev = view.state().scoreboard.revision;
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {}, 0, 0));
	CHECK(view.state().scoreboard.rows.size() == 2);   // held
	CHECK(view.state().scoreboard.revision == rev);    // and not even touched
	// A populated update DOES replace it.
	view.apply(s2c::PLAYER_LIST, make_list(0x01, {{9, 0, 1, 10, 0x02}}, 1, 0));
	CHECK(view.state().scoreboard.rows.size() == 1);
	CHECK(view.state().scoreboard.rows[0].slot_id == 9);
}

// An empty list on a board that was never populated is legitimate.
void test_first_empty_list_is_accepted() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_LIST, make_list(0x00, {}, 0, 0));
	CHECK(view.state().scoreboard.known);
	CHECK(view.state().scoreboard.rows.empty());
}

// The roster is a NAME-JOIN table, not a liveness set: a removal keeps the
// binding so the board can still name the slot.
void test_roster_join_and_removal_keeps_binding() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(5, "SPAGHETTI"));
	CHECK(view.state().roster[5].bound);
	CHECK(view.state().roster[5].name == "SPAGHETTI");
	view.apply(s2c::PLAYER_SYNC, make_sync_removal(5));
	CHECK(view.state().roster[5].bound);               // kept
	CHECK(view.state().roster[5].name == "SPAGHETTI"); // and still named
}

// Per-FIELD last-write-wins: a sync that omits a bit leaves that field alone
// rather than clearing it.
void test_roster_fields_are_last_write_wins_per_bit() {
	ClientReplicaPipeline view;
	view.apply(s2c::PLAYER_SYNC, make_sync_name(3, "Belsman"));
	// A later sync carrying only quality must not wipe the name.
	std::vector<uint8_t> q;
	q.push_back(3);
	u16(q, kPlayerSyncHasQuality);
	q.push_back(0);   // entity_slot_id
	q.push_back(2);   // quality
	view.apply(s2c::PLAYER_SYNC, q);
	CHECK(view.state().roster[3].name == "Belsman");
	CHECK(view.state().roster[3].quality == 2);
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
	test_empty_update_does_not_clobber();
	test_first_empty_list_is_accepted();
	test_roster_join_and_removal_keeps_binding();
	test_roster_fields_are_last_write_wins_per_bit();
	test_malformed_body_is_rejected();
	if (failures == 0) std::printf("client_replica_scoreboard_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
