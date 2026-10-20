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

#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/server_chat.h>
#include <formats/def/reserved_items.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>

#include <formats/mission/bms.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/mission/promote.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
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
	bool reached = false;
	for (int i = 0; i < 64 && !reached; ++i) {
		inmatch::InitialStateStep step =
				inmatch::Server_SendInitialGameStateToPlayer(ctx, conn, /*now_tick=*/1);
		if (!step.advanced) break;
		for (auto &m : step.messages) emitted.push_back(m);
		reached = step.reached_in_game;
	}
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

} // namespace

int main() {
	const int lossy = words_are_lossy_through_a_float();
	const int streams = promotion_and_streams_keep_the_words();
	const int placement = placement_and_world_state_load_keep_the_words();
	const int slot = ai_slot_reads_its_entity_words();
	const int chat = nearest_location_takes_the_word_distances();
	const int zone = zone_placement_keeps_the_words();
	for (const int r : {lossy, streams, placement, slot, chat, zone})
		if (r != 0) return r;
	return 0;
}
