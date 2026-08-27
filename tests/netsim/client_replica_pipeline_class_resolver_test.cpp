// The items.def record-class resolver guard (§5.10b decode dispatch).
//
// The 0x0A event loop is CLASS-SIZED: each tag-1 record's body width comes from the
// recipient's per-type replication class — the retail client dispatches through its
// own items.def serialize callback [orig: itemDef+356 @0x50f2e2 /
// ItemList_FindIndexByTypeId]. Our in-process views (the SP host's own loopback
// client and the LAN joiner) must classify with the SAME items.def table the host
// encoder stamps as Entity::net_class_code, or one mis-sized record desyncs every
// record after it in the frame — and every junk record then decodes anchor-relative,
// i.e. lands scattered around the local player (the #262-era "vehicles follow me"
// symptom: parked vehicles became live placed nodes, rendering the junk).
//
// Five contracts:
//   1. A view carrying the items.def table walks a mixed frame — a header-only
//      no-callback record (bldg/ewep form) followed by vehicle + infantry compacts —
//      and decodes every position exactly (pipeline identity vs the lossy codec).
//   2. A default view with no catalog fails closed at the first unknown codec and
//      does not invent rows from a guessed record width.
//   3. The items.def table OUTRANKS the 0x0D pool-blanket learning: a pool-1
//      no-callback type (an `ewep` emplacement) that arrived via 0x0D must still
//      decode header-only.
//   4. Lenient partial frames update recipient health only after the fixed tail
//      decoded, independently of a later event-loop failure.
//   5. Objective masks likewise commit only after the complete off-wire-gated
//      16-byte phase-3 body decodes.

#include <net/netsim/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h> // FrameUpdate / EntityClass / network_decompress_fixedpoint
#include <net/npwire/ingame_encode.h> // encode_frame_update / encode_pool_spawn_batch / compress
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr uint16_t kNoCallbackType = 0x0111; // a `bldg`/`ewep`-class item (header-only record)
constexpr uint16_t kVehicleType = 0x0222;    // a `cveh`-class item (15/21-B compact)
constexpr uint16_t kInfantryType = 0x0333;   // an `org0`-class item (14-B compact)

constexpr uint16_t kNoCallbackHandle = 0x2005; // pool-2 static
constexpr uint16_t kVehicleHandle = 0x1002;    // pool-1 vehicle
constexpr uint16_t kInfantryHandle = 0x0003;   // pool-0 organic

// The items.def-derived classifier the Godot binding installs (Simulation::
// resolve_item_traits builds the same shape from ItemDatabase).
nw::EntityClass items_table_classify(uint16_t type_id) {
	static const std::unordered_map<uint16_t, nw::EntityClass> table = {
			{kNoCallbackType, nw::EntityClass::NoNetworkCallback},
			{kVehicleType, nw::EntityClass::Vehicle},
			{kInfantryType, nw::EntityClass::Infantry},
	};
	const auto it = table.find(type_id);
	return it != table.end() ? it->second : nw::EntityClass::Unknown;
}

// One frame in the shape the host fan emits every tick on a populated map: a
// no-callback scenery record FIRST, then a parked vehicle, then a walking soldier.
std::vector<uint8_t> build_mixed_frame(int32_t anchor_x, int32_t anchor_y, int32_t anchor_z) {
	nw::FrameUpdate fu;
	fu.anchor_x = anchor_x;
	fu.anchor_y = anchor_y;
	fu.anchor_z = anchor_z;
	fu.flags2 = 0;              // sub-block 0 (the common gameplay frame)
	fu.mount_handle = 0xFFFF;

	nw::FrameUpdateRecord scenery;
	scenery.handle = kNoCallbackHandle;
	scenery.type_id = kNoCallbackType;
	scenery.cls = nw::EntityClass::NoNetworkCallback;
	fu.records.push_back(scenery);

	nw::FrameUpdateRecord vehicle;
	vehicle.handle = kVehicleHandle;
	vehicle.type_id = kVehicleType;
	vehicle.cls = nw::EntityClass::Vehicle;
	vehicle.vehicle.parent_slot_handle = 0xFFFF;
	vehicle.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(50 << 16);
	vehicle.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(-20 << 16);
	vehicle.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(3 << 16);
	vehicle.vehicle.euler_z = 0x1200;
	vehicle.vehicle.is_dead_pose = false;
	vehicle.vehicle.health_word = 550;
	fu.records.push_back(vehicle);

	nw::FrameUpdateRecord soldier;
	soldier.handle = kInfantryHandle;
	soldier.type_id = kInfantryType;
	soldier.cls = nw::EntityClass::Infantry;
	soldier.infantry.pos_x_compressed = nw::network_compress_fixedpoint(-8 << 16);
	soldier.infantry.pos_y_compressed = nw::network_compress_fixedpoint(12 << 16);
	soldier.infantry.pos_z_compressed = nw::network_compress_fixedpoint(1 << 16);
	soldier.infantry.yaw_byte = 0x40;
	fu.records.push_back(soldier);

	return nw::encode_frame_update(fu);
}

