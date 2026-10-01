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
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/connection_fan.h>
#include <formats/def/def.h>
#include <formats/mission/mission.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/angle.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/throwables.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <base/io/bam.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

namespace {
// The USE/label seat scan gates on an aim cone from the eye (standing
// 0x3FFFFFC0, seated 5 deg) [orig: Entity_FindNearestSeatOrArmory
// @0x43608f..0x436123], so a rig must face the seat point it expects to
// be offered, from a step away (the co-located case is atan2(0, 0)).
void look_at(w::World &world, w::EntityHandle player_h, float tx, float ty, float tz) {
	w::Entity *player = world.registry.get(player_h);
	if (player == nullptr) return;
	const double dx = double(tx) - player->position.x - player->eye_offset_x / 65536.0;
	const double dy = double(ty) - player->position.y - player->eye_offset_y / 65536.0;
	const double dz = double(tz) + 0.1875 - player->position.z - player->eye_offset_z / 65536.0;
	const double heading = std::atan2(dy, dx);
	const double pitch = std::atan2(dz, std::hypot(dx, dy));
	player->yaw = int16_t(std::lround(90.0 - heading * 180.0 / 3.14159265358979323846));
	player->pitch = int16_t(std::lround(pitch * 180.0 / 3.14159265358979323846));
	if (w::AiEntity *body = world.ai.for_handle(player_h)) {
		body->heading = int32_t(heading * opennova::io::kBamPerRadian);
		body->pitch = int32_t(pitch * opennova::io::kBamPerRadian);
	}
}
} // namespace

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
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
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

// L spawns on the in-match edge, after the boot's definition sweeps: the edge
// binds its items.def row like the host's boot did, so L's body sounds resolve
// through its own profile instead of the slotless "default" (silent footsteps
// on every joiner). [orig: Entity_InitFromItemDef @0x49e550; the def+0x268
// profile binding @0x49fb0f..0x49fb64]
bool run_in_match_spawn_binds_local_player_definition() {
	Harness h;
	static const char kProfiles[] =
			"begin \"default\"\n"
			"end\n"
			"begin \"SP_JoinerSelf\"\n"
			"     SSLFootGND     T_DIRT_L\n"
			"end\n";
	if (!expect(h.kernel->world.tables.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1) == 2,
			"profiles parsed")) return false;
	std::vector<opennova::def::DefItemDef> rows(1);
	rows[0].id = static_cast<int>(w::kPlayerInfantryTypeId) +
			static_cast<int>(opennova::mission::kItemIdOffset);
	rows[0].hp = 100;
	std::snprintf(rows[0].sound_profile, sizeof(rows[0].sound_profile), "SP_JoinerSelf");
	opennova::def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	h.kernel->set_items_table(&items);

	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const w::EntityHandle L = h.kernel->world.cached.local_player;
	if (!expect(h.role.local_spawned() && L.valid(), "L spawned on the edge")) return false;
	const w::AiEntity *body = h.kernel->world.ai.for_handle(L);
	const w::Entity *e = h.kernel->world.registry.get(L);
	bool ok = expect(body != nullptr && e != nullptr, "L has its body");
	if (!ok) return false;
	ok &= expect(body->profile.sound_profile == 1,
	             "L's sound profile is its items.def row's, not the slotless default");
	ok &= expect(e->has_item_def, "L carries its items.def traits");
	h.kernel->set_items_table(nullptr);
	return ok;
}

// The runtime owns H across removal and repair; every weapon and presenter
// consumer must follow it after the first local spawn instead of caching H.
bool run_self_handle_lifecycle() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, 5, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const w::EntityHandle local = h.kernel->world.cached.local_player;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> body;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::ENTITY_REMOVE, {5, 0})}, body);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	if (!expect(!h.role.runtime->has_self_handle() && !h.role.has_self_wire_handle() &&
					h.role.self_wire_handle() == w::EntityHandle::kInvalid,
			"role stops using the retired runtime identity (kInvalid, never handle 0)")) return false;
	// Slot 0 is a LIVE pool-0 handle (the listen host's own player). While the
	// identity is retired, a remote row there must keep flowing to every
	// consumer that excludes self by handle; a 0 sentinel silently dropped it.
	// The minefield actor fold is the cheapest such consumer on the frame.
	auto &host_row = h.role.runtime->state().upsert(0x0000);
	host_row.cls = EntityClass::Player;
	host_row.type_id = w::kPlayerInfantryTypeId;
	h.role.run_tick(h.input);
	bool host_row_seen = false;
	for (const auto &actor : h.kernel->world.minefields.remote_actors)
		if (actor.handle == 0x0000) host_row_seen = true;
	if (!expect(host_row_seen,
			"the slot-0 remote row is not mistaken for self while the identity is retired")) return false;
	// The replay/session seed is also a public identity replacement seam.
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, 7, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	return expect(h.role.self_wire_handle() == 7 &&
			h.kernel->world.cached.local_player == local,
			"role follows replacement identity without duplicating its local motor entity");
}

