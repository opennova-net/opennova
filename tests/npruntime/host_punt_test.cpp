// The host-initiated close and the two anti-cheat CRC challenges a stock host streams at a joiner.
//
// A live retail 1.7.5.7 co-op host closed an OpenNova joiner parked at the deploy screen after six
// minutes by sending ONE connection-description record (settings flag + tag 3), then went silent.
// The captured 80-byte body is pinned here byte for byte, driven through the joiner's real receive
// path, and asserted to raise session loss once with the decoded reason — while ordinary
// settings-update traffic keeps its behavior. The same session showed 37x S2C 0x30 and 36x S2C 0x31
// challenges; answering them with a value we cannot honestly compute is what a later live run proved
// fatal (C2S 0x20 "PUNT WCRC" / C2S 0x21 "PUNT ACRC"), so the SILENCE is pinned here
// instead (D-NET-181).

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>

#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

constexpr uint32_t kClientKey = 0x11223344u;
constexpr uint32_t kServerKey = 0x55667788u;
const std::string kClientScrk = "CLIENT-PUNT-SCRK";
const std::string kServerScrk = "SERVER-PUNT-SCRK";

// The flag byte a retail host stamps on this record: the high/settings bit plus LEN8 for a body
// under 256 bytes. [orig: NapiNPMessage_Create(msg_id 3, msg_class 1) @0x627fc0 —
// len_field_size 3 @0x628316]
constexpr uint8_t kDescriptionFlags = 0xA0;

// The kick exactly as captured off the wire (nw_pp prints it as tag=0x1103): a flat run of
// NAME 0x00 [u16 LE size] [size bytes]. DSTR "t35" is sprintf("t%d", 35) and DPC 33 / DDSTR
// "LogPuntEvent" are the two literals of the punt helper, so this body identifies its sender
// exactly. [orig: Server_LogCRCMismatchPunt @0x517ed0 (the "t%d" format @0x7cfb9c, the 33 and
// "LogPuntEvent" arguments @0x517f5a); the mismatchType 35 call site is the six-minute
// deploy-screen idle timeout Server_TickUpdate @0x51e109 (0x57E40 ms) -> @0x51e13a]
const std::vector<uint8_t> kCapturedPunt = {
	0x44, 0x53, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x44, 0x43, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x44, 0x50, 0x31, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x44, 0x50, 0x32, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x44, 0x53, 0x54, 0x52, 0x00, 0x04, 0x00, 0x74, 0x33, 0x35, 0x00,
	0x44, 0x50, 0x43, 0x00, 0x04, 0x00, 0x21, 0x00, 0x00, 0x00,
	0x44, 0x44, 0x53, 0x54, 0x52, 0x00, 0x0D, 0x00,
	0x4C, 0x6F, 0x67, 0x50, 0x75, 0x6E, 0x74, 0x45, 0x76, 0x65, 0x6E, 0x74, 0x00,
};

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void seed_joiner(inmatch::JoinerConnection &joiner) {
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, 0x0001, 0x14B9);
}

std::vector<uint8_t> frame_s2c(SessionSequencing &sequencing,
		std::vector<ProtocolMessage> messages,
		uint32_t client_key = kClientKey) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(sequencing, SessionCrypto{kServerScrk, {}, client_key},
	                          messages, body)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

bool drive_to_initial_server_settings(
		inmatch::JoinerConnection &joiner, uint32_t &client_key_out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello client_hello;
	const std::vector<uint8_t> hello = joiner.start();
	if (!expect(nw_decode_inbound(
				hello.data(), hello.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), client_hello),
			"decode the close-race ClientHello"))
		return false;

	ServerHello server_hello = build_server_hello(
			client_hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1,
			"ServerHello emits the close-race ClientAuth"))
		return false;

	ClientAuth client_auth;
	body.clear();
	if (!expect(nw_decode_inbound(
				hello_result.outbound[0].data(), hello_result.outbound[0].size(),
				opcode, body) && opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"decode the close-race ClientAuth"))
		return false;
	client_key_out = client_auth.ck;

	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, kServerScrk,
			"", "", "", false);
	server_auth.mi = 3;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const inmatch::JoinerConnection::PollResult auth_result = joiner.handle_datagram(
			server_auth_datagram.data(), server_auth_datagram.size());
	return expect(auth_result.outbound.empty() &&
				joiner.phase() == inmatch::JoinerConnection::Phase::Driving,
			"ServerAuth waits at the initial-settings boundary");
}

std::vector<uint8_t> frame_c2s(SessionSequencing &sequencing,
		std::vector<ProtocolMessage> messages) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(sequencing, SessionCrypto{kClientScrk, {}, kServerKey},
	                          messages, body)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

ClientAuth make_retail_client_auth(
		uint32_t client_index, uint32_t client_key, uint32_t host_key) {
	ClientAuth auth = make_jointoperations_client_auth(
			client_index, client_key, host_key, "ClosedPeer", kClientScrk);
	auto add_environment = [&](const char *name, const char *value) {
		auth.cu.push_back(make_client_cu_chunk(2, name, value));
	};
	add_environment("BT", "0");
	add_environment("VN", "2");
	add_environment("BN", "1");
	add_environment("DB", "0");
	add_environment("MBN", "20042002");
	add_environment("SOPD", "180");
	add_environment("VERSIONSTRING", "V1.7.5.7");
	add_environment("COUNTRYCODE", "us");
	add_environment("TZB", "300");
	add_environment("MPS", "1300");
	return auth;
}

// One structural CS control record — the initial settings form (low tag 0, high flag set, so full
// tag 0x100): [direction][u32 field mask][one u32 per set bit].
ProtocolMessage make_cs_config(uint8_t direction, uint8_t field, uint32_t value) {
	std::vector<uint8_t> payload{direction};
	const uint32_t mask = uint32_t{1} << field;
	for (int shift = 0; shift < 32; shift += 8)
		payload.push_back(static_cast<uint8_t>((mask >> shift) & 0xFFu));
	for (int shift = 0; shift < 32; shift += 8)
		payload.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
	return make_protocol_message(0x00, std::move(payload), kDescriptionFlags);
}

void append_tlv(std::vector<uint8_t> &body, const char *name,
		const std::vector<uint8_t> &value) {
	for (const char *p = name; *p; ++p) body.push_back(static_cast<uint8_t>(*p));
	body.push_back(0);
	body.push_back(static_cast<uint8_t>(value.size() & 0xFFu));
	body.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xFFu));
	body.insert(body.end(), value.begin(), value.end());
}

std::vector<uint8_t> u32_value(uint32_t value) {
	return {static_cast<uint8_t>(value & 0xFFu),
	        static_cast<uint8_t>((value >> 8) & 0xFFu),
	        static_cast<uint8_t>((value >> 16) & 0xFFu),
	        static_cast<uint8_t>((value >> 24) & 0xFFu)};
}

struct CaptureDatagramSocket final : opennova::IDatagramSocket {
	std::vector<std::vector<uint8_t>> sent;
	std::vector<PeerAddr> sent_to;

	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &to, const uint8_t *data,
			std::size_t len) override {
		sent_to.push_back(to);
		sent.emplace_back(data, data + len);
	}
};

