// The joiner's player NetId fallback (D-NET-853): every client handler that stores a
// player entity's NetId (Flags & 0x100) tests it against the joiner's own avatar table and,
// when the table lacks it, stores the first combo of the alignment the entity's Team picks
// (Team 1 the good side, any other Team the evil); an empty table packs 0.
// [orig: CAvatarDefs_HasPackedId @0x57B140; CAvatarDefs_PackFirstComboOfAlignment @0x57AD40 -
//  the side loop @0x57AD6B..0x57AD80, an empty table's 0 @0x57AE4D; the sites:
//  NapiNPClientMsg_0x00C @0x42E730 (@0x42EAC5..0x42EB03), NapiNPClientMsg_FullEntitySpawn
//  @0x433780 (@0x433DB2..0x433E07), NapiNPClientMsg_TeamAssign @0x431910 (@0x431B4D..0x431B77),
//  NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 (@0x431CB4..0x431CDE)]
// The table is minted here: two combos per side, interleaved, so the first of a side is
// not entry 0, and a wire id from a nationality the table does not carry.
#include <cstdint>
#include <cstdio>
#include <vector>

#include <formats/avatars/avatars.h>
#include <net/npwire/character_id.h>
#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/replication/client_replica_pipeline.h>
#include "../common/title_fixture.h"

using namespace opennova;
using namespace opennova::replication;
using opennova::inmatch::CharacterRegistry;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

constexpr uint16_t kGoodA = character_id::pack(1, 1, 1, 0);
constexpr uint16_t kEvilB = character_id::pack(2, 1, 1, 1);
constexpr uint16_t kGoodC = character_id::pack(3, 2, 4, 0);
constexpr uint16_t kEvilD = character_id::pack(4, 2, 4, 1);
// No combo of the minted table carries nationality 9.
constexpr uint16_t kForeign = character_id::pack(9, 1, 1, 0);

CharacterRegistry minted_table() {
	CharacterRegistry table;
	table.add_entry(0, 0, 0, 1, 1, 1, avatars::AVATAR_ALIGN_GOOD, 0, false);
	table.add_entry(1, 0, 0, 2, 1, 1, avatars::AVATAR_ALIGN_EVIL, 0, false);
	table.add_entry(2, 0, 0, 3, 2, 4, avatars::AVATAR_ALIGN_GOOD, 0, false);
	table.add_entry(3, 0, 0, 4, 2, 4, avatars::AVATAR_ALIGN_EVIL, 0, false);
	return table;
}

std::vector<uint8_t> organic_spawn(uint16_t handle, uint8_t team, uint16_t net_id,
		uint16_t flags) {
	OrganicSpawnRecord rec;
	rec.slot_id = handle;
	rec.def_type = 3; // ItemDefType person
	rec.item_type_id = kPlayerPersonTypeId;
	rec.entity_name = "player";
	rec.minimap_flags = flags;
	rec.team = team;
	rec.anim_slot = 2;
	rec.net_id = net_id;
	rec.player_class = 5;
	OrganicSpawnBatch batch;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	return encode_organic_spawn_batch(batch);
}

uint16_t net_id_of(const ClientReplicaPipeline &view, uint16_t handle) {
	const ClientEntityState *row = view.state().find(handle);
	return row != nullptr ? row->net_id : 0xFFFFu;
}

