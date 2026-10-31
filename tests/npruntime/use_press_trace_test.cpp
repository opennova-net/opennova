// The USE readout (world/use_press_trace.h): a debug instrument for the drive scenario's seat
// question (the use_press_trace probe, Simulation::debug_use_press_trace). Each producer
// records what it met, and the instrument changes nothing:
// - the press: each run of case 0xB1 (LocalPlayer::take_use_item_press). The arm it took,
//   the local body's Flags masked to 0x800 / 0x400000 / 0x200 in our two words, its mount,
//   and the active slot's current / next action.
// - the release's toggle gate (LocalPlayer::mount_toggle_allowed), its answer and the slot
//   it read.
// - a joiner's seat scan and request (JoinerRole::queue_mount_toggle). The candidate and
//   its pick, the outcome, and the runtime's framed-0x26 count at queue time. The count
//   (ClientRuntime::attach_requests_sent) rises once the next send build frames the 0x26.
// The arms and gates themselves are pinned elsewhere (local_player_view, use_item_dead_drop,
// inmatch_joiner_role).

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <base/io/bam.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/use_press_trace.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

// A bare world with the local player's body (use_item_dead_drop's rig).
struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	bool wp_jo = test_titles::jo_world(*wp);
	w::World &world = *wp;
	std::unique_ptr<w::LocalPlayer> local = std::make_unique<w::LocalPlayer>(world);
	w::EntityHandle player;
	Rig() {
		world.registry.configure_pool(0, 4);
		world.local_player_state = local.get();
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF0;
		player = w::spawn_player(world, spawn);
	}
	w::Entity &body() { return *world.registry.get(player); }
	w::UsePressRecord last() const {
		const std::vector<w::UsePressRecord> held = local->use_trace.since(0);
		return held.empty() ? w::UsePressRecord{} : held.back();
	}
};

void test_a_press_records_its_arm_and_the_bodys_words() {
	Rig r;
	r.world.logic_tick = 77;
	r.local->weapon.slot.current = w::weapon_action::kRecoil;
	r.local->weapon.slot.next = w::weapon_action::kIdle;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
	w::UsePressRecord rec = r.last();
	CHECK(rec.seq == 0 && r.local->use_trace.next_seq() == 1);
	CHECK(rec.event == w::UsePressEvent::kPress && rec.arm == w::UseItemArm::kMountChain);
	CHECK(rec.logic_tick == 77);
	CHECK(rec.has_player && !rec.mounted && rec.flags == 0 && rec.engine_flags == 0);
	CHECK(rec.has_slot && rec.slot_current == w::weapon_action::kRecoil &&
			rec.slot_next == w::weapon_action::kIdle);
	CHECK(rec.outcome == w::UseToggleOutcome::kNone);

	// The type-6 volume touch takes the armory arm; the mask keeps it and drops the rest of
	// the word (the binoculars bit here). A second press is refused by the latch.
	r.body().flags |= w::kEntityFlagArmoryZone | w::kEntityFlagBinoculars;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kArmory);
	rec = r.last();
	CHECK(rec.seq == 1 && rec.arm == w::UseItemArm::kArmory);
	CHECK(rec.flags == w::kEntityFlagArmoryZone && rec.engine_flags == 0);
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kArmoryRefused);
	CHECK(r.last().seq == 2 && r.last().arm == w::UseItemArm::kArmoryRefused);
	r.local->release_use_item_key(false);

	// The type-11 touch on the engine word takes the vehicle-loadout arm; the queued-mount
	// bit rides along in the flags word.
	r.body().flags &= ~(w::kEntityFlagArmoryZone | w::kEntityFlagBinoculars);
	r.body().flags |= w::kEntityFlagQueuedMount;
	r.body().engine_flags |= w::kEntityFlagVehicleLoadoutZone;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kVehicleLoadout);
	rec = r.last();
	CHECK(rec.arm == w::UseItemArm::kVehicleLoadout);
	CHECK(rec.flags == w::kEntityFlagQueuedMount &&
			rec.engine_flags == w::kEntityFlagVehicleLoadoutZone);

	// A dead player's press is dropped, and the record says so.
	r.body().flags |= w::kEntityFlagDead;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kDropped);
	CHECK(r.last().arm == w::UseItemArm::kDropped && r.last().seq == 4);
}

