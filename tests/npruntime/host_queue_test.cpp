// npruntime_host_queue: the host's one send queue per connection (net parity lane L1). Retail
// queues every S2C message on its connection as it is produced (NapiNPServer_SendToConn
// @0x4C4F20 -> CNapiNPConnection_QueueMessage @0x628640) and the connection's send boundary packs
// the queue into packets (CNapiNPConnection_BuildOutgoingPackets @0x628430, @0x62844E,
// @0x62860B..0x628619), from the one in-match send pump (Server_TickUpdate @0x51E487). Each case
// drives the session owner's pump (host_session_pump) over a capture socket:
//   - D-NET-525: replies to datagrams that arrive between two send boundaries share a packet,
//     before the connection joined as after;
//   - D-NET-NEW-L1-6: the previous entity update's sends lead what this frame's receive pump
//     queues, the host's own client's requests included [orig: Game_ProcessMainFrame -
//     Entity_UpdateAllEntities @0x52674B after Server_TickUpdate @0x5266B6; the receive pump
//     @0x51D895];
//   - D-NET-686: the server tick's per-player walk runs before its receive pump, so the walk's
//     sends lead the pump's and a message the pump handles is walked in the next frame [orig:
//     Server_TickUpdate - Server_UpdateAllActivePlayerSlots @0x51D88B, then
//     CNapiNetwork_PumpServerProtocolRecv @0x51D895; the walk's spawn-protection decrement
//     @0x5188B8..0x5188C5, its frontier hint @0x51890C..0x5189A6]; the C2S 0x51 convert runs
//     inline in the pump, its sends ahead of a later message's reply [orig:
//     Server_ProcessClientRequestSpectatorRespawn @0x51C8D5 -> Server_ConvertPlayerToSpectator
//     @0x519E00].

#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
using opennova::ProtocolMessage;

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

struct CaptureDatagramSocket final : opennova::IDatagramSocket {
	struct Incoming {
		opennova::PeerAddr peer;
		std::vector<uint8_t> bytes;
	};
	std::deque<Incoming> incoming;
	std::vector<std::vector<uint8_t>> sent;

	int recv_from(uint8_t *data, std::size_t capacity, opennova::PeerAddr &from) override {
		if (incoming.empty()) return 0;
		Incoming item = std::move(incoming.front());
		incoming.pop_front();
		const std::size_t count = std::min(capacity, item.bytes.size());
		std::copy_n(item.bytes.begin(), count, data);
		from = item.peer;
		return static_cast<int>(count);
	}
	void send_to(const opennova::PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
};

const opennova::PeerAddr kPeer{0x0100007Fu, 33200};
const char kServerScrk[] = "SERVERQUEUESCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
const char kClientScrk[] = "CLIENTQUEUESCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

// A session owner with one remote connection past its admission, its transport attached.
// Heap-held: a HostOwner and a World overflow the 1 MB test-main stack under MSVC.
struct QueueFixture {
	std::unique_ptr<inmatch::HostOwner> owner = std::make_unique<inmatch::HostOwner>();
	std::unique_ptr<opennova::world::World> world;
	CaptureDatagramSocket socket;
	uint32_t next_c2s_seq = 1;