bool check_client_goodbye_burst(
		const std::vector<std::vector<uint8_t>> &burst,
		uint32_t server_key, const char *message) {
	if (!expect(burst.size() == 4, message)) return false;
	const std::vector<uint8_t> expected_body = client_goodbye_to_bytes(server_key);
	for (std::size_t i = 0; i < burst.size(); ++i) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(
					burst[i].data(), burst[i].size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_GOODBYE &&
					body == expected_body && burst[i] == burst.front(),
			"every automatic goodbye is the identical keyed 0x46 datagram"))
			return false;
	}
	return true;
}

bool check_captured_punt_decodes_field_for_field() {
	DisconnectEvent event;
	if (!expect(kCapturedPunt.size() == 80,
	            "the captured punt body is the 80 bytes the host sent"))
		return false;
	if (!expect(parse_disconnect_event(
			kCapturedPunt.data(), kCapturedPunt.size(), event),
	            "the captured punt body parses as a disconnect block"))
		return false;
	if (!expect(event.ds == 1 && event.dc == 2 && event.dp1 == 0 && event.dp2 == 0 &&
	                      event.dstr == "t35" && event.dpc == 33 &&
	                      event.ddstr == "LogPuntEvent",
	              "every captured field decodes to its witnessed value"))
		return false;
	return expect(connection_description_to_bytes(event) == kCapturedPunt,
			"the host serializer reproduces all 80 captured description bytes");
}

bool check_decode_is_order_and_shape_robust() {
	// Reverse order with an unknown name wedged in the middle: retail keys on the name and skips
	// what it does not know by its length, so the same values must come back.
	std::vector<uint8_t> shuffled;
	append_tlv(shuffled, "DDSTR", {'L', 'o', 'g', 'P', 'u', 'n', 't', 'E', 'v', 'e', 'n', 't', 0});
	append_tlv(shuffled, "DPC", u32_value(33));
	append_tlv(shuffled, "ZZZZ", {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11});
	append_tlv(shuffled, "DSTR", {'t', '3', '5', 0});
	append_tlv(shuffled, "DP2", u32_value(0));
	append_tlv(shuffled, "DP1", u32_value(0));
	append_tlv(shuffled, "DC", u32_value(2));
	append_tlv(shuffled, "DS", u32_value(1));
	DisconnectEvent shuffled_event;
	if (!expect(parse_disconnect_event(
			shuffled.data(), shuffled.size(), shuffled_event) &&
	                    shuffled_event.ds == 1 && shuffled_event.dc == 2 &&
	                    shuffled_event.dstr == "t35" && shuffled_event.dpc == 33 &&
	                    shuffled_event.ddstr == "LogPuntEvent",
	            "field order is not assumed and an unknown name is skipped by its length"))
		return false;

	// An empty name terminates the walk before its length is read: everything after it is gone.
	std::vector<uint8_t> terminated;
	append_tlv(terminated, "DC", u32_value(2));
	terminated.push_back(0);
	append_tlv(terminated, "DPC", u32_value(33));
	DisconnectEvent terminated_event;
	if (!expect(parse_disconnect_event(
			terminated.data(), terminated.size(), terminated_event) &&
	                    terminated_event.dc == 2 && terminated_event.dpc == 0,
	            "an empty name ends the walk and later fields are not read"))
		return false;

	// Every truncation of the captured body must be rejected or decoded, never read past the end.
	for (std::size_t length = 0; length < kCapturedPunt.size(); ++length) {
		const std::vector<uint8_t> truncated(
				kCapturedPunt.begin(), kCapturedPunt.begin() + static_cast<long>(length));
		DisconnectEvent truncated_event;
		parse_disconnect_event(truncated.data(), truncated.size(), truncated_event);
	}
	// A body whose final TLV claims more bytes than remain is malformed, not a short read.
	std::vector<uint8_t> overrun;
	append_tlv(overrun, "DC", u32_value(2));
	overrun.push_back('D');
	overrun.push_back('P');
	overrun.push_back('C');
	overrun.push_back(0);
	overrun.push_back(0x40);
	overrun.push_back(0x00);
	overrun.push_back(0x01);
	DisconnectEvent overrun_event;
	if (!expect(!parse_disconnect_event(overrun.data(), overrun.size(), overrun_event),
	            "a value that claims more bytes than remain is rejected"))
		return false;

	// A well-formed TLV run with none of the seven names is not a disconnect block — the shape
	// gate the dispatcher relies on to leave other settings traffic alone.
	std::vector<uint8_t> foreign;
	append_tlv(foreign, "XX", u32_value(7));
	DisconnectEvent foreign_event;
	return expect(!parse_disconnect_event(foreign.data(), foreign.size(), foreign_event),
	              "a TLV run carrying no known field is not a disconnect block");
}

bool check_captured_punt_closes_a_joiner_parked_at_the_deploy_screen() {
	inmatch::JoinerConnection joiner("PuntedPlayer");
	seed_joiner(joiner);
	// The captured situation: an established joiner sitting on the deploy screen, owing a pick.
	if (!expect(joiner.begin_redeployment() && joiner.deployment_pick_pending(),
	            "the joiner is parked at the deploy screen before the kick"))
		return false;

	SessionSequencing server_tx{1, 0};
	const std::vector<uint8_t> kick = frame_s2c(server_tx,
			{make_protocol_message(0x03, kCapturedPunt, kDescriptionFlags)});
	if (!expect(!kick.empty(), "frame the captured kick as an S2C packet")) return false;

	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(kick.data(), kick.size());
	if (!expect(result.queued_send_messages.empty() &&
				check_client_goodbye_burst(result.outbound, kServerKey,
						"a host description triggers retail's four-packet goodbye burst"),
			"a closed session emits only its transport-level goodbye burst"))
		return false;
	const std::string reason = joiner.session_loss_reason();
	if (!expect(joiner.session_lost() &&
	                    reason.find("33") != std::string::npos &&
	                    reason.find("LogPuntEvent") != std::string::npos &&
	                    reason.find("t35") != std::string::npos,
	            "session loss is raised carrying the decoded reason code and strings"))
		return false;
	if (!expect(joiner.has_disconnect_event() &&
	                    joiner.last_disconnect_event().dc == 2 &&
	                    joiner.last_disconnect_event().dpc == 33 &&
	                    joiner.last_disconnect_event().ddstr == "LogPuntEvent" &&
	                    joiner.last_disconnect_event().dstr == "t35",
	            "the raw disconnect record is retained field for field for diagnostics"))
		return false;
	if (!expect(joiner.phase() == inmatch::JoinerConnection::Phase::Error &&
	                    !joiner.deployment_pick_pending() &&
	                    joiner.frame_deployment_pick(0xFFFF).empty(),
	            "the connection is terminal and the deploy screen stops accepting picks"))
		return false;
	if (!expect(joiner.frame_inner(c2s::KEEPALIVE, {0, 0, 0, 0}).empty(),
			"a terminal connection rejects direct session framing"))
		return false;
	const inmatch::JoinerConnection::FrameMessagesResult terminal_batch =
			joiner.frame_messages_detailed(
					{make_protocol_message(c2s::KEEPALIVE, {})});
	if (!expect(terminal_batch.datagrams.empty() &&
				terminal_batch.admitted_count == 0 &&
				terminal_batch.framed_count == 0 &&
				!terminal_batch.frame_failed,
			"a terminal connection admits no detailed batch traffic"))
		return false;
	if (!expect(joiner.pump(1).empty(),
	            "a closed session is no longer pumped"))
		return false;
	if (!expect(joiner.disconnect().empty(),
			"the automatic goodbye consumes the one-shot teardown latch"))
		return false;

	// Retail stores the event only while its slot is empty, so the FIRST record wins.
	SessionSequencing repeat_tx{2, 0};
	std::vector<uint8_t> second_body;
	append_tlv(second_body, "DC", u32_value(4));
	append_tlv(second_body, "DPC", u32_value(46));
	append_tlv(second_body, "DDSTR", {'L', 'a', 't', 'e', 'r', 0});
	const std::vector<uint8_t> second_kick = frame_s2c(repeat_tx,
			{make_protocol_message(0x03, second_body, kDescriptionFlags)});
	joiner.handle_datagram(second_kick.data(), second_kick.size());
	return expect(joiner.session_loss_reason() == reason,
	              "a later disconnect record cannot restate the cause");
}

