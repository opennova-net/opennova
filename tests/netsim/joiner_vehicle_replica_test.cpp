// A joiner's replica vehicle rows through the production JoinerRole frame:
// the §5.13 reader's record tail (the host's Flags bits, the health word, the
// destroyed-bit kill edge), the mover freeze, the +0x170 claimant a retail
// client attaches from its remote riders' records, and the 0x0D carrier an
// addeweap child follows.

#include <net/npwire/entity_class.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/peer_addr.h>
#include <runtime/audio/sound_profile.h>
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
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

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
constexpr uint16_t kRemoteDriver = 0x0006;
constexpr int32_t kTankType = 1296;
constexpr uint8_t kControlBone = 1;

class SilentSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	SilentSocket socket;
	inmatch::TickInput input;
	w::EntityHandle tank;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.registry.configure_pool(1, 16);
		role.bind(*kernel);
		kernel->world.load_systems();
		role.set_socket(&socket, PeerAddr{});
		role.create_runtime("TankJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.kit_seams.respawn = [] {};
		role.poll_preload();
		role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
		                           kSelfHandle, w::kPlayerInfantryTypeId);
		role.run_tick(input);

		w::VehicleTraits traits;
		traits.family = w::VehicleFamily::Tank;
		traits.physics = 1;
		traits.player_control = true;
		traits.player_speed = 94 * 293;
		kernel->world.vehicles.traits.set(kTankType, traits);
		const w::Entity *local = kernel->local.player();
		w::Entity seed;
		seed.kind = w::EntityKind::Item;
		seed.item_id = kTankType;
		seed.has_item_def = true;
		seed.item_type_index = 7; // the def row's ordinal the +0x1C kill gate reads
		seed.item_type = 1;
		seed.item_attrib = 0x40u;
		seed.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
		seed.spawn_origin = (1u << 24) | 3u;
		seed.position = local != nullptr ? local->position : w::Vec3{};
		seed.health = seed.health_max = 3000;
		w::Seat driver;
		driver.type = w::SeatType::Driver;
		driver.bone_index = kControlBone;
		driver.retail_slot = 8;
		seed.seats.push_back(driver);
		tank = kernel->world.registry.spawn(1, seed);
		replication::ClientEntityState &row = role.runtime->state().upsert(tank.packed);
		row.type_id = static_cast<uint16_t>(kTankType);
		row.cls = EntityClass::Vehicle;
		row.x = w::to_fixed(seed.position.x);
		row.y = w::to_fixed(seed.position.y);
		row.z = w::to_fixed(seed.position.z);
		role.runtime->view().set_item_class_resolver(
				[](uint16_t type) -> std::optional<EntityClass> {
					if (type == kTankType) return EntityClass::Vehicle;
					if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
					return std::nullopt;
				});
	}
	w::Entity &hull() { return *kernel->world.registry.get(tank); }
	// One §5.13 record for the hull at its own position, then the joiner frame.
	void record(uint8_t flags, uint16_t health_word) {
		const w::Entity &e = hull();
		FrameUpdate fu;
		fu.flags2 = 0;
		fu.mount_handle = 0xFFFF;
		fu.health = 100;
		fu.anchor_x = w::to_fixed(e.position.x);
		fu.anchor_y = w::to_fixed(e.position.y);
		fu.anchor_z = w::to_fixed(e.position.z);
		FrameUpdateRecord r;
		r.handle = tank.packed;
		r.type_id = static_cast<uint16_t>(kTankType);
		r.cls = EntityClass::Vehicle;
		r.vehicle.parent_slot_handle = 0xFFFF;
		r.vehicle.pos_x_compressed = network_compress_fixedpoint(0);
		r.vehicle.pos_y_compressed = network_compress_fixedpoint(0);
		r.vehicle.pos_z_compressed = network_compress_fixedpoint(0);
		r.vehicle.flags_byte = flags;
		r.vehicle.is_dead_pose = (flags & kVehicleCompactFlagDeadPose) != 0;
		r.vehicle.health_word = health_word;
		fu.records.push_back(r);
		role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
		role.run_tick(input);
	}
};

