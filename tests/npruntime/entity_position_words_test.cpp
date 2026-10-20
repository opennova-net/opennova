// D-NET-507: an entity's position is retail's int32 16.16 words, kept from the BMS
// record through the spawn, a placement and every load-stream serializer, never a
// float round trip. Retail copies the record's dwords raw
// [orig: Entity_SpawnFromBMSRecord @0x40EB30..0x40EB3F], the AI slot takes them raw
// [orig: Entity_InitFromModel @0x40E113..0x40E127], a placement copies a marker's raw
// [orig: Server_PositionPlayerForSpawn @0x50CFB7..0x50CFE2 / @0x50D3DB..0x50D403], and the
// 0x0C / 0x0D / 0x0F / 0x10 / 0x20 writers read entity+4 / +8 / +0xC as dwords
// [orig: NetPacket_SerializeEntityStatesToBuffer @0x503250..0x503270;
//  NetPacket_SerializeEntityPoolToPacket_0 @0x503B00..0x503B24, the AI trailer @0x503D59 /
//  @0x503D74; NetPacket_WriteWorldStateLoad0x0F @0x502D3A..0x502D5C;
//  NetPacket_SerializePool2StaticToBuffer @0x5043A8..0x5043CA;
//  NetPacket_SerializeEntityPoolToPacket @0x503543..0x503567].
// The words are the four-topology parity suite's (parity-20261011): the TDH_I3A host's y
// 0xFBF9FF5F (ours sent 0xFBF9FF60), the ASH_I5A joiner's 0x0F x 0x01B8B335 (ours
// 0x01B8B334), the ASH_I5A trailer x -1389996326 (ours -1389996288), and a 21,000-unit
// word with low bits set; each loses bits through a float.

#include "conn_fixture.h"
#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/server_chat.h>
#include <formats/def/reserved_items.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <formats/mission/bms.h>
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/mission/promote.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/match.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/collision.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <variant>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;
namespace bms = opennova::bms;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr int32_t kTdhHostY = static_cast<int32_t>(0xFBF9FF5Fu);
constexpr int32_t kAshJoinerX = 0x01B8B335;
constexpr int32_t kAshTrailerX = -1389996326;
constexpr int32_t kFarZ = (21000 << 16) | 0x5F;

// The old path: the word through a float and back with to_fixed.
int32_t through_float(int32_t word) {
	return w::to_fixed(static_cast<float>(word / 65536.0));
}

bool same_words(const w::FixedVec3 &a, int32_t x, int32_t y, int32_t z) {
	return a.x == x && a.y == y && a.z == z;
}

// A placed row at mission yaw 100 keeps the Yaw word ((90 - 100) << 16) / 360 << 16
// with a truncating divide, 0xF8E40000; the S2C 0x21 streams its high half 0xF8E4.
// The wrapped-degree conversion floors 350 degrees to 0xF8E38E38 (one lower on the
// wire). [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66;
//  NetPacket_SerializeEntityEventToBuffer @0x5055A0 - `movzx ecx, word ptr [ecx+0Eh]`
//  @0x505685]
bool yaw_100_heading(int32_t heading) {
	return heading == static_cast<int32_t>(0xF8E40000u) && w::spawn_angle_bam(90 - 100) == heading &&
	       (static_cast<uint32_t>(w::bam_heading_from_mission_yaw_deg(100.0)) >> 16) == 0xF8E3u;
}

std::vector<opennova::ObjectiveEntityState> objective_states(opennova::replication::UdpSessionTransport &t) {
	std::vector<opennova::ObjectiveEntityState> out;
	opennova::replication::Datagram d;
	while (t.pop_outbound(d)) {
		if (d.tag != opennova::s2c::OBJECTIVE_ENTITY_STATE) continue;
		opennova::ObjectiveEntityState state;
		size_t consumed = 0;
		if (opennova::decode_objective_entity_state(d.body.data(), d.body.size(), state, consumed) &&
		    consumed == d.body.size())
			out.push_back(state);
	}
	return out;
}