bool check_runtime_host_close_bypasses_holdoff_and_discards_queued_traffic() {
	uint64_t now_ms = 0x10203040u;
	inmatch::ClientRuntime client("PuntedRuntime", [&now_ms] { return now_ms; });
	client.seed_session(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9, 0, 1, false);

	SessionSequencing server_tx{1, 0};
	const std::vector<uint8_t> settings = frame_s2c(
			server_tx, {make_cs_config(1, 3, 4)});
	client.receive(settings.data(), settings.size());
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(client.send_holdoff_ticks() == 4 &&
				client.send_holdoff_countdown() == 4,
			"the first open send boundary arms the host's four-tick holdoff"))
		return false;

	ClientFiredRound fire;
	fire.shooter_handle = 0x0001;
	if (!expect(client.queue_fired_round(fire),
			"gameplay traffic queues while the established session is live"))
		return false;
	client.queue_loadout_resubmit();

	const std::vector<uint8_t> time_sync = frame_s2c(
			server_tx,
			{make_protocol_message(0x43, {0x11, 0x22, 0x33, 0x44})});
	client.receive(time_sync.data(), time_sync.size());
	if (!expect(client.Client_ProcessNetworkFrame(2).empty() &&
				client.send_holdoff_countdown() == 3,
			"the closed send boundary holds semantic and gameplay traffic"))
		return false;

	const std::vector<uint8_t> kick = frame_s2c(
			server_tx,
			{make_protocol_message(0x03, kCapturedPunt, kDescriptionFlags)});
	const std::vector<uint8_t> queued_after_kick = frame_s2c(
			server_tx,
			{make_protocol_message(0x43, {0x55, 0x66, 0x77, 0x88})});
	client.receive(kick.data(), kick.size());
	client.receive(queued_after_kick.data(), queued_after_kick.size());
	const std::vector<std::vector<uint8_t>> close_frame =
			client.Client_ProcessNetworkFrame(3);
	if (!expect(client.session_lost() &&
				check_client_goodbye_burst(close_frame, kServerKey,
						"terminal host close bypasses holdoff with exactly four goodbyes"),
			"the close frame emits only retail's immediate transport teardown"))
		return false;

	// A terminal runtime must stay silent past the first 0x34 housekeeping
	// interval. Eight follow-up frames only proved the immediate queue clear;
	// it missed the fresh keepalive producer at 29,761 elapsed network ticks.
	for (uint32_t tick = 4; tick < 29765; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(tick).empty(),
				"terminal teardown discards all pending and queued traffic"))
			return false;
	}
	return expect(!client.queue_fired_round(fire),
			"a terminal runtime rejects new gameplay traffic");
}

bool check_initial_settings_and_close_emit_only_goodbyes() {
	inmatch::JoinerConnection joiner("SettingsCloseRace");
	uint32_t client_key = 0;
	if (!drive_to_initial_server_settings(joiner, client_key)) return false;

	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> close = frame_s2c(
			server_tx,
			{make_cs_config(0, 3, 4), make_cs_config(1, 3, 4),
			 make_protocol_message(0x03, kCapturedPunt, kDescriptionFlags)},
			client_key);
	if (!expect(!close.empty(),
			"frame initial settings and terminal description in one packet"))
		return false;
	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(close.data(), close.size());
	if (!expect(joiner.session_lost() &&
				check_client_goodbye_burst(result.outbound, kServerKey,
						"settings plus close emits exactly four goodbyes"),
			"the terminal description replaces same-packet admission traffic"))
		return false;
	return expect(!result.send_holdoff_set &&
				result.queued_send_messages.empty() && result.inbound_0a.empty() &&
				result.inbound_world.empty() && result.inbound_gameplay.empty(),
			"the terminal result exposes no same-packet semantic effects");
}

bool check_ordinary_settings_traffic_is_undisturbed() {
	inmatch::JoinerConnection joiner("SettingsTraffic");
	seed_joiner(joiner);
	SessionSequencing server_tx{1, 0};
	// The initial connection-control pair (full tag 0x100) plus the send-holdoff field the joiner
	// reads out of the CLIENT-direction record.
	const std::vector<uint8_t> settings = frame_s2c(server_tx,
			{make_cs_config(0, 3, 40), make_cs_config(1, 3, 250)});
	if (!expect(!settings.empty(), "frame the initial settings pair")) return false;
	const inmatch::JoinerConnection::PollResult result =
			joiner.handle_datagram(settings.data(), settings.size());
	if (!expect(!joiner.session_lost() && joiner.session_loss_reason().empty() &&
	                    joiner.phase() == inmatch::JoinerConnection::Phase::InMatch,
	            "settings-update traffic does not close the session"))
		return false;
	if (!expect(result.send_holdoff_set && result.send_holdoff == 250,
	            "the settings pre-pass still reads the client-direction send holdoff"))
		return false;

	// A settings-flagged tag 3 whose body is not a TLV run is not a disconnect: the gate keys on
	// the body parsing, not the tag alone.
	SessionSequencing noise_tx{2, 0};
	const std::vector<uint8_t> noise = frame_s2c(noise_tx,
			{make_protocol_message(0x03, {0xFF, 0xFE, 0xFD, 0xFC}, kDescriptionFlags)});
	joiner.handle_datagram(noise.data(), noise.size());
	return expect(!joiner.session_lost() &&
	                      joiner.phase() == inmatch::JoinerConnection::Phase::InMatch,
	              "a tag-3 body that is not a disconnect block leaves the session open");
}