	explicit QueueFixture(bool joined, bool with_world) {
		inmatch::set_connection_mode(owner->ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(owner->ctx, inmatch::SocketMode::Lan);
		inmatch::GameConfig config;
		config.max_players = 8;
		inmatch::SessionStartup startup;
		startup.title = &kJo;
		startup.host_key = 0x01020304u;
		inmatch::create_session(owner->ctx, config, startup, nullptr);
		if (with_world) {
			world = std::make_unique<opennova::world::World>();
			world->registry.configure_pool(0, 8);
			owner->ctx.world = world.get();
		}
		inmatch::PeerLink &link = owner->peers[kPeer];
		link.transport = std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
		inmatch::NapiNPConnection conn;
		conn.peer = kPeer;
		conn.type = inmatch::NapiNPConnection::kTypeServerSide;
		conn.connection_id = inmatch::kFirstJoinerDcb;
		conn.admission_stage = inmatch::GameAdmissionStage::Complete;
		conn.phase = joined ? inmatch::ConnectionPhase::InMatch : inmatch::ConnectionPhase::PlayerAdded;
		conn.burst.spawned = joined;
		conn.server_scrk = kServerScrk;
		conn.client_scrk = kClientScrk;
		conn.server_sk = 0x55667788u;
		conn.client_ck = 0x10203040u;
		conn.link.transport = link.transport.get();
		conn.link.mode = ns::TransportMode::Client;
		conn.seq.outbound_message_limit = opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX;
		owner->ctx.np_protocol.connection_list.push_back(std::move(conn));
	}

	inmatch::NapiNPConnection &conn() { return owner->ctx.np_protocol.connection_list.front(); }

	// C2S messages in one 0x43 datagram.
	void receive_messages(const std::vector<ProtocolMessage> &messages) {
		opennova::ProtocolPacketHeader header;
		header.session_id = 0x55667788u;
		header.seq_num = next_c2s_seq++;
		header.ack_count = 0;
		std::vector<uint8_t> body;
		(void)opennova::encode_protocol_packet_plaintext(header, messages, kClientScrk, body);
		socket.incoming.push_back(
				{kPeer, opennova::nw_encode_outbound(opennova::SESSION_OPCODE_PROTOCOL_MESSAGE,
						std::move(body))});
	}

	// One C2S message in its own 0x43 datagram.
	void receive(uint8_t tag, std::vector<uint8_t> payload) {
		receive_messages({opennova::make_protocol_message(tag, std::move(payload))});
	}

	// One C2S 0x2C RTT request (answered with an S2C 0x57).
	void receive_rtt_request() { receive(0x2C, {1, 2, 3, 4, 1}); }

	// A live player body in pool 0 with its roster row, owned by `c`.
	opennova::world::EntityHandle give_body(inmatch::NapiNPConnection &c, uint8_t team) {
		opennova::world::Entity body;
		body.kind = opennova::world::EntityKind::Organic;
		body.player_class = 8;
		body.team = team;
		body.health = 150;
		body.alive = true;
		const opennova::world::EntityHandle handle = world->registry.spawn(0, body);
		world->match.upsert_player({handle, 0, c.player_name, {}});
		c.link.owned_entity = handle;
		c.assigned_team = team;
		c.assigned_team_valid = true;
		return handle;
	}

	void pump() { inmatch::host_session_pump(*owner, socket); }

	// Index of the first record tagged `tag` in `packet`, or its size when none is.
	static std::size_t index_of(const std::vector<ProtocolMessage> &packet, uint8_t tag) {
		for (std::size_t i = 0; i < packet.size(); ++i)
			if (packet[i].tag == tag) return i;
		return packet.size();
	}

	// The records of every S2C session packet sent so far, one vector per packet.
	std::vector<std::vector<ProtocolMessage>> packets() const {
		std::vector<std::vector<ProtocolMessage>> out;
		for (const std::vector<uint8_t> &datagram : socket.sent) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			if (!opennova::nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) ||
					opcode != opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
				continue;
			opennova::ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (opennova::decode_protocol_packet_plaintext(
						body.data(), body.size(), kServerScrk, header, messages))
				out.push_back(std::move(messages));
		}
		return out;
	}
};

// D-NET-525: a connection that has not joined yet, its send boundary 12 ticks apart. Two
// datagrams land while the boundary is closed; their replies leave together, one packet, when it
// opens. Ours used to frame each datagram's replies as a packet of its own.
bool run_pre_join_replies_share_a_packet() {
	QueueFixture f(/*joined=*/false, /*with_world=*/false);
	inmatch::NapiNPConnection &c = f.conn();
	c.s2c_send_holdoff_dictated = true;
	c.s2c_send_holdoff_ticks = 12;
	c.s2c_send_holdoff_countdown = 12;
	c.s2c_send_boundary_open = false;
	f.receive_rtt_request();
	f.pump();
	f.receive_rtt_request();
	f.pump();
	if (!expect(f.socket.sent.empty(), "nothing leaves while the boundary is closed")) return false;
	for (int i = 0; i < 16 && f.socket.sent.empty(); ++i) f.pump();
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(sent.size() == 1, "the boundary sends one packet")) return false;
	return expect(sent[0].size() == 2 && sent[0][0].tag == 0x57 && sent[0][1].tag == 0x57,
	              "both datagrams' replies share it, in arrival order");
}

// Stages one entity-update send (a death's 0x13) for the fixture's remote, as the previous
// frame's update would have.
void stage_entity_update_send(QueueFixture &f) {
	inmatch::NapiNPServerCtx &ctx = f.owner->ctx;
	inmatch::StagedEntityUpdateBatch batch;
	batch.seq = 7;
	inmatch::StagedEntityUpdateSend send;
	send.connection_id = f.conn().connection_id;
	send.transport = f.conn().link.transport;
	send.tag = 0x13;
	send.body = {0xAA, 0xBB};
	batch.sends.push_back(std::move(send));
	ctx.entity_update_sends.push_back(std::move(batch));
	opennova::world::RoundDeath marker;
	marker.staged_sends = 7;
	f.world->round_sim.deaths.push_back(marker);
}