// Expected decoded coordinate: anchor + the lossy codec roundtrip (pipeline identity,
// same rule apply_frame_update uses).
int32_t expected_coord(int32_t anchor, int32_t world_fixed) {
	return anchor + nw::network_decompress_fixedpoint(nw::network_compress_fixedpoint(world_fixed));
}

bool run_items_table_sizes_mixed_frame() {
	const int32_t ax = 300 << 16, ay = -40 << 16, az = 7 << 16;
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&items_table_classify);
	view.apply(0x0A, build_mixed_frame(ax, ay, az));

	if (!expect(view.frames_applied() == 1, "frame applied")) return false;
	// The no-callback record is header-only and carries no motion sample — the apply
	// loop must skip it entirely (no upsert; retail jumps back to the loop head
	// [orig: 0x430814..0x43081D]).
	if (!expect(view.state().find(kNoCallbackHandle) == nullptr,
	            "no-callback record does not upsert a row")) return false;

	const ns::ClientEntityState *veh = view.state().find(kVehicleHandle);
	if (!expect(veh != nullptr, "vehicle record decoded")) return false;
	if (!expect(veh->x == expected_coord(ax, 50 << 16) &&
	                    veh->y == expected_coord(ay, -20 << 16) &&
	                    veh->z == expected_coord(az, 3 << 16),
	            "vehicle position decodes anchor-relative to its own sample"))
		return false;

	const ns::ClientEntityState *inf = view.state().find(kInfantryHandle);
	if (!expect(inf != nullptr, "infantry record decoded")) return false;
	if (!expect(inf->x == expected_coord(ax, -8 << 16) &&
	                    inf->y == expected_coord(ay, 12 << 16) &&
	                    inf->z == expected_coord(az, 1 << 16) && inf->yaw_byte == 0x40,
	            "infantry position/yaw decode after the header-only record"))
		return false;
	return true;
}

bool run_default_view_fails_closed_on_unknown_codec() {
	const int32_t ax = 300 << 16, ay = -40 << 16, az = 7 << 16;
	ns::ClientReplicaPipeline view; // no catalog: the first non-built-in type is unresolved
	view.apply(0x0A, build_mixed_frame(ax, ay, az));

	return expect(view.state().find(kNoCallbackHandle) == nullptr &&
	                      view.state().find(kVehicleHandle) == nullptr &&
	                      view.state().find(kInfantryHandle) == nullptr,
	              "unknown codec stops the frame without guessed-width rows");
}

