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
// size (0x0C carries a variable name string → 100; 0x20's records are small → 30). BatchPageLimit
// preserves that post-write guard while retaining the conventional pre-write cap used by 0x45 tiles.
// See docs/net/novaworld-net-re.md (D-NET-135).
namespace opennova::np {

// The result of paging up to `max_pages` datagrams' worth of records out of a batch from a cursor.
struct BatchPageResult {
	std::vector<std::vector<uint8_t>> pages; // encoded page bodies, in emit order
	std::size_t next_cursor = 0;             // resume record index for the next call (== n_records if done)
	bool exhausted = true;                   // true once the whole batch has been paged
};

// Where the serializer checks its byte budget. Most synthetic batches (and the 0x45 tile stream)
// reject the next record before writing it. Retail's four entity-pool serializers instead include a
// full record, then stop when the encoded size plus that pool's maximum-record headroom exceeds 650 B.
struct BatchPageLimit {
	enum class Check { BeforeNextRecord, AfterEachRecord };

	std::size_t byte_budget = 0;
	std::size_t headroom = 0;
	Check check = Check::BeforeNextRecord;

	static constexpr BatchPageLimit pre_write(std::size_t byte_budget) {
		return BatchPageLimit{byte_budget, 0, Check::BeforeNextRecord};
	}
	static constexpr BatchPageLimit post_write(std::size_t byte_budget, std::size_t headroom) {
		return BatchPageLimit{byte_budget, headroom, Check::AfterEachRecord};
	}
};

// Witnessed policies for Server_SendInitialGameStateToPlayer. Naming them here keeps the tag-to-margin
// mapping shared by production and the boundary tests.
namespace initial_state_page_limits {
constexpr BatchPageLimit pool2_static() { return BatchPageLimit::post_write(650, 40); }
constexpr BatchPageLimit pool1_entities() { return BatchPageLimit::post_write(650, 110); }
constexpr BatchPageLimit pool0_organics() { return BatchPageLimit::post_write(650, 100); }
constexpr BatchPageLimit pool3_markers() { return BatchPageLimit::post_write(650, 30); }
constexpr BatchPageLimit terrain_tiles() { return BatchPageLimit::pre_write(650); }
} // namespace initial_state_page_limits

// Slice records [start_cursor, n_records) into pages, at most `max_pages` this call. A pre-write limit
// rejects the next record when its encoded body would exceed the budget. A post-write limit includes
// the next complete record and stops once current encoded bytes + headroom exceeds the budget. Both
// modes always ship at least one record per page. `encode_page(off, cnt)` returns the encoded body for
// records [off, off+cnt); the cursor is retained when the per-call page budget is exhausted.
template <typename EncodePage>
BatchPageResult slice_batch_pages(std::size_t n_records, BatchPageLimit limit,
                                  EncodePage encode_page, std::size_t start_cursor,
                                  std::size_t max_pages) {
	BatchPageResult out;
	std::size_t i = start_cursor;
	while (i < n_records && out.pages.size() < max_pages) {
		std::size_t cnt = 1;
		std::vector<uint8_t> body = encode_page(i, cnt);
		while (i + cnt < n_records) {
			if (limit.check == BatchPageLimit::Check::AfterEachRecord &&
			    (body.size() > limit.byte_budget ||
			     limit.headroom > limit.byte_budget - body.size())) {
				break;
			}
			std::vector<uint8_t> grown = encode_page(i, cnt + 1);
			if (limit.check == BatchPageLimit::Check::BeforeNextRecord &&
			    grown.size() > limit.byte_budget) {
				break;
			}
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

// Compatibility shorthand for callers with a conventional pre-write hard cap.
template <typename EncodePage>
BatchPageResult slice_batch_pages(std::size_t n_records, std::size_t max_page_bytes,
                                  EncodePage encode_page, std::size_t start_cursor,
                                  std::size_t max_pages) {
	return slice_batch_pages(n_records, BatchPageLimit::pre_write(max_page_bytes),
	                         std::move(encode_page), start_cursor, max_pages);
}

} // namespace opennova::np
