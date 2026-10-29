// A joiner's replica vehicle rows through the production JoinerRole frame:
// the §5.13 reader's record tail (the host's Flags bits, the health word, the
// destroyed-bit kill edge), the mover freeze, the +0x170 claimant a retail
// client attaches from its remote riders' records, and the 0x0D carrier an
// addeweap child follows.

#include <formats/aip/aip.h>
#include <formats/def/def.h>
#include <net/protocol/protocol.h>
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
#include <runtime/mission/promote.h> // mission::ItemSeatSpec
#include <runtime/replication/client_state.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

#include "../common/hull_brain.h"
#include "../common/replica_twin.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include "../common/test_expect.h"

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int &failures = test_expect::failures;
using test_expect::expect;

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
		role.set_protocol(&kJo);
		role.set_rules(opennova::world::title_rules("jo"));
		role.create_runtime("TankJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
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
		tank = test_common::materialize_twin(role.materializer(), role.runtime->state(),
				kernel->world, 1, seed, EntityClass::Vehicle);
		// A drivable tank row (AIData, ai_function cveh) takes its brain from the
		// class init on the joiner as on the host (D-NET-433 part A); a brainless
		// hull's client mover would stop at its head.
		test_common::brain_hull(kernel->world, tank);
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
	// One §5.13 record for the hull at its own position, then the joiner frame. The
	// dead-pose form carries the wreck's euler X / Y words.
	void record(uint8_t flags, uint16_t health_word, int16_t euler_x = 0, int16_t euler_y = 0) {
		const w::Entity &e = hull();
		FrameUpdate fu;
		fu.flags2 = 0;
		fu.carried_handle = 0xFFFF;
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
		r.vehicle.euler_x = euler_x;
		r.vehicle.euler_y = euler_y;
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

// D-NET-916, NOT-A-DIVERGENCE (Q9-1b (a)): a spawned hull holds its spawn pose
// until its first compact arms the client mover, as retail's does. Retail's class init
// runs after the 0x0D memset and seeds the chase target from the hull's pose with a zero
// progress word, so the first entity pass measures no distance, skips the snap and takes
// the small arm; the hull holds until its first compact stages a target.
// [orig: NapiNPClientMsg_0x00D @0x432C40 - the memset @0x432D1B..0x432D23;
//  Game_StartMission - NapiClient_WaitForGameStart @0x524E29, then
//  Entity_InitAllFromModels @0x52567F; Entity_InitVehicleAIFromDef @0x4686C0 (the
//  call @0x4687FF); Entity_InitVehicleAI @0x460200 - the seed @0x4602F9..0x460326;
//  Entity_UpdateVehiclePhysics @0x48AF00 - the client chase @0x48B54C, the snap
//  @0x48B563..0x48B640 not reached, the < 0x2000 arm @0x48B64B]
// This rig spawns its hull straight into the registry, so no class init arms the hull's
// mover ahead of the compact; a streamed hull's class init does
// (run_streamed_hull_runs_its_client_mover_from_the_staged_pose).
bool run_hull_holds_its_spawn_pose_until_its_first_compact() {
	Harness h;
	bool ok = true;
	const w::Vec3 spawn{100.0f, 200.0f, 10.0f};
	h.hull().position = spawn;
	replication::ClientEntityState *row = h.role.runtime->state().find(h.tank.packed);
	ok &= expect(row != nullptr, "the hull's replica row exists");
	if (row == nullptr) return ok;
	row->x = w::to_fixed(spawn.x);
	row->y = w::to_fixed(spawn.y);
	row->z = w::to_fixed(spawn.z);
	for (int frame = 0; frame < 4; ++frame) h.role.run_tick(h.input);
	ok &= expect(!h.hull().veh.net_predicted, "a rig hull with no class init: the client mover is not armed");
	ok &= expect(w::to_fixed(h.hull().position.x) == w::to_fixed(spawn.x) &&
	                     w::to_fixed(h.hull().position.y) == w::to_fixed(spawn.y) &&
	                     w::to_fixed(h.hull().position.z) == w::to_fixed(spawn.z),
	             "the hull holds its spawn pose, not the world origin");
	h.record(0x00, 3000);
	ok &= expect(h.hull().veh.net_predicted, "the first compact arms the client mover");
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

// D-NET-1365: a live record on a row the client holds dead is the host's respawn, and
// the client respawns the vehicle itself: the dead bit goes, the record's health word lands
// over the def's, the intact model comes back, the client mover re-arms, and an EWeap
// aircraft's class init re-seeds its spent rockets.
// [orig: Entity_SerializeVehicleState @0x460560 - the wire bit @0x460918, the row's dead bit
//  @0x46092B, the pose copy @0x460933..0x460961, the Entity_RespawnVehicle call @0x460964;
//  Entity_RespawnVehicle @0x45FF40 - the class init call @0x45FF68, Flags @0x460009;
//  Entity_InitAircraftWeaponBones @0x490160 - the bind @0x49017B, the split @0x490192]
bool run_live_record_respawns_the_dead_client_row() {
	Harness h;
	bool ok = true;
	w::World &world = h.kernel->world;
	h.record(0x00, 2000);
	h.record(0x02, 2000);
	ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) != 0,
	             "respawn: the destroyed bit killed the client row");
	h.record(0x06, 0);
	// The hull as an EWeap aircraft row whose rockets the fight spent.
	world.tables.weapons.entries.resize(2);
	w::WeaponTableEntry &rockets = world.tables.weapons.entries[1];
	rockets.name = "WPN_TESTROCKETS";
	rockets.valid = true;
	rockets.clipsize = 12;
	rockets.startrounds = 41;
	w::ItemDeathTraits air;
	air.air_class_init = true;
	world.tables.item_death_traits.set(kTankType, air);
	h.hull().item_attrib |= w::kItemAttribEweap;
	h.hull().primary_weapon = rockets.name;
	h.hull().primary_weapon_slot_adm = 1;
	h.hull().primary_weapon_slot.clip = 0;
	h.hull().primary_weapon_slot.reserve = 0;
	world.out.destruction.husk_swaps.clear();
	h.record(0x00, 2500);
	ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) == 0,
	             "respawn: the live record respawns the client row");
	ok &= expect(h.hull().health == 2500 && h.hull().alive,
	             "respawn: the record's health word lands over the def's");
	bool intact = false;
	for (const w::HuskSwapEvent &swap : world.out.destruction.husk_swaps)
		intact |= swap.restore_intact && swap.wire_handle == h.tank.packed;
	ok &= expect(intact, "respawn: the intact model comes back");
	ok &= expect(h.hull().primary_weapon_slot_adm == 1 &&
	                     h.hull().primary_weapon_slot.clip == 12 &&
	                     h.hull().primary_weapon_slot.reserve == 29,
	             "respawn: the class init re-seeds the spent rockets");
	ok &= expect(h.hull().veh.net_predicted, "respawn: the client mover re-arms");
	return ok;
}

