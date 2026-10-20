// D-NET-348: a player's body is its class's charattr.def camouflage item, not a fixed type.
// The spawn picks the camouflage property by the mission's climate (1 JUNGLE_CAMMO, 2
// ARCTIC_CAMMO, any other DESERT_CAMMO), reads the class row's type and binds the first
// items.def row of that type, the first row of all for a type no row carries; the 0x0C
// organic record and the 0x0A snapshot carry that row's type word; the loadout accept
// rebinds the row by the class it stores.
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the climate pick @0x43C399..0x43C3BE,
//  ItemList_FindIndexByTypeId @0x43C3CA, ItemTypeIndex / itemDef @0x43C426 / @0x43C429;
//  CharAttr_GetCammoTypeId @0x4127B0; ItemList_FindIndexByTypeId @0x49E100 (0 for a miss
//  @0x49E12F); NetPacket_SerializeEntityStatesToBuffer - the def's type word
//  `movzx edi, word ptr [eax+50h]` @0x5031DD; NapiNPServerMsg_HandlePlayerLoadout
//  @0x515AF3..0x515B78]
// Every value is the fixture's own (two classes, two climates), core-only.

#include <formats/def/def.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/mission/item_traits.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "host_test_setup.h"

using namespace opennova;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// Row 0 the "Null" marker (type 0), then three persons: 5305 (hp 150), 5310 (hp 120),
// 5311 (hp 100); then 5312 (hp 0) and the type 0x8000 (id 132768, hp 90).
const char kItems[] =
		"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
		"begin \"Player #1, Multiplayer\"\r\n id 105305\r\n type person\r\n hp 150\r\nend\r\n"
		"begin \"Player #1, Single player\"\r\n id 105310\r\n type person\r\n hp 120\r\nend\r\n"
		"begin \"Generic Soldier\"\r\n id 105311\r\n type person\r\n hp 100\r\nend\r\n"
		"begin \"Hollow Soldier\"\r\n id 105312\r\n type person\r\n hp 0\r\nend\r\n"
		"begin \"High Soldier\"\r\n id 132768\r\n type person\r\n hp 90\r\nend\r\n";

// Classes 1..4 name 5310 everywhere; class 5 jungle 5310, desert 5305, arctic 5311; class 6
// jungle 9999 (no row: the first row), desert 5311, arctic 5305; class 7 5312 and class 8
// 5311 everywhere.
const char kCharattr[] =
		"[CHARACTER1]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER2]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER3]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER4]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER5]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5311\r\n"
		"[CHARACTER6]\r\nJUNGLE_CAMMO\t= 9999\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER7]\r\nJUNGLE_CAMMO\t= 5312\r\nDESERT_CAMMO\t= 5312\r\nARCTIC_CAMMO\t= 5312\r\n"
		"[CHARACTER8]\r\nJUNGLE_CAMMO\t= 5311\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5311\r\n";

struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	def::DefItemsFile items{};
	uint16_t next_net_id = 0xFF00;

	explicit Rig(int32_t climate, bool with_charattr = true) {
		CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
				sizeof(kItems) - 1, &items) == 0);
		world->registry.configure_pool(0, 8);
		mission::resolve_item_traits(*world, items, {}, nullptr);
		world->tables.bms_climate = climate;
		if (with_charattr) {
			inmatch::CharAttrTable table;
			inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(kCharattr),
					sizeof(kCharattr) - 1);
			CHECK(table.loaded);
			inmatch::install_class_attribute_flags(*world, nullptr, table);
		}
	}
	~Rig() { def::def_free_items(&items); }

	w::Entity *spawn(uint8_t player_class, uint8_t item_class = 0) {
		w::PlayerSpawn s;
		s.player_class = player_class;
		s.item_class = item_class;
		s.net_id = next_net_id++;
		const w::EntityHandle h = w::spawn_remote_player(*world, s);
		return world->registry.get(h);
	}
};

// The 0x0C organic record's type word for the body.
uint16_t record_type(const w::World &world, const w::Entity &body) {
	const OrganicSpawnBatch batch = replication::build_pool0_organic_batch(world);
	for (const OrganicSpawnRecord &rec : batch.records)
		if (rec.slot_id == body.handle.packed) return rec.item_type_id;
	return 0xFFFF;
}