bms::Entity record(bms::ItemType type, int32_t type_id, int32_t x, int32_t y, int32_t z) {
	bms::Entity e{};
	e.type = type;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

// The trait sweep's resolved row (+0x1C / +0x20), which the 0x10 and 0x20 empty-slot
// tests read.
void with_def(w::World &world, w::EntityHandle h) {
	w::Entity *e = world.registry.get(h);
	e->item_type_index = 1;
	e->has_item_def = true;
}

int words_are_lossy_through_a_float() {
	// The verdict's evidence: each word differs from its float round trip.
	if (!expect(through_float(kTdhHostY) == static_cast<int32_t>(0xFBF9FF60u) &&
	                    through_float(kAshJoinerX) == 0x01B8B334 &&
	                    through_float(kAshTrailerX) == -1389996288 &&
	                    through_float(kFarZ) != kFarZ,
	            "the verdict's words lose bits through a float"))
		return 1;
	// A float store truncates through to_fixed, the conversion the wire applied to a
	// float position: a float-moved entity's word is the one the old wire sent.
	w::PositionWord word;
	word = 440.7f;
	if (!expect(word.q16 == w::to_fixed(440.7f) && word.q16 == static_cast<int32_t>(440.7f * 65536.0),
	            "a float store truncates through to_fixed"))
		return 1;
	word = -11.2f;
	if (!expect(word.q16 == w::to_fixed(-11.2f), "a negative float store truncates toward zero"))
		return 1;
	std::printf("PASS words_are_lossy_through_a_float\n");
	return 0;
}

// (a) promotion keeps each record's words, on the entity, its spawn backup and the AI
// slot; (b) the 0x0C / 0x0D (+ AI trailer) / 0x10 / 0x20 encoders stream them unchanged.
int promotion_and_streams_keep_the_words() {
	bms::File m{};
	m.organics.push_back(record(bms::ItemType::Organic, 0, kAshJoinerX, kTdhHostY, kFarZ));
	m.organics[0].id = 31;
	m.items.push_back(record(bms::ItemType::Item, 164, kAshTrailerX, kTdhHostY, kFarZ));
	m.items[0].id = 21;
	m.buildings.push_back(record(bms::ItemType::Building, 700, kTdhHostY, kAshJoinerX, kFarZ));
	m.buildings[0].id = 41;
	m.markers.push_back(record(bms::ItemType::Marker, 6001, kFarZ, kAshTrailerX, kAshJoinerX));
	m.markers[0].id = 51;

	auto world = std::make_unique<w::World>();
	opennova::mission::promote_mission(m, *world, {});
	const w::EntityHandle organic = world->registry.find_by_net_id(31);
	const w::EntityHandle item = world->registry.find_by_net_id(21);
	const w::EntityHandle building = world->registry.find_by_net_id(41);
	const w::EntityHandle marker = world->registry.find_by_net_id(51);
	if (!expect(organic.valid() && item.valid() && building.valid() && marker.valid(),
	            "promotion places one entity per pool"))
		return 1;
	const w::Entity &o = *world->registry.get(organic);
	const w::Entity &i = *world->registry.get(item);
	const w::Entity &b = *world->registry.get(building);
	const w::Entity &k = *world->registry.get(marker);
	if (!expect(same_words(o.position.q16(), kAshJoinerX, kTdhHostY, kFarZ) &&
	                    same_words(i.position.q16(), kAshTrailerX, kTdhHostY, kFarZ) &&
	                    same_words(b.position.q16(), kTdhHostY, kAshJoinerX, kFarZ) &&
	                    same_words(k.position.q16(), kFarZ, kAshTrailerX, kAshJoinerX),
	            "promotion keeps each record's three dwords"))
		return 1;
	if (!expect(same_words(i.spawn_position.q16(), kAshTrailerX, kTdhHostY, kFarZ),
	            "the spawn backup keeps the words"))
		return 1;
	const w::AiEntity *ai = world->ai.for_handle(organic);
	if (!expect(ai != nullptr && ai->self->position.x.q16 == kAshJoinerX && ai->self->position.y.q16 == kTdhHostY &&
	                    ai->self->position.z.q16 == kFarZ,
	            "the AI slot takes the entity's words"))
		return 1;

	with_def(*world, organic);
	with_def(*world, item);
	with_def(*world, building);
	with_def(*world, marker);
	world->registry.get(organic)->item_type = 3;      // a person def: the 0x0C's full form
	world->registry.get(item)->is_ai_capable = true; // carries the 0x0D AI trailer

	// 0x0C
	const opennova::OrganicSpawnBatch c = ns::build_pool0_organic_batch(*world);
	opennova::OrganicSpawnBatch c_out;
	const std::vector<uint8_t> c_wire = opennova::encode_organic_spawn_batch(c);
	if (!expect(opennova::decode_organic_spawn_batch(c_wire.data(), c_wire.size(), c_out) &&
	                    c_out.records.size() == 1 && c_out.records[0].pos_x == kAshJoinerX &&
	                    c_out.records[0].pos_y == kTdhHostY && c_out.records[0].pos_z == kFarZ,
	            "the 0x0C streams the organic's words"))
		return 1;
	// 0x0D and its AI trailer (the slot's +0x10 / +0x14, the spawn words)
	const opennova::PoolSpawnBatch d = ns::build_pool1_spawn_batch(*world);
	opennova::PoolSpawnBatch d_out;
	const std::vector<uint8_t> d_wire = opennova::encode_pool_spawn_batch(d);
	if (!expect(opennova::decode_pool_spawn_batch(d_wire.data(), d_wire.size(), d_out) &&
	                    d_out.records.size() == 1 && d_out.records[0].pos_x == kAshTrailerX &&
	                    d_out.records[0].pos_y == kTdhHostY && d_out.records[0].pos_z == kFarZ,
	            "the 0x0D streams the item's words"))
		return 1;
	if (!expect(d_out.records[0].ai_profile_1 == static_cast<uint32_t>(kAshTrailerX) &&
	                    d_out.records[0].ai_profile_2 == static_cast<uint32_t>(kTdhHostY),
	            "the 0x0D AI trailer streams the spawn words"))
		return 1;
	// 0x10
	const opennova::StaticEntityBatch s = ns::build_pool2_static_batch(*world);
	opennova::StaticEntityBatch s_out;
	const std::vector<uint8_t> s_wire = opennova::encode_static_entity_batch(s);
	if (!expect(opennova::decode_static_entity_batch(s_wire.data(), s_wire.size(), s_out) &&
	                    s_out.records.size() == 1 && s_out.records[0].pos_x == kTdhHostY &&
	                    s_out.records[0].pos_y == kAshJoinerX && s_out.records[0].pos_z == kFarZ,
	            "the 0x10 streams the building's words"))
		return 1;
	// 0x20
	const opennova::Pool3SyncBatch p = ns::build_pool3_marker_batch(*world);
	opennova::Pool3SyncBatch p_out;
	const std::vector<uint8_t> p_wire = opennova::encode_pool3_sync_batch(p);
	if (!expect(opennova::decode_pool3_sync_batch(p_wire.data(), p_wire.size(), p_out) &&
	                    p_out.records.size() == 1 && p_out.records[0].pos_x == kFarZ &&
	                    p_out.records[0].pos_y == kAshTrailerX && p_out.records[0].pos_z == kAshJoinerX,
	            "the 0x20 streams the marker's words"))
		return 1;
	std::printf("PASS promotion_and_streams_keep_the_words\n");
	return 0;
}

// (c) a placement copies the start marker's words onto the player, and the 0x0F
// streams them.
int placement_and_world_state_load_keep_the_words() {
	auto world_storage = std::make_unique<w::World>();
	w::World &world = *world_storage;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // the host player's start
	start.position.set_q16({kAshJoinerX, kTdhHostY, kFarZ});
	start.item_type_index = 1;
	start.has_item_def = true;
	const w::EntityHandle marker = world.registry.spawn_at(w::EntityHandle::make(3, 0), start);
	if (!expect(marker.valid(), "the start marker spawns")) return 1;

	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                             inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback);
	ctx.world = &world;
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	const w::Entity *player = world.registry.get(conn.link.owned_entity);
	if (!expect(player != nullptr &&
	                    same_words(player->position.q16(), kAshJoinerX, kTdhHostY, kFarZ),
	            "the placement copies the marker's words onto the player"))
		return 1;

	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);
	const std::vector<uint8_t> *body = nullptr;
	for (const inmatch::InitialStateMessage &m : emitted)
		if (m.tag == 0x0F) body = &m.body;
	opennova::WorldStateLoad state;
	if (!expect(reached && body != nullptr &&
	                    opennova::decode_world_state_load(body->data(), body->size(), state),
	            "the burst carries a decodable 0x0F"))
		return 1;
	if (!expect(state.pos_x == kAshJoinerX && state.pos_y == kTdhHostY && state.pos_z == kFarZ,
	            "the 0x0F streams the player's words"))
		return 1;
	std::printf("PASS placement_and_world_state_load_keep_the_words\n");
	return 0;
}

