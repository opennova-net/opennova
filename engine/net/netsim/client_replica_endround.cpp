// The END-OF-ROUND STAT BOARD lane (S2C 0x56).
//
// The board does not fit one datagram, so it arrives chunked and the client
// reassembles before decoding [orig: NapiNPClientMsg_0x056 @0x431D10]. Three
// witnessed rules shape this fold, and each one is a behaviour rather than a
// convenience:
//
//   * a chunk at offset 0 RESETS the buffer [orig: the
//     CDataStream_SetMaxFrame(stream, 0) call @0x431D80]. Without it a
//     re-sent board merges with the previous one and inherits its tail --
//     which only shows up on the SECOND round of a session, long after the
//     code looks correct.
//   * the board is complete when offset + chunk_len >= total_size
//     [orig: @0x431D9B] -- a >=, not an ==, so a final chunk that overshoots
//     still completes rather than hanging the board forever.
//   * chunks are written AT THEIR OFFSET, not appended, so a repeated chunk
//     overwrites in place instead of corrupting the stream
//     [orig: CDataStream_Seek then CDataStream_Write].
//
// That last rule is about PLACEMENT only -- it is not out-of-order support.
// Completion is by BYTE COUNT and the stream keeps no record of which ranges
// arrived, so a chunk with a high offset satisfies the test on its own and
// completes the board against a head that is still zero-filled. A zero head
// decodes cleanly into a valid but EMPTY board, so a consumer of `known` must
// tolerate an empty player list. This is retail's behaviour, not a shortcut.
//
// A completed board REPLACES the previous one. A partial chunk never clears
// `known`, so the screen keeps showing the last complete board while the next
// one streams in.

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

void ClientReplicaPipeline::apply_end_round_stats_chunk(
		const std::vector<uint8_t> &body) {
	EndRoundStatsChunk chunk;
	if (!decode_end_round_stats_chunk(body.data(), body.size(), chunk))
		return;

	ClientEndRoundStats &st = state_.end_round;

	// Offset 0 restarts the board.
	if (chunk.chunk_offset == 0) {
		st.buffer.clear();
		st.expected = chunk.total_size;
		st.chunks_seen = 0;
	}
	// A chunk arriving before any offset-0 start has no declared size; take
	// this one's rather than dropping it, so a board joined mid-stream can
	// still complete.
	if (st.expected == 0) st.expected = chunk.total_size;

	// Write AT the offset. Growing to fit leaves any gap zeroed, which is what
	// a seek-past-end into retail's stream does.
	const size_t end = static_cast<size_t>(chunk.chunk_offset) + chunk.chunk.size();
	if (end > st.buffer.size()) st.buffer.resize(end, 0u);
	if (!chunk.chunk.empty()) {
		std::copy(chunk.chunk.begin(), chunk.chunk.end(),
				st.buffer.begin() + static_cast<std::ptrdiff_t>(chunk.chunk_offset));
	}
	++st.chunks_seen;

	// Complete on >=, matching the witnessed test.
	if (st.expected == 0 || end < static_cast<size_t>(st.expected)) return;

	EndRoundStats decoded;
	if (decode_end_round_stats(st.buffer.data(), st.buffer.size(), decoded)) {
		st.board = std::move(decoded);
		st.known = true;
	}
	// Either way the reassembly is finished with: a board that fails to decode
	// is dropped rather than retried, and the next offset-0 chunk starts clean.
	st.buffer.clear();
	st.expected = 0;
}

} // namespace opennova::netsim