// reset_for_join re-arms the per-session latches for a fresh dial.
bool run_reset_for_join() {
	Harness h;
	h.role.run_tick(h.input);
	if (!expect(h.role.started(), "reset: latch armed before")) return false;
	h.role.reset_for_join();
	if (!expect(!h.role.started() && !h.role.local_spawned() &&
					h.role.self_wire_handle() == w::EntityHandle::kInvalid &&
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
bool run_confirmed_vehicle_drive(int occupancy, bool server_feedback = false,
		bool internet_conditions = false) {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
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
	vehicle.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
	vehicle.spawn_origin = (1u << 24) | 3u;
	vehicle.position = local->position;
	vehicle.position.x += 1.0f; // a step away: the scan aims from the eye at the seat point
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat controller;
	controller.type = w::SeatType::Controller;
	controller.bone_index = 1;
	controller.retail_slot = 8;
	controller.source_name = "ctrlx00";
	if (occupancy == 1) controller.occupant = w::EntityHandle{self_handle};
	vehicle.seats.push_back(controller);
	if (occupancy == 4) {
		w::Seat passenger;
		passenger.type = w::SeatType::Passenger;
		passenger.bone_index = 4;
		passenger.retail_slot = 0;
		passenger.source_name = "sitex00";
		// Array order differs from the numbered HUD list: driver is key 1.
		vehicle.seats.insert(vehicle.seats.begin(), passenger);
	}
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
	self.type_id = w::kPlayerInfantryTypeId; // the spawn stream's identity: a compact never types a row
	self.carrier_handle = vh.packed;
	self.mount_bone = occupancy == 4 ? 4 : 1;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	if (occupancy == 4) {
		if (!expect(local->mount_type == w::SeatType::Passenger,
				"numbered-seat fixture starts in a confirmed passenger seat")) return false;
		h.socket.datagrams.clear();
		if (!expect(h.role.queue_numbered_seat(0),
				"seat key 1 queues the available driver seat")) return false;
		h.role.run_tick(h.input);
		ProtocolMessage selected;
		if (!expect(h.socket.last_message(0x26, selected) &&
				selected.payload == std::vector<uint8_t>({self_handle, 0,
					static_cast<uint8_t>(vh.packed), static_cast<uint8_t>(vh.packed >> 8), 1, 0}) &&
				h.kernel->local.player()->mount_bone == 4,
				"numbered seat emits the driver bone with H and waits for the host echo")) return false;
		h.role.runtime->state().find(self_handle)->mount_bone = 1;
		h.role.run_tick(h.input);
		local = h.kernel->local.player();
	}
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
			world.registry.get(vh)->seats[occupancy == 4 ? 1 : 0].occupant == local->handle,
			"confirmed controller relation restores its control link")) return false;
	// Keep an independent authority alive: a prediction-only test can pass
	// while the host stays parked and every received compact pulls us back.
	auto authority = std::make_unique<w::World>();
	replication::LoopbackChannel channel;
	replication::Connection connection{
			&channel, replication::TransportMode::Client, w::EntityHandle{self_handle}, 0};
	if (server_feedback) {
		h.role.runtime->view().set_item_class_resolver([](uint16_t type) -> std::optional<EntityClass> {
			if (type == 1291) return EntityClass::Vehicle;
			if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
			return std::nullopt;
		});
		authority->registry.configure_pool(0, 16);
		authority->registry.configure_pool(1, 16);
		authority->load_systems();
		w::Entity peer = *local;
		peer.mounted = false;
		peer.mount_target = {};
		peer.mount_seat = -1;
		peer.flags &= ~w::kEntityFlagMounted;
		authority->registry.spawn_from(0, self_handle, peer);
		authority->ai.attach(w::EntityHandle{self_handle});
		w::Entity carrier = vehicle;
		for (auto &seat : carrier.seats) seat.occupant = {};
		authority->registry.spawn_from(1, vh.slot(), carrier);
		authority->vehicles.traits.set(vehicle.item_id, traits);
		if (!expect(authority->vehicles.process_attach(
				w::EntityHandle{self_handle}, vh, 1), "authority confirms driver")) return false;
	}
	// NovaWorld dictates a 12-tick uplink period. Add six ticks each way
	// (~194 ms RTT at 62 Hz); only newly emitted controls reach the authority.
	// Re-sending the last observed packet every tick would hide a pacing bug.
	const int period = internet_conditions ? 12 : 4;
	const int delay = internet_conditions ? 6 : 0;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	auto frame_server = [&](const std::vector<ProtocolMessage> &messages) {
		std::vector<uint8_t> body;
		if (!frame_session_packet(server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
				messages, body)) return std::vector<uint8_t>{};
		return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	};
	if (internet_conditions) {
		const auto settings = frame_server({make_protocol_message(0x00,
				{0x01, 0x08, 0x00, 0x00, 0x00, 12, 0x00, 0x00, 0x00}, 0xA0)});
		h.role.runtime->receive(settings.data(), settings.size());
	}
	std::deque<std::pair<int, ProtocolMessage>> toward_host;
	std::deque<std::pair<int, std::vector<uint8_t>>> toward_client;
	std::size_t sent_cursor = h.socket.datagrams.size();
	int control_packets = 0;
	h.kernel->local.set_movement_keys(true, false, false, false, false, false, false);
	const int drive_ticks = internet_conditions ? 186 : 62;
	for (int tick = 0; tick < drive_ticks; ++tick) {
		while (!toward_client.empty() && toward_client.front().first <= tick) {
			const auto &packet = toward_client.front().second;
			h.role.runtime->receive(packet.data(), packet.size());
			toward_client.pop_front();
		}
		h.role.run_tick(h.input);
		if (!server_feedback) continue;
		for (; sent_cursor < h.socket.datagrams.size(); ++sent_cursor) {
			const auto &packet = h.socket.datagrams[sent_cursor];
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!nw_decode_inbound(packet.data(), packet.size(), opcode, body) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
						header, messages)) continue;
			for (auto &message : messages) {
				if (message.tag != 0x0C) continue;
				++control_packets;
				toward_host.emplace_back(tick + delay, std::move(message));
			}
		}
		while (!toward_host.empty() && toward_host.front().first <= tick) {
			const auto &message = toward_host.front().second;
			channel.client_send(message.tag, message.payload);
			toward_host.pop_front();
		}
		replication::drain_connection_c2s(*authority, connection);
		authority->run_logic_tick(true);
		if ((tick + 1) % period == 0) {
			if (!expect(replication::emit_connection_s2c(*authority, connection,
					replication::snapshot_world(*authority)), "authority emits vehicle update"))
				return false;
			replication::Datagram update;
			while (channel.client_recv(update)) {
				auto packet = frame_server({make_protocol_message(update.tag, update.body)});
				if (!expect(!packet.empty(), "authority frames a real S2C update")) return false;
				toward_client.emplace_back(tick + delay, std::move(packet));
			}
		}
	}
	if (internet_conditions &&
			!expect(h.role.runtime->send_holdoff_ticks() == 12 &&
					control_packets >= 15 && control_packets <= 16,
					"NovaWorld vehicle controls are emitted once every twelve ticks")) return false;
	if (server_feedback) {
		if (!expect(h.role.runtime->view().malformed_bodies() == 0 &&
				h.role.runtime->state().find(vh.packed)->compact_revision > 1,
				"continuous server vehicle records were decoded and applied")) return false;
		const auto *host_vehicle = authority->registry.get(vh);
		if (!expect(host_vehicle->veh.speed > 0 &&
				std::hypot(host_vehicle->position.x - vehicle.position.x,
						host_vehicle->position.y - vehicle.position.y) > 0.5f,
				"the authority drives the vehicle from real joiner packets")) return false;
	}
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
	if (occupancy == 4) hud_block.seat_count = 1;
	std::vector<hud::HudVehicleSeat> panel;
	w::fill_vehicle_panel_seats(world, vh, local->handle, hud_block, panel, &h.role);
	if (!expect(panel.size() == (occupancy == 4 ? 2u : 1u) && panel[0].occupied && panel[0].own_seat &&
			panel[0].health == local->health,
			"vehicle overlay highlights the confirmed local seat and its health")) return false;

	if (occupancy == 4 || server_feedback) return true;

	// The use-item scan can select another nearby carrier while mounted.
	// It must queue attach, keep the current seat until the echo, then detach
	// only when no alternative seat is available.
	w::Entity adjacent = vehicle;
	adjacent.position = h.kernel->local.player_position();
	adjacent.position.x += 1.0f;
	adjacent.seats[0].occupant = {};
	const auto adjacent_h = world.registry.spawn(1, adjacent);
	// Seated, the scan admits only what the rider looks at (5 deg cone).
	look_at(world, local->handle, adjacent.position.x, adjacent.position.y, adjacent.position.z);
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
	person_def.id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	person_def.hp = 100;
	def::DefItemsFile items{&person_def, 1};
	h.kernel->set_items_table(&items);
	h.role.run_tick(h.input);
	look_at(world, local->handle, vehicle.position.x, vehicle.position.y, vehicle.position.z);
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
	ok &= expect(!panel[0].occupied && panel[0].label.empty(),
	             "an unresolved raw control handle draws neither a rider marker nor a digit");

	// The retained spawn slot can still name our wire H before the first self
	// compact reconciles it to L: that seat is the local player's own.
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	world.registry.get(vh)->seats[0].occupant = w::EntityHandle{0x0005};
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel[0].occupied && panel[0].own_seat && panel[0].health == local->health,
	             "a retained H occupant reads as the local player's own seat");
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.size() == 1 && labels[0].seat_index == 1,
	             "the retained H seat is not offered to the local player again");
	return ok;
}