void test_the_toggle_gate_records_its_answer() {
	Rig r;
	r.local->weapon.slot.current = w::weapon_action::kReload;
	r.local->weapon.slot.next = w::weapon_action::kIdle;
	CHECK(!r.local->mount_toggle_allowed());
	w::UsePressRecord rec = r.last();
	CHECK(rec.event == w::UsePressEvent::kToggleGate && !rec.allowed);
	CHECK(rec.has_slot && rec.slot_current == w::weapon_action::kReload);
	r.local->weapon.slot.current = w::weapon_action::kIdle;
	CHECK(r.local->mount_toggle_allowed());
	rec = r.last();
	CHECK(rec.event == w::UsePressEvent::kToggleGate && rec.allowed && rec.seq == 1);
}

// The ring keeps the last kCapacity records; since() reads from the oldest it holds.
void test_the_ring_keeps_the_last_records() {
	w::UsePressTrace trace;
	for (uint32_t i = 0; i < w::UsePressTrace::kCapacity + 3; ++i) {
		w::UsePressRecord rec;
		rec.logic_tick = i;
		trace.append(rec);
	}
	CHECK(trace.next_seq() == w::UsePressTrace::kCapacity + 3);
	CHECK(trace.oldest_seq() == 3);
	const std::vector<w::UsePressRecord> all = trace.since(0);
	CHECK(all.size() == w::UsePressTrace::kCapacity);
	CHECK(!all.empty() && all.front().seq == 3 && all.front().logic_tick == 3);
	CHECK(!all.empty() && all.back().seq == w::UsePressTrace::kCapacity + 2);
	CHECK(trace.since(trace.next_seq()).empty());
	CHECK(trace.since(trace.next_seq() - 1).size() == 1);
}

// The watched entity's scan inputs ride every record: whether it exists, its Flags words,
// its pool-1 table row and radius, and the local body's slice membership. Bit 0 of its
// Flags keeps it out of the pool-1 table, and so out of the slice.
// [orig: Entity_BuildProximityLists_Pool01 @0x4B9340 - the Flags & 1 skip @0x4B9382, the
//  radius copy @0x4B9389; Entity_BuildProximityListsFromPools @0x4B8EB0 - the arena store
//  @0x4B8FD6]
void test_the_watch_carries_the_scan_inputs() {
	Rig r;
	r.world.registry.configure_pool(1, 4);
	r.body().item_type_index = 1; // a pool-0 slice source (its +0x1C word)
	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.position = r.body().position;
	gun.position.x += 1.0f;
	gun.bound_radius = 1.5f;
	w::Seat seat;
	seat.type = w::SeatType::Gunner;
	seat.bone_index = 6;
	gun.seats.push_back(seat);
	const w::EntityHandle gun_h = r.world.registry.spawn(1, gun);
	r.local->use_trace.watch = gun_h.packed;

	// No collision world: the entity's words alone.
	(void)r.local->take_use_item_press();
	w::UsePressRecord rec = r.last();
	CHECK(rec.watch == gun_h.packed && rec.watch_present);
	CHECK(rec.watch_flags == r.world.registry.get(gun_h)->flags);
	CHECK(!rec.watch_in_table && !rec.watch_in_slice && rec.slice_count == -1);

	std::unique_ptr<w::CollisionWorld> cw = std::make_unique<w::CollisionWorld>();
	r.world.collision = cw.get();
	r.world.ai.collision = cw.get();
	cw->build_initial_tables(r.world);
	(void)r.local->take_use_item_press();
	rec = r.last();
	CHECK(rec.watch_in_table && rec.watch_table_radius_q16 > 0);
	CHECK(rec.watch_in_slice && rec.slice_count >= 1);

	// Flags bit 0: out of the pool-1 table, out of the slice.
	r.world.registry.get(gun_h)->flags |= w::kEntityFlagCarried;
	cw->build_initial_tables(r.world);
	(void)r.local->mount_toggle_allowed();
	rec = r.last();
	CHECK(rec.event == w::UsePressEvent::kToggleGate);
	CHECK((rec.watch_flags & w::kEntityFlagCarried) != 0);
	CHECK(!rec.watch_in_table && !rec.watch_in_slice);

	// An unwatched trace carries none of it.
	r.local->use_trace.watch = 0xFFFFu;
	(void)r.local->take_use_item_press();
	rec = r.last();
	CHECK(rec.watch == 0xFFFFu && !rec.watch_present && rec.slice_count == -1);
	r.world.collision = nullptr;
	r.world.ai.collision = nullptr;
}

