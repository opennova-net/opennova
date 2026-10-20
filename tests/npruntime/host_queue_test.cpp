// npruntime_host_queue: the host's one send queue per connection (net parity lane L1). Retail
// queues every S2C message on its connection as it is produced (NapiNPServer_SendToConn
// @0x4C4F20 -> CNapiNPConnection_QueueMessage @0x628640) and the connection's send boundary packs
// the queue into packets (CNapiNPConnection_BuildOutgoingPackets @0x628430, @0x62844E,
// @0x62860B..0x628619), from the one in-match send pump (Server_TickUpdate @0x51E487). Each case
// drives the session owner's pump (host_session_pump) over a capture socket:
//   - D-NET-525: replies to datagrams that arrive between two send boundaries share a packet,
//     before the connection joined as after;
//   - D-NET-NEW-L1-6: the previous entity update's sends lead what this frame's receive pump
//     queues [orig: Game_ProcessMainFrame - Entity_UpdateAllEntities @0x52674B after
//     Server_TickUpdate @0x5266B6; the receive pump @0x51D895].

#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <base/gameprofile/gameprofile.h>
#include <net/npwire/idatagram_socket.h>
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

	// One C2S 0x2C RTT request (answered with an S2C 0x57) in its own 0x43 datagram.
	void receive_rtt_request() {
		opennova::ProtocolPacketHeader header;
		header.session_id = 0x55667788u;
		header.seq_num = next_c2s_seq++;
		header.ack_count = 0;
		std::vector<uint8_t> body;
		(void)opennova::encode_protocol_packet_plaintext(header,
				{opennova::make_protocol_message(0x2C, {1, 2, 3, 4, 1})}, kClientScrk, body);
		socket.incoming.push_back(
				{kPeer, opennova::nw_encode_outbound(opennova::SESSION_OPCODE_PROTOCOL_MESSAGE,
						std::move(body))});
	}

	void pump() { inmatch::host_session_pump(*owner, socket); }

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

// D-NET-NEW-L1-6: the previous entity update staged one send for this connection (a death's
// 0x13); this frame's receive pump answers a 0x2C. The update's send leads the reply.
bool run_entity_update_sends_lead_the_receive_pump() {
	QueueFixture f(/*joined=*/true, /*with_world=*/true);
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
	f.receive_rtt_request();
	f.pump();
	const std::vector<std::vector<ProtocolMessage>> sent = f.packets();
	if (!expect(!sent.empty() && sent[0].size() >= 2, "the frame's boundary sends its queue"))
		return false;
	return expect(sent[0][0].tag == 0x13 && sent[0][1].tag == 0x57,
	              "the entity update's 0x13 leads the receive pump's 0x57");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_pre_join_replies_share_a_packet()) ++failures;
	if (!run_entity_update_sends_lead_the_receive_pump()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