// The client's respawn drops the rotor speed and rate words after the def's class init, and
// only when the def binds one: the gate is def+0x148, not the row's brain. Every vehicle row
// binds one (here the cveh row's); a brainless client row drops them too, while a def whose
// ai_function row binds none keeps them.
// [orig: Entity_SerializeVehicleState @0x460560 - the Entity_RespawnVehicle call @0x460964;
//  Entity_RespawnVehicle @0x45FF40 - the def+0x148 test @0x45FF53, +0x468 = 0 @0x45FF6D,
//  +0x460 = 0 @0x45FF73; g_EntityClassEventCallbackTable @0x813000 - the cveh row @0x813378,
//  its init column -> Entity_InitVehicleAIFromDef @0x4686C0]
bool run_respawn_drops_the_rotor_words_by_the_def_class_init() {
	bool ok = true;
	for (const bool class_init : {true, false}) {
		Harness h;
		w::World &world = h.kernel->world;
		world.ai.release(h.tank); // a client row with no brain
		w::VehicleTraits traits = *world.vehicles.traits.get(kTankType);
		traits.brain_class = class_init ? w::VehicleBrainClass::Ground : w::VehicleBrainClass::Unset;
		world.vehicles.traits.set(kTankType, traits);
		h.record(0x00, 2000);
		h.record(0x02, 2000);
		h.record(0x06, 0);
		ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) != 0,
		             "rotor drop: the destroyed bit killed the brainless client row");
		h.hull().veh.part_spin.speed = w::kRotorSpeedMax;
		h.hull().veh.part_spin.rate = w::kRotorRateFull;
		h.record(0x00, 2500);
		ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) == 0,
		             "rotor drop: the live record respawns the brainless client row");
		if (class_init)
			ok &= expect(h.hull().veh.part_spin.speed == 0 && h.hull().veh.part_spin.rate == 0,
			             "rotor drop: a def with a class init drops the rotor words, brain or none");
		else
			ok &= expect(h.hull().veh.part_spin.speed != 0,
			             "rotor drop: a def with no class init keeps the rotor speed");
	}
	return ok;
}

