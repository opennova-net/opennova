#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The shared spawn / world-stream batch chunker (ADR 0013). The §5.2a initial-state burst streams each
// world pool (0x10 pool-2 static, 0x0D pool-1, 0x0C pool-0 organics, 0x20 pool-3 markers) PAGED into
// per-datagram batches so a large mission does not exceed the UDP MTU and IP-fragment; the original caps
// each S2C world-stream datagram at ~650 B and advances a pool cursor across calls [orig:
// Server_SendInitialGameStateToPlayer @0x51bba0]. This is the pure byte-budget page slicer, factored out
// of server_initial_state.cpp's emit_paged_pool so it is ONE named, unit-tested chunker (it holds no
// InitialStateStep / InitialStateBurst coupling; emit_paged_pool is a thin wrapper over it).
//
// [orig] The per-pool serializers own the cap: each fills a 4096 B caller buffer but self-limits with a
// guard checked AFTER writing a full record — serialize_entity_states_to_buffer @0x5030a0 (0x0C) breaks
// when `written + 100 > 650`; serialize_entity_pool_to_packet @0x503460 (0x20) breaks when
// `written + 30 > 650`. The common budget is 650; the headroom margin is the pool's max single-record
// size (0x0C carries a variable name string → 100; 0x20's records are small → 30). Our chunker uses a
// single fixed `max_page_bytes` checked BEFORE the record is added (never over-fills), so batch bodies top
// out ~640 and the record-per-datagram split can differ from retail by one record on a boundary. Every
// page is still a valid count-prefixed sub-batch a stock client reassembles into the identical world —
// interop-equivalent, not byte-identical batching. See docs/net/novaworld-net-re.md (D-NET-135).
namespace opennova::np {

// The result of paging up to `max_pages` datagrams' worth of records out of a batch from a cursor.
struct BatchPageResult {
	std::vector<std::vector<uint8_t>> pages; // encoded page bodies, in emit order
	std::size_t next_cursor = 0;             // resume record index for the next call (== n_records if done)
	bool exhausted = true;                   // true once the whole batch has been paged
};

// Slice records [start_cursor, n_records) into pages, at most `max_pages` this call. Each page grows one
// record at a time until adding another would push the ENCODED body past `max_page_bytes` — always >= 1
// record per page, so a lone oversized record still ships as its own page. `encode_page(off, cnt)`
// returns the encoded body for records [off, off+cnt). Stops early once `max_pages` pages are produced,
// saving the cursor for resume next call. A byte-for-byte factoring of the emit_paged_pool inner loop:
// the paced burst passes `max_pages` = its remaining per-tick datagram budget.
template <typename EncodePage>
BatchPageResult slice_batch_pages(std::size_t n_records, std::size_t max_page_bytes,
                                  EncodePage encode_page, std::size_t start_cursor,
                                  std::size_t max_pages) {
	BatchPageResult out;
	std::size_t i = start_cursor;
	while (i < n_records && out.pages.size() < max_pages) {
		std::size_t cnt = 1;
		std::vector<uint8_t> body = encode_page(i, cnt);
		while (i + cnt < n_records) {
			std::vector<uint8_t> grown = encode_page(i, cnt + 1);
			if (grown.size() > max_page_bytes) break;
			body.swap(grown);
			++cnt;
		}
		i += cnt;
		out.pages.push_back(std::move(body));
	}
	out.next_cursor = i;
	out.exhausted = (i >= n_records);
	return out;
}

} // namespace opennova::np