// A real local weapon pump must carry its selected lock through C2S 0x06.
// [orig: WeaponAction_Fire target read @0x542C00..0x542C15;
//  NetPacket_WriteEntityPositionUpdate target store @0x42A759]
bool run_guided_fire_preserves_selected_target() {
    Harness h;
    auto &world = h.kernel->world;
    world.tables.weapons.entries.resize(2);
    auto &def = world.tables.weapons.entries[1];
    def.valid = true; def.name = "WPN_GUIDED"; def.ammo_index = 1;
    world.tables.ammo.entries.resize(2);
    world.tables.ammo.entries[1].valid = true;
    world.tables.ammo.entries[1].max_age_ticks = 100;
    world.tables.ammo.entries[1].velocity = 248;
    world.tables.ammo.entries[1].tracer_item_friendly = 10;
    world.tables.ammo.entries[1].tracer_item_enemy = 10;
    world.throwables.classes.set({10,w::ThrowClass::kNone,w::ThrowClass::kJavelin});
    h.role.kit_seams.apply_authoritative = [&h] {
        h.kernel->local.inventory_valid = true;
        h.inventory().equipped_combo = 0;
        h.inventory().slots[0].adm_index = 1;
    };
    h.role.poll_preload();
    h.role.runtime->seed_session(kSessionId,kClientKey,kClientScrk,kServerScrk,
        1,0,0x0005,w::kPlayerInfantryTypeId);
    h.role.run_tick(h.input);
    w::WeaponInstallData data;
    data.name = "WPN_GUIDED"; data.clipsize = 4; data.rows.resize(3);
    std::snprintf(data.rows[0].name,sizeof(data.rows[0].name),"idle");
    std::snprintf(data.rows[1].name,sizeof(data.rows[1].name),"fire");
    data.rows[1].delayend = 6;
    std::snprintf(data.rows[2].name,sizeof(data.rows[2].name),"recoil");
    w::local_weapon_install(world,h.weapon(),data,false,false,nullptr,h.kernel->local.view);
    h.kernel->local.inventory_valid = false;
    constexpr uint16_t target = 0x1003;
    auto *body = world.ai.for_handle(world.cached.local_player);
    if (!expect(body != nullptr,"guided: local body spawned")) return false;
    body->slot.f[3] = int(target)+1;
    // The latest player lock must win over an older self replica at launch.
    h.role.runtime->state().upsert(0x0005).fire_target_handle = 0x1002;
    auto &target_row = h.role.runtime->state().upsert(target);
    target_row.cls = EntityClass::Vehicle;
    target_row.x = 200*65536; target_row.y = 30*65536; target_row.z = 4*65536;
    h.weapon().fire_pressed = h.weapon().fire_held = true;
    h.role.run_tick(h.input);
    h.role.run_tick(h.input);
    ProtocolMessage message;
    if (!expect(h.socket.last_message(0x06,message),"guided: C2S fire sent")) return false;
    ClientFiredRound fire;
    size_t consumed = 0;
    if (!expect(decode_client_fired_round(message.payload.data(),message.payload.size(),fire,consumed)
        && fire.target_handle == target,"guided: C2S fire retains selected target")) return false;
    for (const auto &round : world.round_sim.rounds) {
        if (round.guided_family == w::GuidedFamily::Javelin)
            return expect(round.active && round.guided.target == target,
                "guided: predicted launch retains current lock despite stale replica");
    }
    return expect(false,"guided: local fire creates a predicted Javelin");
}

