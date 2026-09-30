// The client side of the command map's squad and waypoint legs (D-HUD-19):
// the S2C folds (0x71 / 0x72 / 0x73 / 0x74 / 0x78 / 0x33 / 0x7C) through the
// replica pipeline into the runtime's consequences — the pool-4 waypoint
// rows, the HUD lines, the order lines the HUD draws — plus the CMAP
// screen's own world halves (world/user_waypoints.h) and the line composer
// (hud/squad_feed.h).
// [orig: NapiNPClientMsg_HandleSquadJoin @0x425600, NapiNPClientMsg_0x072
//  @0x425710, NapiNPClientMsg_0x073 @0x425770, NapiNPClientMsg_PlayerRecruited
//  @0x4258a0, NapiNPClientMsg_0x078 @0x425970, NapiNPClientMsg_0x033
//  @0x425fa0, NapiNPClientMsg_0x07C @0x426020, Waypoint_CreateForPlayer
//  @0x4dfcb0, CMap_HandleWaypointCreateConfirm @0x54a0d0,
//  CMap_DestroyFirstActiveLoadingEntity @0x547990,
//  Server_DestroyBatchedEntities @0x548110]

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/squad_messages.h>
#include <runtime/hud/squad_feed.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/world/user_waypoints.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
namespace w = opennova::world;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// A world with the local player at pool-0 slot 0 and a teammate at slot 1,
// pool 4 open for waypoints; roster slots 0 (local, "Lead") and 1 ("Mate").
struct Client {
	inmatch::ClientRuntime runtime{"SquadClient"};
	w::World world;
	w::EntityHandle local;
	w::EntityHandle mate;

	Client() {
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(4, 32);
		world.tables.user_waypoint_type_index = 7; // a def: the sweeps' `+0x1C` test
		w::Entity person;
		person.team = 1;
		person.display_name = "Lead";
		local = world.registry.spawn(0, person);
		person.display_name = "Mate";
		mate = world.registry.spawn(0, person);
		world.cached.local_player = local;
		runtime.view().set_viewer_handle(local.packed);
		bind(0, "Lead", local);
		bind(1, "Mate", mate);
	}
	void bind(uint8_t slot, const std::string &name, w::EntityHandle entity) {
		PlayerReplicationState sync;
		sync.player_slot = slot;
		sync.player_name = name;
		sync.entity_handle = entity.packed;
		sync.team = 1;
		runtime.view().apply(s2c::PLAYER_SYNC, encode_player_sync(sync));
	}
	int pool4_rows() {
		int n = 0;
		world.registry.for_each_in_pool(w::kUserWaypointPool, [&](const w::Entity &) { ++n; });
		return n;
	}
};

void test_join_and_breakup() {
	Client c;
	CHECK(c.runtime.local_roster_slot() == 0);
	// Mate joins the local player: the link, then the join line.
	SquadJoin join;
	join.leader = 0;
	join.member = 1;
	c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
	CHECK(c.runtime.state().roster[1].squad_leader == 0);
	c.runtime.apply_received_effects(c.world);
	auto lines = c.runtime.drain_squad_lines();
	CHECK(lines.size() == 1 && lines[0].kind == hud::SquadFeedLine::Kind::Join &&
			lines[0].text == "Mate");
	// A received waypoint (owner Mate) lands in pool 4 with its SYSTEM line.
	WaypointCreate create;
	create.name = "RALLY";
	create.x = 20 << 16;
	create.y = 30 << 16;
	create.owner_index = static_cast<uint8_t>(c.mate.slot());
	c.runtime.view().apply(s2c::WAYPOINT_CREATE, encode_waypoint_create(create));
	c.runtime.apply_received_effects(c.world);
	CHECK(c.pool4_rows() == 1);
	lines = c.runtime.drain_squad_lines();
	CHECK(lines.size() == 1 && lines[0].kind == hud::SquadFeedLine::Kind::WaypointReceived &&
			lines[0].text == "Waypoint \"RALLY\" received from Mate.");
	// The same share again replaces the row (same owner, point and name).
	c.runtime.view().apply(s2c::WAYPOINT_CREATE, encode_waypoint_create(create));
	c.runtime.apply_received_effects(c.world);
	CHECK(c.pool4_rows() == 1);
	// A local placement stays when the squad breaks up; the foreign row goes.
	// The sweep runs for a 0x71 [0xFF] whichever member it names (Mate here,
	// not the local slot) [orig: @0x4256db..0x4256e0, no member test].
	const w::EntityHandle own = w::place_user_waypoint(c.world, 5 << 16, 6 << 16, "MINE");
	CHECK(own.valid() && c.world.user_waypoints.count == 1 && c.pool4_rows() == 2);
	join.leader = 0xFF;
	c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
	c.runtime.apply_received_effects(c.world);
	CHECK(c.pool4_rows() == 1 && c.world.registry.get(own) != nullptr);
	CHECK(c.runtime.state().roster[1].squad_leader == 0xFF);
}

