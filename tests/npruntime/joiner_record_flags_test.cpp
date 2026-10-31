// The joiner's own player record Flags (D-NET-528): the 0x0A record naming the
// local player folds its Flags byte into L under 0xE1 in the receive, and the
// same frame's C2S 0x0C writes L's Flags byte raw, so the host's deploy-screen
// bit 0x01 rides back the frame it lands. Bits outside the mask stay L's own.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - `and ebx, 0E1h` @0x4C12FF,
//  the store @0x4C1311, the uplink's raw byte @0x4C1B17; Client_ProcessNetworkFrame
//  @0x42C180, the receive @0x42C228 ahead of the send block @0x42C482]

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/replica_twin.h"
#include "../common/test_expect.h"
#include "../common/title_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

const Protocol &kJo = *title_protocol("jo");
const std::string kClientScrk = "CLIENT-FLAGS-SCRK";
const std::string kServerScrk = "SERVER-FLAGS-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelf = 0x0005;

using test_expect::expect;

// The socket seam: keeps what the role ships, receives nothing.
class KeepingSocket final : public IDatagramSocket {
public:
	std::vector<std::vector<uint8_t>> datagrams;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t size) override {
		datagrams.emplace_back(data, data + size);
	}
};

// Every C2S 0x0C body shipped from datagram `from` on, decoded.
std::vector<PlayerExtendedUplink> uplinks_since(const KeepingSocket &socket, std::size_t from) {
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
			if (message.tag != c2s::ENTITY_UPLINK) continue;
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

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	bool kernel_jo = test_titles::jo_world(kernel->world);
	inmatch::JoinerRole role;
	bool role_jo = test_titles::jo_joiner(role);
	KeepingSocket socket;
	inmatch::TickInput input;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.load_systems();
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.set_rules(opennova::world::title_rules("jo"));
		role.create_runtime("FlagsJoiner", inmatch::JoinRole::Player, "", "");
	}

	// One 0x0A carrying the tail and the self player record with `flags`, then
	// the frame that receives it.
	void frame(uint8_t flags) {
		PlayerCompactRecord player;
		player.carrier_handle = 0xFFFF;
		player.state_flags = flags;
		frame(player);
	}
	void frame(const PlayerCompactRecord &player) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = 100;
		FrameUpdateRecord rec;
		rec.handle = kSelf;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player = player;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		role.runtime->receive(datagram.data(), datagram.size());
		role.run_tick(input);
	}
};

bool run_the_self_record_flags_ride_back_the_same_frame() {
	Harness h;
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "L spawned")) return false;
	auto &self_row = h.role.runtime->state().upsert(kSelf);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	h.frame(0x00); // a first alive sample: no fold moves anything
	if (!expect(h.kernel->local.player() != nullptr && h.kernel->local.player()->alive,
			"L alive after the first record"))
		return false;

	// The deploy hold: the host's copy carries 0x01 (net-re section 5.10).
	std::size_t from = h.socket.datagrams.size();
	h.frame(0x01);
	std::vector<PlayerExtendedUplink> ups = uplinks_since(h.socket, from);
	if (!expect(!ups.empty(), "deploy hold: the frame shipped a C2S 0x0C")) return false;
	if (!expect((ups.back().state_flags_byte & 0x01u) != 0,
			"deploy hold: the record's bit 0x01 rides the same frame's uplink"))
		return false;
	const w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && (local->flags & 0x01u) != 0 &&
					(local->engine_flags & 0x01u) != 0,
			"deploy hold: L's Flags took bit 0x01"))
		return false;

	// Bits outside 0xE1 stay L's own: a wire 0xF9 lands 0xE1 and leaves 0x18
	// (binoculars, scope) to L, here clear.
	from = h.socket.datagrams.size();
	h.frame(0xF9);
	ups = uplinks_since(h.socket, from);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && (local->flags & 0xFFu) == 0xE1u,
			"mask: L's low Flags byte is the record's 0xE1 bits alone"))
		return false;
	if (!expect(!ups.empty() && ups.back().state_flags_byte == 0xE1u,
			"mask: the uplink echoes L's byte raw"))
		return false;

	// The host clears them: the next record clears L's.
	from = h.socket.datagrams.size();
	h.frame(0x00);
	ups = uplinks_since(h.socket, from);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && (local->flags & 0xE1u) == 0 &&
					(local->engine_flags & 0xE1u) == 0,
			"clear: the record cleared L's masked bits"))
		return false;
	return expect(!ups.empty() && ups.back().state_flags_byte == 0x00u,
			"clear: the uplink reports 0");
}