// A pilot's countermeasure rides the same C2S 0x06 as the handheld, and the
// descriptor's side bytes are the PILOT's, not the flare's. Retail: the flare
// fires with the vehicle's occupantEntity as the shooter and
// (aiRuntime[3], targetId 1, ammoDefIndex, weaponSlot 0)
// [orig: Weapon_FireProcess @0x53f6d6 -> @0x53f70a]; the writer then stores
// targetId at off6 (@0x42a68a), the ammo-def index at off7 (@0x42a69b),
// *(WORD*)(shooter+352) — the pilot's handheld ammo-def index — at off32
// (@0x42c052 -> @0x42a7da), fire_flags at off33 (@0x42a7ed) and weaponSlot at
// off34 (@0x42a800). fire_flags = Weapon_GetScopeZoomLevel(can_fire, 12) |
// (can_fire ? 0x80 : 0) [orig: Entity_FireWeaponAndSendPacket @0x42bdd6..
// 0x42bdf9] with can_fire = Player_IsOpticalViewVisible() = 0 for a seated pilot
// (parentSlot 2/5 @0x5cf7a8..0x5cf7b6), and weaponActive 0 returns the default
// 12 [orig: @0x422fd1/@0x422fd5]. Expected bytes derived by hand from those
// legs: off6 = 1, off7 = the flare index, off32 = the handheld's ammo-def index
// (the retail capture's 0x03 beside adm 69, net-re 5.66 seeds the row),
// off33 = 12, off34 = 0, off28 = 0xFFFF (no AI runtime target). The yaw/pitch
// words are (bam + 0x8000) >> 16 [orig: @0x42a6e1/@0x42a6fb].
bool run_flare_descriptor_carries_the_pilot_handheld() {
	Harness h;
	constexpr int16_t kHandheldAdm = 69; // WPN_M4AUTO-shaped row
	constexpr int32_t kHandheldAmmo = 3; // its ammo-def index (the captured off32)
	constexpr uint8_t kFlareAmmo = 41;   // the FLARE ammo-def index; carried verbatim
	h.role.kit_seams.apply_authoritative = [&h, kHandheldAdm] {
		h.seams.push_back("loadout");
		h.kernel->local.inventory_valid = true;
		h.inventory().equipped_combo = 0;
		h.inventory().slots[0].adm_index = kHandheldAdm;
	};
	w::World &world = h.kernel->world;
	world.tables.weapons.entries.resize(static_cast<size_t>(kHandheldAdm) + 1);
	w::WeaponTableEntry &handheld = world.tables.weapons.entries[static_cast<size_t>(kHandheldAdm)];
	handheld.valid = true;
	handheld.name = "WPN_M4AUTO";
	handheld.ammo_index = kHandheldAmmo;

	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "flare: L spawned")) return false;
	const w::Entity *L = world.registry.get(world.cached.local_player);
	if (!expect(L != nullptr && L->equipped_adm_index == static_cast<uint8_t>(kHandheldAdm),
			"flare: L carries the handheld adm")) return false;
	if (!expect(world.ai.for_handle(L->handle) != nullptr, "flare: L carries its AI body"))
		return false;

	// What VehicleSystem::release_flares -> RoundSim::fire_source queues on a
	// joiner whose local player pilots the source (the launch already presented).
	w::RoundSpawnParams flare;
	flare.launch_presented = true;
	flare.owner = L->handle;
	flare.shooter_handle = L->handle.packed;
	flare.origin = w::Vec3{12.0f, -7.0f, 30.0f};
	flare.dir_yaw_bam = 0x20000000;
	flare.dir_pitch_bam = 0x10000000;
	flare.ammo_index = kFlareAmmo;
	flare.adm_index = kFlareAmmo;
	world.out.source_fires.push_back(flare);

	// The descriptor queues in the frame's weapon phase and ships with a send.
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	ProtocolMessage msg;
	if (!expect(h.socket.last_message(0x06, msg), "flare: a C2S 0x06 shipped")) return false;
	ClientFiredRound r;
	size_t consumed = 0;
	if (!expect(decode_client_fired_round(msg.payload.data(), msg.payload.size(), r, consumed) &&
					consumed == 45,
			"flare: the fixed 45 B body decodes")) return false;
	bool ok = true;
	ok &= expect(r.shooter_handle == 0x0005, "flare: the shooter is the pilot's wire handle");
	ok &= expect(r.fire_flags == 1, "flare: off6 = targetId 1 (the ammo-def arm)");
	ok &= expect(r.adm_index == kFlareAmmo, "flare: off7 = the flare's ammo-def index");
	ok &= expect(r.extra_byte1 == kHandheldAmmo,
			"flare: off32 = the pilot's handheld ammo-def index (entity+0x160), not the flare's");
	ok &= expect(r.extra_byte2 == 12,
			"flare: off33 = Weapon_GetScopeZoomLevel(0, 12) = 12 for a seated pilot");
	ok &= expect(r.misc_byte == 0, "flare: off34 = weaponSlot 0");
	ok &= expect(r.target_handle == 0xFFFF, "flare: off28 = 0xFFFF without an AI target");
	ok &= expect((r.hit_part & 0x1FF) == 1 &&
					(r.hit_part >> 9) == h.role.runtime->local_player_slot(),
			"flare: hit_part packs (roster slot << 9) | shot seq 1");
	ok &= expect(r.pos_x == 12 * 65536 && r.pos_y == -7 * 65536 && r.pos_z == 30 * 65536,
			"flare: the launch origin rides off8..off19 in full");
	ok &= expect(r.dir_x == 0x2000 && r.dir_y == 0x1000,
			"flare: the launch yaw/pitch ride as (bam + 0x8000) >> 16");
	ok &= expect(world.out.source_fires.empty(), "flare: the source-fire queue drained");
	return ok;
}

bool run_received_loadout_policy_and_sounds() {
    Harness h;
    h.role.poll_preload();
    h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
            1, 0, 5, w::kPlayerInfantryTypeId);
    SessionSequencing tx = inmatch::make_jo_game_session_sequencing();
    auto deliver = [&](const std::vector<ProtocolMessage> &messages) {
        std::vector<uint8_t> body;
        if (!frame_session_packet(tx, SessionCrypto{kServerScrk, {}, kClientKey},
                messages, body)) return false;
        const auto datagram = nw_encode_outbound(
                SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
        h.role.runtime->receive(datagram.data(), datagram.size());
        h.role.run_tick(h.input);
        return true;
    };
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS,
            {5, 5, 0, 7, 2, 9, 1, 10, 3, 255, 0})})) return false;
    auto &availability = h.kernel->local.loadout.availability;
    if (!expect(availability.value_for(5) == 0 && availability.value_for(7) == 2,
            "host weapon bans and armory-only rules reach the joiner's real loadout table")) return false;
    if (!expect(availability.value_for(9) == 1 && availability.value_for(10) == 1,
            "retail ignores restriction values other than 0 and 2")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {0})})) return false;
    if (!expect(availability.value_for(5) == 1 && availability.value_for(7) == 1,
            "an empty restriction list restores the all-allowed table")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {1, 254, 2})})) return false;
    if (!expect(availability.value_for(254) == 2,
            "the final valid weapon index accepts an armory restriction")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {1, 9})})) return false;
    if (!expect(availability.value_for(9) == 0 && availability.value_for(254) == 1,
            "a short restriction pair zero-fills its missing value after resetting the table")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {})})) return false;
    if (!expect(availability.value_for(9) == 1,
            "an empty body still resets retail's availability table")) return false;
    PlaySoundCommand positioned;
    positioned.flag = 1;
    positioned.has_pos = true;
    positioned.sound_name = "MEDIC_REQUEST";
    positioned.pos_x = -12;
    positioned.pos_y = 23;
    positioned.pos_z = 4;
    PlaySoundCommand listener;
    listener.sound_name = "MP_COMMAND1";
    const std::size_t before = h.kernel->world.out.slot_sounds.size();
    const std::size_t direct_before = h.kernel->world.out.script_sounds.size();
    if (!deliver({make_protocol_message(s2c::PLAY_SOUND, encode_play_sound(positioned)),
            make_protocol_message(s2c::PLAY_SOUND, encode_play_sound(listener))})) return false;
    if (!expect(h.kernel->world.out.slot_sounds.size() == before + 1,
            "positioned sound reaches the audio output exactly once")) return false;
    const auto &sound = h.kernel->world.out.slot_sounds.back();
    if (!expect(std::string(sound.set_name) == "MEDIC_REQUEST" &&
            sound.pos[0] == -12 * 65536 && sound.pos[1] == 23 * 65536 && sound.pos[2] == 4 * 65536,
            "wire sound coordinates are signed world units, converted to fixed point")) return false;
    if (!expect(h.kernel->world.out.script_sounds.size() == direct_before + 1 &&
            h.kernel->world.out.script_sounds.back().name == "MP_COMMAND1" &&
            h.kernel->world.out.script_sounds.back().kind == w::ScriptSoundEvent::Kind::Interface,
            "unpositioned sound reaches listener audio")) return false;
    h.role.run_tick(h.input);
    return expect(h.kernel->world.out.script_sounds.size() == direct_before + 1,
            "a received sound is not replayed on the next frame");
}


