// The per-pool LOAD-TIME spawn-batch extractors (libs/netsim entity_wire_bridge) build the
// full world a host streams to a joiner during world-load [orig: Server_SendInitialGameStateToPlayer
// @0x51bba0, phases 0x10 -> 0x0D -> 0x0C -> 0x20]. Routing is by handle.pool() (pool_for_kind:
// Organic->0, Item->1, Building->2, Marker->3). This proves each extractor reads the right pool
// into the right wire batch AND that the batch round-trips its witnessed decoder field-identically.

#include "netsim/entity_wire_bridge.h"

#include <novaworld/ingame_decode.h>
#include <novaworld/ingame_encode.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace w = opennova::world;
namespace ns = opennova::netsim;
namespace nw = opennova;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
int32_t heading_bam(int16_t yaw) {
	return static_cast<int32_t>(static_cast<int64_t>(90 - yaw) * kBamPerDegree);
}

// A world with one entity in each of the four pools (the four EntityKinds promote into).
// `pool1_ai_capable` stamps the pool-1 item's Entity::is_ai_capable (items.def AIData /
// ItemDefAttrib & 0x100000), which gates the faithful 0x0D AI-trailer (D-NET-97).
w::World make_four_pool_world(bool pool1_ai_capable = false) {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	world.registry.configure_pool(2, 8);
	world.registry.configure_pool(3, 8);

	w::Entity organic;
	organic.kind = w::EntityKind::Organic;
	organic.item_id = 0x0816;     // AI infantry
	organic.name = "tango1";
	organic.position = {10.0f, 20.0f, 1.5f};
	organic.yaw = 30;
	organic.team = 2;
	organic.net_id = 0x4242;
	organic.anim_slot = 5;
	world.registry.spawn(0, organic);

	w::Entity item;
	item.kind = w::EntityKind::Item;
	item.item_id = 0x050E;        // a truck / destructible
	item.name = "crate";
	item.position = {30.0f, 40.0f, 0.0f};
	item.yaw = 90;
	item.team = 1;
	item.health = 250;
	item.is_ai_capable = pool1_ai_capable;
	world.registry.spawn(1, item);

	w::Entity building;
	building.kind = w::EntityKind::Building;
	building.item_id = 0x0123;    // armory-shaped static
	building.position = {-5.0f, 6.0f, 2.0f};
	building.yaw = 45;
	building.team = 0;
	world.registry.spawn(2, building);

	w::Entity marker;
	marker.kind = w::EntityKind::Marker;
	marker.item_id = 0x1773;      // a start marker
	marker.position = {1.0f, 2.0f, 3.0f};
	marker.yaw = 270;
	marker.team = 1;
	world.registry.spawn(3, marker);
	return world;
}

bool run_pool0_organic() {
	w::World world = make_four_pool_world();
	nw::OrganicSpawnBatch batch = ns::build_pool0_organic_batch(world);
	if (!expect(batch.records.size() == 1, "exactly one pool-0 organic")) return false;

	std::vector<uint8_t> wire = nw::encode_organic_spawn_batch(batch);
	nw::OrganicSpawnBatch out;
	if (!expect(nw::decode_organic_spawn_batch(wire.data(), wire.size(), out), "0x0C round-trip")) return false;
	if (!expect(out.records.size() == 1, "one decoded organic")) return false;
	const nw::OrganicSpawnRecord &r = out.records[0];
	if (!expect(r.slot_id == w::EntityHandle::make(0, 0).packed, "slot_id = pool-0 handle")) return false;
	if (!expect(r.item_type_id == 0x0816, "AI type id")) return false;
	if (!expect(r.entity_name == "tango1", "name carried")) return false;
	if (!expect(r.pos_x == w::to_fixed(10.0) && r.pos_z == w::to_fixed(1.5), "pos 16.16")) return false;
	if (!expect(r.orientation == heading_bam(30), "orientation = engine heading BAM")) return false;
	if (!expect(r.team == 2, "team carried")) return false;
	if (!expect(r.net_id == 0x4242, "net_id carried")) return false;
	if (!expect(r.anim_slot == 5, "anim slot carried")) return false;
	std::printf("PASS pool0_organic\n");
	return true;
}