// D-NET-1365 (V-ROLL-BV D1): a tilted wreck respawns level. The live form stages zero
// Pitch / Roll words, the respawn edge copies them into the row and the respawn builds its
// matrix from them, so the row and its mover's arming take 0 / 0, not the last dead-pose
// record's euler X / Y.
// [orig: Entity_SerializeVehicleState @0x460560 - `xor eax, eax` @0x4607E5, the live form's
//  stores @0x4608EB / @0x4608F1, the dead-pose form's @0x460823 / @0x460829, the copy
//  @0x46094B..0x460961; Entity_RespawnVehicle @0x45FF40 - the matrix call @0x460179]
bool run_tilted_wreck_respawns_level() {
	Harness h;
	bool ok = true;
	constexpr int16_t kWreckPitch = 0x1000;
	constexpr int16_t kWreckRoll = -0x0800;
	h.record(0x00, 2000);
	h.record(0x02, 2000);
	h.record(0x06, 0, kWreckPitch, kWreckRoll);
	ok &= expect(h.hull().pitch_bam == int32_t(kWreckPitch) * 65536 &&
	                     h.hull().roll_bam == int32_t(kWreckRoll) * 65536,
	             "level respawn: the wreck rests at its dead-pose tilt");
	h.record(0x00, 2500);
	ok &= expect(((h.hull().flags | h.hull().engine_flags) & w::kEntityFlagDead) == 0,
	             "level respawn: the live record respawns the client row");
	ok &= expect(h.hull().pitch_bam == 0 && h.hull().roll_bam == 0,
	             "level respawn: the respawned row stands level");
	ok &= expect(h.hull().veh.air_pitch_bam == 0 && h.hull().veh.air_roll_bam == 0,
	             "level respawn: the client mover arms level");
	const replication::ClientEntityState *row = h.role.runtime->state().find(h.tank.packed);
	ok &= expect(row != nullptr && row->pitch_bam == 0 && row->roll_bam == 0,
	             "level respawn: the replica row carries the level attitude");
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
			"begin \"SP_RemoteTank\"\r\n Soundloop_1 TANK_IDLE 1 1\r\n swivel_shift TANK_SHIFT\r\nend\r\n";
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
	gun_seed.item_type_index = 1; // the def row's ordinal: the walks' +0x1C gate
	gun_seed.spawn_origin = (1u << 24) | 4u;
	gun_seed.position = h.hull().position;
	gun_seed.emplacement_parent = h.tank;
	gun_seed.emplacement_parent_spawn_id = h.hull().registry_spawn_id;
	const w::EntityHandle gun = test_common::materialize_twin(h.role.materializer(),
			h.role.runtime->state(), world, 1, gun_seed, EntityClass::NoNetworkCallback);
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
	ok &= expect(live != nullptr && live->heading == row.heading_bam,
	             "the unresolved attachment adopts the pose recomposed on its target carrier");
	return ok;
}