// An AI slot reads and writes its entity's words (no second store), and a
// restore that replaces the registry's rows rebinds every slot to its row.
// [orig: Entity_InitFromModel @0x40DC30 - the slot's spawn copy @0x40E115..0x40E127;
//  the AI reads entity+4/+8/+0xC through the entity it belongs to]
int ai_slot_reads_its_entity_words() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	w::Entity seed;
	seed.position.set_q16({kAshJoinerX, kTdhHostY, kFarZ});
	const w::EntityHandle h = world->registry.spawn(0, seed);
	w::AiEntity *ae = world->ai.at(world->ai.attach(h));
	if (!expect(ae->self == world->registry.get(h) &&
	                    same_words(ae->self->position.q16(), kAshJoinerX, kTdhHostY, kFarZ),
	            "the slot binds its row and reads its words"))
		return 1;
	world->ai.capture_spawn_baseline();
	const w::World::Snapshot baseline = world->snapshot();
	ae->self->position.x.q16 = kAshTrailerX;
	if (!expect(world->registry.get(h)->position.x.q16 == kAshTrailerX,
	            "a slot write is the entity's word"))
		return 1;
	world->restore(baseline);
	ae = world->ai.for_handle(h);
	if (!expect(ae != nullptr && ae->self == world->registry.get(h) &&
	                    same_words(ae->self->position.q16(), kAshJoinerX, kTdhHostY, kFarZ),
	            "the restore rebinds the slot to the restored row's words"))
		return 1;
	std::printf("PASS ai_slot_reads_its_entity_words\n");
	return 0;
}

