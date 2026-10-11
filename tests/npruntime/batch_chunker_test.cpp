// Unit test for inmatch::slice_batch_pages — the shared byte-budget spawn/world-stream chunker (ADR 0013),
// factored out of server_initial_state.cpp's emit_paged_pool. Verifies conventional pre-write paging,
// retail's per-pool post-write guard, exact tile pages, lone oversized records, budget, and cursor resume.

#include <runtime/inmatch/batch_chunker.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A stand-in encoder with a configurable fixed header (2 B by default, 4 B for pool-3) followed by
// cnt*rec_bytes. Encoded size is what the chunker's byte budget keys on.
struct FixedRecordEncoder {
	std::size_t rec_bytes;
	std::size_t header_bytes = 2;
	std::vector<uint8_t> operator()(std::size_t /*off*/, std::size_t cnt) const {
		return std::vector<uint8_t>(header_bytes + cnt * rec_bytes, 0);
	}
};

// 0x45 uses a 20-byte first-page header and a 4-byte continuation header, then 12 bytes per tile.
struct TerrainTileEncoder {
	std::vector<uint8_t> operator()(std::size_t off, std::size_t cnt) const {
		return std::vector<uint8_t>((off == 0 ? 20 : 4) + cnt * 12, 0);
	}
};

} // namespace