// D-NET-690: an addeweap child the joiner's world resolves runs the ewep class
// update there, every frame, with no role gate: the update composes it from the
// carrier at the anchor its subType names, and with no anchor copies the
// carrier's root pose only while the carrier's matrix bit is clear. Its 0x0D pose
// is only the spawn seed: the world row takes it once and the decoded row reads
// the world's pose back, never a rigid child-in-carrier offset. The tank's client
// mover runs to its matrix-build tail on every visit (it is not predicted: no
// compact has landed), so the bit is current when the child poses and the
// anchorless child keeps the seed. The copy itself is pinned on a stale carrier
// in vehicle_mount (test_an_anchorless_child_copies_only_a_stale_carrier).
// [orig: NapiNPClientMsg_0x00D +0x1C4 @0x43331A; Entity_UpdatePool1Slot
//  @0x4B8E53; Entity_UpdateTransformAndTurret @0x440CA0, the gate
//  @0x4410EA..0x4410F1, the root copy @0x441153..0x4411BC, the child's bit
//  @0x4411C2; Entity_UpdateTankVehiclePhysics @0x488AB0, the tail
//  @0x48AEB6..0x48AEC9 ahead of its one retn @0x48AEDA]
bool run_resolved_attachment_runs_the_ewep_update() {
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
	gun_seed.item_type_index = 1; // the def row's ordinal: the walks' +0x1C gate
	gun_seed.spawn_origin = (1u << 24) | 4u;
	gun_seed.position = h.hull().position;
	gun_seed.emplaced_update = true;
	gun_seed.emplacement_parent = h.tank;
	gun_seed.emplacement_parent_spawn_id = h.hull().registry_spawn_id;
	gun_seed.emplacement_pose_metadata_resolved = true;
	gun_seed.emplacement_bone = 0; // no anchor: the root copy
	const w::EntityHandle gun = test_common::materialize_twin(h.role.materializer(),
			h.role.runtime->state(), world, 1, gun_seed, EntityClass::NoNetworkCallback);
	ok &= expect(gun.valid(), "ewep: the attached gun spawned");
	replication::ClientEntityState &row = h.role.runtime->state().upsert(gun.packed);
	row.type_id = kGunType;
	row.cls = EntityClass::NoNetworkCallback;
	// The 0x0D seed sits 2.0 off the carrier with a heading of its own.
	row.x = h.hull().position.x.q16 + 2 * 65536;
	row.y = h.hull().position.y.q16;
	row.z = h.hull().position.z.q16;
	row.heading_bam = w::bam_heading_from_mission_yaw_deg(30);
	row.target_handle = h.tank.packed;
	row.spawn_revision = 1;
	const int32_t seed_x = row.x;
	const int32_t seed_y = row.y;
	const int32_t seed_z = row.z;
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	const w::Entity *live = world.registry.get(gun);
	const w::Entity &carrier = h.hull();
	const replication::ClientEntityState *after = h.role.runtime->state().find(gun.packed);
	ok &= expect(after != nullptr &&
					after->world_class_update ==
							replication::ClientEntityState::WorldClassUpdate::Ewep,
			"ewep: the row is the world's class update's");
	ok &= expect(!carrier.veh.net_predicted &&
					(carrier.engine_flags & w::kEntityFlagMatrixBuilt) != 0,
			"ewep: the tank's unpredicted client mover ran to its matrix-build tail");
	ok &= expect(live != nullptr && (live->engine_flags & w::kEntityFlagMatrixBuilt) != 0,
			"ewep: the class update ran and set the child's own bit");
	ok &= expect(live != nullptr && live->position.x.q16 == seed_x &&
					live->position.y.q16 == seed_y && live->position.z.q16 == seed_z &&
					live->position.x.q16 != carrier.position.x.q16,
			"ewep: with the carrier's matrix current the anchorless child keeps the "
			"seeded 0x0D pose, no root copy");
	ok &= expect(after != nullptr && live != nullptr && after->x == live->position.x.q16 &&
					after->y == live->position.y.q16 && after->z == live->position.z.q16 &&
					after->heading_bam == live->veh.yaw_bam,
			"ewep: the decoded row reads the world's composed pose back");
	return ok;
}