// The chat / radio nearest-location walk on the words: wrapping X/Y deltas,
// the length truncated, strictly below the row's bound word and the best so
// far (the earliest row wins a tie).
// [orig: NapiNPServer_HandleChatMessage @0x513760 - @0x51398A / @0x51398D,
//  _ftol2_sse @0x5139B7, `cmp eax, [edi]` @0x5139BC, `cmp eax, ebx` @0x5139C0]
int nearest_location_takes_the_word_distances() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(3, 8);
	const int32_t x0 = kFarZ; // 21000 units with low bits: the float view's ulp is 128 words
	const auto location = [&](int32_t x, int32_t index, float bound) {
		w::Entity seed;
		seed.item_id = opennova::def::DEF_TYPE_NAMED_LOCATION;
		seed.position.set_q16({x, 0, 0});
		seed.location_index = index;
		seed.bound_radius = bound;
		return world->registry.spawn(3, seed);
	};
	// Row 0 one word farther than row 1 (a float view rounds both alike); row 2
	// ties row 1; row 3 sits exactly on its bound word.
	location(x0 - 0x10001, 10, 4.0f);
	location(x0 + 0x10000, 11, 4.0f);
	location(x0 - 0x10000, 12, 4.0f);
	location(x0 + 0x8000, 13, 0.5f);
	w::Entity sender;
	sender.position.set_q16({x0, 0, 0});
	if (!expect(inmatch::nearest_location_index(*world, sender) == 11,
	            "the word distances pick the nearer row; a tie keeps the earlier; the bound is strict"))
		return 1;
	std::printf("PASS nearest_location_takes_the_word_distances\n");
	return 0;
}

// The picked-zone placement on the words: the target's pose words, the z word
// lifted by 0x10000, and the 6007 scatter walk's radius test on the words.
// [orig: Server_PositionPlayerForSpawn @0x50CF60 - @0x50D01C; the 6007 walk
//  @0x50D098..0x50D0CD]
int zone_placement_keeps_the_words() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(2, 4);
	world->registry.configure_pool(3, 8);
	w::Entity zone;
	zone.has_item_def = true;
	zone.position.set_q16({kAshJoinerX, kTdhHostY, kAshJoinerX});
	const w::EntityHandle zone_h = world->registry.spawn(2, zone);
	w::SpawnPointResult placed = w::resolve_player_spawn_pose(*world, {}, zone_h, 0, 1, 0);
	if (!expect(placed.found && same_words(placed.position.q16(), kAshJoinerX, kTdhHostY,
	                                               kAshJoinerX + 0x10000),
	            "the target's words, the z word lifted by 0x10000"))
		return 1;
	// A 6007 row exactly on the zone's radius word stays in the walk.
	world->registry.get(zone_h)->zone_number = 1;
	world->registry.get(zone_h)->zone_radius = 3;
	w::Entity scatter;
	scatter.item_id = 6007;
	scatter.has_item_def = true;
	scatter.position.set_q16({kAshJoinerX + (3 << 16), kTdhHostY, 0});
	world->registry.spawn(3, scatter);
	world->zones.spawn_cycle_counter = 1; // the round-robin's second pick: the first 6007 row
	placed = w::resolve_player_spawn_pose(*world, {}, zone_h, 0, 1, 0);
	if (!expect(placed.found && placed.position.x.q16 == kAshJoinerX + (3 << 16),
	            "a 6007 row on the radius word is in the walk"))
		return 1;
	std::printf("PASS zone_placement_keeps_the_words\n");
	return 0;
}

// The 6007 walk compares the word length, not the float views': at 21,000 units
// (a float view's ulp is 128 words) a row one word past the radius word is out,
// though both float views round up and sit exactly 3.0 units apart.
// [orig: Server_PositionPlayerForSpawn @0x50CF60 - the radius `shl eax, 10h`
//  @0x50D03B, the deltas @0x50D098 / @0x50D09B, _ftol2_sse @0x50D0C4, `jg` @0x50D0CD]
int scatter_walk_takes_the_word_length() {
	const int32_t zone_x = (21000 << 16) + 0x41;
	const int32_t row_x = zone_x + (3 << 16) + 1;
	if (!expect(through_float(row_x) - through_float(zone_x) == (3 << 16),
	            "the float views sit exactly on the radius"))
		return 1;
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(2, 4);
	world->registry.configure_pool(3, 8);
	w::Entity zone;
	zone.has_item_def = true;
	zone.zone_number = 1;
	zone.zone_radius = 3;
	zone.position.set_q16({zone_x, 0, 0});
	const w::EntityHandle zone_h = world->registry.spawn(2, zone);
	w::Entity scatter;
	scatter.item_id = 6007;
	scatter.has_item_def = true;
	scatter.position.set_q16({row_x, 0, 0});
	world->registry.spawn(3, scatter);
	world->zones.spawn_cycle_counter = 1; // the float test would pick the row here
	const w::SpawnPointResult placed = w::resolve_player_spawn_pose(*world, {}, zone_h, 0, 1, 0);
	if (!expect(placed.found && placed.position.x.q16 == zone_x &&
	                    world->zones.spawn_cycle_counter == 1,
	            "a 6007 row one word past the radius word is out of the walk"))
		return 1;
	std::printf("PASS scatter_walk_takes_the_word_length\n");
	return 0;
}

