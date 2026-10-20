// The host's load-stream rows' fields, as retail's serializers write them (lane L6 sortie 2).
//
// D-NET-512: the 0x0C and 0x18 netId field is entity+0x15C for every row
// [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030A0 - @0x5032CB;
//  NetPacket_SerializeObjectToBuffer @0x504D10 - @0x5051BF], which the BMS spawn's memset leaves 0
// [orig: Entity_SpawnFromBMSRecord @0x40E9F0 - memset @0x40EA27]; the SSN (+0x7C) never rides it.

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

} // namespace

int main() {
	return organic_net_id_is_its_15c_word();
}