// The reader's tail on every record: Flags bits 0/3/4/5/7 follow the host and
// bits 1/2/6 stay the client's own; the health word lands on the client row.
// The mirrored crash bit then lets the client's own suspension latch the crash.
// [orig: Entity_SerializeVehicleState @0x460AEA..0x460AFC (Flags), @0x460AF1..
//  0x460AFF (Health); Entity_ComputeSuspensionAndOrientation's client arm
//  @0x46997C..0x469982]
bool run_record_tail_mirrors_flags_and_health() {
	Harness h;
	bool ok = true;
	h.hull().flags |= 0x40u; // a client-owned bit
	h.record(0x90, 1234);
	ok &= expect((h.hull().flags & 0xB9u) == 0x90u,
	             "the host's lights and crash bits land on the client row");
	ok &= expect((h.hull().flags & 0x40u) != 0, "bit 6 stays the client's own");
	ok &= expect(h.hull().health == 1234, "the record's health word lands on the client row");
	h.hull().veh.crash_request = 1;
	ok &= expect(h.kernel->world.vehicles.suspension_arm(h.hull(), false, nullptr, nullptr) &&
	                     h.hull().veh.crashed != 0,
	             "the mirrored crash bit lets the client suspension latch the crash");
	h.record(0x00, 1200);
	ok &= expect((h.hull().flags & 0xB9u) == 0u && (h.hull().flags & 0x40u) != 0,
	             "a clear record drops the host bits and keeps the client's");
	return ok;
}

// The destroyed bit on a client row that is not dead yet kills it through
// Entity_KillBySlotId while its Health word is nonzero; the dead-pose form
// then stores a zero health word. [orig: Entity_SerializeVehicleState
// @0x460A1A..0x460AE2; the short form's zero @0x460688]
bool run_destroyed_bit_kills_the_client_row() {
	Harness h;
	bool ok = true;
	h.record(0x00, 2000);
	ok &= expect(h.hull().health == 2000, "the live record's health lands");
	h.record(0x02, 2000);
	ok &= expect(h.hull().health == 0, "the destroyed bit kills the live client row");
	ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) != 0,
	             "the kill ran the class death callback");
	h.record(0x06, 0);
	ok &= expect(h.hull().health == 0 && !h.hull().veh.net_predicted,
	             "the dead-pose record keeps the wreck dead and frozen");
	return ok;
}

// Wire bit0 is not a vehicle freeze: no vehicle mover tests it and the pool-1
// update calls the mover ungated. A frozen wreck also drops the tank's pivot
// latch and yaw rate so its fold cannot keep the fourth loop alive.
// [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53; the tank pivot tail
//  Entity_UpdateTankVehiclePhysics @0x48ACB5..0x48AD53]
bool run_freeze_predicate_and_pivot_clear() {
	Harness h;
	bool ok = true;
	h.record(0x01, 3000);
	ok &= expect(h.hull().veh.net_predicted, "wire bit0 alone keeps the vehicle predicted");
	h.hull().veh.pivot_sound_latched = true;
	h.hull().veh.wheel_rate_bam = 5000;
	h.hull().veh.pivot_sound_prev_rate = 5000;
	h.record(0x06, 0);
	ok &= expect(!h.hull().veh.net_predicted && !h.hull().veh.pivot_sound_latched &&
	                     h.hull().veh.wheel_rate_bam == 0 &&
	                     h.hull().veh.pivot_sound_prev_rate == 0,
	             "the frozen wreck clears the pivot latch and its yaw rate");
	return ok;
}