// Drive the production joiner frame: rendering can read a channel repeatedly
// without integrating another slew, and an NPC's own view follows its tether.
bool run_replica_turret_channel() {
 Harness h;
 h.role.poll_preload();
 // The replica's materialized world twin: an 'ewep' render class.
 auto &world = h.kernel->world;
 world.registry.configure_pool(1, 4);
 w::Entity twin;
 twin.kind = w::EntityKind::Item;
 twin.item_id = 123;
 twin.emplaced_ctrl_publisher = true;
 if (!expect(world.registry.spawn(1, twin).packed == 0x1000,
   "the world twin shares the wire handle")) return false;
 auto &state = h.role.runtime->state();
 mission::ItemSeatSpec spec;
 spec.type_id = 123;
 spec.item_attrib2 = def::DEF_ITEM_ATTRIB2_ISTURRET;
 w::Seat seat;
 seat.type = w::SeatType::Gunner;
 seat.bone_index = 1;
 spec.seats.push_back(seat);
 h.kernel->seat_specs.push_back(spec);
 auto &mount = state.upsert(0x1000);
 mount.type_id = 123;
 mount.cls = EntityClass::NoNetworkCallback;
 mount.heading_known = true;
 mount.heading_bam = 0;
 auto &gunner = state.upsert(2);
 gunner.cls = EntityClass::Player;
 gunner.carrier_handle = 0x1000;
 gunner.mount_bone = 1;
 gunner.heading_bam = 0x55555555;
 gunner.pitch_bam = 0x10000000;
 h.role.run_tick(h.input);
 auto *gun = state.find(0x1000);
 if (!expect(gun != nullptr && gun->emplaced_controls_valid &&
   gun->emplaced_gun_yaw_word == -147 && gun->emplaced_gun_pitch_word == -147,
   "remote turret advances one retail slew on each axis")) return false;
 if (!expect(state.find(2)->heading_bam == 0x55555555,
   "remote Player gunner skips local and NPC tethers")) return false;
 w::EmplacedWeaponControls first, second;
 inmatch::emplaced_weapon_controls_for_client(*gun, first);
 inmatch::emplaced_weapon_controls_for_client(*gun, second);
 if (!expect(first.gun_yaw == second.gun_yaw && gun->emplaced_gun_yaw_word == -147,
   "repeated presentation reads do not advance the turret")) return false;
 h.role.run_tick(h.input);
 if (!expect(state.find(0x1000)->emplaced_gun_yaw_word == -294,
   "second tick integrates from the stored high word")) return false;
 state.find(0x1000)->emplaced_gun_yaw_word = 0;
 state.find(2)->cls = EntityClass::Infantry;
 state.find(2)->heading_bam = 0x55555555;
 h.role.run_tick(h.input);
 if (!expect(state.find(2)->heading_bam == 0x2D7AD80,
   "NPC gunner yaw is written back at the four-degree tether")) return false;
 // Immediate gun, a stamped 45-degree yaw window (pitch +-90): word and
 // gunner look both pin on yaw.
 h.kernel->seat_specs[0].item_attrib2 = 0;
 h.kernel->seat_specs[0].turret_limits_valid = true;
 h.kernel->seat_specs[0].turret_yaw_range_bam = 0x20000000;
 h.kernel->seat_specs[0].turret_pitch_max_bam = 0x40000000;
 h.kernel->seat_specs[0].turret_pitch_min_bam = 0x40000000;
 state.find(2)->heading_bam = 0x55555555;
 state.find(2)->pitch_bam = 0;
 state.find(2)->recoil_pitch = 0x4000000;
 h.role.run_tick(h.input);
 if (!expect(state.find(0x1000)->emplaced_gun_yaw_word == -8192 &&
   (state.find(2)->heading_bam == 0x20400000 || state.find(2)->heading_bam == 0x1FC00000),
   "weapon window pins the barrel and writes back occupant look")) return false;
 if (!expect(state.find(0x1000)->emplaced_gun_pitch_word == -1024,
   "gun pitch consumes recoil before the body decay")) return false;
 state.find(2)->carrier_handle = 0xFFFF;
 h.role.run_tick(h.input);
 // The ewep writer has no occupant test: the dismounted turret keeps
 // publishing the words it was left at.
 // [orig: HUD_CacheWeaponSlotInfo @0x440930 via the 'ewep' render-class row
 //  @0x82CFA0]
 const auto *held = state.find(0x1000);
 if (!expect(held->emplaced_controls_valid && held->emplaced_gun_yaw_word == -8192 &&
   held->emplaced_gun_pitch_word == -1024,
   "a dismounted turret holds its last traverse")) return false;
 world.registry.get(w::EntityHandle{0x1000})->emplaced_ctrl_publisher = false;
 h.role.run_tick(h.input);
 return expect(!state.find(0x1000)->emplaced_controls_valid,
   "another render class publishes no EWEAP words");
}