// D-NET-690: a genx row on a joiner runs its own update, the ground ride and
// the fall a dead ground installs; no wire record writes it after the join and
// its update stores no Flags, so the dead carrier neither hides it nor holds it
// at its last ridden pose.
// [orig: the genx row @0x82AC10 -> Entity_UpdateParentTransform @0x4A88B0, the
//  re-fall @0x4A8CBF..0x4A8CDC; Entity_UpdatePositionAndTransform's store
//  @0x4AE01A..0x4AE01F]
bool run_genx_row_rides_and_falls_on_its_own() {
	Harness h;
	bool ok = true;
	w::World &world = h.kernel->world;
	static constexpr uint16_t kCrateType = 1872;
	h.role.runtime->view().set_item_class_resolver(
			[](uint16_t type) -> std::optional<EntityClass> {
				if (type == kTankType) return EntityClass::Vehicle;
				if (type == kCrateType) return EntityClass::NoNetworkCallback;
				if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
				return std::nullopt;
			});
	w::Entity crate_seed;
	crate_seed.kind = w::EntityKind::Item;
	crate_seed.item_id = kCrateType;
	crate_seed.item_type_index = 1; // the def row's ordinal: the walks' +0x1C gate
	crate_seed.spawn_origin = (1u << 24) | 5u;
	crate_seed.position = h.hull().position;
	crate_seed.parent_follow_update = true;
	crate_seed.drop_motion = w::DropMotion::Ride;
	crate_seed.ground_target = h.tank;
	const w::EntityHandle crate = test_common::materialize_twin(h.role.materializer(),
			h.role.runtime->state(), world, 1, crate_seed, EntityClass::NoNetworkCallback);
	ok &= expect(crate.valid(), "genx: the crate spawned");
	replication::ClientEntityState &row = h.role.runtime->state().upsert(crate.packed);
	row.type_id = kCrateType;
	row.cls = EntityClass::NoNetworkCallback;
	row.x = h.hull().position.x.q16;
	row.y = h.hull().position.y.q16;
	row.z = h.hull().position.z.q16 + 3 * 65536;
	row.target_handle = h.tank.packed;
	row.spawn_revision = 1;
	h.role.run_tick(h.input);
	// The carrier dies on both sides.
	replication::ClientEntityState *carrier_row = h.role.runtime->state().find(h.tank.packed);
	if (carrier_row != nullptr) {
		carrier_row->health_known = true;
		carrier_row->health_word = 0;
	}
	h.hull().alive = false;
	h.hull().flags |= w::kEntityFlagDead;
	h.hull().health = 0;
	h.role.run_tick(h.input);
	const w::Entity *live = world.registry.get(crate);
	const replication::ClientEntityState *after = h.role.runtime->state().find(crate.packed);
	ok &= expect(live != nullptr && live->drop_motion == w::DropMotion::Fall,
			"genx: the dead ground installed the fall");
	ok &= expect(after != nullptr && (after->state_flags & w::kEntityFlagCarried) == 0,
			"genx: the dead carrier does not hide the row (the hide is the ewep update's)");
	// The fall moves the world row; the decoded row follows it and the mirror
	// no longer writes the row's last pose back.
	if (w::Entity *moving = world.registry.get(crate)) moving->position.z.q16 -= 65536;
	live = world.registry.get(crate);
	const int32_t fallen_z = live != nullptr ? live->position.z.q16 : 0;
	h.role.run_tick(h.input);
	live = world.registry.get(crate);
	after = h.role.runtime->state().find(crate.packed);
	ok &= expect(live != nullptr && live->position.z.q16 == fallen_z && after != nullptr &&
					after->z == fallen_z,
			"genx: the world row keeps its fall and the decoded row reads it");
	return ok;
}

} // namespace

// A streamed pool-2 building takes its def's "armory*" points on the joiner,
// as the host's promote installs them on every spawned record: retail's label
// pass walks the proximity list over every pool, so an armory building labels
// "UseArmory" on a joiner too. Before, only pool-1 rows were refreshed and a
// joiner's armory tent never labelled.
// [orig: HUD_DrawVehicleSeatAndArmoryLabels @0x5a32c1..0x5a32e2; the armory
//  walk @0x5a36f5; Entity_InitAllFromModels @0x52567F]
bool run_streamed_building_takes_its_armory_points() {
	constexpr int32_t kArmoryType = 1125; // "Armory Version #1"
	constexpr int kSlot = 3;
	Harness h;
	h.kernel->world.registry.configure_pool(2, 16);
	mission::ItemSeatSpec spec;
	spec.type_id = kArmoryType;
	spec.armory_points = {w::Vec3{0.5f, 1.0f, 0.25f}, w::Vec3{-0.5f, 1.0f, 0.25f}};
	h.kernel->seat_specs.push_back(spec);
	StaticEntityBatch batch;
	batch.start_index = kSlot;
	StaticEntityRecord rec;
	rec.item_type_id = static_cast<uint16_t>(kArmoryType);
	rec.pos_x = 40 * 65536;
	rec.pos_y = 40 * 65536;
	rec.pos_z = 2 * 65536;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	h.role.runtime->view().apply(s2c::STATIC_ENTITY_BATCH, encode_static_entity_batch(batch));
	h.role.run_tick(h.input);
	const w::Entity *tent = h.kernel->world.registry.get(
			w::EntityHandle{static_cast<uint16_t>(0x2000u | kSlot)});
	if (!expect(tent != nullptr && tent->item_id == kArmoryType,
			"armory: the streamed pool-2 row materializes"))
		return false;
	return expect(tent->armory_points.size() == 2,
			"armory: the streamed building takes its def's armory points");
}

