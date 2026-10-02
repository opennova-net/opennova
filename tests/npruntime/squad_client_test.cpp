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
#include <runtime/inmatch/loopback_channel.h>
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
// The loopback form is the listen host's own client, whose squad sends reach
// `wire` (the host side reads them back).
struct Client {
	replication::LoopbackChannel wire;
	inmatch::ClientRuntime runtime;
	w::World world;
	w::EntityHandle local;
	w::EntityHandle mate;

	Client() : runtime("SquadClient") { setup(); }
	explicit Client(bool /*loopback*/) : runtime(wire) { setup(); }

	void setup() {
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

// The duplicate wipe compares the row's x / y dwords exactly: past 256 units
// a float round trip drops the low bits, and a re-placed or re-shared point
// must still wipe its old row. The table is not told of the wipe: its entry
// keeps naming the row, which the allocator re-takes as the first free one.
// [orig: Waypoint_CreateForPlayer @0x4dfdba / @0x4dfdc6 (the compares),
//  memset @0x4dfde6, Pool_AllocEntry(4, 1) @0x4dfdf4]
void test_waypoint_wipe_is_exact() {
	Client c;
	const int32_t x = (300 << 16) + 1;
	const int32_t y = -((700 << 16) + 3);
	const w::EntityHandle first = w::place_user_waypoint(c.world, x, y, "RALLY");
	CHECK(first.valid() && c.pool4_rows() == 1);
	CHECK(c.world.registry.get(first)->waypoint_x_q16 == x &&
			c.world.registry.get(first)->waypoint_y_q16 == y);
	CHECK(w::user_waypoint_row(c.world, first).x == x &&
			w::user_waypoint_row(c.world, first).y == y);
	const w::EntityHandle second = w::place_user_waypoint(c.world, x, y, "rally");
	CHECK(c.pool4_rows() == 1);
	CHECK(second == first);
	CHECK(c.world.user_waypoints.count == 2 && c.world.user_waypoints.entries[0].handle == first &&
			c.world.user_waypoints.entries[1].handle == first);
	// One low bit apart is another point: no wipe.
	w::place_user_waypoint(c.world, x + 1, y, "RALLY");
	CHECK(c.pool4_rows() == 2);
	// A received share at a far point replaces its own row the same way.
	WaypointCreate create;
	create.name = "FAR";
	create.x = (900 << 16) + 5;
	create.y = (1000 << 16) + 7;
	create.owner_index = static_cast<uint8_t>(c.mate.slot());
	for (int i = 0; i < 2; ++i) {
		c.runtime.view().apply(s2c::WAYPOINT_CREATE, encode_waypoint_create(create));
		c.runtime.apply_received_effects(c.world);
	}
	CHECK(c.pool4_rows() == 3);
}

// A table entry whose row was wiped reads as retail's stale pointer: a
// zeroed row until the allocator re-takes it. The squad-join push sends one
// C2S 0x17 per non-null entry with the row's exact dwords, the wiped one
// empty at the origin.
// [orig: Server_BroadcastChatToAllPlayers @0x549200 ->
//  NetPacket_WriteTypeNameAndPosition @0x42b160]
void test_member_join_pushes_each_entry() {
	Client c(true);
	// A foreign row takes slot 0, the local placement slot 1; the foreign row
	// goes, so the local re-placement wipes slot 1 and lands in slot 0.
	WaypointCreate create;
	create.name = "THEIRS";
	create.x = 1 << 16;
	create.y = 1 << 16;
	create.owner_index = static_cast<uint8_t>(c.mate.slot());
	c.runtime.view().apply(s2c::WAYPOINT_CREATE, encode_waypoint_create(create));
	c.runtime.apply_received_effects(c.world);
	c.runtime.drain_squad_lines();
	const int32_t x = (400 << 16) + 9;
	const int32_t y = (260 << 16) + 1;
	CHECK(c.runtime.place_user_waypoint(c.world, x, y, "HOLD"));
	const w::EntityHandle placed = c.world.user_waypoints.entries[0].handle;
	CHECK(placed.slot() == 1);
	c.runtime.view().apply(s2c::DESTROY_ENTITY,
			encode_entity_handle16(w::EntityHandle::make(w::kUserWaypointPool, 0).packed));
	c.runtime.apply_received_effects(c.world);
	CHECK(c.runtime.place_user_waypoint(c.world, x, y, "HOLD"));
	CHECK(c.world.user_waypoints.count == 2 &&
			c.world.user_waypoints.entries[1].handle.slot() == 0);
	const w::UserWaypointRow stale = w::user_waypoint_row(c.world, placed);
	CHECK(stale.x == 0 && stale.y == 0 && stale.z == 0 && stale.name.empty());
	c.runtime.flush_host_sends(); // the placements' own shares leave first
	c.wire.clear();
	// Mate joins the local player's squad: the push.
	SquadJoin join;
	join.leader = 0;
	join.member = 1;
	c.runtime.view().apply(s2c::SQUAD_JOIN, encode_squad_join(join));
	c.runtime.apply_received_effects(c.world);
	c.runtime.flush_host_sends(); // the host frame's post-effects send
	std::vector<WaypointShare> shares;
	replication::Datagram d;
	while (c.wire.host_recv(d)) {
		if (d.tag == c2s::WAYPOINT_SHARE)
			shares.push_back(decode_waypoint_share(d.body.data(), d.body.size()));
	}
	CHECK(shares.size() == 2);
	if (shares.size() == 2) {
		CHECK(shares[0].target == 1 && shares[0].name.empty() && shares[0].x == 0 &&
				shares[0].y == 0 && shares[0].z == 0);
		CHECK(shares[1].target == 1 && shares[1].name == "HOLD" && shares[1].x == x &&
				shares[1].y == y);
	}
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
	test_waypoint_wipe_is_exact();
	test_member_join_pushes_each_entry();
	test_feed_lines();
	if (failures == 0) std::printf("squad_client: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
