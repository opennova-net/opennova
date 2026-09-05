// The joiner role's frame (ADR 0043 d3, slice E8b; ex the S10a joiner world
// bridge). The frame's phase SEQUENCE is the witnessed retail client frame
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame
// @0x42c180] — this pins the portable contract through its effects: the
// ClientHello one-shot over the socket seam, the per-frame clock, the in-match
// spawn edge for L (H latched, the join-wait latches cleared, the weapon pump
// gated open the same frame), the mid-frame loadout-grant stamp, and the
// per-session latch reset. Confirmed vehicle occupancy, prediction, mount
// requests and the matching seat overlays run through the production frame.

#include <net/npwire/ingame_decode.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/angle.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

// The socket seam: counts what the role ships, receives nothing.
class CountingSocket final : public opennova::IDatagramSocket {
public:
	int sends = 0;
	std::vector<std::vector<uint8_t>> datagrams;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t size) override {
		++sends;
		datagrams.emplace_back(data, data + size);
	}
	bool last_message(uint8_t tag, ProtocolMessage &out) const {
		for (auto it = datagrams.rbegin(); it != datagrams.rend(); ++it) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!nw_decode_inbound(it->data(), it->size(), opcode, body) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(body.data(), body.size(),
							kClientScrk, header, messages)) continue;
			for (auto msg = messages.rbegin(); msg != messages.rend(); ++msg) {
				if (msg->tag == tag) { out = *msg; return true; }
			}
		}
		return false;
	}
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	CountingSocket socket;
	inmatch::TickInput input;
	std::vector<std::string> seams;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "");
		role.kit_seams.apply_authoritative = [this] { seams.push_back("loadout"); };
		role.kit_seams.reseed_on_side_change = [this] {
			seams.push_back("reseed");
			return false;
		};
		role.kit_seams.push = [this] { seams.push_back("push_kit"); };
		role.kit_seams.respawn = [this] { seams.push_back("respawn"); };
	}
	w::LocalPlayerWeapon &weapon() { return kernel->local.weapon; }
	w::WeaponInventory &inventory() { return kernel->local.inventory; }
};

// A pre-match frame ships the one-shot ClientHello, runs the loadout seams in
// the witnessed order, advances the clock, and never spawns L.
bool run_pre_match_frame() {
	Harness h;
	h.role.run_tick(h.input);
	const std::vector<std::string> expected = {"loadout", "reseed"};
	if (!expect(h.seams == expected, "pre-match frame: the loadout seams in order")) {
		for (const std::string &c : h.seams) std::fprintf(stderr, "  %s\n", c.c_str());
		return false;
	}
	if (!expect(h.socket.sends == 1, "pre-match frame: the ClientHello shipped once")) return false;
	if (!expect(h.role.started(), "pre-match frame: the hello latch armed")) return false;
	if (!expect(!h.role.local_spawned(), "pre-match frame: no L")) return false;
	if (!expect(!h.kernel->world.cached.local_player.valid(), "pre-match frame: no local player")) return false;
	if (!expect(h.role.now_tick() == 1, "pre-match frame: the clock advanced")) return false;
	if (!expect(h.role.take_diagnostic_sample(), "pre-match frame: the tripwire sampled on tick 0")) return false;

	// Frame 2: no second hello, no tripwire sample (1 % 62 != 0), still no L.
	h.seams.clear();
	h.role.run_tick(h.input);
	if (!expect(h.seams == expected, "frame 2: the seams again")) return false;
	if (!expect(h.socket.sends == 1, "frame 2: hello is a one-shot")) return false;
	if (!expect(!h.role.take_diagnostic_sample(), "frame 2: no tripwire sample")) return false;
	return expect(h.role.now_tick() == 2, "frame 2: the clock advanced again");
}

// The preload frame (no world to tick) shares the hello latch and the clock.
bool run_preload_frame() {
	Harness h;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1 && h.role.started(), "preload: the hello shipped once")) return false;
	if (!expect(h.role.now_tick() == 1, "preload: the clock advanced")) return false;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1, "preload: hello is a one-shot")) return false;
	return expect(h.role.now_tick() == 2, "preload: the clock advanced again");
}