bool check_host_control_punt(uint32_t charattr_silence,
		uint32_t time_sync_silence, uint32_t expected_type) {
	inmatch::HostOwner owner;
	inmatch::set_connection_mode(owner.ctx, inmatch::ConnectionMode::HostOnly);
	owner.ctx.is_in_session = 1;
	owner.ctx.network_quality_broadcast_countdown = 100;

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(
					world, opennova::world::PlayerSpawn{});
	if (!expect(player.valid(), "control-punt fixture spawned its remote player"))
		return false;
	owner.ctx.world = &world;

	replication::UdpSessionTransport transport(
			replication::UdpSessionTransport::Role::Host);
	const PeerAddr peer{0x0100007Fu,
			static_cast<uint16_t>(34000u + expected_type)};
	inmatch::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.server_sk = kServerKey;
	conn.client_ck = kClientKey;
	conn.server_scrk = kServerScrk;
	conn.client_scrk = kClientScrk;
	conn.link.mode = replication::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.reply.minimap_initial_scan_pending = false;
	conn.reply.control_live_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	conn.reply.control_request_countdown = 1;
	conn.reply.charattr_unanswered_count = charattr_silence;
	conn.reply.time_sync_unanswered_count = time_sync_silence;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));

	CaptureDatagramSocket socket;
	inmatch::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1 && socket.sent_to[0] == peer,
			"a control-silence punt is one host-to-joiner datagram"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	if (!expect(nw_decode_inbound(socket.sent[0].data(), socket.sent[0].size(),
				opcode, session_body) &&
				opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			"the staged host punt uses the established S2C session opcode"))
		return false;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(decode_protocol_packet_plaintext(
				session_body.data(), session_body.size(), kServerScrk,
				header, messages) && messages.size() == 1,
			"the host punt decrypts to exactly one inner record"))
		return false;
	DisconnectEvent expected;
	expected.ds = 1;
	expected.dc = 2;
	expected.dstr = "t" + std::to_string(expected_type);
	expected.dpc = 33;
	expected.ddstr = "LogPuntEvent";
	if (!expect(messages[0].tag == hightag::DESCRIPTION_PACKET &&
				messages[0].full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION &&
				messages[0].flags.raw == kDescriptionFlags &&
				messages[0].payload == connection_description_to_bytes(expected),
			"owner staging preserves exact H:0x03 flags and seven TLVs"))
		return false;

	inmatch::NapiNPConnection &live =
			owner.ctx.np_protocol.connection_list.front();
	if (!expect(live.host_disconnect_sent &&
				live.host_disconnect_mismatch_type == expected_type &&
				live.reply.control_request_countdown == 1,
			"the counter is checked before countdown decrement and the cause latches"))
		return false;

	const std::size_t sent_once = socket.sent.size();
	inmatch::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == sent_once &&
				live.host_disconnect_mismatch_type == expected_type,
			"the first host event wins and later pumps remain silent"))
		return false;

	inmatch::JoinerConnection joiner("HostPuntReceiver");
	seed_joiner(joiner);
	const inmatch::JoinerConnection::PollResult receive =
			joiner.handle_datagram(socket.sent[0].data(), socket.sent[0].size());
	if (!expect(joiner.session_lost() &&
				joiner.session_loss_reason().find(expected.dstr) !=
						std::string::npos,
			"the actual host packet closes the joiner with the numeric cause"))
		return false;
	if (!check_client_goodbye_burst(receive.outbound, kServerKey,
			"the actual host packet triggers the keyed goodbye burst"))
		return false;

	// The first copy performs the complete host-side player teardown; the
	// remaining loss-tolerance copies are harmless after the connection is gone.
	bool saw_goodbye = false;
	for (const std::vector<uint8_t> &goodbye : receive.outbound) {
		const inmatch::HandleResult close = inmatch::handle_server_datagram(
				owner.ctx, peer, goodbye.data(), goodbye.size(), owner.now_tick);
		for (const inmatch::HostAcceptEvent &event : close.events) {
			if (event.kind == inmatch::HostAcceptEvent::Kind::PeerGoodbye &&
					event.peer == peer)
				saw_goodbye = true;
		}
	}
	return expect(saw_goodbye &&
				owner.ctx.np_protocol.connection_list.empty() &&
				world.registry.get(player) == nullptr,
			"the automatic goodbye immediately removes the host peer and entity");
}

bool check_host_control_punts_are_ordered_before_the_countdown() {
	if (!check_host_control_punt(8, 8, 16)) return false;
	return check_host_control_punt(7, 8, 24);
}