// D-NET-NEW-L1-6: the previous entity update staged one send for this connection (a death's
// 0x13); this frame's receive pump answers a 0x2C. The update's send leads the reply.
bool run_entity_update_sends_lead_the_receive_pump() {
	QueueFixture f(/*joined=*/true, /*with_world=*/true);
	stage_entity_update_send(f);
	f.receive_rtt_request();
	f.pump();
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(!sent.empty() && sent[0].size() >= 2, "the frame's boundary sends its queue"))
		return false;
	return expect(sent[0][0].tag == 0x13 && sent[0][1].tag == 0x57,
	              "the entity update's 0x13 leads the receive pump's 0x57");
}

// The listen host's own client, in match with a live body: its requests reach the server's
// handlers through the pump's own-client hook.
struct OwnClient {
	QueueFixture *fixture = nullptr;
	std::vector<ProtocolMessage> queued;
};

void drain_own_client(void *context) {
	OwnClient &own = *static_cast<OwnClient *>(context);
	inmatch::NapiNPServerCtx &ctx = own.fixture->owner->ctx;
	inmatch::NapiNPConnection &local = ctx.np_protocol.connection_list.back();
	(void)inmatch::dispatch_session_replies(ctx.config, local, own.queued,
			own.fixture->owner->now_tick, ctx.np_protocol.connection_list, ctx.world);
	own.queued.clear();
}

// D-NET-686: the walk's sends lead the receive pump's. A joined remote with its frontier hint
// armed and 1240 play ticks gets the walk's S2C 0x1E event 58; its C2S 0x2C arrives in the
// same frame. Ours used to run the receive pump first, so its 0x57 led the hint.
bool run_the_walk_leads_the_receive_pump() {
	QueueFixture f(/*joined=*/true, /*with_world=*/true);
	opennova::world::World &world = *f.world;
	world.registry.configure_pool(1, 8);
	world.rules.mp_session = true;
	f.owner->ctx.config.game_type = opennova::game_type::kAdvanceAndSecure;
	opennova::world::MatchRules rules;
	rules.game_type = opennova::game_type::kAdvanceAndSecure;
	world.match.configure(rules);
	const uint8_t teams[3] = {1, 1, 2}; // an A&S chain: zones 1 and 2 blue, 3 red
	for (uint8_t number = 1; number <= 3; ++number) {
		opennova::world::Entity zone;
		zone.kind = opennova::world::EntityKind::Item;
		zone.is_capture_trigger = true;
		zone.is_spawn_point = true;
		zone.zone_number = number;
		zone.zone_radius = 20;
		zone.team = teams[number - 1];
		zone.health = 1;
		zone.alive = true;
		zone.position = {1000.0f * number, 0.0f, 0.0f};
		world.registry.spawn(1, zone);
	}
	world.zones.build_chain_from_mission();
	if (!expect(world.zones.frontier_zone(1) != 0, "the chain gives blue a frontier")) return false;
	const opennova::world::EntityHandle body = f.give_body(f.conn(), 1);
	world.match.player(body)->play_ticks = 1240;
	f.conn().reply.frontier_hint_pending = true;
	f.receive_rtt_request();
	f.pump();
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(!sent.empty(), "the frame's boundary sends its queue")) return false;
	const std::size_t hint = QueueFixture::index_of(sent[0], 0x1E);
	const std::size_t reply = QueueFixture::index_of(sent[0], 0x57);
	return expect(hint < sent[0].size() && reply < sent[0].size() && hint < reply,
	              "the walk's frontier hint leads the receive pump's 0x57");
}

// D-NET-686: a message the receive pump handles is walked in the next frame. The joiner's
// C2S 0x0B runs the join, which stores the 620 spawn protection: the walk ran before the pump,
// so the join's frame keeps the full 620, and the next frame's walk counts it to 619. Ours
// used to walk after the pump and count it down in the join's own frame.
bool run_a_handled_message_is_walked_next_frame() {
	QueueFixture f(/*joined=*/false, /*with_world=*/true);
	opennova::world::World &world = *f.world;
	world.rules.mp_session = true;
	opennova::bms::File mission;
	opennova::mission::make_default(mission);
	f.owner->ctx.mission = &mission;
	inmatch::NapiNPConnection &c = f.conn();
	c.player_name = "Joiner";
	c.reply.player_slot = 1;
	c.slot_state = inmatch::PlayerSlotState::AwaitJoin;
	c.burst.game_state = 9;
	const opennova::world::EntityHandle body = f.give_body(c, 1);
	f.receive(opennova::c2s::MISSION_FILE_STATUS, {0x00});
	f.pump();
	const opennova::world::Entity *player = world.registry.get(body);
	if (!expect(c.slot_state == inmatch::PlayerSlotState::Joined && inmatch::is_in_match(c),
	            "the 0x0B joins the slot in the pump")) return false;
	if (!expect(player != nullptr && player->damage_state == 620,
	            "the join's frame keeps its 620: the walk ran before the pump")) return false;
	f.pump();
	return expect(player->damage_state == 619, "the next frame's walk counts it down");
}