// A seeded in-match runtime spawns L on the first frame (the in-match edge),
// latches H, clears the join-wait latches, and seeds the look heading.
bool run_in_match_spawn_edge() {
	Harness h;
	// Production order: the pre-load preload frame ships the ClientHello (the
	// latch arms there), the session establishes later. runtime.start()
	// RESETS a session, so the latch must already be armed when the in-match
	// frame runs — exactly what poll_preload guarantees.
	h.role.poll_preload();
	if (!expect(h.role.started() && h.socket.sends == 1,
			"in-match: the preload hello armed the latch")) return false;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(h.role.runtime->in_match(), "in-match: seeded runtime is InMatch")) return false;
	h.weapon().fire_held = true;
	h.weapon().fire_pressed = true;
	h.weapon().reload_pressed = true;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "in-match frame: L spawned on the edge")) return false;
	if (!expect(h.socket.sends == 1, "in-match frame: no second hello on the seeded session")) return false;
	if (!expect(h.kernel->world.cached.local_player.valid(),
			"in-match frame: cached.local_player published")) return false;
	if (!expect(h.role.self_wire_handle() == 0x0005,
			"in-match frame: H latched from the runtime")) return false;
	// The join-wait fire latches died at the spawn edge (the weapon pump ran
	// gated open the same frame over the cleared latches).
	if (!expect(!h.weapon().fire_held && !h.weapon().fire_pressed &&
					!h.weapon().reload_pressed,
			"in-match frame: join-wait weapon latches cleared")) return false;

	// Frame 2: the edge is a one-shot.
	const w::EntityHandle L = h.kernel->world.cached.local_player;
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "frame 2: still spawned")) return false;
	return expect(h.kernel->world.cached.local_player == L, "frame 2: no re-spawn");
}

// reset_for_join re-arms the per-session latches for a fresh dial.
bool run_reset_for_join() {
	Harness h;
	h.role.run_tick(h.input);
	if (!expect(h.role.started(), "reset: latch armed before")) return false;
	h.role.reset_for_join();
	if (!expect(!h.role.started() && !h.role.local_spawned() &&
					h.role.self_wire_handle() == 0 &&
					h.role.flat_seconds() == 0 &&
					!h.role.freeze_suspected(),
			"reset: per-session latches cleared")) return false;
	// The clock deliberately survives (retail's per-frame tick is process-scoped).
	return expect(h.role.now_tick() == 1, "reset: the clock is not a session latch");
}

// The D-NET-194 wire-header joiner: no pre-load kit, so inventory_valid is
// false at frame entry. The first S2C 0x5A grant folds and the
// apply_authoritative seam flips the live flag + arms the equipped slot
// INSIDE run_client_net_frame — i.e. BEFORE spawn_and_arm the SAME frame. The
// spawn edge must see that live flip and stamp L's equipped_adm_index;
// otherwise the entity carries the default adm on the C2S 0x0C uplink until
// the next respawn.
bool run_spawn_stamps_equipped_adm_from_midframe_grant() {
	Harness h;
	// Model retail's mid-frame apply: flip the live inventory-valid flag and
	// arm the equipped combo, exactly as apply_authoritative_loadout ->
	// local_loadout_rebuild does before the spawn block runs.
	constexpr int16_t kGrantedAdm = 16; // WPN_M16BURST-shaped grant
	h.role.kit_seams.apply_authoritative = [&h, kGrantedAdm] {
		h.seams.push_back("loadout");
		h.kernel->local.inventory_valid = true;
		h.inventory().equipped_combo = 0;
		h.inventory().slots[0].adm_index = kGrantedAdm;
	};
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(h.role.runtime->in_match(), "midframe grant: seeded runtime InMatch"))
		return false;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "midframe grant: L spawned")) return false;
	const w::Entity *L = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
	if (!expect(L != nullptr, "midframe grant: L resolvable")) return false;
	return expect(L->equipped_adm_index == kGrantedAdm,
			"midframe grant: the same-frame grant stamped L's equipped adm "
			"(regression: a by-value inventory_valid froze it at frame entry)");
}