// The record's mount edge runs in the receive too: the attach-or-detach, then the
// local player's snap to the record's world sample, so the same frame's C2S 0x0C
// carries the new relation's ground link and the snapped position (D-NET-839).
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the stage @0x4C0FE4..0x4C0FF0,
//  Entity_TryAttachOrDetach @0x4C1317 (@0x436610), the local snap @0x4C132B..0x4C1343,
//  the ground link @0x4C1346..0x4C1358; Client_ProcessNetworkFrame @0x42C180, the
//  receive @0x42C228 ahead of the send block @0x42C482]
bool run_the_self_record_mount_edge_snaps_l() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "mount edge: L spawned")) return false;
	auto &self_row = h.role.runtime->state().upsert(kSelf);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	h.frame(0x00);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && local->alive, "mount edge: L alive")) return false;
	// A vehicle 6 u off L with a controller seat on bone 1, and its replica row.
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 1291;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	vehicle.item_attrib = 0x40u;
	vehicle.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
	vehicle.spawn_origin = (1u << 24) | 3u;
	vehicle.position = local->position;
	vehicle.position.x += 6.0f;
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat controller;
	controller.type = w::SeatType::Controller;
	controller.bone_index = 1;
	controller.retail_slot = 8;
	controller.source_name = "ctrlx00";
	vehicle.seats.push_back(controller);
	const w::EntityHandle vh = test_common::materialize_twin(h.role.materializer(),
			h.role.runtime->state(), world, 1, vehicle, EntityClass::Vehicle);
	auto &row = h.role.runtime->state().upsert(vh.packed);
	row.type_id = static_cast<uint16_t>(vehicle.item_id);
	row.cls = EntityClass::Vehicle;
	row.x = w::to_fixed(vehicle.position.x);
	row.y = w::to_fixed(vehicle.position.y);
	row.z = w::to_fixed(vehicle.position.z);
	row.heading_known = true;
	row.net_has_compact = true;
	row.compact_revision = 1;
	const int32_t pre[3] = {local->position.x.q16, local->position.y.q16,
			local->position.z.q16};

	// The attach edge: the record seats L on bone 1 at a seat-local offset.
	PlayerCompactRecord seated;
	seated.carrier_handle = vh.packed;
	seated.vehicle_bone = 1;
	seated.state_flags = static_cast<uint8_t>(w::kEntityFlagMounted);
	seated.pos_x_compressed = 0x0010;
	seated.pos_y_compressed = 0x0008;
	std::size_t from = h.socket.datagrams.size();
	h.frame(seated);
	std::vector<PlayerExtendedUplink> ups = uplinks_since(h.socket, from);
	const replication::ClientEntityState *self = h.role.runtime->state().find(kSelf);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && local->mounted && local->mount_target == vh,
			"attach edge: the record's seat attaches L"))
		return false;
	if (!expect(self != nullptr &&
					(self->record_sample[0] != pre[0] || self->record_sample[1] != pre[1]),
			"attach edge: the record's sample is not where L stood"))
		return false;
	if (!expect(!ups.empty() && ups.back().carrier_handle == 0xFFFFu &&
					ups.back().pos_x == self->record_sample[0] &&
					ups.back().pos_y == self->record_sample[1] &&
					ups.back().pos_z == self->record_sample[2],
			"attach edge: the frame's 0x0C carries the record's world sample under the "
			"seat's ground link (none on open ground)"))
		return false;

	// The detach edge: the record leaves the seat at a world sample of its own.
	PlayerCompactRecord standing;
	standing.carrier_handle = 0xFFFF;
	standing.pos_x_compressed = 0x0040;
	standing.pos_y_compressed = 0x0020;
	from = h.socket.datagrams.size();
	h.frame(standing);
	ups = uplinks_since(h.socket, from);
	self = h.role.runtime->state().find(kSelf);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && !local->mounted,
			"detach edge: the record's empty seat detaches L"))
		return false;
	return expect(!ups.empty() && ups.back().carrier_handle == 0xFFFFu &&
					ups.back().pos_x == self->record_sample[0] &&
					ups.back().pos_y == self->record_sample[1] &&
					ups.back().pos_z == self->record_sample[2],
			"detach edge: the frame's 0x0C carries the record's world sample, unparented");
}

