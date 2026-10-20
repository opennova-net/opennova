// The host's load-stream rows' fields, as retail's serializers write them (lane L6 sortie 2).
//
// D-NET-512: the 0x0C and 0x18 netId field is entity+0x15C for every row
// [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030A0 - @0x5032CB;
//  NetPacket_SerializeObjectToBuffer @0x504D10 - @0x5051BF], which the BMS spawn's memset leaves 0
// [orig: Entity_SpawnFromBMSRecord @0x40E9F0 - memset @0x40EA27]; the SSN (+0x7C) never rides it.
//
// D-NET-487: the 0x0D and 0x0C pass over a row whose ItemTypeIndex (+0x1C) is 0 or whose def
// (+0x20) is null, the 0x0C also one with no AI slot (+0x68), uncounted
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940 - @0x5039DD / @0x5039E6;
//  NetPacket_SerializeEntityStatesToBuffer @0x5030A0 - @0x503134 / @0x50313E / @0x503148].
//
// D-NET-485: an includeInUse-0 allocation passes over a destroyed row (+0x219) below the
// used count and takes the row at the count, raising it; an includeInUse-1 one reuses it
// [orig: Pool_AllocEntry @0x442230 - @0x44225C, @0x442262..0x44226C, @0x44228B;
//  Entity_Destroy @0x43EA7F].

#include <formats/mission/bms.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/mission/promote.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace w = opennova::world;
namespace ns = opennova::replication;
namespace bms = opennova::bms;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
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

int organic_net_id_is_its_15c_word() {
	bms::File m{};
	m.organics.push_back(record(bms::ItemType::Organic, 0x0816, 10 << 16, 20 << 16, 0));
	m.organics[0].id = 31; // the SSN (+0x7C)
	auto world = std::make_unique<w::World>();
	opennova::mission::promote_mission(m, *world, {});
	const w::EntityHandle ai = world->registry.find_by_net_id(31);
	if (!expect(ai.valid(), "the organic promotes")) return 1;
	w::Entity &e = *world->registry.get(ai);
	e.item_type_index = 1;
	e.has_item_def = true;
	e.item_type = 3; // a person def: the 0x0C's full form
	if (!expect(e.minimap_net_id == 0, "the BMS spawn leaves +0x15C 0")) return 1;

	const opennova::OrganicSpawnBatch c = ns::build_pool0_organic_batch(*world);
	opennova::OrganicSpawnBatch out;
	const std::vector<uint8_t> wire = opennova::encode_organic_spawn_batch(c);
	if (!expect(opennova::decode_organic_spawn_batch(wire.data(), wire.size(), out) &&
	                    out.records.size() == 1 && out.records[0].net_id == 0,
	            "the 0x0C streams the organic's +0x15C, 0, never its SSN"))
		return 1;
	if (!expect(ns::build_full_entity_spawn(e).net_id == 0,
	            "the 0x18 streams the organic's +0x15C, 0, never its SSN"))
		return 1;
	std::printf("PASS organic_net_id_is_its_15c_word\n");
	return 0;
}

int streams_pass_over_rows_without_a_type_index_def_or_slot() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->registry.configure_pool(1, 8);
	const auto row = [](int32_t type_index, bool def, uint8_t item_type) {
		w::Entity e;
		e.item_type_index = type_index;
		e.has_item_def = def;
		e.item_type = item_type;
		return e;
	};
	world->registry.spawn(1, row(1, true, 1));            // streamed
	world->registry.spawn(1, row(0, true, 1));            // a type items.def lacks: passed over
	world->registry.spawn(1, row(1, false, 0));           // no def: passed over
	const w::EntityHandle body = world->registry.spawn(0, row(1, true, 3));
	world->ai.attach(body);                                // its AI slot: streamed
	world->registry.spawn(0, row(1, true, 3));            // no AI slot: passed over
	world->ai.attach(world->registry.spawn(0, row(0, true, 3))); // index 0: passed over
	const opennova::PoolSpawnBatch d = ns::build_pool1_spawn_batch(*world);
	if (!expect(d.records.size() == 1 && d.records[0].slot_id == w::EntityHandle::make(1, 0).packed,
	            "the 0x0D streams only the row with a type index and a def"))
		return 1;
	const opennova::OrganicSpawnBatch c = ns::build_pool0_organic_batch(*world);
	if (!expect(c.records.size() == 1 && c.records[0].slot_id == body.packed,
	            "the 0x0C streams only the row with a type index, a def and an AI slot"))
		return 1;
	std::printf("PASS streams_pass_over_rows_without_a_type_index_def_or_slot\n");
	return 0;
}

int fresh_allocation_passes_over_a_destroyed_row() {
	using Reuse = w::EntityRegistry::Reuse;
	w::EntityRegistry registry;
	registry.configure_pool(1, 8);
	const w::EntityHandle a = registry.spawn(1, w::Entity{});
	registry.spawn(1, w::Entity{});
	registry.despawn(a); // Entity_Destroy: +0x219 set on slot 0
	const w::EntityHandle fresh = registry.spawn(1, w::Entity{}, Reuse::Fresh);
	if (!expect(fresh.slot() == 2 && registry.pool_used(1) == 3,
	            "an includeInUse-0 allocation takes the row at the count, raising it"))
		return 1;
	const w::EntityHandle reused = registry.spawn(1, w::Entity{});
	if (!expect(reused.slot() == 0 && registry.pool_used(1) == 3,
	            "an includeInUse-1 allocation reuses the destroyed row"))
		return 1;
	registry.despawn(reused);
	registry.spawn_at(w::EntityHandle::make(1, 0), w::Entity{}); // the next occupant clears it
	registry.despawn(fresh);
	const w::EntityHandle after = registry.spawn(1, w::Entity{}, Reuse::Fresh);
	if (!expect(after.slot() == 3,
	            "a destroyed row stays passed over until an occupant's write clears it"))
		return 1;
	std::printf("PASS fresh_allocation_passes_over_a_destroyed_row\n");
	return 0;
}

} // namespace

int main() {
	const int net_id = organic_net_id_is_its_15c_word();
	const int streams = streams_pass_over_rows_without_a_type_index_def_or_slot();
	const int alloc = fresh_allocation_passes_over_a_destroyed_row();
	return net_id != 0 ? net_id : streams != 0 ? streams : alloc;
}