// A retail client attaches every remote rider from its records, so +0x170
// names a remote driver on the client too; the tank tail then arms the pivot
// cue for a remote pivot. The joiner's own row never answers remotely.
// [orig: NetPacket_SerializePlayerState @0x4C1317 -> Entity_TryAttachOrDetach ->
//  Entity_AttachToVehicleSlot @0x4947D2 / @0x4948D8;
//  Entity_UpdateTankVehiclePhysics @0x48ACB5..0x48AD53]
bool run_remote_driver_is_the_claimant() {
	Harness h;
	bool ok = true;
	w::World &world = h.kernel->world;
	static constexpr char kProfile[] =
			"begin \"SP_RemoteTank\"\n Soundloop_1 TANK_IDLE 1 1\n swivel_shift TANK_SHIFT\nend\n";
	ok &= expect(world.tables.sound_profiles.parse(kProfile, sizeof(kProfile) - 1) == 1,
	             "the tank sound profile parses");
	w::VehicleTraits traits = *world.vehicles.traits.get(kTankType);
	traits.sound_profile = "SP_RemoteTank";
	w::Entity scratch;
	ok &= expect(world.vehicles.claimant(h.hull(), scratch) == nullptr,
	             "an empty hull has no claimant");
	replication::ClientEntityState &driver = h.role.runtime->state().upsert(kRemoteDriver);
	driver.cls = EntityClass::Player;
	driver.type_id = static_cast<uint16_t>(w::kPlayerInfantryTypeId);
	driver.carrier_handle = h.tank.packed;
	driver.mount_bone = kControlBone;
	driver.net_has_compact = true;
	const w::Entity *claimant = world.vehicles.claimant(h.hull(), scratch);
	ok &= expect(claimant != nullptr && claimant->mounted &&
	                     claimant->mount_target == h.tank &&
	                     claimant->handle == w::EntityHandle{kRemoteDriver},
	             "the remote driver on the control seat is the hull's claimant");
	w::Entity::VehicleMotorState &m = h.hull().veh;
	m.settle_2f0 = 0;
	m.speed = 0;
	m.wheel_rate_bam = 40000;
	m.pivot_sound_latched = false;
	m.steer_target_bam = io::bam_add(m.yaw_bam, 0x08000000);
	world.out.slot_sounds.clear();
	world.vehicles.update_traction_sound(h.hull(), traits);
	ok &= expect(m.pivot_sound_latched && world.out.slot_sounds.size() == 1 &&
	                     world.out.slot_sounds[0].slot == 46,
	             "a remote driver's pivot arms the swivel cue on the joiner");
	// The engine-running latch of the part-spin machine reads the same +0x170
	// [orig: Entity_UpdatePartSpinAccumulator @0x4928E8].
	m.part_spin = {};
	world.vehicles.rotor_machine_tick(h.hull(), traits);
	ok &= expect(m.part_spin.speed == w::kRotorRateFull,
	             "a remote driver spins the part accumulator up on the joiner");
	driver.mount_bone = 0;
	driver.carrier_handle = w::EntityHandle::kInvalid;
	ok &= expect(world.vehicles.claimant(h.hull(), scratch) == nullptr,
	             "a dismounted rider leaves no claimant");
	replication::ClientEntityState &self = h.role.runtime->state().upsert(kSelfHandle);
	self.cls = EntityClass::Player;
	self.carrier_handle = h.tank.packed;
	self.mount_bone = kControlBone;
	ok &= expect(world.vehicles.claimant(h.hull(), scratch) == nullptr,
	             "the joiner's own wire row never answers as a remote claimant");
	return ok;
}

// An addeweap child whose authored slot the joiner could not resolve adopts the
// rigid pose its decoded row recomposes from the carrier it follows: the 0x0D
// TARGET (groundEntity), never the occupant back-reference in the parent field.
// [orig: NetPacket_SerializeEntityPoolToPacket_0 +0x170 @0x503BC9, +0x28 @0x503C22;
//  the ewep move fn Entity_UpdateTransformAndTurret reads groundEntity @0x440CBF]
bool run_unresolved_attachment_follows_its_target_carrier() {
	Harness h;
	bool ok = true;
	w::World &world = h.kernel->world;
	static constexpr uint16_t kGunType = 1871;
	h.role.runtime->view().set_item_class_resolver(
			[](uint16_t type) -> std::optional<EntityClass> {
				if (type == kTankType) return EntityClass::Vehicle;
				if (type == kGunType) return EntityClass::NoNetworkCallback;
				if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
				return std::nullopt;
			});
	w::Entity gun_seed;
	gun_seed.kind = w::EntityKind::Item;
	gun_seed.item_id = kGunType;
	gun_seed.spawn_origin = (1u << 24) | 4u;
	gun_seed.position = h.hull().position;
	gun_seed.emplacement_parent = h.tank;
	gun_seed.emplacement_parent_spawn_id = h.hull().registry_spawn_id;
	const w::EntityHandle gun = world.registry.spawn(1, gun_seed);
	ok &= expect(gun.valid(), "the attached gun spawned");
	replication::ClientEntityState &row = h.role.runtime->state().upsert(gun.packed);
	row.type_id = kGunType;
	row.cls = EntityClass::NoNetworkCallback;
	row.x = w::to_fixed(gun_seed.position.x);
	row.y = w::to_fixed(gun_seed.position.y);
	row.z = w::to_fixed(gun_seed.position.z);
	row.heading_bam = w::bam_heading_from_mission_yaw_deg(30);
	row.target_handle = h.tank.packed;
	row.parent_handle = 0x0003; // its gunner, a pool-0 back-reference
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	const w::Entity *live = world.registry.get(gun);
	ok &= expect(live != nullptr && live->yaw == 30,
	             "the unresolved attachment adopts the pose recomposed on its target carrier");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_record_tail_mirrors_flags_and_health();
	ok &= run_destroyed_bit_kills_the_client_row();
	ok &= run_freeze_predicate_and_pivot_clear();
	ok &= run_remote_driver_is_the_claimant();
	ok &= run_unresolved_attachment_follows_its_target_carrier();
	if (!ok || failures != 0) {
		std::fprintf(stderr, "joiner_vehicle_replica_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_vehicle_replica_test: OK\n");
	return 0;
}