// S2C 0x0C: a player's NetId the table lacks takes its Team side's first combo; an id the
// table holds stands whatever its side (the test has no side filter); a record without
// the Player bit keeps its word; no registry passes the id through; an empty one packs 0.
void test_organic_spawn_restamps_a_foreign_player_id() {
	const CharacterRegistry table = minted_table();
	ClientReplicaPipeline view;
	test_titles::jo_folds(view);
	view.set_character_registry(&table);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0001, 1, kForeign, 0x0100u));
	CHECK(net_id_of(view, 0x0001) == kGoodA);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0002, 2, kForeign, 0x0100u));
	CHECK(net_id_of(view, 0x0002) == kEvilB);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0003, 0, kForeign, 0x0100u));
	CHECK(net_id_of(view, 0x0003) == kEvilB);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0004, 2, kGoodC, 0x0100u));
	CHECK(net_id_of(view, 0x0004) == kGoodC);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0005, 1, kForeign, 0x0000u));
	CHECK(net_id_of(view, 0x0005) == kForeign);

	ClientReplicaPipeline bare;
	test_titles::jo_folds(bare);
	bare.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0001, 1, kForeign, 0x0100u));
	CHECK(net_id_of(bare, 0x0001) == kForeign);

	const CharacterRegistry empty;
	ClientReplicaPipeline emptied;
	test_titles::jo_folds(emptied);
	emptied.set_character_registry(&empty);
	emptied.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0001, 1, kForeign, 0x0100u));
	CHECK(net_id_of(emptied, 0x0001) == 0);
}

// S2C 0x18, the full-slot record: the same test after the record's stores.
void test_full_entity_spawn_restamps_a_foreign_player_id() {
	const CharacterRegistry table = minted_table();
	ClientReplicaPipeline view;
	test_titles::jo_folds(view);
	view.set_character_registry(&table);
	FullEntitySpawnRecord rec;
	rec.slot_id = 0x0010u;
	rec.item_type = 3;
	rec.item_type_id = kPlayerPersonTypeId;
	rec.team = 2;
	rec.minimap_flags = 0x0100u;
	rec.net_id = kForeign;
	view.apply(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(rec));
	CHECK(net_id_of(view, 0x0010) == kEvilB);
	rec.slot_id = 0x0011u;
	rec.minimap_flags = 0;
	view.apply(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(rec));
	CHECK(net_id_of(view, 0x0011) == kForeign);
	CHECK(view.malformed_bodies() == 0);
}

// S2C 0x50: the Team lands first (off the authority), then the NetId test reads it.
void test_team_assign_restamps_a_foreign_player_id() {
	const CharacterRegistry table = minted_table();
	ClientReplicaPipeline view;
	test_titles::jo_folds(view);
	view.set_character_registry(&table);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0006, 1, kGoodA, 0x0100u));
	TeamAssign assign;
	assign.entity_handle = 0x0006;
	assign.team = 2;
	assign.net_id = kForeign;
	assign.anim_slot = 7;
	view.apply(s2c::TEAM_ASSIGN, encode_team_assign(assign));
	const ClientEntityState *row = view.state().find(0x0006);
	CHECK(row != nullptr && row->team == 2 && row->net_id == kEvilB &&
			row->spawn_anim_slot == 7);
	assign.net_id = kEvilD;
	view.apply(s2c::TEAM_ASSIGN, encode_team_assign(assign));
	CHECK(net_id_of(view, 0x0006) == kEvilD);
}

// S2C 0x51, one team-change entry: the same test after the entry's Team store.
void test_team_change_confirm_restamps_a_foreign_player_id() {
	const CharacterRegistry table = minted_table();
	ClientReplicaPipeline view;
	test_titles::jo_folds(view);
	view.set_character_registry(&table);
	view.apply(s2c::ENTITY_SPAWN_BATCH, organic_spawn(0x0007, 2, kEvilB, 0x0100u));
	TeamAssign entry;
	entry.entity_handle = 0x0007;
	entry.team = 1;
	entry.net_id = kForeign;
	entry.anim_slot = 5;
	view.apply(s2c::TEAM_CHANGE_CONFIRM, encode_team_change_confirm(0, entry));
	const ClientEntityState *row = view.state().find(0x0007);
	CHECK(row != nullptr && row->team == 1 && row->net_id == kGoodA &&
			row->spawn_anim_slot == 5);
}

} // namespace

int main() {
	test_organic_spawn_restamps_a_foreign_player_id();
	test_full_entity_spawn_restamps_a_foreign_player_id();
	test_team_assign_restamps_a_foreign_player_id();
	test_team_change_confirm_restamps_a_foreign_player_id();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_avatar_fallback: all passed\n");
	return 0;
}