void check_body(const Rig &r, const w::Entity *body, int32_t type, int32_t index, int16_t hp) {
	CHECK(body != nullptr);
	if (body == nullptr) return;
	CHECK(body->item_id == type);
	CHECK(body->item_type_index == index);
	CHECK(body->health == hp);
	CHECK(record_type(*r.world, *body) == static_cast<uint16_t>(type));       // the 0x0C type word
	CHECK(replication::snapshot_of(*body).type_id == static_cast<uint16_t>(type)); // the 0x0A's
}

// Desert (climate 0): class 5 binds 5305, class 6 binds 5311.
void test_desert_binds_each_class_row() {
	Rig r(0);
	check_body(r, r.spawn(5), 0x14B9, 1, 150);
	check_body(r, r.spawn(6), 0x14BF, 3, 100);
}

// Jungle (climate 1): class 5 binds 5310; class 6 names 9999, which no row carries, so the
// body is the first row's, type 0 (ItemList_FindIndexByTypeId's miss), and the 0x0C passes
// over a body of ItemTypeIndex 0 [orig: NetPacket_SerializeEntityStatesToBuffer - the +0x1C
// test @0x503134].
void test_jungle_binds_each_class_row_and_a_miss_reads_row_zero() {
	Rig r(1);
	check_body(r, r.spawn(5), 0x14BE, 2, 120);
	const w::Entity *miss = r.spawn(6);
	CHECK(miss != nullptr && miss->item_id == 0 && miss->item_type_index == 0);
	if (miss != nullptr) {
		CHECK(record_type(*r.world, *miss) == 0xFFFF);
		CHECK(replication::snapshot_of(*miss).type_id == 0);
	}
}

// Arctic (2) and any other selector: class 5 binds 5311 under 2, the desert row under 3.
void test_arctic_and_an_other_selector() {
	Rig arctic(2);
	check_body(arctic, arctic.spawn(5), 0x14BF, 3, 100);
	Rig other(3);
	check_body(other, other.spawn(5), 0x14B9, 1, 150);
}

// The add resolves by its class word ahead of its class stores and clamp: a joiner's word 3
// binds class 3's row (5310 here) while the class clamps to 8.
// [orig: Server_PlayerAdd @0x51D02B..0x51D040, the spawn @0x51D056, the stores @0x51D09A /
//  @0x51D0A8, the clamp @0x51D0FA..0x51D10E]
void test_the_add_resolves_by_its_own_class_word() {
	Rig r(0);
	w::Entity *body = r.spawn(8, 3);
	CHECK(body != nullptr && body->player_class == 8);
	check_body(r, body, 0x14BE, 2, 120);
}

// The loadout accept stores the class, then rebinds the row by it: the type word follows,
// the body's Health does not.
void test_the_loadout_accept_rebinds_the_row() {
	Rig r(0);
	w::Entity *body = r.spawn(5);
	CHECK(body != nullptr);
	if (body == nullptr) return;
	body->player_class = 6;
	w::rebind_player_camouflage_row(*r.world, *body);
	CHECK(body->item_id == 0x14BF && body->item_type_index == 3);
	CHECK(body->health == 150);
	CHECK(record_type(*r.world, *body) == 0x14BF);
}

// The accept's rebind reads the new def's hp word, a zero word included: the field-17
// tier's denominator is the def's (signed) word, which the body's copy follows.
// [orig: Entity_GetHealthClassification @0x4AD4E0 - the def @0x4AD4EF, `movsx eax, word
//  [eax+17Ch]` @0x4AD505]
void test_the_rebind_follows_the_new_def_hp_word() {
	Rig r(0);
	w::Entity *body = r.spawn(5);
	CHECK(body != nullptr && body->health_max == 150);
	if (body == nullptr) return;
	body->player_class = 7;
	w::rebind_player_camouflage_row(*r.world, *body);
	CHECK(body->item_id == 5312 && body->item_type_index == 4);
	CHECK(body->health_max == 0);
	CHECK(body->health == 150);
}