// --- the joiner's scan (joiner_role_test's harness) ---

const opennova::Protocol &kJo = *opennova::title_protocol("jo");
const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

class CountingSocket final : public opennova::IDatagramSocket {
public:
	std::vector<std::vector<uint8_t>> datagrams;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t size) override {
		datagrams.emplace_back(data, data + size);
	}
	int count(uint8_t tag) const {
		int n = 0;
		for (const auto &dg : datagrams) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!nw_decode_inbound(dg.data(), dg.size(), opcode, body) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
							header, messages)) continue;
			for (const auto &msg : messages)
				if (msg.tag == tag) ++n;
		}
		return n;
	}
};

struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	bool kernel_jo = test_titles::jo_world(kernel->world);
	inmatch::JoinerRole role;
	bool role_jo = test_titles::jo_joiner(role);
	CountingSocket socket;
	inmatch::TickInput input;
	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.set_rules(opennova::world::title_rules("jo"));
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
	}
};

// The seat scan aims from the eye at the seat point (joiner_role_test's look_at).
void look_at(w::World &world, w::EntityHandle player_h, float tx, float ty, float tz) {
	w::Entity *player = world.registry.get(player_h);
	if (player == nullptr) return;
	const double dx = double(tx) - player->position.x - player->eye_offset_x / 65536.0;
	const double dy = double(ty) - player->position.y - player->eye_offset_y / 65536.0;
	const double dz = double(tz) + 0.1875 - player->position.z - player->eye_offset_z / 65536.0;
	const double heading = std::atan2(dy, dx);
	const double pitch = std::atan2(dz, std::hypot(dx, dy));
	player->heading = w::placed_heading_bam(
			int16_t(std::lround(90.0 - heading * 180.0 / 3.14159265358979323846)));
	player->pitch_bam = int32_t(pitch * opennova::io::kBamPerRadian);
	if (w::AiEntity *body = world.ai.for_handle(player_h)) {
		body->heading = int32_t(heading * opennova::io::kBamPerRadian);
		body->pitch = int32_t(pitch * opennova::io::kBamPerRadian);
	}
}