// The gnl2 expiry's blast point, which the S2C item-explosion event streams:
// the row's X/Y words and its Z word raised by 0x10000, raw.
// [orig: Entity_HandleDeathEvent @0x4070F0 - the pose copy @0x4071F4..0x40722F,
//  `add eax, 10000h` @0x40720D]
int gnl2_blast_point_is_the_raised_z_word() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(1, 4);
	world->rules.logic_authority = true;
	world->rules.mp_session = true;
	w::Entity seed;
	seed.kind = w::EntityKind::Item;
	seed.item_id = 500;
	seed.has_item_def = true;
	seed.position.set_q16({kAshJoinerX, kTdhHostY, kFarZ});
	seed.heading = w::placed_heading_bam(100);
	seed.engine_flags = w::kEntityFlagDead; // the expiry leg (Flags & 2)
	const w::EntityHandle h = world->registry.spawn(1, seed);
	w::ItemDeathTraits traits;
	traits.death_class = w::item_death_class_from_tag("gnl2");
	world->tables.item_death_traits.set(500, traits);
	w::AmmoTableEntry blast_ammo;
	blast_ammo.valid = true;
	blast_ammo.name = w::kItemExplosionAmmo;
	world->tables.ammo.entries.push_back(blast_ammo);
	w::destruction_notify_item_damage(*world, *world->registry.get(h), 1);
	const w::ItemExplosionEvent *blast = nullptr;
	for (const auto &event : world->out.entity_events)
		if (const auto *x = std::get_if<w::ItemExplosionEvent>(&event)) blast = x;
	if (!expect(blast != nullptr &&
	                    same_words(blast->position, kAshJoinerX, kTdhHostY, kFarZ + 0x10000),
	            "the gnl2 blast point is the row's words, the z word raised by 0x10000"))
		return 1;
	if (!expect(blast->count == 60 && yaw_100_heading(blast->heading),
	            "the gnl2 heading is the row's Yaw word"))
		return 1;
	// The kill-zone entry's centre is the same words [orig: WeaponEffect_QueueExplosion
	//  @0x4E8330; Projectile_ProcessExplosionQueue @0x4EAD80 reads +0 / +4 / +8 in its
	//  `sub` @0x4EAF31 / @0x4EAF43 / @0x4EAF56, the deltas stored @0x4EAF33 / @0x4EAF46 /
	//  @0x4EAF59].
	if (!expect(world->explosions.queue.size() == 1 &&
	                    same_words(world->explosions.queue[0].pos.q16(), kAshJoinerX, kTdhHostY,
	                               kFarZ + 0x10000),
	            "the queued blast's centre is the raised words"))
		return 1;
	std::printf("PASS gnl2_blast_point_is_the_raised_z_word\n");
	return 0;
}

// The barrel's blast pose is the row's words (no raise) and its Yaw word, ten
// draws, no attacker. [orig: Entity_HandleDeathOnAuthority @0x407CC0 - the words
//  @0x407CDB..0x407CE4, the Yaw word @0x407CE8 / @0x407CEB, `push 0Ah` @0x407CEF,
//  `push 0` @0x407D04]
int barrel_blast_takes_the_yaw_word() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(1, 4);
	world->rules.logic_authority = true;
	world->rules.mp_session = true;
	w::Entity seed;
	seed.kind = w::EntityKind::Item;
	seed.item_id = 501;
	seed.has_item_def = true;
	seed.position.set_q16({kAshTrailerX, kTdhHostY, kFarZ});
	seed.heading = w::placed_heading_bam(100);
	seed.engine_flags = w::kEntityFlagDead;
	const w::EntityHandle h = world->registry.spawn(1, seed);
	w::ItemDeathTraits traits;
	traits.death_class = w::item_death_class_from_tag("brrl");
	world->tables.item_death_traits.set(501, traits);
	w::destruction_notify_item_damage(*world, *world->registry.get(h), 1);
	const w::ItemExplosionEvent *blast = nullptr;
	for (const auto &event : world->out.entity_events)
		if (const auto *x = std::get_if<w::ItemExplosionEvent>(&event)) blast = x;
	if (!expect(blast != nullptr && same_words(blast->position, kAshTrailerX, kTdhHostY, kFarZ) &&
	                    blast->count == 10 && blast->source == 0xFFFF &&
	                    yaw_100_heading(blast->heading),
	            "the barrel blast is the row's words and its Yaw word"))
		return 1;
	std::printf("PASS barrel_blast_takes_the_yaw_word\n");
	return 0;
}

