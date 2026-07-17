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
// Three contracts:
//   1. A view carrying the items.def table walks a mixed frame — a header-only
//      no-callback record (bldg/ewep form) followed by vehicle + infantry compacts —
//      and decodes every position exactly (pipeline identity vs the lossy codec).
//   2. The phase-1 default view (no table) mis-sizes that same frame — the witnessed
//      failure this guard exists to prevent.
//   3. The items.def table OUTRANKS the 0x0D pool-blanket learning: a pool-1
//      no-callback type (an `ewep` emplacement) that arrived via 0x0D must still
//      decode header-only.

#include "netsim/net_client_view.h"

#include <npwire/ingame_decode.h> // FrameUpdate / EntityClass / network_decompress_fixedpoint
#include <npwire/ingame_encode.h> // encode_frame_update / encode_pool_spawn_batch / compress

#include <cstdint>
#include <cstdio>
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

// The items.def-derived classifier the Godot host installs (NovaSimulation::
// resolve_item_traits builds the same shape from NovaItemDatabase).
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
	ns::NetClientView view;
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

bool run_default_view_desyncs_on_no_callback_record() {
	const int32_t ax = 300 << 16, ay = -40 << 16, az = 7 << 16;
	ns::NetClientView view; // phase-1 heuristic only: every non-player type = Infantry
	view.apply(0x0A, build_mixed_frame(ax, ay, az));

	// The heuristic walks the header-only record as a 14-B infantry body, eating the
	// vehicle record's header — everything after is junk. The guard: the vehicle must
	// NOT have decoded to its true position (if it ever does, the desync is gone and
	// this test should be rethought, not deleted).
	const ns::ClientEntityState *veh = view.state().find(kVehicleHandle);
	const bool vehicle_correct = veh != nullptr &&
			veh->x == expected_coord(ax, 50 << 16) &&
			veh->y == expected_coord(ay, -20 << 16) &&
			veh->z == expected_coord(az, 3 << 16);
	return expect(!vehicle_correct,
	              "phase-1 view mis-sizes the no-callback record (witnessed desync)");
}

bool run_items_table_outranks_pool_blanket() {
	const int32_t ax = 100 << 16, ay = 0, az = -5 << 16;
	ns::NetClientView view;
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

} // namespace

int main() {
	bool ok = true;
	ok &= run_items_table_sizes_mixed_frame();
	ok &= run_default_view_desyncs_on_no_callback_record();
	ok &= run_items_table_outranks_pool_blanket();
	if (ok) std::printf("client_view_class_resolver: OK\n");
	return ok ? 0 : 1;
}