bool check_staged_host_disconnect_accepts_only_keyed_goodbye() {
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.np_protocol.host_running = 1;
	ctx.np_protocol.host_key = 0xA1B2C3D4u;
	ctx.np_protocol.max_players = 8;
	const PeerAddr peer{0x0100007Fu, 34046};

	inmatch::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	conn.burst.spawned = true;
	conn.spawned_announced = true;
	conn.client_ci = 23;
	conn.client_ck = kClientKey;
	conn.server_sk = kServerKey;
	conn.client_scrk = kClientScrk;
	conn.server_scrk = kServerScrk;
	conn.receive_inactive_ms = 4321;

	// Retain one real S2C message so a valid 0x44 would have something to
	// replay if the staged-close gate accidentally admitted it.
	std::vector<uint8_t> retained_body;
	if (!expect(frame_session_packet(
				conn.seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::SPECTATOR_FLAGS, {0x5A})},
				retained_body) &&
				conn.seq.retained_outbound_message_count == 1,
			"staged-close fixture retains one replayable S2C record"))
		return false;
	conn.host_disconnect_sent = true;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	auto ignored = [](const inmatch::HandleResult &result) {
		return result.outbound.empty() &&
				result.deferred_session_replies.empty() && result.events.empty();
	};
	auto &live = ctx.np_protocol.connection_list.front();

	// A physical FIRST fragment would normally enter c2s_reassembly even
	// though it is not yet a semantic message.
	SessionSequencing client_tx{1, 0};
	const std::vector<uint8_t> first_fragment = frame_c2s(
			client_tx,
			{make_protocol_message(c2s::ENTITY_UPLINK, {0x11, 0x22},
					static_cast<uint8_t>(PROTOCOL_MSG_FLAG_LEN8 |
							PROTOCOL_MSG_FLAG_FRAG_CONT))});
	const inmatch::HandleResult first_result = inmatch::handle_server_datagram(
			ctx, peer, first_fragment.data(), first_fragment.size(), 1,
			/*defer_in_match_replies=*/true);
	if (!expect(ignored(first_result) && live.receive_inactive_ms == 4321 &&
				live.seq.last_inbound_seq == 0 &&
				live.c2s_reassembly.buffer.empty(),
			"a staged close drops C2S before activity, sequencing, or reassembly"))
		return false;

	// Completing that record would normally surface PeerC2SInMatch, and a
	// following PING would normally append a deferred reply to the owner queue.
	const std::vector<uint8_t> final_fragment = frame_c2s(
			client_tx,
			{make_protocol_message(c2s::ENTITY_UPLINK, {0x33, 0x44},
					static_cast<uint8_t>(PROTOCOL_MSG_FLAG_LEN8 |
							PROTOCOL_MSG_FLAG_FRAG_END))});
	const std::vector<uint8_t> ping = frame_c2s(
			client_tx, {make_protocol_message(c2s::PING, {})});
	const inmatch::HandleResult final_result = inmatch::handle_server_datagram(
			ctx, peer, final_fragment.data(), final_fragment.size(), 2,
			/*defer_in_match_replies=*/true);
	const inmatch::HandleResult ping_result = inmatch::handle_server_datagram(
			ctx, peer, ping.data(), ping.size(), 3,
			/*defer_in_match_replies=*/true);
	if (!expect(ignored(final_result) && ignored(ping_result) &&
				live.seq.last_inbound_seq == 0 &&
				live.c2s_reassembly.buffer.empty(),
			"a staged close surfaces no C2S event or deferred reply"))
		return false;

	// A future sequence is the queue-growth case: it must not enter the
	// ordered-recovery map or arm a missing-sequence resend after closure.
	SessionSequencing future_tx{5, 0};
	const std::vector<uint8_t> future = frame_c2s(
			future_tx, {make_protocol_message(c2s::KEEPALIVE, {})});
	const inmatch::HandleResult future_result = inmatch::handle_server_datagram(
			ctx, peer, future.data(), future.size(), 4,
			/*defer_in_match_replies=*/true);
	if (!expect(ignored(future_result) && live.seq.queued_inbound.empty() &&
				!live.seq.missing_request_pending &&
				inmatch::flush_server_missing_requests(ctx).empty(),
			"a staged close cannot grow or drain the ordered receive queue"))
		return false;

	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(kServerKey, {1}, resend_body),
			"staged-close fixture encodes a keyed resend request"))
		return false;
	const std::vector<uint8_t> resend = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(resend_body));
	const inmatch::HandleResult resend_result = inmatch::handle_server_datagram(
			ctx, peer, resend.data(), resend.size(), 5);
	if (!expect(ignored(resend_result) && live.receive_inactive_ms == 4321 &&
				live.seq.retained_outbound_message_count == 1,
			"a staged close neither refreshes nor answers a keyed resend request"))
		return false;

	const ClientHello hello = make_jointoperations_client_hello(99);
	const std::vector<uint8_t> hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	const ClientAuth auth = make_retail_client_auth(
			live.client_ci, live.client_ck, ctx.np_protocol.host_key);
	const std::vector<uint8_t> auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	const inmatch::HandleResult hello_result = inmatch::handle_server_datagram(
			ctx, peer, hello_datagram.data(), hello_datagram.size(), 6);
	const inmatch::HandleResult auth_result = inmatch::handle_server_datagram(
			ctx, peer, auth_datagram.data(), auth_datagram.size(), 7);
	if (!expect(ignored(hello_result) && ignored(auth_result) &&
				ctx.np_protocol.connection_list.size() == 1 &&
				live.host_disconnect_sent && live.receive_inactive_ms == 4321,
			"a staged close ignores same-endpoint hello/auth retries"))
		return false;

	const std::vector<uint8_t> wrong_goodbye = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_GOODBYE,
			client_goodbye_to_bytes(kServerKey + 1));
	const inmatch::HandleResult wrong_result = inmatch::handle_server_datagram(
			ctx, peer, wrong_goodbye.data(), wrong_goodbye.size(), 8);
	if (!expect(ignored(wrong_result) &&
				ctx.np_protocol.connection_list.size() == 1,
			"a staged close rejects an unkeyed ClientGoodbye"))
		return false;

	const std::vector<uint8_t> keyed_goodbye = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_GOODBYE,
			client_goodbye_to_bytes(kServerKey));
	const inmatch::HandleResult close = inmatch::handle_server_datagram(
			ctx, peer, keyed_goodbye.data(), keyed_goodbye.size(), 9);
	return expect(close.outbound.empty() &&
				close.deferred_session_replies.empty() &&
				close.events.size() == 1 &&
				close.events[0].kind == inmatch::HostAcceptEvent::Kind::PeerGoodbye &&
				ctx.np_protocol.connection_list.empty(),
			"only the correctly keyed ClientGoodbye completes staged teardown");
}

// Retail's description builder queues H:0x03 with the owner-only 0x10 flag,
// which bypasses NapiNPMessage_Create's msg_out_max refusal. The flag is not
// part of the encoded 0xA0 record: a saturated host must still put its terminal
// reason on the wire instead of latching a silent, unreachable close.
// [orig: NapiNPDataTransfer_SendDescription @0x628C80 ->
//  NapiNPMessage_Create bypass @0x628031..0x628112]
bool check_host_description_bypasses_saturated_message_capacity() {
	inmatch::HostOwner owner;
	owner.ctx.is_authority = 1;
	owner.ctx.is_in_session = 1;
	owner.ctx.np_protocol.host_running = 1;
	const PeerAddr peer{0x0100007Fu, 34047};

	replication::UdpSessionTransport transport(
			replication::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	conn.burst.spawned = true;
	conn.spawned_announced = true;
	conn.client_ck = kClientKey;
	conn.server_sk = kServerKey;
	conn.server_scrk = kServerScrk;
	conn.link.mode = replication::TransportMode::Client;
	conn.link.transport = &transport;
	conn.seq.outbound_message_limit = 1;

	std::vector<uint8_t> retained_body;
	if (!expect(frame_session_packet(
				conn.seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::SPECTATOR_FLAGS, {0x5A})},
				retained_body) &&
				conn.seq.retained_outbound_message_count == 1,
			"the terminal-capacity fixture saturates its retained-node limit"))
		return false;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &live = owner.ctx.np_protocol.connection_list.front();
	// This ordinary record was produced earlier in the same owner boundary. It
	// remains capacity-rejected, but cannot hide the exempt terminal record that
	// follows it in FIFO order.
	transport.host_send(s2c::SPECTATOR_FLAGS, {0x33});

	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dstr = "t35";
	event.dpc = 33;
	event.ddstr = "LogPuntEvent";
	if (!expect(inmatch::Server_StageHostDisconnect(live, event),
			"the saturated host stages its terminal description"))
		return false;

	CaptureDatagramSocket socket;
	inmatch::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1 && socket.sent_to[0] == peer,
			"the terminal description bypasses saturated message capacity"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	return expect(nw_decode_inbound(
				socket.sent[0].data(), socket.sent[0].size(),
				opcode, session_body) &&
				opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
				decode_protocol_packet_plaintext(
					session_body.data(), session_body.size(), kServerScrk,
					header, messages) &&
				messages.size() == 1 &&
				messages[0].full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION &&
				messages[0].flags.raw == kDescriptionFlags &&
				messages[0].payload == kCapturedPunt,
			"capacity bypass changes no terminal description wire bytes");
}