bool run_items_table_outranks_pool_blanket() {
	const int32_t ax = 100 << 16, ay = 0, az = -5 << 16;
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&items_table_classify);

	// A load-time 0x0D pool-1 spawn for the no-callback type: the blanket learner
	// brands it Vehicle (pool 1 = the vehicle pool). The items.def table must win —
	// an `ewep` emplacement's tag-1 record is header-only, and sizing it as a vehicle
	// compact desynced the retail client mid-frame (retail-join v13, 2026-07-02).
	nw::PoolSpawnBatch batch;
	nw::PoolSpawnRecord spawn;
	spawn.slot_id = 0x1009;
	spawn.item_type_id = kNoCallbackType;
	spawn.pos_x = 640 << 16;
	spawn.pos_y = 480 << 16;
	spawn.pos_z = 2 << 16;
	batch.records.push_back(spawn);
	batch.entity_count = 1;
	view.apply(0x0D, nw::encode_pool_spawn_batch(batch));

	nw::FrameUpdate fu;
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	nw::FrameUpdateRecord emplacement;
	emplacement.handle = 0x1009;
	emplacement.type_id = kNoCallbackType;
	emplacement.cls = nw::EntityClass::NoNetworkCallback;
	fu.records.push_back(emplacement);
	nw::FrameUpdateRecord vehicle;
	vehicle.handle = kVehicleHandle;
	vehicle.type_id = kVehicleType;
	vehicle.cls = nw::EntityClass::Vehicle;
	vehicle.vehicle.parent_slot_handle = 0xFFFF;
	vehicle.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(9 << 16);
	vehicle.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(2 << 16);
	vehicle.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(0);
	vehicle.vehicle.health_word = 550;
	fu.records.push_back(vehicle);
	view.apply(0x0A, nw::encode_frame_update(fu));

	// The emplacement keeps its ABSOLUTE 0x0D spawn position (its 0x0A record carries
	// no motion sample), and the vehicle behind it still decodes cleanly.
	const ns::ClientEntityState *emp = view.state().find(0x1009);
	if (!expect(emp != nullptr && emp->x == (640 << 16) && emp->y == (480 << 16) &&
	                    emp->z == (2 << 16),
	            "0x0D-spawned emplacement keeps its spawn position")) return false;
	const ns::ClientEntityState *veh = view.state().find(kVehicleHandle);
	if (!expect(veh != nullptr && veh->x == expected_coord(ax, 9 << 16) &&
	                    veh->y == expected_coord(ay, 2 << 16) &&
	                    veh->z == expected_coord(az, 0),
	            "vehicle after the emplacement record decodes cleanly")) return false;
	return true;
}

bool run_catalog_presence_outranks_learned_pool_class() {
	constexpr uint16_t ambiguous_type = 0x0444;
	constexpr uint16_t fallback_type = 0x0555;
	constexpr uint16_t ambiguous_handle = 0x1004;
	constexpr uint16_t fallback_handle = 0x1005;
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver([ambiguous_type](uint16_t type_id)
				-> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type_id == ambiguous_type) return nw::EntityClass::Unknown;
		return std::nullopt;
	});

	nw::PoolSpawnBatch spawns;
	nw::PoolSpawnRecord ambiguous;
	ambiguous.slot_id = ambiguous_handle;
	ambiguous.item_type_id = ambiguous_type;
	spawns.records.push_back(ambiguous);
	nw::PoolSpawnRecord fallback = ambiguous;
	fallback.slot_id = fallback_handle;
	fallback.item_type_id = fallback_type;
	spawns.records.push_back(fallback);
	view.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(spawns));

	const ns::ClientEntityState *ambiguous_spawn =
			view.state().find(ambiguous_handle);
	const ns::ClientEntityState *fallback_spawn = view.state().find(fallback_handle);
	if (!expect(ambiguous_spawn != nullptr &&
				ambiguous_spawn->cls == nw::EntityClass::Unknown,
			"present Unknown catalog entry is canonical at spawn")) return false;
	if (!expect(fallback_spawn != nullptr &&
				fallback_spawn->cls == nw::EntityClass::Vehicle,
			"absent catalog entry falls through to learned pool class")) return false;

	nw::FrameUpdate frame;
	frame.mount_handle = 0xFFFF;
	nw::FrameUpdateRecord unresolved;
	unresolved.handle = ambiguous_handle;
	unresolved.type_id = ambiguous_type;
	unresolved.cls = nw::EntityClass::Vehicle;
	unresolved.vehicle.parent_slot_handle = 0xFFFF;
	unresolved.vehicle.health_word = 1;
	frame.records.push_back(unresolved);
	nw::FrameUpdateRecord later = unresolved;
	later.handle = fallback_handle;
	later.type_id = fallback_type;
	frame.records.push_back(later);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(frame));

	ambiguous_spawn = view.state().find(ambiguous_handle);
	fallback_spawn = view.state().find(fallback_handle);
	return expect(ambiguous_spawn != nullptr && fallback_spawn != nullptr &&
				ambiguous_spawn->compact_revision == 0 &&
				fallback_spawn->compact_revision == 0,
			"present Unknown stops compact decoding before guessed-width records");
}

