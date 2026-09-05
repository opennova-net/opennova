// The END-OF-ROUND stat board fold (S2C 0x56). The board does not fit one
// datagram, so this pins the REASSEMBLY rules rather than the payload shape
// (nw_message_coverage already pins that):
//   * a chunk at offset 0 RESETS the stream [orig: @0x431D6D..0x431D79] —
//     without it a re-sent board inherits the previous board's tail, which
//     only shows up on the SECOND round of a session;
//   * complete on offset + len >= total [orig: @0x431D9B..0x431D9F], a >= not
//     an ==;
//   * chunks write AT their offset [orig: @0x431D8A..0x431D96], so a repeated
//     chunk overwrites in place;
//   * the stream SURVIVES a completed decode — only offset 0 resets it.
// [orig: NapiNPClientMsg_0x056 @0x431D10]
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

struct LE {
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
	void str(const char *s) { for (const char *p = s; *p; ++p) u8(uint8_t(*p)); u8(0); }
};

// A minimal but COMPLETE board: no team fields, one player, no team rows.
std::vector<uint8_t> make_board(uint8_t slot, const char *name, int16_t kills) {
	LE p;
	p.u8(1);        // winner_team
	p.u16(10);      // team_score_0
	p.u16(20);      // team_score_1
	p.u8(0);        // field_count = 0
	p.u8(1);        // one player
	p.u8(slot);
	p.str(name);
	p.str("");      // clan
	p.str("");      // tag
	p.u8(1); p.u8(0);                    // team, side
	p.u16(uint16_t(kills));
	p.u16(2); p.u16(3); p.u16(4); p.u16(5); p.u16(6); p.u16(7);
	p.u8(0);        // no trailing team rows
	return p.b;
}

// Wrap a slice of `board` in the 0x56 envelope.
std::vector<uint8_t> chunk_of(const std::vector<uint8_t> &board, size_t off,
		size_t len, uint16_t total) {
	LE e;
	e.u16(total);
	e.u16(uint16_t(off));
	for (size_t i = off; i < off + len && i < board.size(); ++i) e.u8(board[i]);
	return e.b;
}

// A board that arrives whole in one chunk decodes immediately.
void test_single_chunk_board() {
	// Heap-allocated: ClientState is far too large for the 1 MB Windows
	// test stack (the same trap #515 fixed in ai_test).
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	const std::vector<uint8_t> board = make_board(3, "Ace", 11);
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(board, 0, board.size(), uint16_t(board.size())));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players.size() == 1);
	CHECK(view.state().end_round.board.players[0].name == "Ace");
	CHECK(view.state().end_round.board.players[0].kills == 11);
	CHECK(view.state().end_round.board.winner_team == 1);
}

// Split across two chunks: the board is NOT known until the second arrives.
void test_two_chunk_reassembly() {
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	const std::vector<uint8_t> board = make_board(4, "Bee", 22);
	const uint16_t total = uint16_t(board.size());
	const size_t half = board.size() / 2;
	view.apply(s2c::END_ROUND_STATS, chunk_of(board, 0, half, total));
	CHECK(!view.state().end_round.known);   // incomplete
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(board, half, board.size() - half, total));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players[0].name == "Bee");
	CHECK(view.state().end_round.board.players[0].kills == 22);
}

// THE RESET RULE. A second board must not inherit the first board's tail: send
// a long board, then a SHORTER one starting at offset 0. Without the reset the
// leftover bytes would still be in the stream and the shorter board would
// decode against them.
void test_offset_zero_resets_the_buffer() {
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	const std::vector<uint8_t> longer = make_board(5, "LongerNameHere", 33);
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(longer, 0, longer.size(), uint16_t(longer.size())));
	CHECK(view.state().end_round.board.players[0].name == "LongerNameHere");

	const std::vector<uint8_t> shorter = make_board(6, "Cy", 44);
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(shorter, 0, shorter.size(), uint16_t(shorter.size())));
	CHECK(view.state().end_round.board.players.size() == 1);
	CHECK(view.state().end_round.board.players[0].name == "Cy");
	CHECK(view.state().end_round.board.players[0].kills == 44);
	CHECK(view.state().end_round.board.players[0].slot == 6);
}

// COMPLETION IS BY BYTE COUNT, NOT BY COVERAGE -- and that has a sharp edge
// worth pinning rather than smoothing over. A chunk that arrives with a high
// offset satisfies `offset + len >= total` on its own, so the board completes
// against a head that was never delivered and is still zero-filled. Retail's
// stream does exactly this: it seeks, writes, and tests the byte count
// [orig: @0x431D9B..0x431D9F], with no record of which ranges actually arrived.
//
// A zero head decodes cleanly -- winner 0, no fields, ZERO players -- so the
// result is a valid but EMPTY board, not a decode failure. Anything consuming
// `known` must therefore tolerate an empty player list.
void test_completion_is_by_byte_count() {
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	const std::vector<uint8_t> board = make_board(7, "Dee", 55);
	const uint16_t total = uint16_t(board.size());
	const size_t half = board.size() / 2;

	// Tail first: reaches the byte count, so the board "completes".
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(board, half, board.size() - half, total));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players.empty());
	CHECK(view.state().end_round.board.winner_team == 0);

	// The head then arrives at offset 0, which RESETS -- it does not repair the
	// previous board. It is short of the total, so nothing new completes and the
	// last decoded (empty) board still stands.
	view.apply(s2c::END_ROUND_STATS, chunk_of(board, 0, half, total));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players.empty());
}

// THE STREAM SURVIVES COMPLETION. After a complete board, a retransmitted
// tail chunk re-decodes against the bytes still in the stream and yields the
// SAME board. Clearing the stream on completion would decode the tail against
// a zero head and replace a good board with an empty one -- the bug this pins.
void test_stream_survives_completion() {
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	const std::vector<uint8_t> board = make_board(8, "Eve", 66);
	const uint16_t total = uint16_t(board.size());
	const size_t half = board.size() / 2;
	view.apply(s2c::END_ROUND_STATS, chunk_of(board, 0, board.size(), total));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players[0].name == "Eve");

	// The tail again, as a retransmit would deliver it.
	view.apply(s2c::END_ROUND_STATS,
			chunk_of(board, half, board.size() - half, total));
	CHECK(view.state().end_round.known);
	CHECK(view.state().end_round.board.players.size() == 1);
	CHECK(view.state().end_round.board.players[0].name == "Eve");
	CHECK(view.state().end_round.board.players[0].kills == 66);
}

// A malformed board leaves `known` false rather than publishing a half-board.
void test_undecodable_board_is_dropped() {
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;
	LE e;
	e.u16(4);
	e.u16(0);
	e.u8(1); e.u8(2); e.u8(3); e.u8(4);   // not a valid board
	view.apply(s2c::END_ROUND_STATS, e.b);
	CHECK(!view.state().end_round.known);
}

} // namespace

int main() {
	test_single_chunk_board();
	test_two_chunk_reassembly();
	test_offset_zero_resets_the_buffer();
	test_completion_is_by_byte_count();
	test_stream_survives_completion();
	test_undecodable_board_is_dropped();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_endround_test OK\n");
	return 0;
}