// A joiner's own body binds its record's type word read signed: a word of 0x8000 or more
// matches no row (the find compares the sign-extended dword) and binds the first row.
// [orig: NapiNPClientMsg_0x00C `movsx edx, ax` @0x42E840 -> ItemList_FindIndexByTypeId
//  @0x42E844, `cmp [ecx], esi` @0x49E120]
void test_the_record_type_word_reads_signed() {
	Rig r(0);
	w::PlayerSpawn s;
	s.player_class = 5;
	s.net_id = r.next_net_id++;
	s.from_wire_record = true;
	s.item_type_id = 0x14BE;
	const w::Entity *low = r.world->registry.get(w::spawn_remote_player(*r.world, s));
	CHECK(low != nullptr && low->item_id == 0x14BE && low->item_type_index == 2);
	s.net_id = r.next_net_id++;
	s.item_type_id = 0x8000; // the row of id 132768 carries the type 0x8000
	const w::Entity *high = r.world->registry.get(w::spawn_remote_player(*r.world, s));
	CHECK(high != nullptr && high->item_id == 0 && high->item_type_index == 0);
}

// The add's camouflage class is the joiner's class word, unclamped; a zero word reads 8 in a
// session and 1 out of one; the slot's class is the clamped word.
// [orig: Server_PlayerAdd @0x51D02B (the word), `neg is_in_session; sbb; and 7; add 1`
//  @0x51D033..0x51D040, the spawn @0x51D056, the clamp @0x51D0FA..0x51D10E]
void test_the_add_arm_picks_its_camouflage_class() {
	struct Case {
		uint8_t word;
		uint8_t in_session;
		int32_t type;
		int32_t index;
		uint8_t slot_class;
	};
	const Case cases[] = {
		{0, 1, 0x14BF, 3, 8}, // class 8's desert row
		{0, 0, 0x14BE, 2, 8}, // class 1's
		{3, 1, 0x14BE, 2, 8}, // class 3's row; the class clamps to 8
		{6, 1, 0x14BF, 3, 6},
	};
	for (const Case &c : cases) {
		Rig r(0);
		inmatch::NapiNPServerCtx ctx;
		inmatch::GameConfig settings;
		settings.max_players = 4;
		inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, settings);
		ctx.world = r.world.get();
		ctx.is_in_session = c.in_session;
		inmatch::Server_InitNewRoundState(ctx);
		inmatch::NapiNPConnection joiner;
		joiner.type = 1;
		joiner.connection_id = inmatch::kFirstJoinerDcb;
		joiner.self_id_seen = true;
		joiner.phase = inmatch::ConnectionPhase::Joined;
		joiner.char_vars.char_class[0] = c.word;
		joiner.char_vars.char_class[1] = c.word;
		ctx.np_protocol.connection_list.push_back(joiner);
		CHECK(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *r.world) == 1);
		const inmatch::NapiNPConnection &added = ctx.np_protocol.connection_list.back();
		const w::Entity *body = r.world->registry.get(added.link.owned_entity);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		CHECK(body->item_id == c.type && body->item_type_index == c.index);
		CHECK(body->player_class == c.slot_class && added.slot_class == c.slot_class);
	}
}

// A peer that read no charattr.def spawns its players from the seed (the stock class-8
// desert row, 0x14B9).
void test_no_charattr_keeps_the_seed() {
	Rig r(1, /*with_charattr=*/false);
	const w::Entity *body = r.spawn(6);
	CHECK(body != nullptr && body->item_id == w::kPlayerInfantryTypeId &&
			body->item_type_index == 1);
}

} // namespace

int main() {
	test_desert_binds_each_class_row();
	test_jungle_binds_each_class_row_and_a_miss_reads_row_zero();
	test_arctic_and_an_other_selector();
	test_the_add_resolves_by_its_own_class_word();
	test_the_loadout_accept_rebinds_the_row();
	test_no_charattr_keeps_the_seed();
	test_the_rebind_follows_the_new_def_hp_word();
	test_the_record_type_word_reads_signed();
	test_the_add_arm_picks_its_camouflage_class();
	if (failures != 0) {
		std::printf("npruntime_player_camouflage_row: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_player_camouflage_row: OK\n");
	return 0;
}