// A streamed topology change re-runs the joiner's items.def sweep, but the
// Health and Armor stores are an item init's, once per body: a body arriving
// takes them, an existing one keeps its words (a wire health word of exactly
// 100, an armor word a pickup moved), the joiner's own body included. Retail
// stores them at a body's creation (an item init, the player spawn), at the
// mission start's reload and at a vehicle's respawn (D-PWR-14); no topology
// change re-runs one.
// [orig: Entity_InitFromItemDef @0x49E550 - Health @0x49E5C0, Armor
//  @0x49E5CE; Game_ReloadEntityModelsAndCallbacks @0x522830 - @0x522B65 /
//  @0x522B78, called @0x52550E]
bool run_topology_resweep_keeps_initialized_words() {
	constexpr int32_t kShedType = 1700;
	static constexpr char kItems[] =
			"begin \"Resweep Player\"\r\n id 105305\r\n type person\r\n hp 150\r\n mana 5\r\n"
			" move_function org2\r\nend\r\n"
			"begin \"Resweep Shed\"\r\n id 101700\r\n type building\r\n hp 300\r\n mana 7\r\nend\r\n";
	def::DefItemsFile items = {};
	if (!expect(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
			sizeof(kItems) - 1, &items) == 0 && items.count == 2,
			"resweep: the items table parses"))
		return false;
	Harness h;
	h.kernel->world.registry.configure_pool(2, 16);
	h.kernel->set_items_table(&items);
	h.kernel->resolve_item_traits([](int) -> uint8_t { return 0; });
	const auto stream_shed = [&](int slot) {
		StaticEntityBatch batch;
		batch.start_index = static_cast<uint16_t>(slot);
		StaticEntityRecord rec;
		rec.item_type_id = static_cast<uint16_t>(kShedType);
		rec.pos_x = (40 + slot) * 65536;
		rec.pos_y = 40 * 65536;
		rec.pos_z = 2 * 65536;
		batch.records.push_back(rec);
		batch.entity_count = 1;
		h.role.runtime->view().apply(s2c::STATIC_ENTITY_BATCH, encode_static_entity_batch(batch));
		h.role.run_tick(h.input);
		return h.kernel->world.registry.get(
				w::EntityHandle{static_cast<uint16_t>(0x2000u | slot)});
	};
	bool ok = true;
	w::Entity *first = stream_shed(3);
	w::Entity *local = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
	if (!expect(first != nullptr && local != nullptr, "resweep: the first shed materializes"))
		return false;
	ok &= expect(first->health == 300 && first->mana == 7,
			"resweep: an arriving body takes its def's Health and Armor");
	ok &= expect(local->mana == 5, "resweep: the joiner's own body took its def's Armor");
	first->health = 100;
	first->mana = 3;
	local->mana = 2;
	w::Entity *second = stream_shed(4);
	first = h.kernel->world.registry.get(w::EntityHandle{0x2003});
	local = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
	ok &= expect(second != nullptr && second->health == 300 && second->mana == 7,
			"resweep: the second arrival takes its def's Health and Armor");
	ok &= expect(first != nullptr && first->health == 100,
			"resweep: an existing body at exactly 100 keeps its Health");
	ok &= expect(first != nullptr && first->mana == 3 && first->mana_max == 7,
			"resweep: an existing body keeps its Armor word");
	ok &= expect(local != nullptr && local->mana == 2,
			"resweep: the joiner's own body keeps its Armor word");
	def::def_free_items(&items);
	return ok;
}