bool run_local_replica_turret_channel() {
 Harness h;
 auto &world = h.kernel->world;
 world.registry.configure_pool(1,4);
 world.load_systems();
 h.role.poll_preload();
 constexpr uint16_t self_handle = 5;
 h.role.runtime->seed_session(kSessionId,kClientKey,kClientScrk,kServerScrk,
   1,0,self_handle,w::kPlayerInfantryTypeId);
 h.role.run_tick(h.input);
 auto *local = h.kernel->local.player();
 if (!expect(local != nullptr,"local turret fixture has L")) return false;
 w::Entity gun;
 gun.kind = w::EntityKind::Item;
 gun.item_id = 123;
 gun.spawn_origin = 1u << 24;
 gun.yaw = 90;
 gun.item_attrib2 = def::DEF_ITEM_ATTRIB2_ISTURRET;
 gun.health = 100;
 gun.team = local->team;
 w::Seat seat;
 seat.type = w::SeatType::Gunner;
 seat.bone_index = 1;
 seat.source_name = "UseGun";
 gun.seats.push_back(seat);
 const auto handle = world.registry.spawn(1,gun);
 mission::ItemSeatSpec spec;
 spec.type_id = 123;
 spec.item_attrib2 = gun.item_attrib2;
 spec.seats = gun.seats;
 h.kernel->seat_specs.push_back(spec);
 auto &state = h.role.runtime->state();
 auto &row = state.upsert(handle.packed);
 row.type_id = 123;
 row.cls = EntityClass::NoNetworkCallback;
 row.heading_known = true;
 row.heading_bam = 0;
 auto &self = state.upsert(self_handle);
 self.cls = EntityClass::Player;
 self.carrier_handle = handle.packed;
 self.mount_bone = 1;
 self.state_flags = w::kEntityFlagMounted;
 self.state_flags_known = true;
 h.role.run_tick(h.input);
 local = h.kernel->local.player();
 if (!expect(local->mounted && local->mount_type == w::SeatType::Gunner,
   "confirmed UseGun mounts local L")) return false;
 world.registry.get(handle)->emplaced_gun_yaw_word = 0;
 world.registry.get(handle)->emplaced_spin_phase = 0xFEDC;
 h.kernel->local.input.look_heading = 0x55555555;
 h.role.run_tick(h.input);
 const auto *mount = state.find(handle.packed);
 if (!expect(mount && mount->emplaced_gun_yaw_word == -147 &&
   world.registry.get(handle)->emplaced_gun_yaw_word == -147,
   "local joiner publishes exactly one slew from the world body")) return false;
 w::EmplacedWeaponControls controls;
 if (!expect(inmatch::emplaced_weapon_controls_for_client(*mount, controls) &&
   controls.spin == 0xFEDC,
   "local joiner forwards the world barrel phase without a wire field")) return false;
 return expect(h.kernel->local.input.look_heading == 0x3FFF7FC0,
   "local joiner input is tethered within ninety degrees of the gun");
}

// Every in-match pump stamps World::rules from the joiner's session: the
// session opens, and auto_scope_zero follows bit 0x10000 of the S2C
// 0x64 +44 mpattrib word (g_RulesFlags @0x24D1E34, the
// Player_AdjustWeaponZoomLevel test @0x4dbd15). The runtime view carries
// that word once the 0x64 transfer lands; here it is set on the view.
bool run_rules_stamp_from_mp_attributes(uint32_t mp_attributes, bool zoom_allowed) {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.runtime->view().set_mp_attributes(mp_attributes);
	w::World &world = h.kernel->world;
	if (!expect(!world.rules.auto_scope_zero,
			"rules stamp: a bare kernel starts without the zero rule")) return false;
	h.role.run_tick(h.input);
	return expect(world.rules.auto_scope_zero == zoom_allowed,
			"rules stamp: auto_scope_zero follows mpattrib bit 0x10000");
}

// A folded S2C 0x1D latches the joiner's round-over gate, so the frame that
// folds it holds its entity update, as retail's client does.
// [orig: NapiNPClientMsg_0x01D @0x430858 (`mov g_SpawnSuccessGate,1`);
//  Game_ProcessMainFrame -- the is_in_session / g_SpawnSuccessGate tests
//  @0x526734..0x526742 ahead of the Entity_UpdateAllEntities call @0x52674B]
bool run_end_round_header_holds_the_entity_update() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	w::World &world = h.kernel->world;
	world.rules.mp_session = true; // the joiner kernel boot's stamp
	h.role.run_tick(h.input);
	const uint32_t counted = world.entity_update_counter;
	h.role.run_tick(h.input);
	if (!expect(world.entity_update_counter == counted + 1,
			"end round: the round's frames run the entity update")) return false;
	if (!expect(!world.match.outcome().ended, "end round: no latch before the header"))
		return false;
	h.role.runtime->view().set_game_type(0x10000u);
	EndRoundHeader header;
	header.winner_team = 1;
	h.role.runtime->view().apply(s2c::END_ROUND_HEADER,
			encode_end_round_header(header, /*non_team_form=*/false));
	if (!expect(h.role.runtime->state().end_round.header_known,
			"end round: the header folded")) return false;
	h.role.run_tick(h.input);
	if (!expect(world.match.outcome().ended, "end round: the header latches the round over"))
		return false;
	return expect(world.entity_update_counter == counted + 1,
			"end round: the latched frame holds the entity update");
}

// The frame input packs onto L BEFORE the client net frame builds the C2S
// 0x0C: a key held this frame rides THIS frame's uplink, not the next one
// [orig: Client_ProcessNetworkFrame @0x42C180 — Player_PackInputStateToEntity
//  @0x42C3E9 precedes Player_BuildTag0CInputBody @0x42C46F].
bool run_uplink_carries_same_frame_input() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame: L exists, no key held
	if (!expect(h.role.local_spawned(), "same-frame input: L spawned")) return false;
	h.kernel->local.set_movement_keys(true, false, false, false, false, false, false);
	h.role.run_tick(h.input);
	ProtocolMessage movement;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, movement) &&
					decode_entity_packet_sub_header(movement.payload.data(),
							movement.payload.size(), sub, header_bytes) &&
					decode_player_extended_uplink(movement.payload.data() + header_bytes,
							movement.payload.size() - header_bytes, uplink, body_bytes),
			"same-frame input: the frame shipped a decodable 0x0C"))
		return false;
	return expect((uplink.move_input_byte & w::Entity::kMoveOrderMoving) != 0,
			"the forward key held THIS frame rides this frame's uplink");
}

// The session hands the main loop's frame statistics to the role ahead of the
// drain; the next C2S 0x0C carries their low bytes. [orig: Game_MainLoop
// @0x52B948 / @0x52B98F ahead of the drain @0x52BA08; NetPacket_SerializePlayerState
// case 3 @0x4C1BA2 / @0x4C1BBC]
bool run_uplink_carries_the_frame_statistics() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	if (!expect(h.role.local_spawned(), "frame statistics: L spawned")) return false;
	h.role.observe_frame_rate(0x13A);
	h.role.observe_cpu_share(37);
	h.role.run_tick(h.input);
	ProtocolMessage message;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, message) &&
					decode_entity_packet_sub_header(message.payload.data(),
							message.payload.size(), sub, header_bytes) &&
					decode_player_extended_uplink(message.payload.data() + header_bytes,
							message.payload.size() - header_bytes, uplink, body_bytes),
			"frame statistics: the frame shipped a decodable 0x0C"))
		return false;
	return expect(uplink.stat_byte_0 == 0x3A && uplink.stat_byte_1 == 37,
			"the 0x0C stat bytes carry the frame rate and CPU share low bytes");
}