// The self record's echo stages L's chase, which L's own update walks off the
// authority (D-NET-840): a record 0.07 units off moves nothing (the deadband),
// one exactly 2 units off steps by (256 + 0x20000) / 512 = 256 while L stands still,
// one over 2 units snaps L's position to it.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the stage @0x4C0FD7..0x4C0FFC;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the local path @0x4B4279..0x4B42A2, the
//  snap @0x4B4332..0x4B434A, 512 @0x4B449E]
bool run_the_self_record_echo_chases_l() {
	Harness h;
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "echo: L spawned")) return false;
	auto &self_row = h.role.runtime->state().upsert(kSelf);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	h.frame(0x00);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && local->alive, "echo: L alive")) return false;
	// A record at L's x plus `dx` (y, z on L's), then the frame.
	auto echo = [&](int32_t dx) {
		local = h.kernel->local.player();
		PlayerCompactRecord rec;
		rec.carrier_handle = 0xFFFF;
		rec.pos_x_compressed = network_compress_fixedpoint(local->position.x.q16 + dx);
		rec.pos_y_compressed = network_compress_fixedpoint(local->position.y.q16);
		rec.pos_z_compressed = network_compress_fixedpoint(local->position.z.q16);
		const int32_t before = local->position.x.q16;
		h.frame(rec);
		local = h.kernel->local.player();
		return local->position.x.q16 - before;
	};
	const int32_t held = echo(0x11EB);
	if (!expect(held == 0, "echo: 0.07 units off, the deadband holds L")) return false;
	const int32_t stepped = echo(0x20000);
	if (!expect(stepped == 256, "echo: 2 units off, L steps by its still bucket")) return false;
	// The step lands in the frame's entity update, after its client net frame built the
	// 0x0C, so the NEXT frame's C2S 0x0C carries the chased x [orig: Game_ProcessMainFrame
	// @0x5263F0 - Client_ProcessNetworkFrame @0x526692 ahead of Entity_UpdateAllEntities
	// @0x52674B; Client_ProcessNetworkFrame @0x42C180 - Player_BuildTag0CInputBody @0x42A550
	// (the call @0x42C482)].
	{
		local = h.kernel->local.player();
		const int32_t chased_x = local->position.x.q16;
		PlayerCompactRecord again;
		again.carrier_handle = 0xFFFF;
		again.pos_x_compressed = network_compress_fixedpoint(chased_x - 256 + 0x20000);
		again.pos_y_compressed = network_compress_fixedpoint(local->position.y.q16);
		again.pos_z_compressed = network_compress_fixedpoint(local->position.z.q16);
		const std::size_t from = h.socket.datagrams.size();
		h.frame(again);
		const std::vector<PlayerExtendedUplink> ups = uplinks_since(h.socket, from);
		if (!expect(!ups.empty() && ups.back().pos_x == chased_x,
				"echo: the next frame's 0x0C carries the chased x"))
			return false;
	}
	const int32_t snapped = echo(0x30000);
	const replication::ClientEntityState *self = h.role.runtime->state().find(kSelf);
	return expect(self != nullptr && local->position.x.q16 == self->record_sample[0] &&
					snapped > 0x20000,
			"echo: over 2 units off, L snaps to the record's sample");
}

// D-NET-526: the frame that bears L ships its first C2S 0x0C. L is born in the net frame's
// after-receive leg, ahead of the send block, as retail's handlers write the local player
// inside the receive (it exists from the mission start); before, the edge frame's builder
// found no L and the first 0x0C left a boundary later.
// [orig: Client_ProcessNetworkFrame @0x42C180 - the receive @0x42C228 ahead of the send
//  block @0x42C3DD, the build from g_LocalPlayerEntity @0x42C46F..0x42C482;
//  Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC]
bool run_the_edge_frame_ships_its_first_uplink() {
	Harness h;
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	const std::size_t from = h.socket.datagrams.size();
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "edge frame: L spawned")) return false;
	return expect(uplinks_since(h.socket, from).size() == 1,
			"edge frame: the frame that bears L ships its first 0x0C");
}