// The non-AI-trailer fields are identical regardless of AI-capability; share the check.
bool check_pool1_common(const nw::PoolSpawnRecord &r) {
	if (!expect(r.slot_id == w::EntityHandle::make(1, 0).packed, "slot_id = pool-1 handle")) return false;
	if (!expect(r.item_type_id == 0x050E, "item type id")) return false;
	if (!expect(r.entity_name == "crate", "name carried")) return false;
	if (!expect(r.pos_x == w::to_fixed(30.0), "pos 16.16")) return false;
	if (!expect(r.euler_z == heading_bam(90), "euler_z = engine heading BAM")) return false;
	if (!expect(r.team_byte == 1, "team carried")) return false;
	if (!expect(r.health_short == 250, "health carried (0x8000 path)")) return false;
	return true;
}

// AI-capable pool-1 item (Entity::is_ai_capable = items.def AIData / ItemDefAttrib & 0x100000):
// the 0x0D record FAITHFULLY carries the 0x0800 AI-trailer — matching the stock decoder's own
// gate (itemDef.attrib & 0x100000 @0x433327), so it is crash-safe (the strcpy @0x433370 reads a
// valid in-packet NUL-terminated name). [D-NET-97]
bool run_pool1_spawn_ai_capable() {
	w::World world = make_four_pool_world(/*pool1_ai_capable=*/true);
	nw::PoolSpawnBatch batch = ns::build_pool1_spawn_batch(world);
	if (!expect(batch.records.size() == 1, "exactly one pool-1 item")) return false;

	std::vector<uint8_t> wire = nw::encode_pool_spawn_batch(batch);
	nw::PoolSpawnBatch out;
	if (!expect(nw::decode_pool_spawn_batch(wire.data(), wire.size(), out), "0x0D round-trip")) return false;
	if (!expect(out.records.size() == 1, "one decoded item")) return false;
	const nw::PoolSpawnRecord &r = out.records[0];
	if (!check_pool1_common(r)) return false;
	if (!expect((r.spawn_flags & 0x0800) != 0, "0x0800 AI-trailer present for AI-capable item")) return false;
	if (!expect(r.ai_name == "crate", "ai_name carried (the strcpy-safe trailer name)")) return false;
	if (!expect(r.ai_profile_1 == w::to_fixed(30.0), "ai_profile_1 mirrors pos_x (retail trailer convention)")) return false;
	std::printf("PASS pool1_spawn_ai_capable\n");
	return true;
}

// Non-AI pool-1 item (Entity::is_ai_capable = false): the 0x0D record carries NO AI-trailer —
// the 0x0800 flag is clear and no name rides the wire, matching retail (the stock decoder never
// enters its attrib-gated strcpy for a non-AI item). [D-NET-97]
bool run_pool1_spawn_non_ai() {
	w::World world = make_four_pool_world(/*pool1_ai_capable=*/false);
	nw::PoolSpawnBatch batch = ns::build_pool1_spawn_batch(world);
	if (!expect(batch.records.size() == 1, "exactly one pool-1 item")) return false;

	std::vector<uint8_t> wire = nw::encode_pool_spawn_batch(batch);
	nw::PoolSpawnBatch out;
	if (!expect(nw::decode_pool_spawn_batch(wire.data(), wire.size(), out), "0x0D round-trip")) return false;
	if (!expect(out.records.size() == 1, "one decoded item")) return false;
	const nw::PoolSpawnRecord &r = out.records[0];
	if (!check_pool1_common(r)) return false;
	if (!expect((r.spawn_flags & 0x0800) == 0, "0x0800 AI-trailer absent for non-AI item")) return false;
	if (!expect(r.ai_name.empty(), "no ai_name on a non-AI record")) return false;
	std::printf("PASS pool1_spawn_non_ai\n");
	return true;
}

bool run_pool2_static() {
	w::World world = make_four_pool_world();
	nw::StaticEntityBatch batch = ns::build_pool2_static_batch(world);
	// Building is at slot 0, so a single slot-aligned record (no holes).
	if (!expect(batch.start_index == 0, "start index 0")) return false;
	if (!expect(batch.records.size() == 1, "one slot 0..0 record")) return false;

	std::vector<uint8_t> wire = nw::encode_static_entity_batch(batch);
	nw::StaticEntityBatch out;
	if (!expect(nw::decode_static_entity_batch(wire.data(), wire.size(), out), "0x10 round-trip")) return false;
	if (!expect(out.records.size() == 1, "one decoded static")) return false;
	const nw::StaticEntityRecord &r = out.records[0];
	if (!expect(!r.is_empty_slot, "real record")) return false;
	if (!expect(r.item_type_id == 0x0123, "building type id")) return false;
	if (!expect(r.pos_x == w::to_fixed(-5.0) && r.pos_z == w::to_fixed(2.0), "pos 16.16")) return false;
	if (!expect(r.euler_z == heading_bam(45), "euler_z heading BAM (slot 0)")) return false;
	std::printf("PASS pool2_static\n");
	return true;
}