bool run_recipient_health_requires_a_decoded_tail() {
	ns::ClientReplicaPipeline view([](uint16_t) { return nw::EntityClass::Unknown; });

	// Establish a known valid sample first.
	nw::FrameUpdate baseline;
	baseline.flags2 = 1;
	baseline.mount_handle = 0xFFFF;
	baseline.health = 123;
	view.apply(0x0A, nw::encode_frame_update(baseline));
	if (!expect(view.state().local_health == 123 &&
	                    view.state().health_updates_applied == 1,
	            "complete recipient tail establishes health")) return false;

	// The view intentionally counts/applies partial frames for diagnostic and
	// presentation state, but this one ends before its selected sub-block and tail.
	std::vector<uint8_t> short_frame(14, 0);
	view.apply(0x0A, short_frame);
	if (!expect(view.state().frames_applied == 2,
	            "pre-tail partial frame remains visible to the lenient fold")) return false;
	if (!expect(view.state().local_health == 123 &&
	                    view.state().health_updates_applied == 1,
	            "pre-tail partial frame preserves authoritative health")) return false;

	// Conversely, failure after the tail does not invalidate health the retail
	// handler already consumed. Append an unresolved tag-1 header before EOB.
	nw::FrameUpdate later;
	later.flags2 = 1;
	later.mount_handle = 0xFFFF;
	later.health = 77;
	std::vector<uint8_t> malformed_event = nw::encode_frame_update(later);
	malformed_event.pop_back();
	malformed_event.insert(malformed_event.end(), {0x01, 0x34, 0x12, 0xEF, 0xBE});
	nw::FrameUpdate decoded;
	if (!expect(!nw::decode_frame_update(malformed_event.data(), malformed_event.size(),
	                    [](uint16_t) { return nw::EntityClass::Unknown; }, decoded) &&
	                    decoded.local_tail_present,
	            "post-tail event failure retains explicit tail validity")) return false;
	view.apply(0x0A, malformed_event);
	return expect(view.state().local_health == 77 &&
	                      view.state().health_updates_applied == 2,
	              "post-tail partial frame advances authoritative health");
}

bool run_objectives_require_a_complete_phase3_block() {
	ns::ClientReplicaPipeline view;
	// Replay/bare-view folds learn the off-wire width hint from either session
	// message before the first objective frame.
	std::vector<uint8_t> session_config(51, 0);
	session_config[12] = 0x20;
	session_config[14] = 0x03; // little-endian field[3] = 0x00030020
	view.apply(0x08, session_config);
	if (!expect(view.game_type() == 0x30020,
	            "S2C 0x08 teaches the view objective frame layout")) return false;
	view.set_game_type(0);
	const std::vector<uint8_t> full_info = {
		0, 0, 0, 0, 0,       // five empty cstrings
		0x20, 0x00, 0x03, 0x00, // extra = g_GameType
		0, 0,                 // motd + game-name cstrings
	};
	view.apply(0x7B, full_info);
	if (!expect(view.game_type() == 0x30020,
	            "S2C 0x7B teaches the view objective frame layout")) return false;

	// Establish a known authoritative sample first.
	nw::FrameUpdate baseline;
	baseline.flags2 = 3;
	baseline.objective.present = true;
	baseline.objective.state[0] = 0x11;
	baseline.objective.state[1] = 0x22;
	baseline.objective.state[2] = 0x44;
	baseline.objective.state[3] = 0x88;
	baseline.mount_handle = 0xFFFF;
	view.apply(0x0A, nw::encode_frame_update(baseline));
	const ns::ClientState &established = view.state();
	if (!expect(established.objective_won == 0x11 &&
	                    established.objective_lost == 0x22 &&
	                    established.objective_show_win == 0x44 &&
	                    established.objective_show_lose == 0x88 &&
	                    established.objective_updates_applied == 1,
	            "complete phase-3 block establishes objective masks")) return false;

	// Cut a different sample ten bytes into its 16-byte objective body. The
	// lenient frame fold may retain earlier fields, but must not publish a mixed
	// partial/default objective snapshot over the last complete one.
	nw::FrameUpdate truncated;
	truncated.flags2 = 3;
	truncated.objective.present = true;
	truncated.objective.state[0] = 0xAA;
	truncated.objective.state[1] = 0xBB;
	truncated.objective.state[2] = 0xCC;
	truncated.objective.state[3] = 0xDD;
	truncated.mount_handle = 0xFFFF;
	std::vector<uint8_t> short_objective = nw::encode_frame_update(truncated);
	constexpr std::size_t kHeaderBytes = 12 + 2;
	short_objective.resize(kHeaderBytes + 10);
	view.apply(0x0A, short_objective);

	const ns::ClientState &after = view.state();
	return expect(after.objective_won == 0x11 && after.objective_lost == 0x22 &&
	                      after.objective_show_win == 0x44 &&
	                      after.objective_show_lose == 0x88 &&
	                      after.objective_updates_applied == 1,
	              "truncated phase-3 block preserves authoritative objective masks");
}

