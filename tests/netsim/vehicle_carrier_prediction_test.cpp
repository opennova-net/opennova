// The joiner's prediction freeze for carried vehicle rows, through the
// production JoinerRole frame. A vehicle row riding a pool-1 deck (LCAC/ship)
// is the seat-follow case: it stays frozen between records and recomposes onto
// the deck every tick. A vehicle whose §5.13 carrier is a pool-2 STATIC (the
// bridge, roof or ramp its groundEntity resolves to while it drives over a
// structure) composes the record into a world sample and the family mover
// predicts from it exactly as the 0xFFFF form does — retail's reader composes
// the carrier form and still runs the not-driven client leg for every vehicle
// the client is not driving [orig: Entity_SerializeVehicleState read side,
// Entity_TransformLocalToWorld @0x4608ce; the not-driven leg @0x48B7F0].

#include <net/npwire/entity_class.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelfHandle = 0x0005;
constexpr int32_t kHullType = 1291;
constexpr int32_t kBridgeType = 0x0777;

// The socket seam: swallows what the role ships, receives nothing.
class CountingSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	CountingSocket socket;
	inmatch::TickInput input;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.kit_seams.respawn = [] {};
	}
};

int32_t fixed_of(float v) { return w::to_fixed(v); }

w::Entity hull_seed(w::Vec3 pos, uint8_t team) {
	w::Entity v;
	v.kind = w::EntityKind::Item;
	v.item_id = kHullType;
	v.has_item_def = true;
	v.item_type = 1;
	v.item_attrib = 0x40u;
	v.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
	v.spawn_origin = (1u << 24) | 3u;
	v.position = pos;
	v.health = v.health_max = 3000;
	v.team = team;
	return v;
}

// Mirror a spawned registry entity into the joiner's replica row the way the
// load stream would have (identity + pose), so the fold and the mirror agree.
replication::ClientEntityState &seed_row(replication::ClientState &state,
                                         w::EntityHandle h, const w::Entity &e,
                                         EntityClass cls) {
	replication::ClientEntityState &row = state.upsert(h.packed);
	row.type_id = static_cast<uint16_t>(e.item_id);
	row.cls = cls;
	row.x = fixed_of(e.position.x);
	row.y = fixed_of(e.position.y);
	row.z = fixed_of(e.position.z);
	row.heading_known = true;
	row.heading_bam = w::bam_heading_from_mission_yaw_deg(e.yaw);
	return row;
}

FrameUpdateRecord vehicle_record(uint16_t handle, uint16_t parent, int32_t x, int32_t y,
                                 int32_t z, int16_t euler_z, int32_t speed_reg,
                                 int16_t steer_hi) {
	FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = static_cast<uint16_t>(kHullType);
	r.cls = EntityClass::Vehicle;
	r.vehicle.parent_slot_handle = parent;
	r.vehicle.pos_x_compressed = network_compress_fixedpoint(x);
	r.vehicle.pos_y_compressed = network_compress_fixedpoint(y);
	r.vehicle.pos_z_compressed = network_compress_fixedpoint(z);
	r.vehicle.euler_z = euler_z;
	r.vehicle.flags_byte = 0;
	r.vehicle.is_dead_pose = false;
	r.vehicle.health_word = 3000;
	r.vehicle.weapon_aim_y = network_compress_fixedpoint(speed_reg);
	r.vehicle.weapon_heading_bam = steer_hi;
	return r;
}

double moved_units(const w::Entity &e, w::Vec3 from) {
	return std::hypot(double(e.position.x) - from.x, double(e.position.y) - from.y);
}