int main() {
	bool ok = true;

	// 10-byte records, 25-byte page cap: header(2)+2*10=22 <= 25 fits, +3rd = 32 > 25 -> 2 records/page.
	// 5 records, unlimited pages this call -> [2, 2, 1] = 3 pages, exhausted.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(5, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, 0, 100);
		ok = expect(r.pages.size() == 3, "5 recs @ 2/page -> 3 pages") && ok;
		ok = expect(r.pages[0].size() == 22 && r.pages[1].size() == 22, "full pages are 2 records (22 B)") && ok;
		ok = expect(r.pages[2].size() == 12, "last page is the leftover 1 record (12 B)") && ok;
		ok = expect(r.next_cursor == 5 && r.exhausted, "batch fully paged -> cursor at end + exhausted") && ok;
		for (const std::vector<uint8_t> &p : r.pages)
			ok = expect(p.size() <= 25, "no page exceeds the byte cap") && ok;
	}

	// Retail pool-3 guard: record 62 (index 61) crosses written+30 > 650 but remains in the page, and
	// the cursor stays on it, so page 2 starts with it again: records 61..64, 4+4*10 = 44 B (D-NET-397).
	// [orig: NetPacket_SerializeEntityPoolToPacket @0x503460 — the count @0x503687, `jg` @0x503694
	//  past the cursor step]
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				65, inmatch::initial_state_page_limits::pool3_markers(), FixedRecordEncoder{10, 4}, 0, 100);
		ok = expect(r.pages.size() == 2, "pool-3: two pages") && ok;
		ok = expect(r.pages.size() == 2 && r.pages[0].size() == 624, "pool-3: page 1 holds 62 records") && ok;
		ok = expect(r.pages.size() == 2 && r.pages[1].size() == 44,
				"pool-3: page 2 repeats the crossing record (61..64)") && ok;
		ok = expect(r.next_cursor == 65 && r.exhausted, "pool-3: exhausted") && ok;
	}

	// The retail comparison is strict: 610 written + 40 margin == 650 continues one more record. Record
	// 102 (index 101) crosses; page 2 repeats it: records 101..102, 4+2*6 = 16 B.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				103, inmatch::initial_state_page_limits::pool2_static(), FixedRecordEncoder{6, 4}, 0, 100);
		ok = expect(r.pages.size() == 2, "pool-2: two pages") && ok;
		ok = expect(r.pages.size() == 2 && r.pages[0].size() == 616, "pool-2: page 1 holds 102 records") && ok;
		ok = expect(r.pages.size() == 2 && r.pages[1].size() == 16,
				"pool-2: page 2 repeats the crossing record (101..102)") && ok;
	}

	// Each page after the first starts with its predecessor's last record: record the (off, cnt) of
	// every page. 0x0C, 2-byte header, 50-byte records: 2+50k+100 > 650 at k = 11, so each page holds
	// 11 records and the next starts at its last one (0..10, 10..20, 20..29). [orig:
	// NetPacket_SerializeEntityStatesToBuffer @0x5030A0 — the count @0x5033F7, `jg` @0x50340D]
	{
		std::vector<std::pair<std::size_t, std::size_t>> spans;
		auto encoder = [&spans](std::size_t off, std::size_t cnt) {
			if (!spans.empty() && spans.back().first == off) spans.back().second = cnt;
			else spans.emplace_back(off, cnt);
			return std::vector<uint8_t>(2 + cnt * 50, 0);
		};
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				30, inmatch::initial_state_page_limits::pool0_organics(), encoder, 0, 100);
		ok = expect(r.pages.size() == 3 && spans.size() == 3, "pool-0: three pages") && ok;
		ok = expect(spans.size() == 3 && spans[0] == std::pair<std::size_t, std::size_t>(0, 11) &&
		                    spans[1] == std::pair<std::size_t, std::size_t>(10, 11) &&
		                    spans[2] == std::pair<std::size_t, std::size_t>(20, 10),
		            "pool-0: each page starts at its predecessor's last record") && ok;
		ok = expect(r.exhausted && r.next_cursor == 30, "pool-0: exhausted") && ok;
	}

	// The test runs after the pool's last record too: 62 pool-3 records cross on the last one, so it is
	// sent again as a page of its own (4+10 = 14 B), which does not cross and ends the pool.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				62, inmatch::initial_state_page_limits::pool3_markers(), FixedRecordEncoder{10, 4}, 0, 100);
		ok = expect(r.pages.size() == 2 && r.pages[0].size() == 624 && r.pages[1].size() == 14,
				"a crossing last record is sent again alone") && ok;
		ok = expect(r.exhausted && r.next_cursor == 62, "the lone repeat ends the pool") && ok;
	}

	// One page a call: the saved cursor is the crossing record, not the one after it.
	{
		inmatch::BatchPageResult a = inmatch::slice_batch_pages(
				65, inmatch::initial_state_page_limits::pool3_markers(), FixedRecordEncoder{10, 4}, 0, 1);
		ok = expect(a.pages.size() == 1 && a.next_cursor == 61 && !a.exhausted,
				"call 1: the cursor stays on the crossing record") && ok;
		inmatch::BatchPageResult b = inmatch::slice_batch_pages(
				65, inmatch::initial_state_page_limits::pool3_markers(), FixedRecordEncoder{10, 4},
				a.next_cursor, 1);
		ok = expect(b.pages.size() == 1 && b.pages[0].size() == 44 && b.exhausted,
				"call 2: the page restarts at the crossing record") && ok;
	}

	// A record that crosses alone still moves the stream on (one record a page), or it would never end.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				3, inmatch::initial_state_page_limits::pool3_markers(), FixedRecordEncoder{700, 4}, 0, 100);
		ok = expect(r.pages.size() == 3 && r.exhausted, "a lone crossing record steps on") && ok;
	}

	// The named production policies pin every witnessed entity-pool margin and the tile pre-check.
	{
		const inmatch::BatchPageLimit p2 = inmatch::initial_state_page_limits::pool2_static();
		const inmatch::BatchPageLimit p1 = inmatch::initial_state_page_limits::pool1_entities();
		const inmatch::BatchPageLimit p0 = inmatch::initial_state_page_limits::pool0_organics();
		const inmatch::BatchPageLimit p3 = inmatch::initial_state_page_limits::pool3_markers();
		const inmatch::BatchPageLimit tiles = inmatch::initial_state_page_limits::terrain_tiles();
		ok = (p2.byte_budget == 650 && p2.headroom == 40) && ok;
		ok = (p1.byte_budget == 650 && p1.headroom == 110) && ok;
		ok = (p0.byte_budget == 650 && p0.headroom == 100) && ok;
		ok = (p3.byte_budget == 650 && p3.headroom == 30) && ok;
		ok = (p2.check == inmatch::BatchPageLimit::Check::AfterEachRecord &&
		      p1.check == inmatch::BatchPageLimit::Check::AfterEachRecord &&
		      p0.check == inmatch::BatchPageLimit::Check::AfterEachRecord &&
		      p3.check == inmatch::BatchPageLimit::Check::AfterEachRecord) && ok;
		ok = (tiles.byte_budget == 650 && tiles.headroom == 0 &&
		      tiles.check == inmatch::BatchPageLimit::Check::BeforeNextRecord) && ok;
	}

	// Retail 0x45: first page 20+52*12=644 bytes; continuation 4+53*12=640 bytes.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(
				105, inmatch::initial_state_page_limits::terrain_tiles(), TerrainTileEncoder{}, 0, 100);
		ok = (r.pages.size() == 2) && ok;
		ok = (r.pages[0].size() == 644) && ok;
		ok = (r.pages[1].size() == 640) && ok;
		ok = (r.next_cursor == 105 && r.exhausted) && ok;
	}

	// Lone oversized record: 100-byte records, 25-byte cap -> each page must still ship exactly 1 record
	// (>= 1 per page even when it alone exceeds the cap), so 3 records -> 3 pages.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(3, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{100}, 0, 100);
		ok = expect(r.pages.size() == 3, "oversized records -> one record per page") && ok;
		ok = expect(r.pages[0].size() == 102, "an oversized page still carries its single record") && ok;
		ok = expect(r.exhausted, "oversized batch fully paged") && ok;
	}

	// Per-call page budget + cursor resume: max_pages=1 emits one page/call and saves the cursor.
	{
		std::size_t cursor = 0;
		inmatch::BatchPageResult a = inmatch::slice_batch_pages(5, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, cursor, 1);
		ok = expect(a.pages.size() == 1 && a.next_cursor == 2 && !a.exhausted, "call 1: 1 page, resume @2") && ok;
		inmatch::BatchPageResult b = inmatch::slice_batch_pages(5, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, a.next_cursor, 1);
		ok = expect(b.pages.size() == 1 && b.next_cursor == 4 && !b.exhausted, "call 2: 1 page, resume @4") && ok;
		inmatch::BatchPageResult c = inmatch::slice_batch_pages(5, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, b.next_cursor, 1);
		ok = expect(c.pages.size() == 1 && c.next_cursor == 5 && c.exhausted, "call 3: last page, exhausted") && ok;
	}

	// Zero page budget (tick already full): no pages, cursor preserved, not exhausted mid-batch.
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(5, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, 2, 0);
		ok = expect(r.pages.empty() && r.next_cursor == 2 && !r.exhausted, "budget 0 -> no pages, cursor held") && ok;
	}

	// Empty batch: no records -> no pages, exhausted (the emit_paged_pool wrapper emits the header-only marker).
	{
		inmatch::BatchPageResult r = inmatch::slice_batch_pages(0, inmatch::BatchPageLimit::pre_write(25), FixedRecordEncoder{10}, 0, 100);
		ok = expect(r.pages.empty() && r.exhausted, "empty batch -> no pages, exhausted") && ok;
	}

	if (!ok) {
		std::fprintf(stderr, "batch_chunker_test FAILED\n");
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