void test_orders_fireteam_go_code_and_destroy() {
	Client c;
	// The local player under Mate: a 0x73 on the local slot posts the line.
	SquadJoin join;
	join.leader = 1;
	join.member = 0;
	c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
	FireteamSet set;
	set.member = 0;
	set.fireteam = 2;
	c.runtime.view().apply(s2c::FIRETEAM_SET, encode_fireteam_set(set));
	CHECK(c.runtime.state().roster[0].fireteam == 2);
	// The order lines the HUD's two-line block draws.
	SquadOrder order;
	order.kind = 0;
	order.text = "Move to Bravo";
	c.runtime.view().apply(s2c::SQUAD_ORDER, encode_squad_order(order));
	CHECK(c.runtime.state().squad_orders[0] == "Move to Bravo");
	GoCode go;
	go.leader = 1;
	go.code = 5;
	c.runtime.view().apply(s2c::GO_CODE, encode_go_code(go));
	c.runtime.apply_received_effects(c.world);
	const auto lines = c.runtime.drain_squad_lines();
	CHECK(lines.size() == 2);
	if (lines.size() == 2) {
		CHECK(lines[0].kind == hud::SquadFeedLine::Kind::Fireteam && lines[0].value == 2);
		CHECK(lines[1].kind == hud::SquadFeedLine::Kind::GoCode && lines[1].value == 5);
	}
	// 0x7C destroys whatever pool row its handle names; 0xFFFF is ignored.
	const w::EntityHandle row = w::place_user_waypoint(c.world, 1 << 16, 1 << 16, "X");
	c.runtime.view().apply(s2c::DESTROY_ENTITY, encode_entity_handle16(0xFFFF));
	c.runtime.view().apply(s2c::DESTROY_ENTITY, encode_entity_handle16(row.packed));
	c.runtime.apply_received_effects(c.world);
	CHECK(c.world.registry.get(row) == nullptr);
}

// The local slot the squad folds compare is the latched player slot
// (retail's g_LocalPlayerEntity+0x154, stamped from the slot id 0x04 byte 17
// carries), not a roster scan for the viewer's entity: it holds while the
// local roster row has no entity bound (dead, spectating) and on a view with
// no viewer handle (the listen host's).
// [orig: NapiNPClientMsg_HandleSquadJoin @0x425666..0x42566c;
//  NapiNPClientMsg_0x073 @0x4257d6..0x4257dc; Server_PlayerAdd @0x51d08b]
void test_local_slot_is_the_latched_slot() {
	{
		Client c;
		c.runtime.view().set_local_player_slot(0);
		c.bind(0, "Lead", w::EntityHandle{static_cast<uint16_t>(0x00FF)}); // no entity
		CHECK(c.runtime.state().roster[0].entity_slot == -1);
		CHECK(c.runtime.local_roster_slot() == 0);
		SquadJoin join;
		join.leader = 0;
		join.member = 1;
		c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
		c.runtime.apply_received_effects(c.world);
		const auto lines = c.runtime.drain_squad_lines();
		CHECK(lines.size() == 1 && lines[0].kind == hud::SquadFeedLine::Kind::Join);
	}
	{
		Client c;
		c.runtime.view().set_viewer_handle(0xFFFF); // the listen host's view
		c.runtime.view().set_local_player_slot(1);
		CHECK(c.runtime.local_roster_slot() == 1);
		SquadJoin join;
		join.leader = 0;
		join.member = 1;
		c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
		FireteamSet set;
		set.member = 1;
		set.fireteam = 3;
		c.runtime.view().apply(s2c::FIRETEAM_SET, encode_fireteam_set(set));
		c.runtime.apply_received_effects(c.world);
		const auto lines = c.runtime.drain_squad_lines();
		CHECK(lines.size() == 1 && lines[0].kind == hud::SquadFeedLine::Kind::Fireteam &&
				lines[0].value == 3);
	}
}