// Every C2S 0x0C body the role shipped from datagram `from` on, decoded.
std::vector<PlayerExtendedUplink> uplinks_since(const CountingSocket &socket, std::size_t from) {
	std::vector<PlayerExtendedUplink> out;
	for (std::size_t i = from; i < socket.datagrams.size(); ++i) {
		const std::vector<uint8_t> &packet = socket.datagrams[i];
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(packet.data(), packet.size(), opcode, body) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
						header, messages)) continue;
		for (const ProtocolMessage &message : messages) {
			if (message.tag != 0x0C) continue;
			EntityPacketSubHeader sub;
			PlayerExtendedUplink uplink;
			size_t header_bytes = 0, body_bytes = 0;
			if (decode_entity_packet_sub_header(message.payload.data(), message.payload.size(),
						sub, header_bytes) &&
					decode_player_extended_uplink(message.payload.data() + header_bytes,
						message.payload.size() - header_bytes, uplink, body_bytes))
				out.push_back(uplink);
		}
	}
	return out;
}

// Under a dictated send holdoff the input pack rides the uplink's send block:
// the keys held on any frame of a 12-tick window fold into the input-flag word
// and pack once, at the boundary. A jump, a lean and a forward tap between two
// boundaries therefore all reach the boundary 0x0C, and only that one: the next
// window's pack clears what the last one reported. The local MoveOrder is the
// same packed word, so L's own body sees the taps at the boundary too and keeps
// that word until the next pack.
// [orig: Input_ProcessFrame @0x49d541; Client_ProcessNetworkFrame @0x42C3DD ->
//  Player_PackInputStateToEntity @0x42C3E9 (the clear @0x4DF904/@0x4DF909) ->
//  Player_BuildTag0CInputBody @0x42C482]
bool run_holdoff_window_taps_reach_the_boundary_uplink() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	if (!expect(h.role.local_spawned(), "holdoff taps: L spawned")) return false;
	// The host dictates the NovaWorld period: CS dir-0 field 3 = 12.
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> framed;
	if (!expect(frame_session_packet(server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(0x00, {0x01, 0x08, 0x00, 0x00, 0x00, 12, 0x00, 0x00, 0x00},
						0xA0)}, framed), "holdoff taps: the CS update frames")) return false;
	const std::vector<uint8_t> settings =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(framed));
	h.role.runtime->receive(settings.data(), settings.size());

	const auto local_move = [&h]() -> uint8_t {
		const w::Entity *local = h.kernel->local.player();
		return local != nullptr ? local->net_move_input : 0xFFu;
	};
	struct Keys { bool forward, lean_left, jump; };
	// One frame: the keys, the role tick, and the 0x0C bodies it shipped.
	const auto frame = [&h](Keys k) {
		h.kernel->local.set_movement_keys(k.forward, false, false, false, k.lean_left, false,
				k.jump);
		const std::size_t before = h.socket.datagrams.size();
		h.role.run_tick(h.input);
		return uplinks_since(h.socket, before);
	};
	constexpr Keys kNone{false, false, false};
	// B0: the reset counter keeps this first boundary open; it then re-arms
	// from the dictated period.
	std::vector<PlayerExtendedUplink> shipped = frame(kNone);
	if (!expect(shipped.size() == 1 && h.role.runtime->send_holdoff_ticks() == 12,
			"holdoff taps: the first boundary ships one 0x0C under a 12-tick period"))
		return false;
	if (!expect(shipped[0].move_input_byte == 0, "holdoff taps: B0 carries no input"))
		return false;
	// Window 1: jump held on ticks 2..4, lean-left on 6..7, forward on 9 only.
	for (int tick = 1; tick <= 11; ++tick) {
		const Keys keys{tick == 9, tick == 6 || tick == 7, tick >= 2 && tick <= 4};
		shipped = frame(keys);
		if (!expect(shipped.empty(), "holdoff taps: no 0x0C between boundaries")) {
			std::fprintf(stderr, "  window tick %d shipped %zu\n", tick, shipped.size());
			return false;
		}
		if (!expect(local_move() == 0,
				"holdoff taps: the local MoveOrder waits for the boundary pack")) {
			std::fprintf(stderr, "  window tick %d move %02x\n", tick, local_move());
			return false;
		}
	}
	// B1 (tick 12): no key is held on the boundary frame itself.
	shipped = frame(kNone);
	if (!expect(shipped.size() == 1, "holdoff taps: B1 ships one 0x0C")) return false;
	std::printf("holdoff taps: B1 move=%02x local=%02x\n", shipped[0].move_input_byte,
			local_move());
	if (!expect((shipped[0].move_input_byte & w::Entity::kMoveOrderJump) != 0,
			"holdoff taps: the jump tapped on window ticks 2..4 rides the B1 0x0C"))
		return false;
	if (!expect((shipped[0].move_input_byte & w::Entity::kMoveOrderLeanLeft) != 0,
			"holdoff taps: the lean tapped on window ticks 6..7 rides the B1 0x0C"))
		return false;
	if (!expect((shipped[0].move_input_byte & 0x0Fu) == w::Entity::kMoveOrderMoving,
			"holdoff taps: the forward tap on window tick 9 rides B1 as moving, dir 0"))
		return false;
	if (!expect(local_move() == shipped[0].move_input_byte,
			"holdoff taps: L's own MoveOrder is the packed boundary word"))
		return false;
	const uint8_t packed = shipped[0].move_input_byte;
	// Window 2: nothing held. L keeps the B1 word until the next pack.
	for (int tick = 1; tick <= 11; ++tick) {
		shipped = frame(kNone);
		if (!expect(shipped.empty() && local_move() == packed,
				"holdoff taps: the packed word persists through the next window")) {
			std::fprintf(stderr, "  window 2 tick %d shipped %zu move %02x\n", tick,
					shipped.size(), local_move());
			return false;
		}
	}
	shipped = frame(kNone);
	if (!expect(shipped.size() == 1, "holdoff taps: B2 ships one 0x0C")) return false;
	if (!expect((shipped[0].move_input_byte &
						(w::Entity::kMoveOrderJump | w::Entity::kMoveOrderLeanLeft |
								w::Entity::kMoveOrderMoving)) == 0,
			"holdoff taps: B2 no longer carries the window-1 taps"))
		return false;
	return expect(local_move() == shipped[0].move_input_byte,
			"holdoff taps: L's MoveOrder follows the B2 pack");
}