bool run_pool2_static_slot_alignment() {
	// A building at a NON-zero slot must pad lower slots with empty-slot sentinels so the
	// client's slot = start_index + index stays correct.
	w::World world;
	world.registry.configure_pool(2, 8);
	w::Entity a; a.kind = w::EntityKind::Building; a.item_id = 0x10; world.registry.spawn(2, a);
	w::Entity b; b.kind = w::EntityKind::Building; b.item_id = 0x20; world.registry.spawn(2, b);
	world.registry.despawn(w::EntityHandle::make(2, 0)); // leave a hole at slot 0; b is at slot 1

	nw::StaticEntityBatch batch = ns::build_pool2_static_batch(world);
	if (!expect(batch.records.size() == 2, "slots 0..1 emitted (incl. the hole)")) return false;
	if (!expect(batch.records[0].is_empty_slot, "slot 0 is an empty-slot sentinel")) return false;
	if (!expect(!batch.records[1].is_empty_slot && batch.records[1].item_type_id == 0x20,
	            "slot 1 carries the live building")) return false;

	std::vector<uint8_t> wire = nw::encode_static_entity_batch(batch);
	nw::StaticEntityBatch out;
	if (!expect(nw::decode_static_entity_batch(wire.data(), wire.size(), out), "0x10 hole round-trip")) return false;
	if (!expect(out.records.size() == 2 && out.records[0].is_empty_slot, "hole preserved on the wire")) return false;
	std::printf("PASS pool2_static_slot_alignment\n");
	return true;
}

bool run_pool3_marker() {
	w::World world = make_four_pool_world();
	nw::Pool3SyncBatch batch = ns::build_pool3_marker_batch(world);
	if (!expect(batch.records.size() == 1, "exactly one pool-3 marker")) return false;

	std::vector<uint8_t> wire = nw::encode_pool3_sync_batch(batch);
	nw::Pool3SyncBatch out;
	if (!expect(nw::decode_pool3_sync_batch(wire.data(), wire.size(), out), "0x20 round-trip")) return false;
	if (!expect(out.records.size() == 1, "one decoded marker")) return false;
	const nw::Pool3SyncRecord &r = out.records[0];
	if (!expect(r.item_type_id == 0x1773, "marker type id")) return false;
	if (!expect(r.net_handle == w::EntityHandle::make(3, 0).packed, "net_handle = pool-3 handle")) return false;
	if (!expect(r.movement_val == static_cast<uint32_t>(heading_bam(270)), "movement_val = heading BAM (D-NET-59)")) return false;
	if (!expect(r.team_byte == 1, "team carried")) return false;
	std::printf("PASS pool3_marker\n");
	return true;
}

bool run_pools_are_disjoint() {
	// Each extractor reads ONLY its own pool — no cross-contamination.
	w::World world = make_four_pool_world();
	if (!expect(ns::build_pool0_organic_batch(world).records.size() == 1, "pool0 sees only organics")) return false;
	if (!expect(ns::build_pool1_spawn_batch(world).records.size() == 1, "pool1 sees only items")) return false;
	if (!expect(ns::build_pool3_marker_batch(world).records.size() == 1, "pool3 sees only markers")) return false;
	std::printf("PASS pools_are_disjoint\n");
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run_pool0_organic() && ok;
	ok = run_pool1_spawn_ai_capable() && ok;
	ok = run_pool1_spawn_non_ai() && ok;
	ok = run_pool2_static() && ok;
	ok = run_pool2_static_slot_alignment() && ok;
	ok = run_pool3_marker() && ok;
	ok = run_pools_are_disjoint() && ok;
	if (ok) std::printf("ALL netsim_world_stream_extractors tests passed\n");
	return ok ? 0 : 1;
}