void test_placed_waypoint_table() {
	Client c;
	// The name cuts at 31 characters; sixteen fit.
	const std::string long_name(40, 'N');
	const w::EntityHandle first = w::place_user_waypoint(c.world, 1 << 16, 2 << 16, long_name);
	CHECK(c.world.registry.get(first) != nullptr &&
			c.world.registry.get(first)->display_name == std::string(31, 'N'));
	CHECK(c.world.registry.get(first)->primary_occupant == c.local);
	for (int i = 1; i < 16; ++i)
		CHECK(w::place_user_waypoint(c.world, (i + 1) << 16, 2 << 16, "P").valid());
	CHECK(c.world.user_waypoints.count == 16);
	CHECK(!w::place_user_waypoint(c.world, 99 << 16, 2 << 16, "FULL").valid());
	// The delete button takes the first hovered one and closes the gap.
	CHECK(!w::delete_hovered_user_waypoint(c.world).valid());
	c.world.user_waypoints.entries[3].hover = true;
	const w::EntityHandle fourth = c.world.user_waypoints.entries[3].handle;
	const w::EntityHandle fifth = c.world.user_waypoints.entries[4].handle;
	CHECK(w::delete_hovered_user_waypoint(c.world) == fourth);
	CHECK(c.world.registry.get(fourth) == nullptr && c.world.user_waypoints.count == 15);
	CHECK(c.world.user_waypoints.entries[3].handle == fifth &&
			!c.world.user_waypoints.entries[15].handle.valid());
	// CLEAR_WAYPOINTS destroys the rest in table order.
	std::vector<w::EntityHandle> removed;
	w::clear_user_waypoints(c.world, removed);
	CHECK(removed.size() == 15 && removed[0] == first && c.world.user_waypoints.count == 0);
}

void test_feed_lines() {
	const hud::GameTextLookup text = [](const char *section, const char *key,
											  const char *fallback) -> std::string {
		const std::string s(section), k(key);
		if (s == "HUD" && k == "HUD_CMAP_JOIN") return "%s joined your squad";
		if (s == "HUD" && k == "HUD_CMAP_SETFIRETEAM") return "Fireteam: %s";
		if (s == "MENU" && k == "STR_CMAP_FIRETEAMB") return "Bravo";
		if (s == "HUD" && k == "HUD_CMAP_GOCODEXRAY") return "Go X-Ray";
		return fallback;
	};
	hud::SquadFeedLine line;
	line.kind = hud::SquadFeedLine::Kind::Join;
	line.text = "Mate";
	hud::SquadFeedPost post = hud::squad_feed_post(line, text);
	CHECK(post.post && !post.system_ring && post.argb == 0xFFF0F000u &&
			post.text == "Mate joined your squad");
	line.kind = hud::SquadFeedLine::Kind::Fireteam;
	line.value = 2;
	CHECK(hud::squad_feed_post(line, text).text == "Fireteam: Bravo");
	line.kind = hud::SquadFeedLine::Kind::GoCode;
	line.value = 3;
	CHECK(hud::squad_feed_post(line, text).text == "Go X-Ray");
	line.value = 6;
	CHECK(!hud::squad_feed_post(line, text).post);
	line.kind = hud::SquadFeedLine::Kind::WaypointReceived;
	line.text = "Waypoint \"A\" received from B.";
	post = hud::squad_feed_post(line, text);
	CHECK(post.system_ring && post.argb == 0xFFFFFFFFu && post.text == line.text);
}

} // namespace

int main() {
	test_join_and_breakup();
	test_orders_fireteam_go_code_and_destroy();
	test_local_slot_is_the_latched_slot();
	test_placed_waypoint_table();
	test_feed_lines();
	if (failures == 0) std::printf("squad_client: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
