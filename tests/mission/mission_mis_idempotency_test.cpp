// Retail-fixture .mis idempotency: load the committed .bms fixture, export .mis (gen1), parse
// gen1, export again (gen2) -> gen1 == gen2 BYTE-EQUAL. This is the fixture-blind-spot closer
// for D-MIS-5: the authored-fixture tests exercise the same authoring defaults the parser seeds
// (spawns/no_more_than = 1, ...), which is exactly how the writer/parser default asymmetry hid —
// on retail data every zero-valued field mutated per round-trip (all 1365 entities on 00TRg
// gained `nomorethan 1`). Pool-kind collapse on .mis read is tracked as D-MIS-1: all reparsed
// entities land in the item pool, so the TOTAL count is asserted conserved and the per-pool spot
// checks resolve entities by id. See docs/mission/mis-format-re.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "mission/bms.h"
#include "mission/mission.h"

namespace {

std::string fixture_path() {
	const std::string root = test_paths_repo_root(__FILE__);
	return root + "/fixtures/bms/ash_i5b.reference.bms";
}

const opennova::bms::Entity *find_by_id(const std::vector<opennova::bms::Entity> &pool, int32_t id) {
	for (const opennova::bms::Entity &entity : pool) {
		if (entity.id == id) {
			return &entity;
		}
	}
	return nullptr;
}

// On mismatch, print the first differing offset with context so a regression is debuggable from
// the ctest log alone.
bool texts_equal(const std::string &a, const std::string &b) {
	if (a == b) {
		return true;
	}
	size_t i = 0;
	const size_t limit = std::min(a.size(), b.size());
	while (i < limit && a[i] == b[i]) {
		++i;
	}
	const size_t from = i < 60 ? 0 : i - 60;
	std::fprintf(stderr, "gen1 size %zu, gen2 size %zu, first diff at byte %zu\n", a.size(), b.size(), i);
	std::fprintf(stderr, "gen1: ...%s...\n", a.substr(from, 120).c_str());
	std::fprintf(stderr, "gen2: ...%s...\n", b.substr(from, 120).c_str());
	return false;
}

} // namespace

int main() {
	using namespace opennova::mission;
	namespace bms = opennova::bms;

	MissionDocument doc;
	TEST_EXPECT(doc.load_bms_file(fixture_path()));
	const bms::File &source = doc.bms_file();
	const size_t total = source.items.size() + source.buildings.size() +
	                     source.markers.size() + source.organics.size();
	TEST_EXPECT(total > 0);

	// Spot references: the first entity of each populated pool, keyed by id (the .mis text does
	// not carry the pool kind, so the reparse is resolved by id inside the collapsed item pool).
	struct Spot {
		int32_t id;
		int32_t x, y, z;
		uint8_t team;
	};
	std::vector<Spot> spots;
	for (const std::vector<bms::Entity> *pool :
	     {&source.items, &source.buildings, &source.markers, &source.organics}) {
		if (!pool->empty()) {
			const bms::Entity &e = pool->front();
			spots.push_back({e.id, e.x, e.y, e.z, e.team});
		}
	}
	TEST_EXPECT(!spots.empty());

	std::string gen1;
	TEST_EXPECT(doc.write_mis_text(gen1));
	TEST_EXPECT(!gen1.empty());
	// Every BMS-sourced item block declares its z absolute (D-MIS-4)
	// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll].
	TEST_EXPECT(gen1.find("  height_lock 1\r\n") != std::string::npos);

	MissionDocument reparsed;
	TEST_EXPECT(reparsed.load_mis_text(gen1));
	// D-MIS-1 pool collapse: every entity lands in the item pool; the total is conserved.
	TEST_EXPECT(reparsed.bms_file().items.size() == total);
	TEST_EXPECT(reparsed.bms_file().buildings.empty());
	TEST_EXPECT(reparsed.bms_file().markers.empty());
	TEST_EXPECT(reparsed.bms_file().organics.empty());
	for (const Spot &spot : spots) {
		const bms::Entity *entity = find_by_id(reparsed.bms_file().items, spot.id);
		TEST_EXPECT(entity != nullptr);
		TEST_EXPECT(entity->x == spot.x);
		TEST_EXPECT(entity->y == spot.y);
		TEST_EXPECT(entity->z == spot.z);
		TEST_EXPECT(entity->team == spot.team);
		TEST_EXPECT(entity->mis_height_lock == 1);
	}

	std::string gen2;
	TEST_EXPECT(reparsed.write_mis_text(gen2));
	TEST_EXPECT(texts_equal(gen1, gen2));

	return 0;
}
