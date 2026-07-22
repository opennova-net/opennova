// The per-pool LOAD-TIME spawn-batch extractors (libs/netsim entity_wire_bridge) build the
// full world a host streams to a joiner during world-load [orig: Server_SendInitialGameStateToPlayer
// @0x51bba0, phases 0x10 -> 0x0D -> 0x0C -> 0x20]. Routing is by handle.pool() (pool_for_kind:
// Organic->0, Item->1, Building->2, Marker->3). This proves each extractor reads the right pool
// into the right wire batch AND that the batch round-trips its witnessed decoder field-identically.

#include "netsim/entity_wire_bridge.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
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
int32_t axis_bam(int16_t degrees) {
	return static_cast<int32_t>(static_cast<int64_t>(degrees) * kBamPerDegree);
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
	// The 0x8000-gated u16 is the capture-zone radius (entity+0x15E), NOT health — unmodeled,
	// so it must stay ABSENT (golden ASH_I5A vehicle records carry no 0x8000 flag). The old
	// build planted Entity::health here (witness 2026-07-02).
	if (!expect((r.spawn_flags & 0x8000) == 0, "zone-radius word absent")) return false;
	if (!expect(r.zone_radius == 0, "zone radius not populated")) return false;
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

// The S2C 0x18 FULL-ENTITY-SPAWN record (§5.46) — the 0x0F-query reply. A PLAYER record
// must carry the same wire rules as its 0x0C sibling (per-recipient flags, minimap
// net_id, playerClass clamp) or the client's rebuild re-breaks what the query was
// trying to repair. [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180]
bool run_full_entity_spawn_player() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity player;
	player.kind = w::EntityKind::Organic;
	player.item_id = 0x14B9;             // the player infantry template
	player.has_item_def = true;
	player.item_type = 3;
	player.item_attrib = 0x100000u;      // AIData: exact retail name gate
	player.name = "Player";
	player.position = {12.0f, 34.0f, 5.0f};
	player.yaw = 30;
	player.team = 1;
	player.player_class = 0;             // unset -> must clamp to 8 on the wire
	player.owner_connection_id = 0x0C;
	const w::EntityHandle h = world.registry.spawn(0, player);
	const w::Entity *e = world.registry.get(h);
	if (!expect(e != nullptr, "player spawned")) return false;

	// Remote view (requester != owner): flags 0x0100, minimap net_id, class 8.
	nw::FullEntitySpawnRecord rec = ns::build_full_entity_spawn(*e);
	if (!expect(rec.slot_id == h.packed, "slot_id = wire handle")) return false;
	if (!expect(rec.item_type_id == 0x14B9, "type id carried")) return false;
	if (!expect(rec.item_type == 3, "resolved ItemType_Person carried")) return false;
	if (!expect(rec.minimap_flags == 0x0100, "remote player flags 0x0100")) return false;
	if (!expect(rec.entity_flags == 0x0C, "owner connection id carried")) return false;
	if (!expect(rec.entity_name == "Player", "name carried")) return false;
	if (!expect(rec.net_id == (0x0200u | (h.slot() & 0x1Fu)), "minimap net_id (team 1)")) return false;
	if (!expect(rec.player_class == 8, "playerClass clamped to 8")) return false;
	if (!expect(rec.parent_entity_handle == 0xFFFF && rec.seat_mask == 0, "on foot, no seats")) return false;
	if (!expect(rec.mount_handle_8 == 0xFFFF && rec.mount_handle_9 == 0xFFFF,
	            "live entity empty fixed seats use invalid-handle sentinels")) return false;
	if (!expect(rec.pos_x == w::to_fixed(12.0), "pos 16.16")) return false;
	const uint16_t want_heading = static_cast<uint16_t>(static_cast<uint32_t>(heading_bam(30)) >> 16);
	if (!expect(rec.heading_hi == want_heading, "heading = engine BAM high word")) return false;

	// Own view (requester == owner): bit 0 set, same everything else.
	nw::FullEntitySpawnRecord own = ns::build_full_entity_spawn(*e, h);
	if (!expect(own.minimap_flags == 0x0101, "own player flags 0x0101")) return false;

	// And the record survives the wire byte-identically.
	std::vector<uint8_t> wire = nw::encode_full_entity_spawn(rec);
	nw::FullEntitySpawnRecord out;
	if (!expect(nw::decode_full_entity_spawn(wire.data(), wire.size(), out), "0x18 round-trip")) return false;
	if (!expect(out.item_type_id == rec.item_type_id && out.minimap_flags == rec.minimap_flags &&
	                    out.net_id == rec.net_id && out.player_class == rec.player_class &&
	                    out.entity_name == rec.entity_name && out.heading_hi == rec.heading_hi,
	            "decoded fields match")) return false;
	std::printf("PASS full_entity_spawn_player\n");
	return true;
}