// The static wreck slides by adding its velocity words to its position words.
// [orig: Entity_UpdateStaticDeathPhysics @0x494230 - `add [edi], eax` @0x4942D4,
//  `add [esi+8], eax` @0x4942EA, `add [esi+0Ch], eax` @0x4942F9]
int static_wreck_slides_on_the_words() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(1, 4);
	w::Entity seed;
	seed.kind = w::EntityKind::Item;
	seed.item_id = 502;
	seed.position.set_q16({kFarZ, kAshJoinerX, kTdhHostY});
	seed.engine_flags = w::kEntityFlagHusk;
	seed.death_motion = w::DeathMotionMode::Static;
	const w::EntityHandle h = world->registry.spawn(1, seed);
	w::Entity &e = *world->registry.get(h);
	e.veh.vel_x = 0x10001;
	e.veh.vel_y = -0x8003;
	e.veh.slide_z = 0x35;
	w::tick_item_death_motion(*world, e, nullptr, -1.0e9f, world->out.destruction);
	if (!expect(same_words(e.position.q16(), kFarZ + 0x10001, kAshJoinerX - 0x8003, kTdhHostY + 0x35),
	            "the wreck's slide is a word add"))
		return 1;
	std::printf("PASS static_wreck_slides_on_the_words\n");
	return 0;
}

// The S2C 0x2F streams a flag's words raw: the carry break's drop pose is the
// carrier's words, its home pose the spawn backup's.
// [orig: NetPacket_SerializeEntityWithParentAndTarget @0x505810 - entity+4 / +8 /
//  +0xC @0x505878 / @0x505889 / @0x50589F; Entity_SyncPositionFromDefinition
//  @0x43A9B0 - the snap @0x43AA7D..0x43AA96]
int flag_state_streams_the_words() {
	auto world = std::make_unique<w::World>();
	world->rules.mp_session = true;
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = opennova::game_type::kCaptureTheFlag;
	world->match.configure(rules);
	w::PlayerSpawn spawn;
	spawn.position.set_q16({kAshJoinerX, kTdhHostY, 2 << 16});
	spawn.team = 1;
	const w::EntityHandle carrier = w::spawn_remote_player(*world, spawn);
	world->match.upsert_player({carrier, 3, "Carrier"});
	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.item_id = 4091;
	flag_seed.has_item_def = true;
	flag_seed.alive = true;
	flag_seed.spawn_position.set_q16({kAshTrailerX, kFarZ, 2 << 16});
	flag_seed.position = spawn.position;
	flag_seed.primary_occupant = carrier;
	flag_seed.flags = w::kEntityFlagCarried;
	const w::EntityHandle flag = world->registry.spawn(1, flag_seed);
	world->registry.get(carrier)->mounted_child = flag;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = world.get();
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.config.flag_reset_seconds = 1;
	ctx.config.permanent_death = true;
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_seeded_conn(
			3, 1, &remote, ns::TransportMode::Client, carrier, true));
	// Run to the limit second (the carry breaks there): the sweep sends its drop and
	// re-sync 0x2F inline, ahead of that second's refresh (D-NET-NEW-V3-1).
	std::vector<opennova::ObjectiveEntityState> pair;
	for (int i = 0; i < 4 && world->registry.get(carrier)->mounted_child.valid(); ++i) {
		do {
			inmatch::Server_TickUpdate(ctx);
		} while (!ctx.world->match.periodic_second());
		pair = objective_states(remote);
	}
	if (!expect(pair.size() == 3 && pair[0].entity_handle == flag.packed &&
	                    pair[0].pos_x == kAshJoinerX && pair[0].pos_y == kTdhHostY &&
	                    pair[1].entity_handle == flag.packed && pair[1].pos_x == kAshTrailerX &&
	                    pair[1].pos_y == kFarZ,
	            "the drop and home 0x2F stream the carrier's and the spawn backup's words"))
		return 1;
	std::printf("PASS flag_state_streams_the_words\n");
	return 0;
}

// A ballistic round flies on its words: the spawn keeps the origin's words, a
// miss commits start + velocity as a word add, and the next tick starts from that
// commit. [orig: RoundData_SpawnRound @0x4EC0D0 - Position @0x4EC62B..0x4EC63A, the +0x80
//  copy @0x4EC643..0x4EC655;
//  Projectile_UpdatePhysics @0x4E9D70 - the end point @0x4EA232..0x4EA27D, the
//  commit @0x4EAA3F..0x4EAA45, `vz -= 167` @0x4EAA5A]
int round_flies_on_the_words() {
	auto world = std::make_unique<w::World>();
	w::AmmoTableEntry ammo;
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	world->tables.ammo.entries.push_back(ammo);
	w::RoundSpawnParams params;
	params.origin.set_q16({kFarZ, kAshJoinerX, kFarZ});
	params.dir_yaw_bam = 0x12340000;
	params.dir_pitch_bam = 0x01230000;
	params.ammo_index = 0;
	const int slot = world->round_sim.spawn(*world, params);
	if (!expect(slot >= 0, "the round spawns")) return 1;
	const w::LiveRound &r = world->round_sim.rounds[static_cast<size_t>(slot)];
	if (!expect(same_words(r.pos.q16(), kFarZ, kAshJoinerX, kFarZ) && r.prev_z_q16 == kFarZ,
	            "the round starts on the origin's words"))
		return 1;
	const w::FixedVec3 v0 = r.vel.q16();
	world->round_sim.tick(*world, nullptr);
	if (!expect(r.active && same_words(r.pos.q16(), kFarZ + v0.x, kAshJoinerX + v0.y, kFarZ + v0.z),
	            "a miss commits start + velocity on the words"))
		return 1;
	const w::FixedVec3 p1 = r.pos.q16();
	const w::FixedVec3 v1 = r.vel.q16();
	if (!expect(v1.z != v0.z, "gravity and drag step the velocity words")) return 1;
	world->round_sim.tick(*world, nullptr);
	if (!expect(same_words(r.pos.q16(), p1.x + v1.x, p1.y + v1.y, p1.z + v1.z),
	            "the next tick starts from the committed words"))
		return 1;
	std::printf("PASS round_flies_on_the_words\n");
	return 0;
}