// D-NET-863: the self record of a body standing on a building names the building
// with seat bone 0 (the retail host echoes 0x2011 so on every record after a respawn into
// Jungle Hut 2), and its fold lands that carrier as L's ground link: a bone-0 floor is
// the ground link too, not only a seat's carrier. Between the idle resolves, whose skip
// band runs no ground probe, nothing else writes the link, so the frame's C2S 0x0C names
// the floor, the next frame keeps it, and only an 0xFFFF record nulls it.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the carrier word @0x4C0E05..0x4C0E08,
//  its resolve into ebp @0x4C0F9C..0x4C106D (0 @0x4C0FAB for 0xFFFF), the store @0x4C1358;
//  Entity_MovementCollisionResolver @0x4B2BD0 - the skip band's return @0x4B2CBC..0x4B2CF5]
bool run_the_self_record_floor_holds_l_ground_link() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(2, 32);
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "floor: L spawned")) return false;
	auto &self_row = h.role.runtime->state().upsert(kSelf);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	h.frame(0x00);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && local->alive, "floor: L alive")) return false;
	// A building under L (its origin at L's feet), placed, and its replica row.
	w::Entity floor;
	floor.kind = w::EntityKind::Building;
	floor.item_id = 0x04B3;
	floor.has_item_def = true;
	floor.spawn_origin = w::spawn_origin_pack(static_cast<uint32_t>(w::EntityKind::Building), 0);
	floor.position = local->position;
	floor.bound_radius = 8.0f;
	const w::EntityHandle fh = test_common::materialize_twin(h.role.materializer(),
			h.role.runtime->state(), world, 2, floor, EntityClass::NoNetworkCallback);
	if (!expect(fh.valid() && fh.pool() == 2, "floor: the building took a pool-2 slot"))
		return false;
	auto &row = h.role.runtime->state().upsert(fh.packed);
	row.type_id = static_cast<uint16_t>(floor.item_id);
	row.cls = EntityClass::NoNetworkCallback;
	row.x = floor.position.x.q16;
	row.y = floor.position.y.q16;
	row.z = floor.position.z.q16;
	row.heading_known = true;

	// The standing record: the building at bone 0, L at its origin.
	PlayerCompactRecord standing;
	standing.carrier_handle = fh.packed;
	standing.vehicle_bone = 0;
	std::size_t from = h.socket.datagrams.size();
	h.frame(standing);
	std::vector<PlayerExtendedUplink> ups = uplinks_since(h.socket, from);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && !local->mounted && local->ground_target == fh,
			"floor: the bone-0 record's building is L's ground link, L unmounted"))
		return false;
	if (!expect(!ups.empty() && ups.back().carrier_handle == fh.packed,
			"floor: the frame's 0x0C names the building"))
		return false;
	// A frame with no record: nothing re-stores the link, and it holds.
	from = h.socket.datagrams.size();
	h.role.run_tick(h.input);
	ups = uplinks_since(h.socket, from);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && local->ground_target == fh,
			"floor: a frame with no record keeps the link"))
		return false;
	if (!expect(ups.empty() || ups.back().carrier_handle == fh.packed,
			"floor: a 0x0C of that frame still names the building"))
		return false;
	// The next record again: the fold re-stores the same link.
	h.frame(standing);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && local->ground_target == fh,
			"floor: the next record keeps the link"))
		return false;
	// The host's 0xFFFF echo nulls it.
	PlayerCompactRecord open;
	open.carrier_handle = 0xFFFF;
	open.pos_x_compressed = network_compress_fixedpoint(local->position.x.q16);
	open.pos_y_compressed = network_compress_fixedpoint(local->position.y.q16);
	open.pos_z_compressed = network_compress_fixedpoint(local->position.z.q16);
	h.frame(open);
	local = h.kernel->local.player();
	return expect(local != nullptr && !local->ground_target.valid(),
			"floor: an 0xFFFF record nulls the link");
}

} // namespace

// B-100: a self row built anew (a 0x0C's memset, a 0x18 rebuild, a reload's fresh state) counts
// its compact revision from 0 again, so its first record carries the revision the old row's first
// record did: the fold keys on the row's lifetime too, and the record's Flags land on L.
bool run_a_rebuilt_self_row_folds_its_first_record() {
	Harness h;
	h.role.poll_preload();
	h.role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "rebuilt row: L spawned")) return false;
	auto &self_row = h.role.runtime->state().upsert(kSelf);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	h.frame(0x00);
	const replication::ClientEntityState *before = h.role.runtime->state().find(kSelf);
	if (!expect(before != nullptr && before->compact_revision == 1,
			"rebuilt row: the old row's first record folds at revision 1"))
		return false;
	const uint32_t lifetime = before->spawn_revision;
	replication::ClientEntityState &rebuilt = h.role.runtime->state().upsert(kSelf);
	rebuilt = replication::ClientEntityState{};
	rebuilt.handle = kSelf;
	rebuilt.cls = EntityClass::Player;
	rebuilt.type_id = w::kPlayerInfantryTypeId;
	rebuilt.spawn_revision = lifetime + 1;
	h.frame(0x01); // the new row's first record: revision 1 again
	const w::Entity *local = h.kernel->local.player();
	return expect(local != nullptr && (local->flags & 0x01u) != 0,
			"rebuilt row: its first record's Flags land on L");
}

int main() {
	bool ok = true;
	ok &= run_the_edge_frame_ships_its_first_uplink();
	ok &= run_the_self_record_flags_ride_back_the_same_frame();
	ok &= run_the_self_record_mount_edge_snaps_l();
	ok &= run_the_self_record_echo_chases_l();
	ok &= run_the_self_record_floor_holds_l_ground_link();
	ok &= run_a_rebuilt_self_row_folds_its_first_record();
	if (ok) std::printf("joiner_record_flags: ok\n");
	return ok ? 0 : 1;
}