// A streamed 0x0D row of an AIData brain-class def takes its brain on the joiner, from the
// profile its AI trailer names, as the mission start's pool-1 class init gives it there; a
// row of any other def takes none, and the row's retirement frees the brain. The rows and
// the profiles are the test's own (the profiles told apart by their aim_skill).
// [orig: NapiNPClientMsg_0x00D @0x432C40 (the AI slot @0x43332A, its +156 name
//  @0x433363..0x43337C); Game_StartMission - the Entity_InitAllFromModels call @0x52567F
//  (the pool-1 class init @0x40E5D3); Entity_InitVehicleAIFromDef @0x4686C0 (the +156
//  name @0x46879E..0x4687A3); Entity_Destroy @0x43E810 (@0x43E98E..0x43E99D)]
bool run_streamed_ai_row_takes_its_brain() {
	constexpr int32_t kBrainType = 1500;
	constexpr int32_t kPlainType = 1501;
	std::array<def::DefItemDef, 3> rows{};
	rows[0].id = mission::kItemIdOffset + 1;
	rows[0].type = 1;
	rows[1].id = mission::kItemIdOffset + kBrainType;
	rows[1].type = 1;
	rows[1].attrib = w::kItemAttribAIData;
	std::strcpy(rows[1].ai_function, "cveh");
	rows[2].id = mission::kItemIdOffset + kPlainType;
	rows[2].type = 1;
	std::strcpy(rows[2].ai_function, "cveh");
	def::DefItemsFile items{rows.data(), rows.size()};
	Harness h;
	h.kernel->set_items_table(&items);
	h.kernel->resolve_item_traits([](int) -> uint8_t { return 0; });
	aip::Profile wire_profile;
	wire_profile.aim_skill = 1;
	aip::Profile fallback;
	fallback.aim_skill = 3;
	h.kernel->ai_profiles = {{"wire_profile", wire_profile}, {"helo1", fallback}};
	const auto stream = [&](uint16_t handle, int32_t type, bool ai_trailer) {
		PoolSpawnBatch batch;
		PoolSpawnRecord rec;
		rec.slot_id = handle;
		rec.item_type_id = static_cast<uint16_t>(type);
		rec.pos_x = 30 * 65536;
		rec.pos_y = 30 * 65536;
		rec.pos_z = 2 * 65536;
		rec.has_ai_trailer = ai_trailer;
		if (ai_trailer) rec.ai_name = "Wire_Profile";
		batch.records.push_back(rec);
		batch.entity_count = 1;
		h.role.runtime->view().apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(batch));
		h.role.run_tick(h.input);
	};
	const w::EntityHandle brained{0x1005};
	const w::EntityHandle plain{0x1006};
	stream(brained.packed, kBrainType, true);
	stream(plain.packed, kPlainType, false);
	bool ok = true;
	const replication::ClientEntityState *wire = h.role.runtime->state().find(brained.packed);
	ok &= expect(wire != nullptr && wire->spawn_ai_name == "Wire_Profile",
			"ai row: the decoded row keeps the AI trailer's name");
	const w::Entity *row = h.kernel->world.registry.get(brained);
	const w::AiEntity *ai = h.kernel->world.ai.for_handle(brained);
	ok &= expect(row != nullptr && ai != nullptr && !ai->inf.active,
			"ai row: the AIData brain-class row takes a brain");
	ok &= expect(ai != nullptr && ai->profile.accuracy == 1,
			"ai row: the brain's profile is the one the trailer names");
	// The allocator seeds the staged target from the row's 0x0D pose, progress 0
	// [orig: Entity_InitVehicleAI @0x460200 - @0x4602F9..0x460314, @0x460326].
	ok &= expect(row != nullptr && row->veh.net_smooth_target[0] == 30 * 65536 &&
					row->veh.net_smooth_target[1] == 30 * 65536 &&
					row->veh.net_smooth_target[2] == 2 * 65536 &&
					row->veh.net_interp_progress == 0,
			"ai row: the class init stages the row's own pose");
	ok &= expect(h.kernel->world.registry.get(plain) != nullptr &&
					h.kernel->world.ai.for_handle(plain) == nullptr,
			"ai row: a def without the AIData attrib takes none");
	// A new lifetime at the slot (another type) retires the row and frees its brain.
	stream(brained.packed, kPlainType, false);
	ok &= expect(h.kernel->world.registry.get(brained) != nullptr &&
					h.kernel->world.ai.for_handle(brained) == nullptr,
			"ai row: the retired row's brain is freed with it");
	return ok;
}

