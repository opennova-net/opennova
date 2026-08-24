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

#include <netsim/client_roster_tags.h>
#include <netsim/client_state.h>

using namespace opennova;
using namespace opennova::netsim;

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
	bind_slot(s, 0, "Self", 1);
	bind_slot(s, 1, "Ace", 2);
	bind_slot(s, 2, "Bee", 3, 87, true);
	bind_slot(s, 3, "Foe", 4);
	bind_slot(s, 4, "Cargo", 5);
	bind_slot(s, 5, "Ghost", -1); // bound, no entity

	std::vector<world::FriendlyTagSource> tags;
	collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags,
			[](uint16_t) { return 80; });
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
	collect_roster_tags(s, 0x0001, 1, false, 0, tags);
	CHECK(tags.empty());
	// The death screen lifts both the team gate and the game-type gate
	// [orig: @0x5a4564 / @0x5a4576]: the enemy joins, self and hidden stay out.
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, true, 0, tags);
	CHECK(tags.size() == 3);
	// No def hp -> max 1: any positive health is the full good tier
	// [orig: `if (!max) max = 1` @0x5a3b95].
	tags.clear();
	collect_roster_tags(s, 0x0001, 1, false, 0x30020u, tags);
	for (const world::FriendlyTagSource &t : tags)
		if (t.name == "Ace") CHECK(t.health_ratio_fp16 == 0x10000);
}

} // namespace

int main() {
	test_walk_and_gates();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_roster_tags_test OK\n");
	return 0;
}