// Every S2C 0x18 field whose retail source now exists on world::Entity is copied
// directly, including the fixed retail mount slots and raw low/high-word truncations.
// [orig: serialize_object_to_buffer @0x504d10; net-re §5.46]
bool run_full_entity_spawn_rich_fields() {
	w::Entity entity;
	entity.handle = w::EntityHandle::make(1, 2);
	entity.kind = w::EntityKind::Item;
	entity.item_id = 0x050E;
	entity.has_item_def = true;
	entity.item_type = 1;
	entity.item_attrib = 0x100000u;
	entity.name = "repair_target";
	entity.team = 2;
	entity.owner_connection_id = 0x10203040u;
	entity.primary_occupant = w::EntityHandle::make(0, 4);
	entity.ground_target = w::EntityHandle::make(2, 5);
	entity.mount_target = w::EntityHandle::make(1, 1);
	// Production keeps this vector dense and in model-userpoint order. Retail's
	// entity record does not: passenger slots are 0..7, control/driver is 8, and
	// UseGun is 9. Deliberately scramble vector order and use a sparse passenger
	// slot so an index-based bridge cannot satisfy this fixture.
	entity.seats.resize(4);
	entity.seats[0].type = w::SeatType::Gunner;
	entity.seats[0].retail_slot = 9;
	entity.seats[0].occupant = w::EntityHandle::make(0, 9);
	entity.seats[1].type = w::SeatType::Passenger;
	entity.seats[1].retail_slot = 7;
	entity.seats[1].occupant = w::EntityHandle::make(0, 7);
	entity.seats[2].type = w::SeatType::Controller;
	entity.seats[2].retail_slot = 8;
	entity.seats[2].occupant = w::EntityHandle::make(0, 8);
	entity.seats[3].type = w::SeatType::Passenger;
	entity.seats[3].retail_slot = 0;
	entity.seats[3].occupant = w::EntityHandle::make(0, 1);
	entity.position = {-12.5f, 8.25f, 3.0f};
	entity.yaw = 215;
	entity.pitch = -15;
	entity.ai_state = 0x1234;
	entity.anim_slot = 6;
	entity.net_id = 0x4242;
	entity.player_class = 7;
	entity.ref_num = 0xAB;
	entity.sub_type = 0xCD;

	const nw::FullEntitySpawnRecord rec = ns::build_full_entity_spawn(entity);
	if (!expect(rec.slot_id == entity.handle.packed, "rich slot id")) return false;
	if (!expect(rec.item_type_id == 0x050E && rec.item_type == 1,
	            "resolved item id/type")) return false;
	if (!expect(rec.team == 2 && rec.minimap_flags == 0 &&
	                    rec.entity_flags == 0x10203040u,
	            "team/minimap/owner flags")) return false;
	if (!expect(rec.entity_name == "repair_target", "AIData-gated name")) return false;
	if (!expect(rec.parent_vehicle_handle == entity.primary_occupant.packed,
	            "entity+368 primary occupant")) return false;
	if (!expect(rec.ground_entity_handle == entity.ground_target.packed,
	            "entity+40 ground target")) return false;
	if (!expect(rec.parent_entity_handle == entity.mount_target.packed,
	            "entity+364 mount target")) return false;
	if (!expect(rec.seat_mask == 0x81, "sparse passenger slots masked")) return false;
	if (!expect(rec.mount_handles[0] == entity.seats[3].occupant.packed &&
	                    rec.mount_handles[1] == 0xFFFF &&
	                    rec.mount_handles[7] == entity.seats[1].occupant.packed,
	            "masked seat occupants 0..7")) return false;
	if (!expect(rec.mount_handle_8 == entity.seats[2].occupant.packed &&
	                    rec.mount_handle_9 == entity.seats[0].occupant.packed,
	            "always-present seat occupants 8/9")) return false;
	if (!expect(rec.pos_x == w::to_fixed(-12.5) && rec.pos_y == w::to_fixed(8.25) &&
	                    rec.pos_z == w::to_fixed(3.0),
	            "rich position 16.16")) return false;
	const uint16_t want_heading =
			static_cast<uint16_t>(static_cast<uint32_t>(heading_bam(215)) >> 16);
	const uint16_t want_pitch =
			static_cast<uint16_t>(static_cast<uint32_t>(axis_bam(-15)) >> 16);
	if (!expect(rec.heading_hi == want_heading && rec.pitch_hi == want_pitch,
	            "yaw/pitch engine BAM high words")) return false;
	if (!expect(rec.ai_state == 0x34, "ai_state low byte")) return false;
	if (!expect(rec.anim_slot == 6 && rec.net_id == 0x4242 && rec.player_class == 7,
	            "anim/net/class raw fields")) return false;
	if (!expect(rec.unused_byte == 0 && rec.alert_level == 0xAB && rec.sub_type == 0xCD,
	            "tail leaves only entity+340 zero")) return false;

	const std::vector<uint8_t> wire = nw::encode_full_entity_spawn(rec);
	nw::FullEntitySpawnRecord out;
	if (!expect(nw::decode_full_entity_spawn(wire.data(), wire.size(), out),
	            "rich 0x18 round-trip")) return false;
	if (!expect(out.slot_id == rec.slot_id && out.item_type_id == rec.item_type_id &&
	                    out.item_type == rec.item_type && out.team == rec.team &&
	                    out.minimap_flags == rec.minimap_flags &&
	                    out.entity_flags == rec.entity_flags &&
	                    out.entity_name == rec.entity_name &&
	                    out.parent_vehicle_handle == rec.parent_vehicle_handle &&
	                    out.ground_entity_handle == rec.ground_entity_handle &&
	                    out.parent_entity_handle == rec.parent_entity_handle,
	            "rich decoded identity and links")) return false;
	if (!expect(out.seat_mask == rec.seat_mask &&
	                    out.mount_handles[0] == rec.mount_handles[0] &&
	                    out.mount_handles[1] == rec.mount_handles[1] &&
	                    out.mount_handles[7] == rec.mount_handles[7] &&
	                    out.mount_handle_8 == rec.mount_handle_8 &&
	                    out.mount_handle_9 == rec.mount_handle_9,
	            "rich decoded seats")) return false;
	if (!expect(out.pos_x == rec.pos_x && out.pos_y == rec.pos_y &&
	                    out.pos_z == rec.pos_z && out.heading_hi == rec.heading_hi &&
	                    out.pitch_hi == rec.pitch_hi && out.ai_state == rec.ai_state &&
	                    out.anim_slot == rec.anim_slot && out.net_id == rec.net_id &&
	                    out.player_class == rec.player_class &&
	                    out.unused_byte == rec.unused_byte &&
	                    out.alert_level == rec.alert_level && out.sub_type == rec.sub_type,
	            "rich decoded pose and tail")) return false;
	std::printf("PASS full_entity_spawn_rich_fields\n");
	return true;
}

