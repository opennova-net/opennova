// .mis idempotency: load a .bms, export .mis (gen1), parse gen1, export again (gen2) -> gen1 ==
// gen2 BYTE-EQUAL, over the minted fixtures/bms/synth_dense.bms (tests/fixtures/minimal_bms_gen.cpp:
// four populated pools, zero-valued optional fields) and fixtures/bms/synth_areas.bms
// (tests/fixtures/mission_set_gen.cpp: a shipped training mission's areas). This is the fixture-blind-spot closer
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
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_mis.h>

namespace {

std::string fixture_path() {
	const std::string root = test_paths_repo_root(__FILE__);
	return root + "/fixtures/bms/synth_dense.bms";
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

int check(const std::string &path) {
	using namespace opennova::mission;
	namespace bms = opennova::bms;

	bms::File doc;
	std::string error;
	TEST_EXPECT(bms::parse_file(path, doc, error));
	sync_counts(doc);
	const bms::File &source = doc;
	const std::vector<bms::Entity> *source_pools[4] = {
			&source.items, &source.buildings, &source.markers, &source.organics};
	const size_t total = source.items.size() + source.buildings.size() +
	                     source.markers.size() + source.organics.size();
	TEST_EXPECT(total > 0);
	// The classification is only exercised when the fixture populates more than the
	// generic item pool (both fixtures carry all four).
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
	TEST_EXPECT(write_mis_text(doc, gen1, error));
	TEST_EXPECT(!gen1.empty());
	// A BMS-sourced item is unlocked (D-MIS-4); with no terrain height known (none given here) each is written
	// locked, its z absolute as it stands, exact in the original editor over whatever terrain it loads (D-MIS-17).
	size_t locks = 0;
	for (size_t at = gen1.find("  height_lock 1\r\n"); at != std::string::npos; at = gen1.find("  height_lock 1\r\n", at + 1))
		++locks;
	TEST_EXPECT(locks ==
			total + 2 * doc.area_triggers.size()); // and each area's two locked corners (D-MIS-14)

	bms::File reparsed;
	TEST_EXPECT(parse_mis_text(gen1, resolver, reparsed, error));
	sync_counts(reparsed);
	// D-MIS-1 classification: every pool round-trips at its source size (total conserved by
	// construction).
	const bms::File &back = reparsed;
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
	TEST_EXPECT(write_mis_text(reparsed, gen2, error));
	TEST_EXPECT(texts_equal(gen1, gen2));
	// The areas come back by zone id, in id order, with their flags and their bounds, which two
	// 6013 items at the box's corners carry (D-MIS-14).
	std::vector<std::vector<int64_t>> areas_in, areas_back;
	const auto row = [](const bms::AreaTrigger &a) {
		return std::vector<int64_t>{ a.id, a.x_min, a.x_max, a.y_min, a.y_max, a.z_min, a.z_max,
			int64_t(a.flags) };
	};
	for (const bms::AreaTrigger &area : doc.area_triggers)
		areas_in.push_back(row(area));
	for (const bms::AreaTrigger &area : reparsed.area_triggers)
		areas_back.push_back(row(area));
	std::sort(areas_in.begin(), areas_in.end());
	TEST_EXPECT(areas_back == areas_in);

	// The wav list (bms.h WavListRecord, D-MIS-13) rides `begin wav_list N`, slots 1..31 as the
	// original editor writes them (its slot 0 never), and the variables' names lines both original
	// readers skip
	// (`//var N "name"`): each comes back, and the text again.
	{
		bms::File listed = doc;
		std::memcpy(listed.wav_list_records[2].file_name, "alarm.wav", 9);
		std::memcpy(listed.wav_list_records[31].file_name, "last.wav", 8);
		MisNames vars;
		vars.variables[2] = "alarm";
		vars.variables[255] = "bridge blown";
		std::string listed1;
		TEST_EXPECT(write_mis_text(listed, listed1, error, nullptr, &vars));
		TEST_EXPECT(listed1.find("begin wav_list 2\r\n  description \"\"\r\n  file_name "
								 "\"alarm.wav\"\r\nend wav_list\r\n") != std::string::npos);
		TEST_EXPECT(listed1.find("//var 255 \"bridge blown\"\r\n") != std::string::npos);
		bms::File listed_back;
		TEST_EXPECT(parse_mis_text(listed1, resolver, listed_back, error));
		TEST_EXPECT(std::string(listed_back.wav_list_records[2].file_name) == "alarm.wav" &&
				std::string(listed_back.wav_list_records[31].file_name) == "last.wav");
		MisNames vars_back;
		TEST_EXPECT(parse_mis_names(listed1, vars_back, error));
		TEST_EXPECT(vars_back.variables[2] == "alarm" &&
				vars_back.variables[255] == "bridge blown" && vars_back.variables[3].empty());
		sync_counts(listed_back);
		std::string listed2;
		TEST_EXPECT(write_mis_text(listed_back, listed2, error, nullptr, &vars_back));
		TEST_EXPECT(texts_equal(listed1, listed2));
	}

	// With names (MisNames): every event named, every area described by its zone id and every path by its
	// index, the paths with no stop among them written for their description; the names read back are the
	// names written, and the text written from them again is the same.
	MisNames names;
	for (size_t i = 0; i < doc.events.size(); ++i) names.events.push_back("event " + std::to_string(i));
	for (const bms::AreaTrigger &area : doc.area_triggers)
		if (area.id > 0 && area.id < static_cast<int32_t>(kMisAreaSlots))
			names.areas[static_cast<size_t>(area.id)] = "area " + std::to_string(area.id);
	for (size_t i = 1; i < names.paths.size(); ++i) names.paths[i] = "path " + std::to_string(i);
	std::string named1;
	TEST_EXPECT(write_mis_text(doc, named1, error, nullptr, &names));
	MisNames names_back;
	TEST_EXPECT(parse_mis_names(named1, names_back, error));
	TEST_EXPECT(names_back.events == names.events);
	TEST_EXPECT(names_back.areas == names.areas);
	TEST_EXPECT(names_back.paths == names.paths);
	bms::File named_back;
	TEST_EXPECT(parse_mis_text(named1, resolver, named_back, error));
	sync_counts(named_back);
	std::string named2;
	TEST_EXPECT(write_mis_text(named_back, named2, error, nullptr, &names_back));
	TEST_EXPECT(texts_equal(named1, named2));

	return 0;
}

// The default kit of a shipped mission's loadout (D-MIS-19), over the authored armory
// (fixtures/def/weapon.def, the shipped table's rows): 00TRg's chunk, as the original
// editor's export builds it from its kit {WPN_M16M203BURST, WPN_colt45, WPN_AT4} -- the
// burst rifle and its two subclass rows, the pistol, the launcher, then kMisLoadoutTail --
// each record its name and two "-1" strings. The kit the chunk leads with is the kit, the
// .mis written over the table carries its three lines, and the .mis read back builds the
// chunk's records, name for name.
int check_default_kit(const std::string &weapons_path) {
	using namespace opennova::mission;
	namespace bms = opennova::bms;
	namespace def = opennova::def;
	std::string error;
	bms::File file;
	make_default(file);
	for (const char *name : {"WPN_M16M203BURST", "WPN_M16M203", "WPN_M16M203HE", "WPN_colt45", "WPN_AT4",
	                         "WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE"}) {
		bms::WeaponLoadoutRecord record;
		record.name = name;
		record.ammo_primary = "-1";
		record.ammo_secondary = "-1";
		record.has_flags = false;
		file.loadout.entries.push_back(record);
	}
	const std::vector<uint8_t> weapon_bytes = test_io::read_file(weapons_path);
	def::DefWeaponsFile weapons{};
	TEST_EXPECT(def::def_parse_weapons_memory(weapon_bytes.data(), weapon_bytes.size(), &weapons) == 0);
	const MisWeaponTable table = mis_weapon_table(weapons);
	def::def_free_weapons(&weapons);
	// The original's first row [orig: JOTACmed.exe sub_432E50 @ 0x432e8d].
	TEST_EXPECT(!table.empty() && table[0].name == "None");
	TEST_EXPECT(mis_default_kit(file.loadout, &table) == MisDefaultKit({"WPN_M16M203BURST", "WPN_colt45", "WPN_AT4"}));
	sync_counts(file);
	std::string written;
	TEST_EXPECT(write_mis_text(file, written, error, nullptr, nullptr, &table));
	const std::string lines = "  default_primary   WPN_M16M203BURST\r\n  default_secondary WPN_colt45\r\n"
	                          "  default_accessory WPN_AT4\r\n";
	TEST_EXPECT(written.find(lines) != std::string::npos);
	bms::File read;
	TEST_EXPECT(parse_mis_text(written, {}, read, error, nullptr, &table));
	TEST_EXPECT(read.loadout.entries.size() == file.loadout.entries.size());
	for (size_t i = 0; i < read.loadout.entries.size() && i < file.loadout.entries.size(); ++i)
		TEST_EXPECT(read.loadout.entries[i].name == file.loadout.entries[i].name);
	return 0;
}

int main() {
	const std::string root = test_paths_repo_root(__FILE__);
	if (check(fixture_path()) != 0) return 1;
	// A mission with areas (synth_dense has none): fixtures/bms/synth_areas.bms, thirteen zones in a
	// shipped training mission's shape (constrain-Z among their flags) and the mission boundary.
	if (check(root + "/fixtures/bms/synth_areas.bms") != 0) return 1;
	std::printf("synth_areas .mis export is idempotent, its areas by zone id\n");
	if (check_default_kit(root + "/fixtures/def/weapon.def") != 0) return 1;
	std::printf("the default kit is the one the loadout leads with, and builds its records\n");
	return 0;
}