bool run_mounted_ammo_requires_a_complete_phase8_record() {
	nw::FrameUpdate mounted;
	mounted.flags2 = 8;
	mounted.mount_handle = 0x1234;
	mounted.passenger.present = true;
	mounted.passenger.has_mount = true;
	mounted.passenger.mount_handle = 0x1234;
	mounted.passenger.clip = 0xFFFE;
	mounted.passenger.reserve = 19;
	const std::vector<uint8_t> full = nw::encode_frame_update(mounted);
	if (!expect(full.size() >= 7, "phase-8 mounted fixture has record plus EOB"))
		return false;
	const std::size_t mount_offset = full.size() - 7; // u16 handle + u16 clip + u16 reserve + EOB
	for (const std::size_t cut : {mount_offset + 1, mount_offset + 2,
	                              mount_offset + 4}) {
		std::vector<uint8_t> short_record(full.begin(), full.begin() + cut);
		nw::FrameUpdate decoded;
		if (!expect(!nw::decode_frame_update(
		                    short_record.data(), short_record.size(),
		                    [](uint16_t) { return nw::EntityClass::Unknown; }, decoded) &&
		                    !decoded.passenger.present,
		            "truncated phase-8 handle/clip/reserve is not publishable"))
			return false;
	}

	ns::ClientReplicaPipeline view;
	view.apply(nw::s2c::PER_FRAME_UPDATE, full);
	const ns::ClientMountedAmmoState &established = view.state().mounted_ammo;
	if (!expect(established.present && established.has_mount &&
	                    established.mount_handle == 0x1234 &&
	                    established.clip == 0xFFFE && established.reserve == 19 &&
	                    established.revision == 1,
	            "complete phase-8 record folds into recipient mounted-ammo state"))
		return false;

	std::vector<uint8_t> truncated(full.begin(), full.begin() + mount_offset + 4);
	view.apply(nw::s2c::PER_FRAME_UPDATE, truncated);
	if (!expect(view.state().mounted_ammo.revision == 1 &&
	                    view.state().mounted_ammo.clip == 0xFFFE &&
	                    view.state().mounted_ammo.reserve == 19,
	            "truncated phase-8 record preserves the last complete snapshot"))
		return false;

	nw::FrameUpdate on_foot;
	on_foot.flags2 = 8;
	on_foot.mount_handle = 0xFFFF;
	on_foot.passenger.present = true;
	on_foot.passenger.mount_handle = 0xFFFF;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(on_foot));
	return expect(view.state().mounted_ammo.present &&
	                      !view.state().mounted_ammo.has_mount &&
	                      view.state().mounted_ammo.mount_handle == 0xFFFF &&
	                      view.state().mounted_ammo.revision == 2,
	              "complete on-foot phase-8 sentinel advances the snapshot");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_items_table_sizes_mixed_frame();
	ok &= run_default_view_fails_closed_on_unknown_codec();
	ok &= run_items_table_outranks_pool_blanket();
	ok &= run_catalog_presence_outranks_learned_pool_class();
	ok &= run_recipient_health_requires_a_decoded_tail();
	ok &= run_objectives_require_a_complete_phase3_block();
	ok &= run_mounted_ammo_requires_a_complete_phase8_record();
	if (ok) std::printf("client_replica_pipeline_class_resolver: OK\n");
	return ok ? 0 : 1;
}