// The 0x0C carries a mounted body's attachBoneId (+0x157), parentSlot (+0x168) and its
// parent's packed handle (+0x16C); an unmounted one 0 / 0 / 0xFFFF. Every body's record
// also carries the low byte of aiState (+0x2B4), the AI slot's +0x20 byte (the low byte of
// its tick accumulator), refNum (+0x215) and subType (+0x214), each of which a stock
// joiner stores.
// [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030A0 - @0x503353, @0x50336D,
//  @0x50337B..0x5033DD, the null 0xFFFF @0x5033EC; aiState @0x5032A3, the slot byte
//  @0x5032F3..0x5032F6, refNum @0x503329, subType @0x50333E; NapiNPClientMsg_0x00C
//  @0x42E730 - @0x42E991, @0x42E9EB, @0x42EA20, @0x42EA35]
int organic_record_streams_the_mount() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(1, 4);
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 700;
	vehicle.has_item_def = true;
	const w::EntityHandle carrier = world->registry.spawn(1, vehicle);
	w::Entity seed;
	seed.kind = w::EntityKind::Organic;
	seed.item_id = 5311;
	seed.item_type = 3;
	seed.item_type_index = 1;
	seed.has_item_def = true;
	const w::EntityHandle gunner = world->registry.spawn(0, seed);
	const w::EntityHandle walker = world->registry.spawn(0, seed);
	world->ai.attach(gunner);
	world->ai.attach(walker);
	w::Entity &g = *world->registry.get(gunner);
	g.mounted = true;
	g.mount_target = carrier;
	g.mount_type = w::SeatType::Gunner;
	g.mount_bone = 4;
	g.ai_state = 0x0115;
	g.ref_num = 7;
	g.sub_type = 9;
	world->ai.for_handle(gunner)->brain.f[w::AiBrain::kTickAccum] = 0x1234;
	const opennova::OrganicSpawnBatch c = ns::build_pool0_organic_batch(*world);
	opennova::OrganicSpawnBatch out;
	const std::vector<uint8_t> wire = opennova::encode_organic_spawn_batch(c);
	if (!expect(opennova::decode_organic_spawn_batch(wire.data(), wire.size(), out) &&
	                    out.records.size() == 2,
	            "the 0x0C carries both bodies"))
		return 1;
	const opennova::OrganicSpawnRecord &mounted = out.records[0].slot_id == gunner.packed
	                                                      ? out.records[0] : out.records[1];
	const opennova::OrganicSpawnRecord &free = out.records[0].slot_id == gunner.packed
	                                                   ? out.records[1] : out.records[0];
	if (!expect(mounted.weapon_type == 4 && mounted.parent_slot == 3 &&
	                    mounted.parent_handle == carrier.packed,
	            "a mounted body streams its bone, seat type and parent handle"))
		return 1;
	if (!expect(free.weapon_type == 0 && free.parent_slot == 0 && free.parent_handle == 0xFFFF,
	            "an unmounted body streams 0 / 0 / 0xFFFF"))
		return 1;
	if (!expect(mounted.ai_state == 0x15 && mounted.ai_action == 0x34 && mounted.alert_level == 7 &&
	                    mounted.sub_type == 9,
	            "the record streams aiState's low byte, the slot's +0x20 byte, refNum and subType"))
		return 1;
	std::printf("PASS organic_record_streams_the_mount\n");
	return 0;
}