// S2C 0x0F re-snaps L to the authoritative pose it carries (position, yaw,
// pitch, roll and the look yaw) once per decoded 0x0F, and the un-hide runs
// while no death screen is up [orig: NapiNPClientMsg_0x00F @0x42E200 — the
//  pose stores (Pitch @0x42E3E9, Roll @0x42E3F2), `Flags &= ~1`].
bool run_world_state_load_resnaps_local_pose() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr, "0x0F re-snap: L spawned")) return false;
	local->hidden = true;
	// 23-byte header [u32 tick][i32 x][i32 y][i32 z][i16 yaw][i16 pitch][i16 roll][u8 flags],
	// the 128 pool dwords, zero waypoint/location counts.
	std::vector<uint8_t> body(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const auto put_i32 = [&body](size_t at, int32_t v) {
		body[at] = uint8_t(v);
		body[at + 1] = uint8_t(v >> 8);
		body[at + 2] = uint8_t(v >> 16);
		body[at + 3] = uint8_t(v >> 24);
	};
	put_i32(4, 100 << 16);
	put_i32(8, 200 << 16);
	put_i32(12, 10 << 16);
	body[16] = 0x00; body[17] = 0x40; // yaw high word 0x4000 -> BAM 0x40000000
	body[18] = 0x00; body[19] = 0x08; // pitch 0x0800 -> 0x08000000
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::WORLD_STATE_LOAD, body)}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	const w::AiEntity *local_ai = h.kernel->local.player_ai();
	if (!expect(local != nullptr && local_ai != nullptr, "0x0F re-snap: L still resolvable"))
		return false;
	const bool posed = std::fabs(local->position.x - 100.0f) < 0.01f &&
			std::fabs(local->position.y - 200.0f) < 0.01f &&
			std::fabs(local->position.z - 10.0f) < 0.01f;
	if (!expect(posed, "0x0F re-snap: L sits at the wire position")) return false;
	if (!expect(local_ai->heading == 0x40000000 && local_ai->pitch == 0x08000000 &&
					h.kernel->local.input.look_heading == 0x40000000,
			"0x0F re-snap: the body heading, pitch and the look yaw follow the wire"))
		return false;
	return expect(!local->hidden, "0x0F re-snap: the un-hide ran with no death screen up");
}

// The socket seam that also records where each datagram went.
class TargetSocket final : public opennova::IDatagramSocket {
public:
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t size) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + size));
	}
};

// The proxy-assisted join: with all six proxy fields installed the enumerator
// pump ships the 48-byte rendezvous to the game node ahead of the hello, once
// per 3000 ms, and only while the hello is outstanding.
bool run_proxy_rendezvous_pump() {
	Harness h;
	TargetSocket socket;
	h.role.set_socket(&socket, PeerAddr{0x0A00A8C0u, 32768});
	inmatch::JoinerRole::JoinProxyOptions proxy;
	proxy.proxy_node_ip = "10.1.2.3";
	proxy.proxy_node_port = 4000;
	proxy.proxy_cookie = 77;
	proxy.proxy_relay_ip = "192.168.0.9";
	proxy.proxy_relay_port = 32768;
	h.role.set_join_proxy(proxy);
	h.role.poll_preload();
	h.role.poll_preload();
	if (!expect(socket.sent.size() == 2, "proxy: one rendezvous and one hello across two preload frames"))
		return false;
	const PeerAddr node = peer_addr_from_octets({10, 1, 2, 3}, 4000);
	const std::vector<uint8_t> &rendezvous = socket.sent[0].second;
	if (!expect(socket.sent[0].first == node && rendezvous.size() == 48, "proxy: the rendezvous goes to the node first"))
		return false;
	const bool layout = rendezvous[0] == 0 && rendezvous[4] == '@' && rendezvous[5] == 0 &&
			rendezvous[6] == 0x00 && rendezvous[7] == 0x80 &&
			rendezvous[8] == 192 && rendezvous[9] == 168 && rendezvous[10] == 0 && rendezvous[11] == 9 &&
			rendezvous[12] == 77 && rendezvous[13] == 0 && rendezvous[14] == 0 && rendezvous[15] == 0 &&
			rendezvous[47] == 0;
	if (!expect(layout, "proxy: '@' at 4, the relay port at 6, the relay addr at 8, the cookie at 12"))
		return false;
	if (!expect(socket.sent[1].first == PeerAddr{0x0A00A8C0u, 32768}, "proxy: the hello still dials the host"))
		return false;
	// A missing field disables the whole install.
	Harness g;
	TargetSocket quiet;
	g.role.set_socket(&quiet, PeerAddr{});
	proxy.proxy_cookie = 0;
	g.role.set_join_proxy(proxy);
	g.role.poll_preload();
	return expect(quiet.sent.size() == 1, "proxy: no rendezvous without all six fields");
}

} // namespace

int main() {
	if (!run_local_replica_turret_channel()) return 1;
	if (!run_replica_turret_channel()) return 1;
	if (!run_received_loadout_policy_and_sounds()) return 1;
	bool ok = true;
	ok &= run_pre_match_frame();
	ok &= run_preload_frame();
	ok &= run_in_match_spawn_edge();
	ok &= run_in_match_spawn_binds_local_player_definition();
	ok &= run_self_handle_lifecycle();
	ok &= run_spawn_stamps_equipped_adm_from_midframe_grant();
	ok &= run_reset_for_join();
	ok &= run_remote_vehicle_occupancy();
	ok &= run_confirmed_vehicle_drive(0);
	ok &= run_confirmed_vehicle_drive(1);
	ok &= run_confirmed_vehicle_drive(2);
	ok &= run_confirmed_vehicle_drive(3);
	ok &= run_confirmed_vehicle_drive(4);
	ok &= run_confirmed_vehicle_drive(0, true);
	ok &= run_confirmed_vehicle_drive(0, true, true);
	ok &= run_flare_descriptor_carries_the_pilot_handheld();
	ok &= run_guided_fire_preserves_selected_target();
	ok &= run_rules_stamp_from_mp_attributes(0x10000u, true);
	ok &= run_rules_stamp_from_mp_attributes(0x3A02u, false);
	ok &= run_uplink_carries_same_frame_input();
	ok &= run_uplink_carries_the_frame_statistics();
	ok &= run_holdoff_window_taps_reach_the_boundary_uplink();
	ok &= run_end_round_header_holds_the_entity_update();
	ok &= run_world_state_load_resnaps_local_pose();
	ok &= run_proxy_rendezvous_pump();
	if (!ok) return 1;
	std::printf("joiner_role_test: OK\n");
	return 0;
}