// Retail drives the carrier from the mounted person's C2S 0x0C. Exercise
// the joiner frame's confirmed mount, input and client prediction together.
// [orig: Client_ProcessNetworkFrame @0x42c180;
// Entity_UpdateVehiclePhysics @0x48af00; net-re section 5.13]
bool run_confirmed_vehicle_drive(int occupancy) {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.add_system(&world.ai);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local && local->handle.packed != self_handle,
			"vehicle joiner: local body and wire identity differ")) return false;
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 1291;
	vehicle.has_item_def = true;
	vehicle.item_type = 1;
	vehicle.item_attrib = 0x40u;
	vehicle.spawn_origin = (1u << 24) | 3u;
	vehicle.position = local->position;
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat controller;
	controller.type = w::SeatType::Controller;
	controller.bone_index = 1;
	controller.retail_slot = 8;
	controller.source_name = "ctrlx00";
	if (occupancy == 1) controller.occupant = w::EntityHandle{self_handle};
	vehicle.seats.push_back(controller);
	const auto vh = world.registry.spawn(1, vehicle);
	w::EntityHandle previous_driver;
	if (occupancy == 2) {
		w::Entity peer;
		peer.kind = w::EntityKind::Organic;
		peer.team = local->team;
		peer.health = peer.health_max = 100;
		previous_driver = world.registry.spawn(0, peer);
		if (!expect(world.vehicles.process_attach(previous_driver, vh, 1),
				"fixture: previous driver occupies the confirmed seat")) return false;
		world.registry.get(previous_driver)->team = local->team == 1 ? 2 : 1;
	}
	w::VehicleTraits traits;
	traits.physics = 1;
	traits.player_control = true;
	traits.player_speed = 94 * 293;
	traits.acceleration = 15 * 4;
	traits.deceleration = 70 * 4;
	traits.turn_rate = 65 * 192426;
	traits.turn_rate2 = 41 * 192426;
	world.vehicles.traits.set(vehicle.item_id, traits);
	auto &row = h.role.runtime->state().upsert(vh.packed);
	row.type_id = static_cast<uint16_t>(vehicle.item_id);
	row.cls = EntityClass::Vehicle;
	row.x = w::to_fixed(vehicle.position.x);
	row.y = w::to_fixed(vehicle.position.y);
	row.z = w::to_fixed(vehicle.position.z);
	row.heading_known = true;
	row.heading_bam = w::bam_heading_from_mission_yaw_deg(vehicle.yaw);
	row.net_has_compact = true;
	row.compact_revision = 1;
	auto &self = h.role.runtime->state().upsert(self_handle);
	self.cls = EntityClass::Player;
	self.carrier_handle = vh.packed;
	self.mount_bone = 1;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	std::printf("vehicle joiner occupied=%d: mounted=%d local=%04x wire=%04x\n",
			occupancy, local->mounted, local->handle.packed, self_handle);
	if (!expect(local->mounted && local->mount_target == vh &&
			local->mount_type == w::SeatType::Controller,
			"retail-confirmed controller seat attaches the local joiner body")) return false;
	if (previous_driver.valid() &&
			!expect(!world.registry.get(previous_driver)->mounted,
					"confirmed seat assignment detaches the previous occupant")) return false;
	if (occupancy == 3) {
		world.registry.get(vh)->primary_occupant = {};
		world.registry.get(vh)->seats[0].occupant = {};
		h.role.run_tick(h.input);
	}
	if (!expect(world.registry.get(vh)->primary_occupant == local->handle &&
			world.registry.get(vh)->seats[0].occupant == local->handle,
			"confirmed controller relation restores its control link")) return false;
	h.kernel->local.set_movement_keys(true, false, false, false, false, false, false);
	for (int tick = 0; tick < 62; ++tick) h.role.run_tick(h.input);
	const auto *driven = world.registry.get(vh);
	ProtocolMessage movement;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, movement) &&
			decode_entity_packet_sub_header(movement.payload.data(), movement.payload.size(),
					sub, header_bytes) &&
			decode_player_extended_uplink(movement.payload.data() + header_bytes,
					movement.payload.size() - header_bytes, uplink, body_bytes) &&
			sub.handle == self_handle && header_bytes + body_bytes == movement.payload.size(),
			"the real outgoing C2S movement packet identifies the joiner by its host handle"))
		return false;
	const float dx = driven->position.x - vehicle.position.x;
	const float dy = driven->position.y - vehicle.position.y;
	std::printf("vehicle joiner: move=%02x carrier=%04x speed=%d distance_squared=%.4f\n",
			uplink.move_input_byte, uplink.carrier_handle, driven->veh.speed, dx * dx + dy * dy);
	if (!expect(uplink.carrier_handle == vh.packed &&
			(uplink.move_input_byte & w::Entity::kMoveOrderMoving) != 0,
			"mounted joiner uplinks its vehicle and held forward input") ||
			!expect(driven->veh.speed > 0 && dx * dx + dy * dy > 0.25f,
					"holding forward predicts motion of the confirmed driver vehicle")) return false;

	def::DefVehicleHudBlock hud_block{};
	std::vector<hud::HudVehicleSeat> panel;
	w::fill_vehicle_panel_seats(world, vh, local->handle, hud_block, panel, &h.role);
	if (!expect(panel.size() == 1 && panel[0].occupied && panel[0].own_seat &&
			panel[0].health == local->health,
			"vehicle overlay highlights the confirmed local seat and its health")) return false;

	// The use-item scan can select another nearby carrier while mounted.
	// It must queue attach, keep the current seat until the echo, then detach
	// only when no alternative seat is available.
	w::Entity adjacent = vehicle;
	adjacent.position = h.kernel->local.player_position();
	adjacent.position.x += 1.0f;
	adjacent.seats[0].occupant = {};
	const auto adjacent_h = world.registry.spawn(1, adjacent);
	std::vector<w::AttachLabel> labels;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	bool adjacent_highlighted = false;
	for (const auto &label : labels) {
		if (label.nearest && label.entity == adjacent_h && label.seat_index == 0)
			adjacent_highlighted = true;
	}
	if (!expect(adjacent_highlighted,
			"seat overlay highlights the same nearby seat the mounted action selects")) return false;
	h.socket.datagrams.clear();
	if (!expect(h.role.queue_mount_toggle(), "mounted use-item queues a request")) return false;
	h.role.run_tick(h.input);
	ProtocolMessage request;
	if (!expect(h.socket.last_message(0x26, request) &&
			request.payload == std::vector<uint8_t>({
					static_cast<uint8_t>(self_handle), 0,
					static_cast<uint8_t>(adjacent_h.packed),
					static_cast<uint8_t>(adjacent_h.packed >> 8), 1, 0}) &&
			h.kernel->local.player()->mount_target == vh,
			"mounted use-item requests the nearby seat and waits for host confirmation"))
		return false;
	h.role.runtime->state().find(self_handle)->carrier_handle = adjacent_h.packed;
	h.role.run_tick(h.input);
	if (!expect(h.kernel->local.player()->mount_target == adjacent_h,
			"seat swap applies on the host's relationship echo")) return false;
	world.registry.get(vh)->health = 0;
	h.socket.datagrams.clear();
	if (!expect(h.role.queue_mount_toggle(), "use-item without another seat queues detach"))
		return false;
	h.role.run_tick(h.input);
	if (!expect(h.socket.last_message(0x27, request) &&
			h.kernel->local.player()->mount_target == adjacent_h,
			"dismount also waits for the host's confirmation")) return false;
	auto *detached_self = h.role.runtime->state().find(self_handle);
	detached_self->carrier_handle = w::EntityHandle::kInvalid;
	detached_self->mount_bone = 0;
	h.role.run_tick(h.input);
	return expect(!h.kernel->local.player()->mounted &&
			!world.registry.get(adjacent_h)->seats[0].occupant.valid() &&
			!world.registry.get(adjacent_h)->primary_occupant.valid(),
			"confirmed dismount releases the local seat and controller");
}


