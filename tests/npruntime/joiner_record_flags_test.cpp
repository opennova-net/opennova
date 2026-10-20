// The joiner's own player record Flags (D-NET-528): the 0x0A record naming the
// local player folds its Flags byte into L under 0xE1 in the receive, and the
// same frame's C2S 0x0C writes L's Flags byte raw, so the host's deploy-screen
// bit 0x01 rides back the frame it lands. Bits outside the mask stay L's own.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - `and ebx, 0E1h` @0x4C12FF,
//  the store @0x4C1311, the uplink's raw byte @0x4C1B17; Client_ProcessNetworkFrame
//  @0x42C180, the receive @0x42C228 ahead of the send block @0x42C482]

#include <base/gameprofile/gameprofile.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
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

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);
const std::string kClientScrk = "CLIENT-FLAGS-SCRK";
const std::string kServerScrk = "SERVER-FLAGS-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelf = 0x0005;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

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
	inmatch::JoinerRole role;
	KeepingSocket socket;
	inmatch::TickInput input;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.load_systems();
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_title(&kJo);
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
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
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
// carries the new relation's ground link and the snapped position (D-NET-NEW-L2-13).
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the stage @0x4C0FE4..0x4C0FF0,
//  Entity_TryAttachOrDetach @0x4C1317 (@0x436610), the local snap @0x4C132B..0x4C1343,
//  the ground link @0x4C1346..0x4C1358; Client_ProcessNetworkFrame @0x42C180, the
//  receive @0x42C228 ahead of the send block @0x42C482]
bool run_the_self_record_mount_edge_snaps_l() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
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
	const w::EntityHandle vh = world.registry.spawn(1, vehicle);
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
	if (!expect(self != nullptr && (self->x != pre[0] || self->y != pre[1]),
			"attach edge: the record's sample is not where L stood"))
		return false;
	if (!expect(!ups.empty() && ups.back().carrier_handle == 0xFFFFu &&
					ups.back().pos_x == self->x && ups.back().pos_y == self->y &&
					ups.back().pos_z == self->z,
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
					ups.back().pos_x == self->x && ups.back().pos_y == self->y &&
					ups.back().pos_z == self->z,
			"detach edge: the frame's 0x0C carries the record's world sample, unparented");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_the_self_record_flags_ride_back_the_same_frame();
	ok &= run_the_self_record_mount_edge_snaps_l();
	if (ok) std::printf("joiner_record_flags: ok\n");
	return ok ? 0 : 1;
}