bool run_static_carrier_predicts_and_deck_rider_freezes() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.registry.configure_pool(2, 16);
	world.add_system(&world.ai);
	world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
	                             kSelfHandle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr, "the joiner's local body spawned")) return false;
	const w::Vec3 origin = local->position;
	const uint8_t team = local->team;

	// The drive rig's ground hull traits: a physics-1 PlayerControl vehicle.
	w::VehicleTraits traits;
	traits.physics = 1;
	traits.player_control = true;
	traits.player_speed = 94 * 293;
	traits.acceleration = 15 * 4;
	traits.deceleration = 70 * 4;
	traits.turn_rate = 65 * 192426;
	traits.turn_rate2 = 41 * 192426;
	world.vehicles.traits.set(kHullType, traits);

	// A deck (pool-1 vehicle) with a hull riding it; a bridge (pool-2 static)
	// with a hull driving over it. Both carried hulls receive the same forward
	// speed register from the wire.
	const w::Entity deck = hull_seed({origin.x + 20.0f, origin.y, origin.z}, team);
	const w::EntityHandle deck_h = world.registry.spawn(1, deck);
	// The rider's spawn pose IS the composed seat pose: the record's (2, 0, 1)
	// offset is CARRIER-LOCAL (the deck's engine heading is 90 deg at mission
	// yaw 0), so it is lifted through the deck pose rather than added in world.
	const int32_t rider_lx = fixed_of(2.0f), rider_ly = 0, rider_lz = fixed_of(1.0f);
	const WorldPose rider_seat = network_transform_local_to_world(rider_lx, rider_ly, rider_lz,
			fixed_of(deck.position.x), fixed_of(deck.position.y), fixed_of(deck.position.z),
			uint32_t(w::bam_heading_from_mission_yaw_deg(deck.yaw)), 0u, 0u);
	const w::Entity rider = hull_seed({float(rider_seat.x) / 65536.0f,
	                                          float(rider_seat.y) / 65536.0f,
	                                          float(rider_seat.z) / 65536.0f}, team);
	const w::EntityHandle rider_h = world.registry.spawn(1, rider);
	w::Entity bridge;
	bridge.kind = w::EntityKind::Building;
	bridge.item_id = kBridgeType;
	bridge.position = {origin.x - 20.0f, origin.y, origin.z};
	const w::EntityHandle bridge_h = world.registry.spawn(2, bridge);
	const w::Entity bridged = hull_seed({origin.x - 20.0f, origin.y, origin.z + 1.0f}, team);
	const w::EntityHandle bridged_h = world.registry.spawn(1, bridged);
	if (!expect(deck_h.valid() && rider_h.valid() && bridge_h.valid() && bridged_h.valid(),
	            "the deck, its rider, the bridge and the bridged hull spawned"))
		return false;
	if (!expect(bridge_h.pool() == 2 && deck_h.pool() == 1, "carrier pools as intended"))
		return false;

	replication::ClientState &state = h.role.runtime->state();
	seed_row(state, deck_h, deck, EntityClass::Vehicle);
	seed_row(state, rider_h, rider, EntityClass::Vehicle);
	seed_row(state, bridged_h, bridged, EntityClass::Vehicle);
	seed_row(state, bridge_h, bridge, EntityClass::Unknown);
	h.role.runtime->view().set_item_class_resolver(
			[](uint16_t type) -> std::optional<EntityClass> {
				if (type == kHullType) return EntityClass::Vehicle;
				if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
				return std::nullopt;
			});

	// One §5.13 frame: the deck free-standing; the rider parented to the deck
	// (its seat-local offset); the bridged hull parented to the bridge. Both
	// carried hulls carry a 30 u/s forward register and a straight steer.
	const int32_t forward = 30 * 293;
	const int16_t heading_hi = int16_t(uint32_t(w::bam_heading_from_mission_yaw_deg(0)) >> 16);
	{
		FrameUpdate fu;
		fu.flags2 = 0;
		fu.mount_handle = 0xFFFF;
		fu.health = 100;
		fu.anchor_x = fixed_of(origin.x);
		fu.anchor_y = fixed_of(origin.y);
		fu.anchor_z = fixed_of(origin.z);
		fu.records.push_back(vehicle_record(deck_h.packed, 0xFFFF, fixed_of(20.0f), 0, 0,
		                                    heading_hi, 0, heading_hi));
		fu.records.push_back(vehicle_record(rider_h.packed, deck_h.packed, rider_lx, rider_ly,
		                                    rider_lz, 0, forward, heading_hi));
		fu.records.push_back(vehicle_record(bridged_h.packed, bridge_h.packed, 0, 0,
		                                    fixed_of(1.0f), 0, forward, heading_hi));
		h.role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	}
	bool ok = true;
	{
		const replication::ClientEntityState *brow = state.find(bridged_h.packed);
		const replication::ClientEntityState *rrow = state.find(rider_h.packed);
		ok &= expect(brow != nullptr && brow->carrier_handle == bridge_h.packed &&
		                     brow->net_seat_valid,
		             "the bridged hull's row carries the static as its carrier");
		ok &= expect(rrow != nullptr && rrow->carrier_handle == deck_h.packed &&
		                     rrow->net_seat_valid,
		             "the rider's row carries the deck as its carrier");
	}

	const w::Vec3 bridged_start = world.registry.get(bridged_h)->position;
	const w::Vec3 rider_start = world.registry.get(rider_h)->position;
	for (int t = 0; t < 62; ++t) h.role.run_tick(h.input);

	const w::Entity *bridged_now = world.registry.get(bridged_h);
	const w::Entity *rider_now = world.registry.get(rider_h);
	const replication::ClientEntityState *brow = state.find(bridged_h.packed);
	const replication::ClientEntityState *rrow = state.find(rider_h.packed);
	const replication::ClientEntityState *drow = state.find(deck_h.packed);
	if (!expect(bridged_now && rider_now && brow && rrow && drow, "rows and entities persist"))
		return false;
	const double bridged_moved = moved_units(*bridged_now, bridged_start);
	const double rider_moved = moved_units(*rider_now, rider_start);
	std::printf("[carrier-prediction] bridged: predicted=%d moved=%.3f u; rider: predicted=%d "
	            "moved=%.3f u\n",
	            int(bridged_now->veh.net_predicted), bridged_moved,
	            int(rider_now->veh.net_predicted), rider_moved);

	ok &= expect(bridged_now->veh.net_predicted,
	             "a vehicle carried by a pool-2 static keeps prediction armed");
	ok &= expect(bridged_moved > 0.25,
	             "the static-carried vehicle integrates its received speed between records");
	ok &= expect(brow->x == fixed_of(bridged_now->position.x) &&
	                     brow->y == fixed_of(bridged_now->position.y),
	             "the bridged row publishes the predicted pose (the post-mover recompose "
	             "does not drag it back to the record sample)");
	ok &= expect(!rider_now->veh.net_predicted, "a vehicle riding a pool-1 deck stays frozen");
	// The deck itself is parked (a zero register), so the rider's seat-follow
	// keeps it where it spawned; the pose it holds is the deck's live pose plus
	// the record's carrier-local offset, both on the row and on the registry
	// entity that adopts it.
	const w::Entity *deck_now = world.registry.get(deck_h);
	ok &= expect(deck_now != nullptr && moved_units(*deck_now, deck.position) < 0.01,
	             "the parked deck did not move");
	ok &= expect(rider_moved < 0.05, "the deck rider holds its seat-follow pose");
	{
		const WorldPose posed = network_transform_local_to_world(rider_lx, rider_ly, rider_lz,
				drow->x, drow->y, drow->z, uint32_t(drow->heading_bam),
				uint32_t(drow->pitch_bam), uint32_t(drow->roll_bam));
		const double row_err = std::sqrt(std::pow((rrow->x - posed.x) / 65536.0, 2) +
		                                 std::pow((rrow->y - posed.y) / 65536.0, 2) +
		                                 std::pow((rrow->z - posed.z) / 65536.0, 2));
		ok &= expect(row_err < 0.05, "the deck rider's row composes onto the deck's pose");
		const double entity_err = std::hypot(double(rider_now->position.x) - posed.x / 65536.0,
		                                     double(rider_now->position.y) - posed.y / 65536.0);
		ok &= expect(entity_err < 0.05, "the frozen rider's registry entity adopts the composed pose");
	}
	return ok;
}

} // namespace

int main() {
	bool ok = run_static_carrier_predicts_and_deck_rider_freezes();
	if (!ok || failures != 0) {
		std::fprintf(stderr, "vehicle_carrier_prediction_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_carrier_prediction_test: OK\n");
	return 0;
}