// A remote player has only a decoded pool-0 row on a joiner. Its current
// carrier/bone must still occupy the seat for both Use and the health panel.
bool run_remote_vehicle_occupancy() {
	Harness h;
	auto &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const auto *local = h.kernel->local.player();
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 1291;
	vehicle.has_item_def = true;
	vehicle.item_type = 1;
	vehicle.item_attrib = 0x40u;
	vehicle.position = local->position;
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat seat;
	seat.type = w::SeatType::Controller;
	seat.bone_index = 1;
	seat.retail_slot = 8;
	vehicle.seats.push_back(seat);
	const auto vh = world.registry.spawn(1, vehicle);
	auto &peer = h.role.runtime->state().upsert(0x0006);
	peer.cls = EntityClass::Player;
	peer.type_id = w::kPlayerInfantryTypeId;
	peer.carrier_handle = vh.packed;
	peer.mount_bone = 1;
	peer.net_has_compact = true;
	peer.health_class_byte = 0x18;
	peer.team = static_cast<uint8_t>(local->team);
	peer.team_known = true;
	def::DefItemDef person_def{};
	person_def.id = w::kPlayerInfantryTypeId;
	person_def.hp = 100;
	def::DefItemsFile items{&person_def, 1};
	h.kernel->set_items_table(&items);
	h.role.run_tick(h.input);
	std::vector<w::AttachLabel> labels;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	bool ok = expect(labels.empty(), "remote occupied seat has no free-seat label");
	ok &= expect(!h.role.queue_mount_toggle(), "Use cannot request a remote occupied seat");
	def::DefVehicleHudBlock block{};
	std::vector<hud::HudVehicleSeat> panel;
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel.size() == 1 && panel[0].occupied && !panel[0].own_seat,
	             "remote rider appears as occupied in the vehicle panel");
	ok &= expect(panel[0].health == 59 && panel[0].max_health == 100,
	             "remote rider health uses the retail compact-tier midpoint");

	// Another wire player can occupy the numeric handle of native L.
	auto old_peer = *h.role.runtime->state().find(0x0006);
	old_peer.handle = local->handle.packed;
	h.role.runtime->state().find(0x0006)->mount_bone = 0;
	h.role.runtime->state().upsert(local->handle.packed) = old_peer;
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel[0].occupied && !panel[0].own_seat,
	             "a remote wire handle equal to L never highlights an own seat");

	// Same-team passengers can use the remaining seat; an enemy in the
	// controller seat blocks the whole vehicle, for labels and Use alike.
	w::Seat passenger = seat;
	passenger.type = w::SeatType::Passenger;
	passenger.bone_index = 2;
	passenger.retail_slot = 0;
	world.registry.get(vh)->seats.push_back(passenger);
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.size() == 1 && labels[0].seat_index == 1,
	             "same-team remote rider leaves other seats available");
	auto *remote = h.role.runtime->state().find(local->handle.packed);
	remote->team = local->team == 1 ? 2 : 1;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.empty() && !h.role.queue_mount_toggle(),
	             "a remote enemy blocks the entire vehicle for labels and Use");

	// Compact dismount supersedes even a retained spawn mountHandles entry.
	world.registry.get(vh)->seats[0].occupant = local->handle;
	remote->mount_bone = 0;
	remote->carrier_handle = w::EntityHandle::kInvalid;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(labels.size() == 2 && !panel[0].occupied,
	             "remote dismount frees selection and panel despite stale spawn occupancy");
	world.registry.get(vh)->seats[0].occupant = w::EntityHandle{0x5000};
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(!panel[0].occupied,
	             "an unresolved raw control handle does not draw a rider health marker");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_pre_match_frame();
	ok &= run_preload_frame();
	ok &= run_in_match_spawn_edge();
	ok &= run_spawn_stamps_equipped_adm_from_midframe_grant();
	ok &= run_reset_for_join();
	ok &= run_remote_vehicle_occupancy();
	ok &= run_confirmed_vehicle_drive(0);
	ok &= run_confirmed_vehicle_drive(1);
	ok &= run_confirmed_vehicle_drive(2);
	ok &= run_confirmed_vehicle_drive(3);
	if (!ok) return 1;
	std::printf("joiner_role_test: OK\n");
	return 0;
}