// D-NET-NEW-L1-6 on a listen host: the host player's own requests are handled in the receive
// pump, behind the previous entity update's sends. The update staged a 0x13 for the remote;
// the host player's reload (C2S 0x25) fans an S2C 0x49 to it in this frame. The remote reads
// the 0x13 first; ours used to dispatch the host's requests ahead of the release.
bool run_own_client_requests_follow_the_entity_update_sends() {
	QueueFixture f(/*joined=*/true, /*with_world=*/true);
	f.owner->ctx.np_protocol.connection_list.emplace_back();
	inmatch::NapiNPConnection &local = f.owner->ctx.np_protocol.connection_list.back();
	local.type = inmatch::NapiNPConnection::kTypeClientSide;
	local.player_name = "Host";
	local.phase = inmatch::ConnectionPhase::InMatch;
	local.burst.spawned = true;
	local.link.mode = ns::TransportMode::Loopback;
	const opennova::world::EntityHandle own_body = f.give_body(local, 1);
	(void)f.give_body(f.conn(), 2);
	stage_entity_update_send(f);
	OwnClient own{&f, {}};
	opennova::WeaponReload reload;
	reload.entity_handle = own_body.packed;
	reload.reload_param = 0;
	own.queued.push_back(opennova::make_protocol_message(
			opennova::c2s::WEAPON_RELOAD_REQUEST, opennova::encode_weapon_reload(reload)));
	inmatch::host_session_pump(*f.owner, f.socket, nullptr, nullptr, nullptr, nullptr,
			&drain_own_client, &own);
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(!sent.empty(), "the frame's boundary sends its queue")) return false;
	const std::size_t death = QueueFixture::index_of(sent[0], 0x13);
	const std::size_t relay = QueueFixture::index_of(sent[0], opennova::s2c::WEAPON_RELOAD);
	return expect(death < sent[0].size() && relay < sent[0].size() && death < relay,
	              "the entity update's 0x13 leads the host player's reload relay");
}

// D-NET-686: the C2S 0x51 convert runs inline in the receive pump (retail's handler tail-jumps
// into Server_ConvertPlayerToSpectator), so its sends queue at their calls: the deploy leg's
// 0x5A and 0x61 (Server_DeployPlayer @0x5178F2, @0x51796A), then the 0x32 (@0x519F43), all
// ahead of the reply to the 0x2C that follows the 0x51 in the same datagram. Ours used to
// stage the convert for the tick, after the pump, so the 0x57 led them.
bool run_the_convert_sends_lead_a_later_reply() {
	QueueFixture f(/*joined=*/true, /*with_world=*/true);
	f.world->rules.mp_session = true;
	f.owner->ctx.config.permanent_death = true;
	f.owner->ctx.config.spectator_slots = 1;
	(void)f.give_body(f.conn(), 1);
	f.receive_messages({opennova::make_protocol_message(0x51, {0xFF, 0xFF}),
			opennova::make_protocol_message(0x2C, {1, 2, 3, 4, 1})});
	f.pump();
	if (!expect(f.conn().link.spectator, "the 0x51 converts the slot")) return false;
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(!sent.empty(), "the frame's boundary sends its queue")) return false;
	const std::vector<ProtocolMessage> &packet = sent[0];
	const std::size_t loadout = QueueFixture::index_of(packet, opennova::s2c::WEAPON_LOADOUT);
	const std::size_t seed = QueueFixture::index_of(packet, opennova::s2c::TICK_SEED);
	const std::size_t line = QueueFixture::index_of(packet, opennova::s2c::FORMATTED_GAME_TEXT);
	const std::size_t reply = QueueFixture::index_of(packet, 0x57);
	return expect(loadout < seed && seed < line && line < reply && reply < packet.size(),
	              "the convert's 0x5A, 0x61 and 0x32 lead the later 0x2C's 0x57");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_pre_join_replies_share_a_packet()) ++failures;
	if (!run_entity_update_sends_lead_the_receive_pump()) ++failures;
	if (!run_the_walk_leads_the_receive_pump()) ++failures;
	if (!run_a_handled_message_is_walked_next_frame()) ++failures;
	if (!run_own_client_requests_follow_the_entity_update_sends()) ++failures;
	if (!run_the_convert_sends_lead_a_later_reply()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
