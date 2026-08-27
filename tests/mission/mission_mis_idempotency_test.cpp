// Retail-fixture .mis idempotency: load the committed .bms fixture, export .mis (gen1), parse
// gen1, export again (gen2) -> gen1 == gen2 BYTE-EQUAL. This is the fixture-blind-spot closer
// for D-MIS-5: the authored-fixture tests exercise the same authoring defaults the parser seeds
// (spawns/no_more_than = 1, ...), which is exactly how the writer/parser default asymmetry hid —
// on retail data every zero-valued field mutated per round-trip (all 1365 entities on 00TRg
// gained `nomorethan 1`). The reparse also pins the D-MIS-1 pool-kind classification: the .mis
// text does not carry the pool, so the reader derives it from the items.def TYPE of each item id
// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. The fixture ships without an
// items.def, so the resolver here is built from the fixture's OWN pool membership (each pool's
// ids mapped to a def TYPE that classifies back onto that pool — legal because the mapping is
// 1:1 across shipping JO data), which exercises the real classification path end to end.
// See docs/mission/mis-format-re.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>

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
	const std::vector<bms::Entity> *source_pools[4] = {
			&source.items, &source.buildings, &source.markers, &source.organics};
	const size_t total = source.items.size() + source.buildings.size() +
	                     source.markers.size() + source.organics.size();
	TEST_EXPECT(total > 0);
	// The classification is only exercised when the fixture populates more than the
	// generic item pool (ash_i5b carries all four).
	TEST_EXPECT(source.buildings.size() + source.markers.size() + source.organics.size() > 0);

	// The items.def TYPE resolver, derived from the fixture's own pool membership: each
	// pool's ids map to a def TYPE that entity_kind_for_item_type classifies back onto that
	// pool (items -> 1 vehicle, buildings -> 5 building, markers -> 4 marker,
	// organics -> 3 person). The pool <-> TYPE mapping being a function of the item id is
	// the empirically pinned 1:1 (185,325 entities / 114 shipping JO missions), and the
	// contradiction check below re-asserts it on this fixture.
	constexpr int kPoolDefTypes[4] = {1, 5, 4, 3};
	std::unordered_map<int, int> def_types; // items.def id -> items.def TYPE
	for (int pool = 0; pool < 4; ++pool) {
		for (const bms::Entity &entity : *source_pools[pool]) {
			const int def_item_id = entity.type_id + kItemIdOffset;
			const auto [it, inserted] = def_types.emplace(def_item_id, kPoolDefTypes[pool]);
			TEST_EXPECT(inserted || it->second == kPoolDefTypes[pool]); // 1:1 id -> pool
		}
	}
	const MisItemTypeResolver resolver = [&def_types](int def_item_id) {
		const auto it = def_types.find(def_item_id);
		return it != def_types.end() ? it->second : -1;
	};

	// Spot references: the first entity of each populated pool, keyed by id. The reparse must
	// land each one back in its ORIGINAL pool (the D-MIS-1 classification), byte-identical on
	// the sampled fields.
	struct Spot {
		int pool;
		int32_t id;
		int32_t x, y, z;
		uint8_t team;
	};
	std::vector<Spot> spots;
	for (int pool = 0; pool < 4; ++pool) {
		if (!source_pools[pool]->empty()) {
			const bms::Entity &e = source_pools[pool]->front();
			spots.push_back({pool, e.id, e.x, e.y, e.z, e.team});
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
	TEST_EXPECT(reparsed.load_mis_text(gen1, resolver));
	// D-MIS-1 classification: every pool round-trips at its source size (total conserved by
	// construction).
	const bms::File &back = reparsed.bms_file();
	const std::vector<bms::Entity> *back_pools[4] = {
			&back.items, &back.buildings, &back.markers, &back.organics};
	for (int pool = 0; pool < 4; ++pool) {
		TEST_EXPECT(back_pools[pool]->size() == source_pools[pool]->size());
	}
	for (const Spot &spot : spots) {
		const bms::Entity *entity = find_by_id(*back_pools[spot.pool], spot.id);
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