void test_a_joiner_scan_records_its_pick_and_the_0x26_leaving() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
			self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	CHECK(local != nullptr);
	if (local == nullptr) return;
	// A passenger seat a step away, faced (the D-NET-421 rig).
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.position = local->position;
	vehicle.position.x += 1.0f;
	vehicle.health = vehicle.health_max = 100;
	vehicle.team = local->team;
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	seat.bone_index = 2;
	seat.retail_slot = 0;
	seat.source_name = "sitex00";
	vehicle.seats.push_back(seat);
	const w::EntityHandle vh = world.registry.spawn(1, vehicle);
	look_at(world, local->handle, vehicle.position.x, vehicle.position.y, vehicle.position.z);
	// The view the scan reads, taken before the press: the request that follows a pick
	// turns L to the seat (its yaw snap), so the body's heading after the press is the
	// seat's, not the view the scan subtracted.
	// [orig: Entity_TryEnterNearestVehicle @0x4368C0 - the scan @0x43691A ahead of the
	//  request @0x436931; Entity_RequestVehicleAttach @0x4364A0 - the yaw writes
	//  @0x43656E / @0x4365C3]
	const w::AiEntity *view_body = world.ai.for_handle(local->handle);
	CHECK(view_body != nullptr);
	if (view_body == nullptr) return;
	const int32_t view_heading = view_body->heading;
	const int32_t view_pitch = view_body->pitch;
	h.socket.datagrams.clear();
	// The scan's watch rides the scan record (UseScanWatch).
	h.kernel->local.use_trace.watch = vh.packed;

	// The release's path on a joiner: the gate, then the scan and the request
	// (Simulation::local_player_toggle_mount).
	const uint32_t from = h.kernel->local.use_trace.next_seq();
	CHECK(h.kernel->local.mount_toggle_allowed());
	CHECK(h.role.queue_mount_toggle());
	std::vector<w::UsePressRecord> held = h.kernel->local.use_trace.since(from);
	CHECK(held.size() == 2);
	if (held.size() != 2) return;
	CHECK(held[0].event == w::UsePressEvent::kToggleGate && held[0].allowed);
	const w::UsePressRecord &scan = held[1];
	CHECK(scan.event == w::UsePressEvent::kToggleScan);
	CHECK(scan.logic_tick == world.logic_tick);
	CHECK(scan.has_player && !scan.mounted);
	CHECK(scan.candidate && scan.vehicle == vh.packed && scan.seat_index == 0 && scan.bone == 2);
	CHECK(scan.outcome == w::UseToggleOutcome::kAttachQueued);
	CHECK(scan.attach_sent_before == 0);
	// The watched carrier was picked: its one seat, the reach inside 4.0 u, and the eye
	// Position + CameraOffset and the body's view words the scan read.
	// [orig: Entity_FindNearestSeatOrArmory @0x435D50 - the eye @0x436010..0x436041, the
	//  reach gate @0x436113, the Yaw / Pitch subtractions @0x4360B6 / @0x4360C6]
	CHECK(scan.scan.entity == vh.packed && scan.scan.gate == w::UseScanGate::kPicked);
	CHECK(scan.scan.seats == 1 && scan.scan.seat_index == 0 && scan.scan.bone == 2);
	CHECK(scan.scan.reach_q16 > 0 && scan.scan.reach_q16 <= 0x40000);
	CHECK(scan.scan.eye_q16[0] == local->position.x.q16 + local->eye_offset_x);
	CHECK(scan.scan.eye_q16[2] == local->position.z.q16 + local->eye_offset_z);
	CHECK(scan.scan.view_heading == view_heading && scan.scan.view_pitch == view_pitch);
	// The watched entity's seat source, read ahead of the scan: this rig's carrier carries
	// its seats by hand, its type has no row in the empty installed table, and the
	// materializer does not own a row spawned straight into the registry.
	CHECK(scan.scan.type_id == 0 && scan.scan.spec_rows == 0 && scan.scan.spec_seats == -1);
	CHECK(scan.scan.owned == 0);
	// Queued, not yet framed: the count rises at the next send build, with the 0x26 on the
	// wire.
	CHECK(h.role.runtime->attach_requests_sent() == 0);
	h.role.run_tick(h.input);
	CHECK(h.socket.count(c2s::VEHICLE_ATTACH_REQUEST) == 1);
	CHECK(h.role.runtime->attach_requests_sent() == 1);

	// No carrier in reach on foot: no candidate, nothing queued. The carrier now carries a
	// type word whose row the installed table holds, with its one seat.
	world.registry.get(vh)->position.x += 500.0f;
	world.registry.get(vh)->item_id = 0x058B;
	mission::ItemSeatSpec spec;
	spec.type_id = 0x058B;
	spec.seats.push_back(seat);
	h.kernel->seat_specs.push_back(spec);
	CHECK(!h.role.queue_mount_toggle());
	const w::UsePressRecord none = h.kernel->local.use_trace.since(0).back();
	CHECK(none.event == w::UsePressEvent::kToggleScan && !none.candidate);
	CHECK(none.outcome == w::UseToggleOutcome::kNoCandidate && none.vehicle == 0xFFFFu);
	// The watch names the gate that stopped it: its seat lies past the 4.0 u reach.
	CHECK(none.scan.gate == w::UseScanGate::kReach && none.scan.seat_index == 0);
	CHECK(none.scan.reach_q16 > 0x40000);
	CHECK(none.scan.point_q16[0] > local->position.x.q16 + 499 * 65536);
	CHECK(none.scan.type_id == 0x058B && none.scan.spec_rows == 1 && none.scan.spec_seats == 1);
	CHECK(none.scan.owned == 0);
	h.role.run_tick(h.input);
	CHECK(h.role.runtime->attach_requests_sent() == 1);
}

} // namespace

int main() {
	test_a_press_records_its_arm_and_the_bodys_words();
	test_the_toggle_gate_records_its_answer();
	test_the_ring_keeps_the_last_records();
	test_the_watch_carries_the_scan_inputs();
	test_a_joiner_scan_records_its_pick_and_the_0x26_leaving();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("use_press_trace: ok\n");
	return 0;
}