bool run_full_entity_spawn_itemdef_gates() {
	w::Entity unresolved;
	unresolved.handle = w::EntityHandle::make(1, 3);
	unresolved.item_id = 0x050E;
	unresolved.has_item_def = false;
	unresolved.item_type = 1;              // stale carrier must not defeat null semantics
	unresolved.item_attrib = 0x100000u;
	unresolved.is_ai_capable = true;
	unresolved.name = "must_not_cross";
	const nw::FullEntitySpawnRecord null_def = ns::build_full_entity_spawn(unresolved);
	if (!expect(null_def.item_type_id == 0 && null_def.item_type == 0,
	            "null ItemDef writes type id/type zero")) return false;
	if (!expect(null_def.entity_name.empty(), "null ItemDef writes empty name")) return false;

	w::Entity non_ai = unresolved;
	non_ai.has_item_def = true;
	non_ai.item_type = 1;
	non_ai.item_attrib = 0;
	non_ai.is_ai_capable = true; // the raw ItemDef attrib, not this mirror, is authoritative
	const nw::FullEntitySpawnRecord no_name = ns::build_full_entity_spawn(non_ai);
	if (!expect(no_name.item_type_id == 0x050E && no_name.item_type == 1,
	            "non-AI resolved type still carried")) return false;
	if (!expect(no_name.entity_name.empty(), "non-AI ItemDef writes empty name")) return false;

	non_ai.item_attrib = 0x100000u;
	non_ai.is_ai_capable = false;
	const nw::FullEntitySpawnRecord raw_gate = ns::build_full_entity_spawn(non_ai);
	if (!expect(raw_gate.entity_name == "must_not_cross",
	            "raw ItemDef AIData bit gates name")) return false;
	std::printf("PASS full_entity_spawn_itemdef_gates\n");
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
	ok = run_full_entity_spawn_player() && ok;
	ok = run_full_entity_spawn_rich_fields() && ok;
	ok = run_full_entity_spawn_itemdef_gates() && ok;
	if (ok) std::printf("ALL netsim_world_stream_extractors tests passed\n");
	return ok ? 0 : 1;
}