bool check_join_deploy_idle_punt_uses_state6_elapsed_time() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.config.max_players = 4;
	ctx.network_quality_broadcast_countdown = 100;
	ctx.np_protocol.host_run_duration_ms = 1234;

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	world.registry.configure_pool(3, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	opennova::world::Entity zone;
	zone.kind = opennova::world::EntityKind::Building;
	zone.item_id = 0x0500;
	zone.is_spawn_point = true;
	zone.alive = true;
	zone.team = 1;
	if (!expect(world.registry.spawn(2, zone).valid(),
			"idle-punt fixture installs a selectable spawn zone"))
		return false;
	ctx.world = &world;

	replication::UdpSessionTransport transport(
			replication::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection conn;
	conn.peer = {0x0100007Fu, 34035};
	conn.type = 1;
	conn.connection_id = inmatch::kFirstJoinerDcb;
	conn.link.mode = replication::TransportMode::Client;
	conn.link.transport = &transport;
	conn.player_name = "IdleJoiner";
	ctx.np_protocol.connection_list.push_back(std::move(conn));
	inmatch::NapiNPConnection &live = ctx.np_protocol.connection_list.front();
	const opennova::world::EntityHandle player =
			inmatch::Server_BuildPlayerInfoAndAdd(ctx, live, world);
	if (!expect(player.valid() && live.link.respawn_pending &&
				live.phase == inmatch::ConnectionPhase::PlayerAdded &&
				live.reply.state6_entry_host_ms_valid &&
				live.reply.state6_entry_host_ms == 1234,
			"player-add stamps state-6 time and the join-only pending bit"))
		return false;

	auto expect_no_description = [&](const char *message) {
		replication::Datagram datagram;
		return expect(!live.host_disconnect_sent &&
					!transport.pop_outbound(datagram), message);
	};
	const uint32_t state6_ms = live.reply.state6_entry_host_ms;
	live.reply.state6_entry_host_ms_valid = false;
	ctx.np_protocol.host_run_duration_ms = state6_ms + 360001u;
	inmatch::Server_TickUpdate(ctx);
	if (!expect_no_description("an invalid state-6 timestamp blocks t35"))
		return false;

	live.reply.state6_entry_host_ms_valid = true;
	live.phase = inmatch::ConnectionPhase::PendingSpawn;
	inmatch::Server_TickUpdate(ctx);
	if (!expect_no_description("a pre-state-6 player cannot receive t35"))
		return false;

	live.phase = inmatch::ConnectionPhase::PlayerAdded;
	live.link.respawn_pending = false;
	inmatch::Server_TickUpdate(ctx);
	if (!expect_no_description("deployment release blocks the join-idle punt"))
		return false;

	live.link.respawn_pending = true;
	ctx.np_protocol.host_run_duration_ms = state6_ms + 360000u;
	inmatch::Server_TickUpdate(ctx);
	if (!expect_no_description("exactly six minutes is not strictly over the t35 limit"))
		return false;

	ctx.np_protocol.host_run_duration_ms = state6_ms + 360001u;
	inmatch::Server_TickUpdate(ctx);
	replication::Datagram punt;
	if (!expect(live.host_disconnect_sent &&
				live.host_disconnect_mismatch_type == 35 &&
				transport.pop_outbound(punt) &&
				!transport.has_outbound() && punt.reliable &&
				punt.protocol_flags_raw == kDescriptionFlags &&
				punt.tag == hightag::DESCRIPTION_PACKET &&
				punt.body == kCapturedPunt,
			"the first millisecond over six minutes emits the captured t35 record"))
		return false;

	DisconnectEvent later;
	later.ds = 1;
	later.dc = 2;
	later.dpc = 46;
	later.ddstr = "PUNT ACRC";
	if (!expect(!inmatch::Server_StageHostDisconnect(live, later) &&
				!transport.has_outbound(),
			"a later generic description cannot replace the first host event"))
		return false;

	SessionSequencing server_tx{1, 0};
	const std::vector<uint8_t> wire = frame_s2c(
			server_tx,
			{make_protocol_message(
					punt.tag, punt.body, punt.protocol_flags_raw)});
	inmatch::JoinerConnection joiner("IdlePuntReceiver");
	seed_joiner(joiner);
	joiner.handle_datagram(wire.data(), wire.size());
	return expect(joiner.session_lost() &&
				joiner.session_loss_reason().find("t35") != std::string::npos,
			"the strict-boundary host record is terminal to the retail-client path");
}

bool check_dead_player_punt_uses_a_consecutive_state6_counter() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.network_quality_broadcast_countdown = 100;

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(
					world, opennova::world::PlayerSpawn{});
	if (!expect(player.valid(), "dead-age fixture spawned its remote player"))
		return false;
	ctx.world = &world;

	replication::UdpSessionTransport transport(
			replication::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection conn;
	conn.peer = {0x0100007Fu, 34007};
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	// Suppress the ordinary fresh 0x0A; the punt is staged independently of
	// the frame boundary and is the only semantic record this fixture examines.
	conn.s2c_send_boundary_open = false;
	conn.link.mode = replication::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	conn.reply.roster_seen_gen = ctx.np_protocol.roster_generation;
	conn.reply.minimap_initial_scan_pending = false;
	ctx.np_protocol.connection_list.push_back(std::move(conn));
	inmatch::NapiNPConnection &live = ctx.np_protocol.connection_list.front();
	opennova::world::Entity *entity = world.registry.get(player);
	if (!expect(entity != nullptr, "dead-age fixture retains its player entity"))
		return false;

	entity->engine_flags |= opennova::world::kEntityFlagDead;
	live.reply.dead_live_ticks = 359;
	inmatch::Server_TickUpdate(ctx);
	replication::Datagram datagram;
	if (!expect(live.reply.dead_live_ticks == 360 &&
				!live.host_disconnect_sent && !transport.pop_outbound(datagram),
			"exactly 360 consecutive dead ticks does not emit t7"))
		return false;

	// A single live tick resets the slot counter rather than pausing it.
	entity->engine_flags &= ~opennova::world::kEntityFlagDead;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(live.reply.dead_live_ticks == 0 &&
				!live.host_disconnect_sent && !transport.pop_outbound(datagram),
			"a live state-6 frame resets the dead-age counter"))
		return false;

	entity->engine_flags |= opennova::world::kEntityFlagDead;
	live.reply.dead_live_ticks = 360;
	ctx.config.permanent_death = true;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(live.reply.dead_live_ticks == 361 &&
				!live.host_disconnect_sent && !transport.pop_outbound(datagram),
			"permanent-death mode suppresses t7 after the strict boundary"))
		return false;

	ctx.config.permanent_death = false;
	inmatch::Server_TickUpdate(ctx);
	DisconnectEvent expected;
	expected.ds = 1;
	expected.dc = 2;
	expected.dstr = "t7";
	expected.dpc = 33;
	expected.ddstr = "LogPuntEvent";
	return expect(live.host_disconnect_sent &&
				live.host_disconnect_mismatch_type == 7 &&
				transport.pop_outbound(datagram) && !transport.has_outbound() &&
				datagram.reliable &&
				datagram.protocol_flags_raw == kDescriptionFlags &&
				datagram.tag == hightag::DESCRIPTION_PACKET &&
				datagram.body == connection_description_to_bytes(expected),
			"the first non-permanent tick over 360 emits exact t7");
}

bool reply_body(const inmatch::JoinerConnection::PollResult &result, uint8_t tag,
		std::vector<uint8_t> &body_out) {
	for (const ProtocolMessage &message : result.queued_send_messages) {
		if (message.flags.settings_update || message.tag != tag) continue;
		body_out = message.payload;
		return true;
	}
	return false;
}