// A streamed brained hull's client mover runs from its class init, ahead of its first
// compact: the allocator staged the row's own pose as the chase target and its Yaw as the
// received steer, so the chase takes its hold arm and the mirrored command holds the hull's
// heading. The first compact then stages a target without re-seeding the attitude.
// [orig: Entity_InitVehicleAI @0x460200 - @0x4602F9..0x460326, the received registers
//  @0x46032D / @0x460333 / @0x46034F..0x460352; Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53;
//  Entity_UpdateVehiclePhysics @0x48AF00 - the client chase @0x48B53F..0x48B7C5 (the
//  < 0x2000 arm @0x48B64B, the progress count @0x48B7BC..0x48B7C5), the received-register
//  mirror @0x48B7F8..0x48B80A]
bool run_streamed_hull_runs_its_client_mover_from_the_staged_pose() {
	constexpr int32_t kHullType = 1502;
	constexpr int32_t kYaw = 0x40000000;
	std::array<def::DefItemDef, 2> rows{};
	rows[0].id = mission::kItemIdOffset + 1;
	rows[0].type = 1;
	rows[1].id = mission::kItemIdOffset + kHullType;
	rows[1].type = 1;
	rows[1].hp = 500;
	rows[1].physics = 1;
	rows[1].attrib = w::kItemAttribAIData;
	std::strcpy(rows[1].ai_function, "cveh");
	std::strcpy(rows[1].move_function, "cveh");
	def::DefItemsFile items{rows.data(), rows.size()};
	Harness h;
	h.kernel->set_items_table(&items);
	h.kernel->resolve_item_traits([](int) -> uint8_t { return 0; });
	aip::Profile profile;
	h.kernel->ai_profiles = {{"wire_profile", profile}, {"helo1", profile}};
	const w::EntityHandle hull_h{0x1007};
	PoolSpawnBatch batch;
	PoolSpawnRecord rec;
	rec.slot_id = hull_h.packed;
	rec.item_type_id = static_cast<uint16_t>(kHullType);
	rec.pos_x = 40 * 65536;
	rec.pos_y = -25 * 65536;
	rec.pos_z = 3 * 65536;
	rec.euler_z = kYaw;
	rec.has_ai_trailer = true;
	rec.ai_name = "Wire_Profile";
	batch.records.push_back(rec);
	batch.entity_count = 1;
	h.role.runtime->view().apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(batch));
	h.role.run_tick(h.input);
	bool ok = true;
	const w::Entity *hull = h.kernel->world.registry.get(hull_h);
	ok &= expect(hull != nullptr && h.kernel->world.ai.for_handle(hull_h) != nullptr &&
					h.kernel->world.vehicles.traits.get(kHullType) != nullptr && hull->health > 0,
			"staged pose: the streamed AIData hull takes its brain and its mover traits");
	if (hull == nullptr) return ok;
	ok &= expect(hull->veh.net_predicted,
			"staged pose: the class init arms the client mover, no compact needed");
	ok &= expect(hull->veh.net_recv_speed == 0 && hull->veh.net_recv_lat == 0 &&
					hull->veh.net_recv_steer_bam == kYaw,
			"staged pose: the allocator seeds the received registers, the steer its Yaw");
	ok &= expect(hull->veh.net_interp_progress == 1 && hull->veh.net_interp_steps == 0,
			"staged pose: the stream's tick ran the chase's hold arm once");
	ok &= expect(hull->veh.cmd_speed == 0 && hull->veh.steer_target_bam == kYaw,
			"staged pose: the mirror stages the seeded registers as the command");
	for (int frame = 0; frame < 3; ++frame) h.role.run_tick(h.input);
	hull = h.kernel->world.registry.get(hull_h);
	if (hull == nullptr) return expect(false, "staged pose: the hull row stays");
	ok &= expect(hull->veh.net_interp_progress == 4,
			"staged pose: every tick ahead of the first compact runs the chase");
	ok &= expect(hull->position.x.q16 == 40 * 65536 && hull->position.y.q16 == -25 * 65536 &&
					hull->veh.yaw_bam == kYaw,
			"staged pose: the hull holds its staged ground position and heading");
	return ok;
}

int main() {
	bool ok = true;
	ok &= run_record_tail_mirrors_flags_and_health();
	ok &= run_hull_holds_its_spawn_pose_until_its_first_compact();
	ok &= run_destroyed_bit_kills_the_client_row();
	ok &= run_live_record_respawns_the_dead_client_row();
	ok &= run_respawn_drops_the_rotor_words_by_the_def_class_init();
	ok &= run_tilted_wreck_respawns_level();
	ok &= run_freeze_predicate_and_pivot_clear();
	ok &= run_remote_driver_is_the_claimant();
	ok &= run_unresolved_attachment_follows_its_target_carrier();
	ok &= run_resolved_attachment_runs_the_ewep_update();
	ok &= run_genx_row_rides_and_falls_on_its_own();
	ok &= run_streamed_building_takes_its_armory_points();
	ok &= run_topology_resweep_keeps_initialized_words();
	ok &= run_streamed_ai_row_takes_its_brain();
	ok &= run_streamed_hull_runs_its_client_mover_from_the_staged_pose();
	if (!ok || failures != 0) {
		std::fprintf(stderr, "joiner_vehicle_replica_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_vehicle_replica_test: OK\n");
	return 0;
}
