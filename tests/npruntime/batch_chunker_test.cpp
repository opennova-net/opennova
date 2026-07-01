// Unit test for np::slice_batch_pages — the shared byte-budget spawn/world-stream chunker (ADR 0013),
// factored out of server_initial_state.cpp's emit_paged_pool. Verifies page-fit, the lone-oversized-record
// rule, the per-call page budget, and cursor resume — the behavior the paced §5.2a world-stream relies on.

#include <npruntime/batch_chunker.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace np = opennova::np;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A stand-in encoder: records [off, off+cnt) encode to a 2-byte header + cnt*rec_bytes body, mirroring
// the real encoders' `[u16 count][records...]` shape. Size is what the chunker's byte budget keys on.
struct FixedRecordEncoder {
	std::size_t rec_bytes;
	std::vector<uint8_t> operator()(std::size_t /*off*/, std::size_t cnt) const {
		return std::vector<uint8_t>(2 + cnt * rec_bytes, 0);
	}
};

} // namespace

int main() {
	bool ok = true;

	// 10-byte records, 25-byte page cap: header(2)+2*10=22 <= 25 fits, +3rd = 32 > 25 -> 2 records/page.
	// 5 records, unlimited pages this call -> [2, 2, 1] = 3 pages, exhausted.
	{
		np::BatchPageResult r = np::slice_batch_pages(5, 25, FixedRecordEncoder{10}, 0, 100);
		ok = expect(r.pages.size() == 3, "5 recs @ 2/page -> 3 pages") && ok;
		ok = expect(r.pages[0].size() == 22 && r.pages[1].size() == 22, "full pages are 2 records (22 B)") && ok;
		ok = expect(r.pages[2].size() == 12, "last page is the leftover 1 record (12 B)") && ok;
		ok = expect(r.next_cursor == 5 && r.exhausted, "batch fully paged -> cursor at end + exhausted") && ok;
		for (const std::vector<uint8_t> &p : r.pages)
			ok = expect(p.size() <= 25, "no page exceeds the byte cap") && ok;
	}

	// Lone oversized record: 100-byte records, 25-byte cap -> each page must still ship exactly 1 record
	// (>= 1 per page even when it alone exceeds the cap), so 3 records -> 3 pages.
	{
		np::BatchPageResult r = np::slice_batch_pages(3, 25, FixedRecordEncoder{100}, 0, 100);
		ok = expect(r.pages.size() == 3, "oversized records -> one record per page") && ok;
		ok = expect(r.pages[0].size() == 102, "an oversized page still carries its single record") && ok;
		ok = expect(r.exhausted, "oversized batch fully paged") && ok;
	}

	// Per-call page budget + cursor resume: max_pages=1 emits one page/call and saves the cursor.
	{
		std::size_t cursor = 0;
		np::BatchPageResult a = np::slice_batch_pages(5, 25, FixedRecordEncoder{10}, cursor, 1);
		ok = expect(a.pages.size() == 1 && a.next_cursor == 2 && !a.exhausted, "call 1: 1 page, resume @2") && ok;
		np::BatchPageResult b = np::slice_batch_pages(5, 25, FixedRecordEncoder{10}, a.next_cursor, 1);
		ok = expect(b.pages.size() == 1 && b.next_cursor == 4 && !b.exhausted, "call 2: 1 page, resume @4") && ok;
		np::BatchPageResult c = np::slice_batch_pages(5, 25, FixedRecordEncoder{10}, b.next_cursor, 1);
		ok = expect(c.pages.size() == 1 && c.next_cursor == 5 && c.exhausted, "call 3: last page, exhausted") && ok;
	}

	// Zero page budget (tick already full): no pages, cursor preserved, not exhausted mid-batch.
	{
		np::BatchPageResult r = np::slice_batch_pages(5, 25, FixedRecordEncoder{10}, 2, 0);
		ok = expect(r.pages.empty() && r.next_cursor == 2 && !r.exhausted, "budget 0 -> no pages, cursor held") && ok;
	}

	// Empty batch: no records -> no pages, exhausted (the emit_paged_pool wrapper emits the header-only marker).
	{
		np::BatchPageResult r = np::slice_batch_pages(0, 25, FixedRecordEncoder{10}, 0, 100);
		ok = expect(r.pages.empty() && r.exhausted, "empty batch -> no pages, exhausted") && ok;
	}

	if (!ok) {
		std::fprintf(stderr, "batch_chunker_test FAILED\n");
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
