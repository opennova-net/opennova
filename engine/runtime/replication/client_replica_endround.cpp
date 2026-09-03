// The END-OF-ROUND STAT BOARD lane (S2C 0x56).
//
// The board does not fit one datagram, so it arrives chunked and the client
// reassembles before decoding [orig: NapiNPClientMsg_0x056 @0x431D10]. Three
// witnessed rules shape this fold, and each one is a behaviour rather than a
// convenience:
//
//   * a chunk at offset 0 RESETS the stream [orig: `test edi, edi` @0x431D6D
//     gating CDataStream_SetMaxFrame(stream, 0) @0x431D79]. Without it a
//     re-sent board merges with the previous one and inherits its tail --
//     which only shows up on the SECOND round of a session, long after the
//     code looks correct.
//   * the board is complete when offset + chunk_len >= THIS chunk's declared
//     total [orig: the add @0x431D9B, `cmp`/`jge` @0x431D9D..0x431D9F] -- a
//     >=, not an ==, so a final chunk that overshoots still completes rather
//     than hanging the board forever.
//   * chunks are written AT THEIR OFFSET, not appended, so a repeated chunk
//     overwrites in place instead of corrupting the stream [orig: the
//     chunk_len > 0 gate @0x431D80, CDataStream_Seek @0x431D8A then
//     CDataStream_Write @0x431D96].
//
// That last rule is about PLACEMENT only -- it is not out-of-order support.
// Completion is by BYTE COUNT and the stream keeps no record of which ranges
// arrived, so a chunk with a high offset satisfies the test on its own and
// completes the board against a head that is still zero-filled. A zero head
// decodes cleanly into a valid but EMPTY board, so a consumer of `known` must
// tolerate an empty player list. This is retail's behaviour, not a shortcut.
//
// THE STREAM SURVIVES COMPLETION. Retail decodes the stream in place and
// leaves it standing [orig: the parse from @0x431E0B reads the same stream;
//  nothing resets it after the decode -- only the next offset-0 chunk does].
// So a retransmitted tail chunk after a complete board re-decodes the SAME
// board; clearing the buffer on completion would instead decode a zero head
// and replace a good board with an empty one. A partial chunk never clears
// `known` either, so the screen keeps showing the last complete board while
// the next one streams in.
//
// The connection state machine owns the pull replies because it owns reliable
// sequencing. This reducer owns only the header and reassembly state.

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

void ClientReplicaPipeline::apply_end_round_header(
		const std::vector<uint8_t> &body) {
	EndRoundHeader header;
	// The retail client picks the form from `is_in_session && !(g_GameType &
	// 0x10000)` [orig: NapiNPClientMsg_0x01D @0x43086c..0x430883]; the
	// SP listen host's loopback replica stands in world.mp_session for
	// is_in_session (see mp_session_).
	if (!decode_end_round_header(body.data(), body.size(),
			mp_session_ && (game_type_ & 0x10000u) == 0, header))
		return;
	state_.end_round.header = header;
	state_.end_round.header_known = true;
	state_.mark_changed();
}

void ClientReplicaPipeline::apply_end_round_stats_chunk(
		const std::vector<uint8_t> &body) {
	EndRoundStatsChunk chunk;
	if (!decode_end_round_stats_chunk(body.data(), body.size(), chunk))
		return;

	ClientEndRoundStats &st = state_.end_round;

	// Offset 0 restarts the stream [orig: @0x431D6D..0x431D79].
	if (chunk.chunk_offset == 0) {
		st.buffer.clear();
		st.chunks_seen = 0;
	}

	// Write AT the offset [orig: @0x431D80..0x431D96]. Growing to fit leaves
	// any gap zeroed, which is what a seek-past-end into retail's stream does.
	const size_t end = static_cast<size_t>(chunk.chunk_offset) + chunk.chunk.size();
	if (!chunk.chunk.empty()) {
		if (end > st.buffer.size()) st.buffer.resize(end, 0u);
		std::copy(chunk.chunk.begin(), chunk.chunk.end(),
				st.buffer.begin() + static_cast<std::ptrdiff_t>(chunk.chunk_offset));
	}
	++st.chunks_seen;

	// Complete on >= this chunk's total [orig: @0x431D9B..0x431D9F]. Short of
	// it, the connection state machine requests the next chunk (C2S 0x2B).
	if (end < static_cast<size_t>(chunk.total_size)) return;

	// Decode the stream AS IT STANDS and leave it standing [orig: the parse
	// from @0x431E0B; g_scoreboardDirty = 1 @0x4321BE]. A board our decoder
	// rejects is dropped, leaving the last good one: retail's parser clips
	// every read to the stream end and cannot reject, so the drop is the
	// port-side policy for input retail would have read as zeros.
	EndRoundStats decoded;
	if (decode_end_round_stats(st.buffer.data(), st.buffer.size(), decoded)) {
		st.board = std::move(decoded);
		st.known = true;
	}
}

} // namespace opennova::netsim