// The seat pose is the seat point rotated through the carrier's frame onto the
// carrier's Position words, a word add, and the occupant takes those words.
// [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 - outPos = Position @0x4b0cf1,
//  Math_FixedPointTransformPoint22 @0x4b0d42]
int seat_pose_is_words() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(1, 4);
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 700;
	vehicle.heading = w::placed_heading_bam(30);
	vehicle.position.set_q16({kFarZ, kAshJoinerX, kTdhHostY});
	const w::EntityHandle carrier = world->registry.spawn(1, vehicle);
	w::Entity rider;
	rider.kind = w::EntityKind::Organic;
	const w::EntityHandle occupant = world->registry.spawn(0, rider);
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	seat.seat_local = {1.25f, -0.5f, 0.75f};
	world->vehicles.pose_mounted_occupant(*world->registry.get(occupant), *world->registry.get(carrier), seat);
	// The oracle: the carrier frame's rotation of the raw seat point, added to the words.
	const int32_t origin[3] = {0, 0, 0};
	const w::CollisionMatrix m = w::collision_matrix_from_euler(w::spawn_angle_bam(90 - 30), 0, 0, origin);
	const int32_t local[3] = {static_cast<int32_t>(-0.5f * 65536.0f), static_cast<int32_t>(-1.25f * 65536.0f),
	                          static_cast<int32_t>(0.75f * 65536.0f)};
	int32_t rotated[3];
	m.rotate_point(local, rotated);
	if (!expect(same_words(world->registry.get(occupant)->position.q16(), kFarZ + rotated[0],
	                       kAshJoinerX + rotated[1], kTdhHostY + rotated[2]),
	            "the occupant takes the carrier's words plus the rotated seat point"))
		return 1;
	std::printf("PASS seat_pose_is_words\n");
	return 0;
}

// The ground sampler takes the words: the weights from the words' low halves (x, and
// -y) as rounded 16.16 products, four raw16 << 8 taps, each weighted and rounded, summed.
// [orig: Terrain_SampleHeightBilinear @0x6067B0 - the weights @0x606851 / @0x60686D /
//  @0x606889 / @0x60689D, the sum @0x606806]
int ground_sampler_takes_the_words() {
	std::vector<uint16_t> heights(1024 * 1024, 0);
	std::array<int, 256> sectors{};
	sectors.fill(1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heights.data();
	field.dim = 1024;
	field.layout.sector_grid = sectors.data();
	const auto at = [&](int x, int z) -> uint16_t & { return heights[static_cast<size_t>(z) * 1024 + x]; };
	at(8, 20) = 1000;
	at(9, 20) = 3001;
	at(8, 21) = 5003;
	at(9, 21) = 7007;
	// 21,000 units on x (texel 8 of its quadrant), y = -(20 units + 0xC003 words).
	const int32_t x = (21000 << 16) + 0x4001;
	const int32_t y = -((20 << 16) + 0xC003);
	const uint64_t fx = 0x4001, fy = 0xC003;
	const auto w = [](uint64_t a, uint64_t b) { return int64_t((a * b + 0x8000) >> 16); };
	const auto t = [](int64_t weight, int64_t raw) { return int32_t(uint64_t(weight * (raw << 8) + 0x8000) >> 16); };
	const int32_t expected = t(w(0x10000 - fx, 0x10000 - fy), 1000) + t(w(fx, 0x10000 - fy), 3001) +
	                         t(w(0x10000 - fx, fy), 5003) + t(w(fx, fy), 7007);
	if (!expect(opennova::terrain::height_field_height_bilinear_fixed(field, x, y) == expected,
	            "the word sampler's rounded weights and taps"))
		return 1;
	sectors.fill(0);
	if (!expect(opennova::terrain::height_field_height_bilinear_fixed(field, x, y) == 0,
	            "an empty cell samples 0"))
		return 1;
	std::printf("PASS ground_sampler_takes_the_words\n");
	return 0;
}

// entity+0 is a word: a model-derived bound radius past 256 units keeps its low bits on
// the 0x0A's priority term (a float would round them away).
// [orig: entity+0; NetPacket_SerializeEntityStatesToPacket's priority list (D-NET-530)]
int bound_radius_is_a_word() {
	w::Entity e;
	e.bound_radius.q16 = 0x01000001; // 256 units and one word
	if (!expect(through_float(0x01000001) != 0x01000001, "a float view loses the word")) return 1;
	const opennova::GameEntitySnapshot s = ns::snapshot_of(e);
	if (!expect(s.bound_radius_q16 == 0x01000001, "the 0x0A streams the bound word")) return 1;
	std::printf("PASS bound_radius_is_a_word\n");
	return 0;
}

} // namespace

int main() {
	const int lossy = words_are_lossy_through_a_float();
	const int streams = promotion_and_streams_keep_the_words();
	const int placement = placement_and_world_state_load_keep_the_words();
	const int slot = ai_slot_reads_its_entity_words();
	const int chat = nearest_location_takes_the_word_distances();
	const int zone = zone_placement_keeps_the_words();
	const int scatter = scatter_walk_takes_the_word_length();
	const int gnl2 = gnl2_blast_point_is_the_raised_z_word();
	const int barrel = barrel_blast_takes_the_yaw_word();
	const int wreck = static_wreck_slides_on_the_words();
	const int flag = flag_state_streams_the_words();
	const int round = round_flies_on_the_words();
	const int mount = organic_record_streams_the_mount();
	const int seat = seat_pose_is_words();
	const int sampler = ground_sampler_takes_the_words();
	const int bound = bound_radius_is_a_word();
	for (const int r : {lossy, streams, placement, slot, chat, zone, scatter, gnl2, barrel, wreck, flag, round, mount,
	                    seat, sampler, bound})
		if (r != 0) return r;
	return 0;
}
