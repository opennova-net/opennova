// The joiner's PLAYER walk of the friendly-tags pass over ClientState
// (netsim/client_roster_tags.h): the roster slots with an entity are the
// visited set [orig: HUD_DrawFriendlyTagsPass @0x5a4507..0x5a4597], the
// drawer's entry bails (self, Flags & 1) and the team / game-type gates are
// applied, and every source carries the slot's downed facts (slot+0x10 /
// slot+0x2C) plus the row's dead bit.
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <runtime/replication/client_roster_tags.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

ClientEntityState &add_player(ClientState &s, uint16_t handle, uint8_t team,
		uint8_t flags, uint16_t health) {
	ClientEntityState &row = s.upsert(handle);
	row.type_id = 0x14B9;
	row.team = team;
	row.state_flags = flags;
	row.state_flags_known = true;
	row.health_word = health;
	row.health_known = true;
	row.x = 10 << 16;
	row.y = 20 << 16;
	row.z = 3 << 16;
	return row;
}

void bind_slot(ClientState &s, int slot, const char *name, int16_t entity_slot,
		uint8_t revive = 0, bool medic = false) {
	ClientRosterSlot &r = s.roster[static_cast<size_t>(slot)];
	r.bound = true;
	r.name = name;
	r.entity_slot = entity_slot;
	r.downed_revive_seconds = revive;
	r.medic_request_active = medic;
}

void test_walk_and_gates() {
	auto owned = std::make_unique<ClientState>();
	ClientState &s = *owned;
	add_player(s, 0x0001, 1, 0x00, 60); // self
	add_player(s, 0x0002, 1, 0x00, 40); // teammate, alive
	add_player(s, 0x0003, 1, 0x02, 0);  // teammate, dead
	add_player(s, 0x0004, 2, 0x00, 80); // enemy
	add_player(s, 0x0005, 1, 0x01, 80); // teammate, CARRIED (hidden)
	add_player(s, 0x0006, 1, 0x00, 80); // no slot owns it
	add_player(s, 0x0007, 0, 0x00, 80); // neutral is not a teammate
	bind_slot(s, 0, "Self", 1);
	bind_slot(s, 1, "Ace", 2);
	bind_slot(s, 2, "Bee", 3, 87, true);
	bind_slot(s, 3, "Foe", 4);
	bind_slot(s, 4, "Cargo", 5);
	bind_slot(s, 5, "Ghost", -1); // bound, no entity
	bind_slot(s, 6, "Neutral", 7);

	std::vector<world::FriendlyTagSource> tags;
	collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags,
			[](uint16_t) { return 80; }, nullptr);
	CHECK(tags.size() == 2);
	bool saw_ace = false;
	bool saw_bee = false;
	for (const world::FriendlyTagSource &t : tags) {
		CHECK(t.player && t.has_slot);
		if (t.name == "Ace") {
			saw_ace = true;
			CHECK(!t.dead && t.revive_seconds == 0 && !t.medic_request);
			CHECK(t.health_ratio_fp16 == 0x8000); // 40/80
			CHECK(t.position.x == 10.0f && t.position.z == 3.0f);
			CHECK(t.net_id == 0x0002);
		}
		if (t.name == "Bee") {
			saw_bee = true;
			CHECK(t.dead && t.revive_seconds == 87 && t.medic_request);
			CHECK(t.health_ratio_fp16 == 0);
		}
	}
	CHECK(saw_ace && saw_bee);

	// Game type 0 without the death screen draws nothing [orig: @0x5a456d].
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, false, 0, tags, {}, nullptr);
	CHECK(tags.empty());
	// The death screen lifts both the team gate and the game-type gate
	// [orig: @0x5a4564 / @0x5a4576]: the enemy joins, self and hidden stay out.
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, true, 0, tags, {}, nullptr);
	CHECK(tags.size() == 4);
	// A team-0 local player keeps only the team-0 row [orig: @0x5a3c6b..0x5a3c95].
	tags.clear();
	collect_roster_tags(s, 0x0001, 0, false, 0x30020u, tags, {}, nullptr);
	CHECK(tags.size() == 1);
	if (tags.size() == 1) CHECK(tags[0].name == "Neutral");
	// No def hp -> max 1: any positive health is the full good tier
	// [orig: `if (!max) max = 1` @0x5a3b95].
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags, {}, nullptr);
	for (const world::FriendlyTagSource &t : tags)
		if (t.name == "Ace") CHECK(t.health_ratio_fp16 == 0x10000);
	// A row whose type has NO def takes the drawer's entry bail — the shared
	// predicate of world/friendly_tag_gates.h [orig: itemDef == NULL
	// @0x5a39fb]; the roster copy had dropped it.
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags,
			[](uint16_t) { return 0; }, nullptr);
	CHECK(tags.empty());
}

// The radio-request fold on a roster row [orig: the +885 latch @0x5a3bfe,
// Entity_FindChildByDefType(entity, 1, 1) @0x5a3c0e]: the receive-event
// 0x6D latch stands until the carrier walk over the joiner's materialized
// twins, from the row's echoed carrier handle, reaches a def-type-1 vehicle.
void test_radio_request_fold() {
	auto owned = std::make_unique<ClientState>();
	ClientState &s = *owned;
	add_player(s, 0x0001, 1, 0x00, 60); // self
	add_player(s, 0x0002, 1, 0x00, 40);
	add_player(s, 0x0003, 1, 0x00, 40);
	bind_slot(s, 0, "Self", 1);
	bind_slot(s, 1, "Ace", 2);
	bind_slot(s, 2, "Bee", 3);
	ClientEntityState &ace = *s.find(0x0002); // after the last upsert: the rows are a vector
	const RosterTagMaxHealth hp = [](uint16_t) { return 80; };
	auto gather = [&](const world::World *twins) {
		std::vector<world::FriendlyTagSource> tags;
		collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags, hp, twins);
		CHECK(tags.size() == 2);
		bool ace_request = false;
		for (const world::FriendlyTagSource &t : tags) {
			if (t.name == "Ace") ace_request = t.radio_request;
			if (t.name == "Bee") CHECK(!t.radio_request);
		}
		return ace_request;
	};
	CHECK(!gather(nullptr));
	ace.radio_request = 1;
	CHECK(gather(nullptr)); // no twins: the latch alone

	world::World w;
	w.registry.configure_pool(1, 4);
	world::Entity vehicle;
	vehicle.kind = world::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.item_type = 1;
	const world::EntityHandle veh = w.registry.spawn(1, vehicle);
	world::Entity deck = vehicle;
	deck.item_type = 2;
	const world::EntityHandle floor = w.registry.spawn(1, deck);
	CHECK(gather(&w)); // free-standing (0xFFFF carrier)
	ace.carrier_handle = veh.packed;
	CHECK(!gather(&w)); // aboard the vehicle twin
	ace.carrier_handle = floor.packed;
	CHECK(gather(&w)); // on a building floor: not a carrier ...
	w.registry.get(floor)->ground_target = veh;
	CHECK(!gather(&w)); // ... unless the floor itself rides the vehicle
}

} // namespace

int main() {
	test_walk_and_gates();
	test_radio_request_fold();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_roster_tags_test OK\n");
	return 0;
}