// The anti-cheat CRC challenges stay SILENT unless an exact, named corpus profile is
// selected. A guessed value cannot be right, and this is the one challenge pair whose
// mismatch arm disconnects: the host recomputes the checksum over its own tables and punts
// on a difference [orig: handle_anti_cheat_crc_check @0x502050], where an unanswered
// challenge costs nothing. Witnessed live 2026-07-26 against a stock retail co-op host —
// a single placeholder C2S 0x20 / 0x21 drew "PUNT WCRC" / "PUNT ACRC" (DC=2, DPC=46) and
// ended the session mid-join, while the same client sending nothing stayed connected.
// This pins the default SILENCE so a future "helpful" reply cannot regress joining
// (D-NET-181).
bool check_crc_challenges_are_not_answered() {
	inmatch::JoinerConnection joiner("ChallengeSilence");
	seed_joiner(joiner);
	SessionSequencing server_tx{1, 0};

	const std::vector<uint8_t> forms[] = {
			frame_s2c(server_tx, {make_protocol_message(0x30, {0x02, 0xEF, 0xBE})}),
			frame_s2c(server_tx, {make_protocol_message(0x30, {0xFF, 0x01, 0x00})}),
			frame_s2c(server_tx, {make_protocol_message(0x30, {0xFF, 0x00, 0x00})}),
			frame_s2c(server_tx, {make_protocol_message(0x31, {0x07, 0x34, 0x12})}),
	};
	for (const std::vector<uint8_t> &challenge : forms) {
		const inmatch::JoinerConnection::PollResult result =
				joiner.handle_datagram(challenge.data(), challenge.size());
		std::vector<uint8_t> body;
		if (!expect(!reply_body(result, 0x20, body) && !reply_body(result, 0x21, body),
		            "an anti-cheat CRC challenge draws no reply while its image is unmodelled"))
			return false;
		// Silence here must not be silence everywhere: the connection is still live and
		// still answers the challenges whose honest values we DO produce.
		if (!expect(!joiner.session_lost(),
		            "and the unanswered challenge does not itself end the session"))
			return false;
	}
	// The diagnostics counters record the silent traffic: what a live-join
	// punt investigation reads back to distinguish "challenges never arrived"
	// from "challenges arrived and the deliberate-silence policy held".
	const inmatch::JoinerConnection::ChallengeDiagnostics &challenges =
			joiner.challenge_diagnostics();
	return expect(challenges.entity_checksum_seen == 3 &&
	                      challenges.entity_checksum_answered == 0 &&
	                      challenges.loadout_crc_seen == 1 &&
	                      challenges.loadout_crc_answered == 0,
	              "the challenge counters record the seen-but-unanswered traffic");
}

// A CR=0 ServerSessionInit carries the NP-layer reject family (JFC) and the
// validate-callback sub-reason (JFP). The joiner maps the witnessed families to
// player-facing text AND retains the raw fields so a live-join investigation
// can name the family after the mapped string replaced it.
// [orig: NapiNPProtocol_SendJoinRejection @0x620cd0; client store
//  NapiNP_HandleServerJoinResponse @0x629840]
bool check_join_rejection_retains_the_raw_reject_record() {
	inmatch::JoinerConnection joiner("RejectedPlayer");
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello client_hello;
	const std::vector<uint8_t> hello = joiner.start();
	if (!expect(nw_decode_inbound(hello.data(), hello.size(), opcode, body) &&
					parse_client_hello(body.data(), body.size(), client_hello),
			"decode the rejected joiner's ClientHello"))
		return false;
	ServerHello server_hello = build_server_hello(client_hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	ClientAuth client_auth;
	body.clear();
	if (!expect(hello_result.outbound.size() == 1 &&
					nw_decode_inbound(hello_result.outbound[0].data(),
							hello_result.outbound[0].size(), opcode, body) &&
					parse_client_auth(body.data(), body.size(), client_auth),
			"decode the rejected joiner's ClientAuth"))
		return false;

	ServerAuth rejection;
	rejection.ci = client_auth.ci;
	rejection.ck = client_auth.ck;
	rejection.cr = 0;
	rejection.jfc = 14;
	rejection.jfp = 2;
	const std::vector<uint8_t> rejection_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(rejection));
	joiner.handle_datagram(rejection_datagram.data(), rejection_datagram.size());
	if (!expect(joiner.phase() == inmatch::JoinerConnection::Phase::Error &&
					joiner.last_error() == "The server is locked",
			"the witnessed JFC=14/JFP=2 family maps to the locked-server text"))
		return false;
	return expect(joiner.last_join_reject().set &&
					joiner.last_join_reject().jfc == 14 &&
					joiner.last_join_reject().jfp == 2 &&
					joiner.last_join_reject().jfs.empty(),
			"the raw JFC/JFP reject record is retained past the mapping");
}

// The PV2 identity gate (JFC=7): a live retail server on a different JO patch
// pins proto+364 to a different token than our byte-correct "16", and rejects
// the join at auth. The reason must name the incompatibility, not print "code 7"
// [orig: HandleClientJoin @0x62b750 PV2 gate @0x62be40]. Witnessed live against
// the NovaWorld server "THOR THUNDER" 2026-08-31.
bool check_pv2_mismatch_reports_an_incompatible_version() {
	inmatch::JoinerConnection joiner("VersionMismatch");
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello client_hello;
	const std::vector<uint8_t> hello = joiner.start();
	if (!expect(nw_decode_inbound(hello.data(), hello.size(), opcode, body) &&
					parse_client_hello(body.data(), body.size(), client_hello),
			"decode the version-mismatch joiner's ClientHello"))
		return false;
	ServerHello server_hello = build_server_hello(client_hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	ClientAuth client_auth;
	body.clear();
	if (!expect(hello_result.outbound.size() == 1 &&
					nw_decode_inbound(hello_result.outbound[0].data(),
							hello_result.outbound[0].size(), opcode, body) &&
					parse_client_auth(body.data(), body.size(), client_auth),
			"decode the version-mismatch joiner's ClientAuth"))
		return false;
	// The joiner sends the retail JO game-session PV2 constant.
	if (!expect(client_auth.pv2 == "16",
			"the joiner sends the byte-correct 1.7.5.7 game-session PV2"))
		return false;
	ServerAuth rejection;
	rejection.ci = client_auth.ci;
	rejection.ck = client_auth.ck;
	rejection.cr = 0;
	rejection.jfc = 7;
	const std::vector<uint8_t> rejection_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(rejection));
	joiner.handle_datagram(rejection_datagram.data(), rejection_datagram.size());
	return expect(joiner.phase() == inmatch::JoinerConnection::Phase::Error &&
					joiner.last_error().find("incompatible protocol version") != std::string::npos,
			"JFC=7 reports an incompatible protocol version, not a bare code");
}

// A named, independently witnessed retail-corpus profile may answer only the
// checksum sources it proves. These values were reproduced from all 102 live
// ammo-definition rows in one revx02 process, independently of packet captures:
//   - the sanitized 140-entry ADM/weapon-slot walk => 0x024F56F2
//   - sanitized ammo record 0x18 (AMMO_M16_556MM) => 0x2D087374
//   - sanitized ammo record 0x1A => 0x616CD2EE
// The same witness proved the table capacity is 102, so every index 0..101 is
// covered and index 102 takes retail's exact out-of-range zero-CRC arm.
bool check_verified_revx02_profile_answers_exact_crc_challenges() {
	inmatch::JoinerConnection joiner("VerifiedChallenge");
	if (!expect(joiner.set_integrity_challenge_profile(
				"retail-revx02-024f56f2-2d087374"),
			"the witnessed revx02 integrity profile is registered"))
		return false;
	seed_joiner(joiner);
	SessionSequencing server_tx{1, 0};

	struct Case {
		uint8_t request_tag;
		std::vector<uint8_t> request_body;
		uint8_t reply_tag;
		std::vector<uint8_t> reply_body;
	};
	const std::vector<Case> cases = {
		// Golden retail request/reply pair: challenge zero asks for the whole
		// sanitized weapon-slot table.
		{0x30, {0xFF, 0x00, 0x00}, 0x20,
		 {0xFF, 0xF2, 0x56, 0x4F, 0x02}},
		// The id=0xFF/nonzero branch XORs the u16 challenge with literal 42.
		{0x30, {0xFF, 0x34, 0x12}, 0x20,
		 {0xFF, 0x1E, 0x12, 0x00, 0x00}},
		// Golden ammo record 0x18, key zero.
		{0x31, {0x18, 0x00, 0x00}, 0x21,
		 {0x18, 0x74, 0x73, 0x08, 0x2D, 0x00, 0x00, 0x00, 0x00}},
		// A different equipped weapon may select any witnessed row; this
		// independently cross-checks the sanitizer against a second definition.
		{0x31, {0x1A, 0x00, 0x00}, 0x21,
		 {0x1A, 0xEE, 0xD2, 0x6C, 0x61, 0x00, 0x00, 0x00, 0x00}},
		// An ordinary non-capture row is covered too: the profile is the full
		// corpus, not a hard-coded exception for row 0x18.
		{0x31, {0x07, 0x00, 0x00}, 0x21,
		 {0x07, 0xC6, 0x73, 0x13, 0xBB, 0x00, 0x00, 0x00, 0x00}},
		// First index beyond the witnessed 102-record table: zero CRC, key echo.
		{0x31, {0x66, 0x34, 0x12}, 0x21,
		 {0x66, 0x00, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00}},
	};
	for (const Case &c : cases) {
		const std::vector<uint8_t> challenge = frame_s2c(
				server_tx, {make_protocol_message(c.request_tag, c.request_body)});
		const inmatch::JoinerConnection::PollResult result =
				joiner.handle_datagram(challenge.data(), challenge.size());
		std::vector<uint8_t> body;
		if (!expect(reply_body(result, c.reply_tag, body) && body == c.reply_body,
				"the verified profile emits the exact witnessed integrity reply"))
			return false;
	}

	std::vector<uint8_t> body;
	// Compact whole-corpus pin: FNV-1a over each `[row][crc32-le]` tuple.
	// This catches an omitted, reordered, or mistyped row without duplicating the
	// 102-entry source table in the test. The digest was computed from the same
	// sanitized live-process witness as the two human-readable anchors above.
	uint64_t corpus_digest = 14695981039346656037ull;
	for (uint16_t row = 0; row < 102; ++row) {
		const std::vector<uint8_t> request = frame_s2c(
				server_tx,
				{make_protocol_message(0x31,
						{static_cast<uint8_t>(row), 0x00, 0x00})});
		const inmatch::JoinerConnection::PollResult result =
				joiner.handle_datagram(request.data(), request.size());
		body.clear();
		if (!expect(reply_body(result, 0x21, body) && body.size() == 9 &&
					body[0] == row,
				"the verified profile covers every witnessed ammo row"))
			return false;
		for (const uint8_t byte : {body[0], body[1], body[2], body[3], body[4]}) {
			corpus_digest ^= byte;
			corpus_digest *= 1099511628211ull;
		}
	}
	if (!expect(corpus_digest == 0xCAED42DE09D8AD38ull,
			"all 102 indexed CRC constants match the live revx02 witness digest"))
		return false;

	const std::vector<uint8_t> trailing = frame_s2c(
			server_tx, {make_protocol_message(0x30, {0xFF, 0x00, 0x00, 0x99})});
	const inmatch::JoinerConnection::PollResult trailing_result =
			joiner.handle_datagram(trailing.data(), trailing.size());
	if (!expect(!reply_body(trailing_result, 0x20, body),
			"a checksum request with trailing bytes is not answered"))
		return false;

	// Answered traffic shows up in the diagnostics counters: 3 x 0x30 (two
	// answered by the profile, the trailing-bytes form not), and 4 + 102
	// 0x31s all answered.
	const inmatch::JoinerConnection::ChallengeDiagnostics &challenges =
			joiner.challenge_diagnostics();
	if (!expect(challenges.entity_checksum_seen == 3 &&
	                    challenges.entity_checksum_answered == 2 &&
	                    challenges.loadout_crc_seen == 106 &&
	                    challenges.loadout_crc_answered == 106,
	            "the challenge counters record the profile-answered traffic"))
		return false;

	if (!expect(!joiner.set_integrity_challenge_profile("not-a-profile"),
			"an unknown integrity profile is rejected"))
		return false;
	const std::vector<uint8_t> after_clear = frame_s2c(
			server_tx, {make_protocol_message(0x30, {0xFF, 0x00, 0x00})});
	const inmatch::JoinerConnection::PollResult after_clear_result =
			joiner.handle_datagram(after_clear.data(), after_clear.size());
	return expect(!reply_body(after_clear_result, 0x20, body),
			"rejecting an unknown profile clears a previously selected corpus");
}

} // namespace

int main() {
	bool ok = true;
	ok = check_captured_punt_decodes_field_for_field() && ok;
	ok = check_decode_is_order_and_shape_robust() && ok;
	ok = check_captured_punt_closes_a_joiner_parked_at_the_deploy_screen() && ok;
	ok = check_runtime_host_close_bypasses_holdoff_and_discards_queued_traffic() && ok;
	ok = check_initial_settings_and_close_emit_only_goodbyes() && ok;
	ok = check_ordinary_settings_traffic_is_undisturbed() && ok;
	ok = check_host_control_punts_are_ordered_before_the_countdown() && ok;
	ok = check_staged_host_disconnect_accepts_only_keyed_goodbye() && ok;
	ok = check_host_description_bypasses_saturated_message_capacity() && ok;
	ok = check_join_deploy_idle_punt_uses_state6_elapsed_time() && ok;
	ok = check_dead_player_punt_uses_a_consecutive_state6_counter() && ok;
	ok = check_crc_challenges_are_not_answered() && ok;
	ok = check_join_rejection_retains_the_raw_reject_record() && ok;
	ok = check_pv2_mismatch_reports_an_incompatible_version() && ok;
	ok = check_verified_revx02_profile_answers_exact_crc_challenges() && ok;
	return ok ? 0 : 1;
}
