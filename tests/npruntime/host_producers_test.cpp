// The in-match host producers and handlers ported from the 2026-09-20 net
// parity audit (plan PR 9):
//   - the C2S 0x2C return leg: the RTT ring with its aliasing cursor and the
//     min/max-ping strike punts [orig: NapiNPServerMsg_HandlePingResponse @0x515070];
//   - C2S 0x4C store + dirty-on-change [orig: NapiNPServerMsg_0x04C @0x5111B0 -> sub_5006E0];
//   - the 1 Hz S2C 0x46 quality resend, eight per second with a persisting
//     cursor [orig: Server_TickUpdate @0x51DE79..0x51DF4A];
//   - the host CNetQuality send window behind S2C 0x79 [orig: CNetQuality_UpdateMetrics @0x4C52C0];
//   - C2S 0x0D chat: strip, throttle, the int-triplet sender-only rule and the
//     per-channel fan [orig: NapiNPServer_HandleChatMessage @0x513760];
//   - C2S 0x3D stamps the frame clock [orig: NapiNPServerMsg_0x03D @0x500EC0];
//   - C2S 0x42 -> S2C 0x70 from the limit table [orig: NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0];
//   - C2S 0x51 permanent-death spectator convert + S2C 0x32 [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C840];
//   - the medic revive transaction 0x3A / 0x61 / 0x1E ev 38 with victim then
//     healer [orig: GameEvent_RevivePlayer @0x517CD0].

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_admin_command.h>
#include <runtime/inmatch/server_chat.h>
#include <runtime/inmatch/server_medic.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/novaworld_link.h>
#include <runtime/inmatch/server_net_quality.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_vehicle_spawn.h>
#include <runtime/inmatch/server_visible_players.h>
#include <runtime/inmatch/server_emote.h>
#include <runtime/inmatch/server_designations.h>
#include <runtime/inmatch/server_radio_call.h>

#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_ping.h>
#include <net/npwire/visible_players.h>
#include <net/npwire/emote_wire.h>

#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(float x, float y, float z, uint8_t team = 1) {
	w::PlayerSpawn s;
	s.position = {x, y, z};
	s.team = team;
	return s;
}

std::vector<ns::Datagram> drain(ns::UdpSessionTransport &transport) {
	std::vector<ns::Datagram> out;
	ns::Datagram datagram;
	while (transport.pop_outbound(datagram)) out.push_back(datagram);
	return out;
}

size_t count_tag(const std::vector<ns::Datagram> &datagrams, uint8_t tag) {
	size_t n = 0;
	for (const ns::Datagram &d : datagrams) n += d.tag == tag ? 1 : 0;
	return n;
}

struct HostFixture {
	inmatch::NapiNPServerCtx ctx;
	w::World world;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	explicit HostFixture(size_t peers, uint8_t first_team = 1) {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.network_quality_broadcast_countdown = 1000;
		ctx.config.max_players = 16;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 32);
		world.registry.configure_pool(1, 8);
		world.registry.configure_pool(3, 8);
		ctx.world = &world;
		transports.reserve(peers);
		for (size_t i = 0; i < peers; ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			const uint8_t team = static_cast<uint8_t>(first_team + (i % 2));
			players.push_back(w::spawn_remote_player(world,
					player_spawn(10.0f * float(i), 0.0f, 0.0f, team)));
		}
		for (size_t i = 0; i < peers; ++i) {
			inmatch::NapiNPConnection conn = conn_fixture::make_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			conn.reply.player_slot = static_cast<uint8_t>(i + 1);
			conn.reply.player_name = "P" + std::to_string(i + 1);
			(void)inmatch::Server_RerollPlayerTickSeed(conn);
			ctx.np_protocol.connection_list.push_back(std::move(conn));
		}
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	std::vector<ProtocolMessage> dispatch(size_t i, uint8_t tag, std::vector<uint8_t> body,
			uint32_t now_tick = 100) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &ctx;
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(tag, std::move(body)));
		return inmatch::dispatch_session_replies(ctx.config, conn(i), msgs, now_tick,
				ctx.np_protocol.connection_list, &world, inputs);
	}
};

// --------------------------------------------------------------------------
// The 0x2C return leg.
// --------------------------------------------------------------------------

bool check_ping_ring_and_strikes() {
	HostFixture f(1);
	inmatch::NapiNPConnection &conn = f.conn(0);
	inmatch::GameConfig config;
	// Eleven samples: the cursor pre-increments 1..10, sample 10 lands on the
	// cursor dword itself, so the twelfth wraps to 0.
	for (uint32_t i = 1; i <= 11; ++i)
		inmatch::Server_RecordPingSample(config, conn, 1000, 1000 + 10 * i, true);
	if (!expect(conn.reply.rtt_ms == 110, "the last sample is now - sent")) return false;
	if (!expect(conn.reply.rtt_ring[1] == 10 && conn.reply.rtt_ring[9] == 90,
			"samples 1..9 land in ring slots 1..9"))
		return false;
	if (!expect(conn.reply.rtt_ring[10] == 0 && conn.reply.rtt_ring[0] == 110,
			"sample 10 overwrote the cursor, which then wrapped sample 11 to slot 0"))
		return false;
	// The min-ping policy: 20 strikes hold, the 21st punts with chat 36.
	config.do_min_ping_check = true;
	config.min_ping = 500;
	for (int i = 0; i < 20; ++i)
		inmatch::Server_RecordPingSample(config, conn, 1000, 1010, true);
	if (!expect(!conn.host_disconnect_sent && conn.reply.min_ping_strikes == 20,
			"twenty consecutive under-min samples do not punt"))
		return false;
	inmatch::Server_RecordPingSample(config, conn, 1000, 1010, true);
	if (!expect(conn.host_disconnect_sent, "the 21st under-min sample punts")) return false;
	// A compliant sample resets the max strike counter.
	HostFixture g(1);
	inmatch::NapiNPConnection &other = g.conn(0);
	config = inmatch::GameConfig{};
	config.do_max_ping_check = true;
	config.max_ping = 100;
	for (int i = 0; i < 20; ++i)
		inmatch::Server_RecordPingSample(config, other, 1000, 1500, true);
	inmatch::Server_RecordPingSample(config, other, 1000, 1050, true);
	if (!expect(other.reply.max_ping_strikes == 0 && !other.host_disconnect_sent,
			"an in-range sample clears the max strikes"))
		return false;
	// The local slot measures zero and is exempt.
	other.link.mode = ns::TransportMode::Loopback;
	inmatch::Server_RecordPingSample(config, other, 1000, 9000, true);
	if (!expect(other.reply.rtt_ms == 0 && other.reply.max_ping_strikes == 0,
			"the listen host's own slot samples zero and takes no strikes"))
		return false;
	// A joiner advertising the DB (debug build) join tag is exempt too: 21
	// under-min samples take no strike and never punt [orig: @0x515171 —
	// NetPlayer+0xD8 = NapiNetConfig::db].
	HostFixture d(1);
	inmatch::NapiNPConnection &debug = d.conn(0);
	debug.join_environment.db = 1;
	config = inmatch::GameConfig{};
	config.do_min_ping_check = true;
	config.min_ping = 500;
	for (int i = 0; i < 21; ++i)
		inmatch::Server_RecordPingSample(config, debug, 1000, 1010, true);
	return expect(!debug.host_disconnect_sent && debug.reply.min_ping_strikes == 0 &&
					debug.reply.rtt_ms == 10,
			"a DB joiner still samples its round trip but takes no ping strikes");
}

bool check_ping_dispatch_return_leg() {
	HostFixture f(1);
	// [u32 ts][u8 0]: the return leg, no reply.
	std::vector<ProtocolMessage> replies = f.dispatch(0, c2s::RTT_CONSUMED, {0, 0, 0, 0, 0});
	if (!expect(replies.empty(), "the return leg produces no reply")) return false;
	if (!expect(f.conn(0).reply.rtt_ring[1] != 0 || f.conn(0).reply.rtt_ms != 0,
			"the return leg stored a sample"))
		return false;
	replies = f.dispatch(0, c2s::RTT_CONSUMED, {1, 0, 0, 0, 1});
	return expect(replies.size() == 1 && replies[0].tag == s2c::RTT_ECHO,
			"the echo leg still bounces 0x57");
}

// --------------------------------------------------------------------------
// Quality: 0x4C store, the 1 Hz 0x46 resend, the host window.
// --------------------------------------------------------------------------

bool check_client_quality_store_and_resend() {
	HostFixture f(10);
	inmatch::Server_StoreClientQuality(f.conn(0), 9);
	if (!expect(f.conn(0).reply.client_quality == 4 && f.conn(0).reply.client_quality_dirty,
			"0x4C clamps to 4 and dirties on change"))
		return false;
	f.conn(0).reply.client_quality_dirty = false;
	inmatch::Server_StoreClientQuality(f.conn(0), 4);
	if (!expect(!f.conn(0).reply.client_quality_dirty, "an unchanged level does not dirty"))
		return false;
	std::vector<ProtocolMessage> replies = f.dispatch(1, c2s::CLIENT_QUALITY, {2});
	if (!expect(replies.empty() && f.conn(1).reply.client_quality == 2 &&
					f.conn(1).reply.client_quality_dirty,
			"the dispatcher stores a 0x4C report"))
		return false;
	// Ten dirty slots: the first walk sends eight, the second the remaining two.
	for (size_t i = 0; i < 10; ++i) {
		f.conn(i).reply.client_quality = static_cast<uint8_t>(i % 5);
		f.conn(i).reply.client_quality_dirty = true;
	}
	for (auto &t : f.transports) (void)drain(t);
	inmatch::Server_EmitQualityResends(f.ctx, f.world);
	std::vector<ns::Datagram> first = drain(f.transports[0]);
	if (!expect(count_tag(first, s2c::PLAYER_SYNC) == 8, "at most eight 0x46 per second"))
		return false;
	PlayerSync rec;
	bool decoded = false;
	for (const ns::Datagram &d : first) {
		if (d.tag != s2c::PLAYER_SYNC) continue;
		decoded = decode_player_sync(d.body.data(), d.body.size(), rec);
		break;
	}
	if (!expect(decoded && rec.field_bitmask == kPlayerSyncHasQuality && rec.quality == 0,
			"the resend carries field 0x0400 with the stored level"))
		return false;
	// The eighth send breaks BEFORE the cursor advance, so the walk resumes on
	// the (now clean) eighth slot [orig: @0x51DF1B precedes @0x51DF2F].
	if (!expect(f.ctx.quality_broadcast_slot_cursor == 7, "the cursor persists across seconds"))
		return false;
	inmatch::Server_EmitQualityResends(f.ctx, f.world);
	std::vector<ns::Datagram> second = drain(f.transports[0]);
	if (!expect(count_tag(second, s2c::PLAYER_SYNC) == 2, "the next second drains the rest"))
		return false;
	for (size_t i = 0; i < 10; ++i)
		if (!expect(!f.conn(i).reply.client_quality_dirty, "every slot is clean after two walks"))
			return false;
	// The pre-round timer holds the walk.
	f.conn(3).reply.client_quality_dirty = true;
	f.world.preround_delay_seconds = 5;
	inmatch::Server_EmitQualityResends(f.ctx, f.world);
	return expect(f.conn(3).reply.client_quality_dirty, "no resend during pre-round");
}

bool check_host_quality_window() {
	HostFixture f(2);
	// A healthy measured frame rate (the session's FR counter): the
	// frame-pressure term scores its floor 1.
	f.ctx.stats_avg_fps = 62;
	// The sample fires on the first frame, then every 62.
	f.ctx.host_quality_window = ns::NetQualityWindow{};
	inmatch::Server_SampleHostNetQuality(f.ctx);
	if (!expect(f.ctx.net_quality_sample_countdown == 62, "the countdown reloads 62"))
		return false;
	// One slot mature with a 1000 ms mean ping: 255/1000*1000 -> 255, /5 = 51.
	f.conn(0).reply.control_live_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	for (uint32_t i = 0; i < 10; ++i) f.conn(0).reply.rtt_ring[i] = 1000;
	f.ctx.net_quality_sample_countdown = 1;
	inmatch::Server_SampleHostNetQuality(f.ctx);
	// The per-slot mean clamps at 255 before the scale: 255 * 255 / 1000 = 65;
	// the first (idle, no eligible slot) sample stored 0, so the five-slot
	// average is 65 / 5 = 13,
	// which the max-fold selects over the floor-1 bandwidth/loss terms.
	if (!expect(f.ctx.host_network_quality == 13,
			"0x79 carries the send window's folded quality"))
		return false;
	// Frame pressure reads the FR counter [orig: @0x4C531B g_StatsAvgFps]:
	// 8 fps scores 256 - 128 = 128, 0 (before the first 2 s window) the
	// ceiling 255. One sample each into a fresh window averages /5.
	HostFixture slow(2);
	slow.ctx.stats_avg_fps = 8;
	slow.ctx.host_quality_window = ns::NetQualityWindow{};
	inmatch::Server_SampleHostNetQuality(slow.ctx);
	if (!expect(slow.ctx.host_quality_window.avg_bandwidth == 128 / 5 &&
					slow.ctx.host_network_quality == 128 / 5,
			"an 8 fps host samples frame pressure 128"))
		return false;
	HostFixture cold(2);
	cold.ctx.host_quality_window = ns::NetQualityWindow{};
	inmatch::Server_SampleHostNetQuality(cold.ctx);
	return expect(cold.ctx.host_network_quality == 255 / 5,
			"an unmeasured frame rate samples the ceiling 255");
}

// The NovaWorld exit leads the host's 62-frame block too (D-NET-220): a
// NovaWorld host whose registration's NWU session is in use exits with 12 once
// the word holds neither 2 nor 3, and HostRole reports it on either host kind
// [orig: Game_ProcessMainFrame @0x52655d..0x52657c].
bool check_host_novaworld_exit() {
	const auto sample = [](inmatch::NetworkType type, bool in_use, int32_t role, bool in_session) {
		HostFixture f(1);
		f.ctx.transport_mode = type;
		f.ctx.nwu_in_use = in_use;
		f.ctx.nwu_session_role = role;
		f.ctx.is_in_session = in_session ? 1 : 0;
		f.ctx.net_quality_sample_countdown = 1;
		inmatch::Server_SampleHostNetQuality(f.ctx);
		return f.ctx.mission_exit_reason;
	};
	using NT = inmatch::NetworkType;
	if (!expect(sample(NT::NovaWorld, true, 2, true) == 0, "a hosting word holds the match"))
		return false;
	if (!expect(sample(NT::NovaWorld, true, 1, true) == inmatch::kMissionExitNovaWorld,
			"a stopped registration (word 1) exits with 12"))
		return false;
	if (!expect(sample(NT::NovaWorld, true, 0, false) == 0, "outside a session the block skips it"))
		return false;
	if (!expect(sample(NT::Lan, true, 0, true) == 0, "a LAN host never takes it"))
		return false;
	return expect(sample(NT::NovaWorld, false, 0, true) == 0, "nor an unused NWU session");
}

// --------------------------------------------------------------------------
// Chat.
// --------------------------------------------------------------------------

bool check_chat_helpers() {
	if (!expect(inmatch::chat_strip_angle_tags("a<b>c</b>d>e") == "acd>e",
			"tags strip; a stray '>' while open is kept"))
		return false;
	if (!expect(inmatch::chat_text_is_int_triplet("1 2 3") &&
					!inmatch::chat_text_is_int_triplet("0 0 0") &&
					!inmatch::chat_text_is_int_triplet("hello 1 2"),
			"the int-triplet rule"))
		return false;
	return true;
}

std::vector<uint8_t> chat_uplink(uint8_t channel, const std::string &text) {
	std::vector<uint8_t> body{channel};
	for (const char ch : text) body.push_back(static_cast<uint8_t>(ch));
	body.push_back(0);
	return body;
}

bool check_chat_routing() {
	// Four players: P1/P3 team 1, P2/P4 team 2.
	HostFixture f(4);
	for (auto &t : f.transports) (void)drain(t);
	// Team chat from P1: P3 (same team) receives it, P2/P4 do not, P1 gets its
	// own copy as the reply.
	std::vector<ProtocolMessage> replies = f.dispatch(0, c2s::CHAT_MESSAGE, chat_uplink(2, "<b>go</b>"));
	if (!expect(replies.size() == 1 && replies[0].tag == s2c::CHAT_BROADCAST,
			"the sender's own copy rides the reply"))
		return false;
	ChatBroadcast own;
	if (!expect(decode_chat_broadcast(replies[0].payload.data(), replies[0].payload.size(), own) &&
					own.channel == 2 && own.sender_slot == 1 && own.text == "P1: go",
			"the line is name-prefixed and stripped"))
		return false;
	if (!expect(count_tag(drain(f.transports[2]), s2c::CHAT_BROADCAST) == 1 &&
					count_tag(drain(f.transports[1]), s2c::CHAT_BROADCAST) == 0 &&
					count_tag(drain(f.transports[3]), s2c::CHAT_BROADCAST) == 0,
			"channel 2 reaches the sender's team only"))
		return false;
	// The 1000 ms throttle drops a second message on the same tick.
	replies = f.dispatch(0, c2s::CHAT_MESSAGE, chat_uplink(2, "again"));
	if (!expect(replies.empty() && drain(f.transports[2]).empty(), "the per-sender throttle"))
		return false;
	// Channel 5 (side A = team 1) from P2 after the throttle window: P1/P3
	// receive with the wire channel 2; P2 itself is not on side A.
	replies = f.dispatch(1, c2s::CHAT_MESSAGE, chat_uplink(5, "hi"), 100 + 70);
	if (!expect(replies.empty(), "a side chat from the other side has no own copy"))
		return false;
	std::vector<ns::Datagram> to_p1 = drain(f.transports[0]);
	ChatBroadcast side;
	if (!expect(count_tag(to_p1, s2c::CHAT_BROADCAST) == 1 &&
					decode_chat_broadcast(to_p1[0].body.data(), to_p1[0].body.size(), side) &&
					side.channel == 2 && side.text == "P2: hi",
			"channel 5 fans to side A with the wire channel 2"))
		return false;
	(void)drain(f.transports[2]);
	// An int triplet reaches only the sender.
	replies = f.dispatch(2, c2s::CHAT_MESSAGE, chat_uplink(1, "1 2 3"), 100 + 140);
	if (!expect(replies.size() == 1 && drain(f.transports[0]).empty() &&
					drain(f.transports[1]).empty(),
			"a coordinate triplet is echoed to the sender alone"))
		return false;
	// An unknown channel sends nothing.
	replies = f.dispatch(3, c2s::CHAT_MESSAGE, chat_uplink(7, "x"), 100 + 210);
	if (!expect(replies.empty() && drain(f.transports[0]).empty(), "channel 7 is dropped"))
		return false;
	// A spectator sender is refused.
	f.conn(0).link.spectator = true;
	replies = f.dispatch(0, c2s::CHAT_MESSAGE, chat_uplink(1, "x"), 100 + 280);
	return expect(replies.empty(), "a spectator cannot chat");
}

// --------------------------------------------------------------------------
// 0x3D, 0x42 -> 0x70, 0x51.
// --------------------------------------------------------------------------

bool check_loaded_model_reply_stamp() {
	HostFixture f(1);
	(void)f.dispatch(0, c2s::LOADED_MODEL_PAGE_REPLY, {}, 777);
	return expect(f.conn(0).reply.loaded_model_reply_frame == 777, "0x3D stamps the frame clock");
}

bool check_vehicle_spawn_availability() {
	HostFixture f(1);
	inmatch::NapiNPServerCtx::VehicleSpawnLimitRow row;
	row.type_id = 7;
	row.type_cap = 3;
	row.per_team_flag = 0;
	row.team_slots[1] = 2;
	f.ctx.vehicle_spawn_limits.push_back(row);
	inmatch::NapiNPServerCtx::VehicleSpawnLimitRow open;
	open.type_id = 9;
	f.ctx.vehicle_spawn_limits.push_back(open);
	// One live team-1 deployable of type 7.
	w::Entity seed;
	seed.item_id = 7;
	seed.team = 1;
	seed.has_item_def = true;
	seed.flags = 0x1000u;
	seed.kind = w::EntityKind::Item;
	(void)f.world.registry.spawn(1, seed);
	if (!expect(inmatch::Server_CountEntitiesByTypeAndTeam(f.world, 7, 1) == 1,
			"the live deployable counts"))
		return false;
	// The stock host config's unlimited_vehicles reads every row unlimited.
	// [orig: g_RulesUnlimitedVehicles @0x5105F5..0x5105FF; Config_SetDefaults @0x54D352]
	std::vector<ProtocolMessage> replies = f.dispatch(0, c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST, {});
	const std::vector<uint8_t> unlimited = {3, 7, 0, 0xFF, 0xFF, 9, 0, 0xFF, 0xFF, 0, 0};
	if (!expect(replies.size() == 1 && replies[0].tag == s2c::VEHICLE_SPAWN_AVAILABILITY &&
					replies[0].payload == unlimited,
			"0x70 carries FF/FF for every row under the stock unlimited_vehicles"))
		return false;
	f.ctx.config.unlimited_vehicles = false;
	replies = f.dispatch(0, c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST, {});
	const std::vector<uint8_t> want = {3, 7, 0, 2, 2, 9, 0, 0xFF, 0xFF, 0, 0};
	if (!expect(replies.size() == 1 && replies[0].tag == s2c::VEHICLE_SPAWN_AVAILABILITY &&
					replies[0].payload == want,
			"0x70 carries min(cap - live, team slots) / team slots, then FF/FF for an open row"))
		return false;
	// The spawn gate consumes a finite team slot.
	if (!expect(inmatch::Server_VehicleSpawnAllowed(f.ctx, f.world, 7, 1, true) &&
					f.ctx.vehicle_spawn_limits[0].team_slots[1] == 1,
			"an allowed spawn consumes a team slot"))
		return false;
	return expect(!inmatch::Server_VehicleSpawnAllowed(f.ctx, f.world, 7, 2, false),
			"a team without slots is refused");
}

bool check_spectator_respawn_request() {
	HostFixture f(2);
	// Gates: permanent death and a spectator capacity.
	(void)f.dispatch(0, c2s::SPECTATOR_RESPAWN, {0xFF, 0xFF});
	if (!expect(!f.conn(0).reply.spectator_convert_pending, "no permanent death: refused"))
		return false;
	f.ctx.config.permanent_death = true;
	f.ctx.config.spectator_slots = 1;
	(void)f.dispatch(0, c2s::SPECTATOR_RESPAWN, {0xFF, 0xFF});
	if (!expect(f.conn(0).reply.spectator_convert_pending, "the request is admitted")) return false;
	for (auto &t : f.transports) (void)drain(t);
	inmatch::Server_TickUpdate(f.ctx);
	if (!expect(f.conn(0).link.spectator, "the host tick converts the slot")) return false;
	const w::Entity *player = f.world.registry.get(f.players[0]);
	if (!expect(player != nullptr && player->team == 0 && player->health == 1,
			"the convert clears the team and leaves health 1"))
		return false;
	std::vector<ns::Datagram> to_p2 = drain(f.transports[1]);
	const std::vector<uint8_t> want = {5, 'P', '1', 0};
	bool saw = false;
	for (const ns::Datagram &d : to_p2)
		if (d.tag == s2c::FORMATTED_GAME_TEXT && d.body == want) saw = true;
	return expect(saw, "S2C 0x32 [5][name] reaches the other player");
}

// The S2C 0x32 join and leave lines: [1][name][team] from the player add,
// [2][name][team] from the disconnect, each to every OTHER in-match
// connection [orig: Server_PlayerAdd @0x51d21e..0x51d291;
// Server_HandlePlayerDisconnect @0x51b69b..0x51b6d3].
bool check_join_leave_lines() {
	HostFixture f(3);
	for (auto &t : f.transports) (void)drain(t);
	inmatch::broadcast_player_joined_text(f.ctx.np_protocol.connection_list,
			f.conn(0), &f.world);
	const std::vector<uint8_t> joined = {1, 'P', '1', 0, 1};
	const auto count = [](std::vector<ns::Datagram> dgs, const std::vector<uint8_t> &want) {
		int n = 0;
		for (const ns::Datagram &d : dgs)
			if (d.tag == s2c::FORMATTED_GAME_TEXT && d.body == want) ++n;
		return n;
	};
	if (!expect(count(drain(f.transports[0]), joined) == 0, "the joiner gets no join line"))
		return false;
	if (!expect(count(drain(f.transports[1]), joined) == 1 &&
					count(drain(f.transports[2]), joined) == 1,
			"every other in-match player gets [1][name][team]"))
		return false;
	inmatch::broadcast_player_leaving_text(f.ctx.np_protocol.connection_list, f.conn(1),
			&f.world);
	const std::vector<uint8_t> leaving = {2, 'P', '2', 0, 2};
	if (!expect(count(drain(f.transports[1]), leaving) == 0, "the leaver gets no line"))
		return false;
	return expect(count(drain(f.transports[0]), leaving) == 1 &&
					count(drain(f.transports[2]), leaving) == 1,
			"every other in-match player gets [2][name][team]");
}

// --------------------------------------------------------------------------
// The revive transaction.
// --------------------------------------------------------------------------

bool check_medic_revive_transaction() {
	HostFixture f(2, 1);
	f.conn(1).link.owned_entity = f.players[1];
	w::Entity *victim = f.world.registry.get(f.players[0]);
	w::Entity *healer = f.world.registry.get(f.players[1]);
	healer->team = 1;
	healer->player_class = 5;
	victim->flags |= w::kEntityFlagDead;
	victim->health = 0;
	victim->alive = false;
	victim->position = {12.0f, 34.0f, 5.0f};
	// The body's own position words: the entity update mirrors them onto the row.
	if (w::AiEntity *body = f.world.ai.for_handle(f.players[0])) {
		body->pos[0] = 12 << 16;
		body->pos[1] = 34 << 16;
		body->pos[2] = 5 << 16;
		body->health = 0;
	}
	w::MatchRules medic_rules;
	medic_rules.game_type = 0x10000u; // TDM
	medic_rules.score_values.emplace();
	(*medic_rules.score_values)[7] = 4; // MEDICSAVE
	f.world.match.configure(medic_rules);
	f.world.match.upsert_player({f.players[1], 2, "P2"});
	// A normal other-player kill: the 120-second revive window is open, the
	// respawn penalty runs and the victim has called for a medic.
	f.conn(0).link.downed_revive_seconds = 120;
	f.conn(0).link.respawn_delay_seconds = 5;
	f.conn(0).link.medic_request_active = true;
	for (auto &t : f.transports) (void)drain(t);
	f.world.round_sim.medic_interactions.push_back(
			w::MedicInteraction{f.players[0], f.players[1], /*revive=*/true});
	inmatch::Server_TickUpdate(f.ctx);
	if (!expect(victim->medic_reviving, "the victim's +0x1E0 latch is set")) return false;
	// The revive closes the victim slot's window, penalty and request at its
	// retail points [orig: @0x517D61, @0x517D67, @0x517E14, @0x517E1A].
	if (!expect(f.conn(0).link.downed_revive_seconds == 0 &&
					f.conn(0).link.respawn_delay_seconds == 0 &&
					!f.conn(0).link.medic_request_active,
			"the revive closes the window, the respawn penalty and the medic request"))
		return false;
	// The revive scores the medic's MEDICSAVE [orig: GameEvent_RevivePlayer
	// @0x517CD0 (the event-6 call @0x517DC5)].
	const w::MatchPlayer *medic = f.world.match.player(f.players[1]);
	if (!expect(medic != nullptr && medic->stats[w::MatchStats::kMedicSaves] == 1 &&
					medic->stats[w::MatchStats::kPoints] == 4 &&
					f.world.match.team_stats(1)[w::MatchStats::kMedicSaves] == 1,
			"the revive scores event 6 on the medic and its team row"))
		return false;
	if (!expect(f.conn(0).reply.revive_pose_valid &&
					f.conn(0).reply.revive_pos[2] == (5 << 16) + 0x4000,
			"the revive pose is saved raised 0x4000"))
		return false;
	std::vector<ns::Datagram> to_victim = drain(f.transports[0]);
	int reviving_at = -1, seed_at = -1, event_at = -1;
	for (size_t i = 0; i < to_victim.size(); ++i) {
		const ns::Datagram &d = to_victim[i];
		if (d.tag == s2c::MEDIC_REVIVING && reviving_at < 0) reviving_at = int(i);
		if (d.tag == s2c::TICK_SEED && reviving_at >= 0 && seed_at < 0) seed_at = int(i);
		if (d.tag == s2c::GAME_EVENT && d.body.size() == 8 && d.body[0] == 38) event_at = int(i);
	}
	if (!expect(reviving_at >= 0 && seed_at > reviving_at && event_at > seed_at,
			"0x3A, then 0x61, then 0x1E event 38 on the victim"))
		return false;
	const ns::Datagram &ev = to_victim[static_cast<size_t>(event_at)];
	const std::vector<uint8_t> want = {38, uint8_t(f.players[0].slot()), uint8_t(f.players[1].slot()),
			0xFF, 12, 0, 34, 0};
	if (!expect(ev.body == want, "event 38 carries victim, healer, 0xFF, x, y")) return false;
	// The other player sees the event too (mask 128).
	return expect(count_tag(drain(f.transports[1]), s2c::GAME_EVENT) >= 1,
			"event 38 reaches every active player");
}

// The revive's head gates and the healer's hide bytes: both entities need a
// player slot, the victim's revive window must be open, a victim who turned
// automedic off needs a live medic request, and the victim's last attacker is
// neither the healer nor the victim; a hidden (spectator) medic still revives
// but neither scores nor sends event 38.
// [orig: GameEvent_RevivePlayer @0x517CD0 — the slots @0x517CD9..0x517D01, the
//  window @0x517D14..0x517D1A, the opt-out @0x517D20..0x517D2E, the last
//  attacker @0x517D34..0x517D44, the hide bytes @0x517E4C..0x517E61]
bool check_medic_revive_gates() {
	auto setup = [](HostFixture &f) {
		f.conn(1).link.owned_entity = f.players[1];
		w::Entity *victim = f.world.registry.get(f.players[0]);
		w::Entity *healer = f.world.registry.get(f.players[1]);
		healer->team = 1;
		healer->player_class = 5;
		victim->flags |= w::kEntityFlagDead;
		victim->health = 0;
		victim->alive = false;
		f.conn(0).link.downed_revive_seconds = 120;
		w::MatchRules rules;
		rules.game_type = 0x10000u; // TDM
		rules.score_values.emplace();
		(*rules.score_values)[7] = 4; // MEDICSAVE
		f.world.match.configure(rules);
		f.world.match.upsert_player({f.players[1], 2, "P2"});
		for (auto &t : f.transports) (void)drain(t);
	};
	auto revive = [](HostFixture &f, w::EntityHandle healer) {
		f.world.round_sim.medic_interactions.push_back(
				w::MedicInteraction{f.players[0], healer, /*revive=*/true});
		inmatch::Server_RouteMedicInteractions(f.ctx, f.world);
	};
	auto reviving = [](HostFixture &f) {
		return f.world.registry.get(f.players[0])->medic_reviving;
	};
	bool ok = true;
	{
		HostFixture f(2, 1);
		setup(f);
		f.conn(1).link.spectator = true;
		f.world.match.set_player_spectator(f.players[1], true);
		revive(f, f.players[1]);
		const std::vector<ns::Datagram> to_victim = drain(f.transports[0]);
		const std::vector<ns::Datagram> to_healer = drain(f.transports[1]);
		size_t event38 = 0;
		for (const std::vector<ns::Datagram> *list : {&to_victim, &to_healer})
			for (const ns::Datagram &d : *list)
				if (d.tag == s2c::GAME_EVENT && !d.body.empty() && d.body[0] == 38) ++event38;
		const w::MatchPlayer *medic = f.world.match.player(f.players[1]);
		ok = expect(reviving(f) && count_tag(to_victim, s2c::MEDIC_REVIVING) == 1 &&
		                    count_tag(to_victim, s2c::TICK_SEED) == 1 && event38 == 0 &&
		                    medic != nullptr && medic->stats[w::MatchStats::kMedicSaves] == 0,
		            "a hidden medic revives without event 38 or a MEDICSAVE") && ok;
	}
	{
		HostFixture f(2, 1);
		setup(f);
		f.conn(0).link.downed_revive_seconds = 0;
		revive(f, f.players[1]);
		ok = expect(!reviving(f) && count_tag(drain(f.transports[0]), s2c::MEDIC_REVIVING) == 0,
		            "a closed revive window refuses the revive") && ok;
	}
	{
		HostFixture f(2, 1);
		setup(f);
		f.conn(0).link.auto_medic_enabled = false;
		revive(f, f.players[1]);
		ok = expect(!reviving(f), "automedic off without a medic request refuses the revive") && ok;
		f.conn(0).link.medic_request_active = true;
		revive(f, f.players[1]);
		ok = expect(reviving(f) && !f.conn(0).link.medic_request_active,
		            "automedic off with a live request revives and clears the request") && ok;
	}
	{
		HostFixture f(2, 1);
		setup(f);
		f.world.registry.get(f.players[0])->last_attacker = f.players[1];
		revive(f, f.players[1]);
		ok = expect(!reviving(f), "a medic never revives the teammate he killed") && ok;
		f.world.registry.get(f.players[0])->last_attacker = f.players[0];
		revive(f, f.players[1]);
		ok = expect(!reviving(f), "nobody revives a suicide") && ok;
	}
	{
		HostFixture f(2, 1);
		setup(f);
		w::Entity bot;
		bot.kind = w::EntityKind::Organic;
		bot.team = 1;
		bot.player_class = 5;
		bot.alive = true;
		const w::EntityHandle bot_handle = f.world.registry.spawn(0, bot);
		revive(f, bot_handle);
		ok = expect(!reviving(f), "a healer without a player slot never revives") && ok;
	}
	return ok;
}

// The heal transaction: a live, hurt teammate's health goes back to its def
// max, the medic scores MEDICHEAL (event 5) and every active player gets S2C
// 0x1E event 45 [patient][medic][0xFF][x][y]. A hidden (spectator) medic still
// restores the health but neither scores nor sends; a medic of another class
// and a patient without a player slot are refused.
// [orig: GameEvent_HealPlayer @0x50DE30 — the class @0x50DE58, the victim's
//  slot @0x50DE66..0x50DE70, the restore @0x50DE77..0x50DE7F, the hide bytes
//  @0x50DE86..0x50DE96, scoring @0x50DEA4, 0x1E @0x50DEAC..0x50DF02]
bool check_medic_heal_transaction() {
	HostFixture f(2, 1);
	w::Entity *patient = f.world.registry.get(f.players[0]);
	w::Entity *medic = f.world.registry.get(f.players[1]);
	medic->team = 1;
	medic->player_class = 5;
	patient->has_item_def = true;
	patient->health_max = 150;
	patient->health = 40;
	patient->position = {12.0f, 34.0f, 5.0f};
	w::MatchRules heal_rules;
	heal_rules.game_type = 0x10000u; // TDM
	heal_rules.score_values.emplace();
	(*heal_rules.score_values)[6] = 3; // MEDICHEAL
	f.world.match.configure(heal_rules);
	f.world.match.upsert_player({f.players[0], 1, "P1"});
	f.world.match.upsert_player({f.players[1], 2, "P2"});
	auto heal = [&f](w::EntityHandle target) {
		for (auto &t : f.transports) (void)drain(t);
		f.world.round_sim.medic_interactions.push_back(
				w::MedicInteraction{target, f.players[1], /*revive=*/false});
		inmatch::Server_RouteMedicInteractions(f.ctx, f.world);
	};
	auto event45_everywhere = [&f]() {
		size_t seen = 0;
		for (auto &t : f.transports)
			for (const ns::Datagram &d : drain(t))
				if (d.tag == s2c::GAME_EVENT && !d.body.empty() && d.body[0] == 45) ++seen;
		return seen;
	};
	heal(f.players[0]);
	bool ok = expect(patient->health == 150, "the heal restores the def max");
	const w::MatchPlayer *scorer = f.world.match.player(f.players[1]);
	ok = expect(scorer != nullptr && scorer->stats[w::MatchStats::kMedicHeals] == 1 &&
	                    scorer->stats[w::MatchStats::kPoints] == 3 &&
	                    f.world.match.team_stats(1)[w::MatchStats::kMedicHeals] == 1,
	            "the heal scores event 5 on the medic and its team row") && ok;
	const std::vector<uint8_t> want = {45, uint8_t(f.players[0].slot()),
			uint8_t(f.players[1].slot()), 0xFF, 12, 0, 34, 0};
	bool both = true;
	for (auto &t : f.transports) {
		bool saw = false;
		for (const ns::Datagram &d : drain(t))
			if (d.tag == s2c::GAME_EVENT && d.body == want) saw = true;
		both = both && saw;
	}
	ok = expect(both, "event 45 [patient][medic][0xFF][x][y] reaches every active player") && ok;

	// A hidden (spectator) medic: the restore runs, the score and the event do not.
	patient->health = 40;
	f.conn(1).link.spectator = true;
	f.world.match.set_player_spectator(f.players[1], true);
	heal(f.players[0]);
	ok = expect(patient->health == 150 && event45_everywhere() == 0 &&
	                    scorer->stats[w::MatchStats::kMedicHeals] == 1,
	            "a hidden medic restores the health without a score or an event") && ok;
	f.conn(1).link.spectator = false;
	f.world.match.set_player_spectator(f.players[1], false);

	// Another class: refused before the restore.
	patient->health = 40;
	medic->player_class = 1;
	heal(f.players[0]);
	ok = expect(patient->health == 40 && event45_everywhere() == 0,
	            "a medic of another class is refused") && ok;
	medic->player_class = 5;

	// A patient without a player slot: refused before the restore.
	w::Entity bot;
	bot.kind = w::EntityKind::Organic;
	bot.has_item_def = true;
	bot.item_type = 3;
	bot.team = 1;
	bot.health = 40;
	bot.health_max = 150;
	bot.alive = true;
	const w::EntityHandle bot_handle = f.world.registry.spawn(0, bot);
	heal(bot_handle);
	ok = expect(f.world.registry.get(bot_handle)->health == 40 && event45_everywhere() == 0,
	            "a patient without a player slot is refused") && ok;
	return ok;
}

// --------------------------------------------------------------------------
// The shared 0x45/0x85 ping body codec.
// --------------------------------------------------------------------------

bool check_session_ping_codec() {
	const std::vector<uint8_t> body = build_session_ping_body(0xCAFEBABEu, true, 0x01020304u);
	const std::vector<uint8_t> want = {
			0xBE, 0xBA, 0xFE, 0xCA,
			'W', 'R', 0, 1, 0, 1,
			'M', 'S', 0, 4, 0, 0x04, 0x03, 0x02, 0x01};
	if (!expect(body == want, "the ping body is [key][WR u8][MS u32] as flat TLVs")) return false;
	SessionPingBody parsed;
	if (!expect(parse_session_ping_body(body.data(), body.size(), parsed) &&
					parsed.receiver_local_key == 0xCAFEBABEu && parsed.wants_reply &&
					parsed.timestamp_ms == 0x01020304u,
			"the body round-trips"))
		return false;
	// Tags compare case-insensitively; a value short of its width loads its bytes and those
	// after it, zero past the body's end (D-NET-415) [orig: Nwu_HandlePing @0x623A70 - MS
	// @0x623C2A].
	const std::vector<uint8_t> odd = {1, 0, 0, 0, 'w', 'r', 0, 1, 0, 0, 'm', 's', 0, 2, 0, 9, 9};
	if (!expect(parse_session_ping_body(odd.data(), odd.size(), parsed) &&
					parsed.receiver_local_key == 1 && !parsed.wants_reply &&
					parsed.timestamp_ms == 0x0909u,
			"lower-case tags parse, a two-byte MS loads its bytes and zero past the end"))
		return false;
	const std::vector<uint8_t> tiny = {1, 2, 3};
	return expect(!parse_session_ping_body(tiny.data(), tiny.size(), parsed),
			"a body without the key dword is rejected");
}

// --------------------------------------------------------------------------
// The NovaWorld ServerCommand verbs.
// --------------------------------------------------------------------------

bool check_server_commands() {
	HostFixture f(2, 1);
	for (auto &t : f.transports) (void)drain(t);
	// TextChatServer: channel 10 from no slot to every in-game player.
	inmatch::ServerCommandOutcome out = inmatch::Server_ExecuteServerCommand(
			f.ctx, &f.world, "TextChatServer", "", {"hi"});
	const std::vector<uint8_t> chat = {10, 0xFF, 'h', 'i', 0};
	bool both = out.handled;
	for (auto &t : f.transports) {
		bool saw = false;
		for (const ns::Datagram &d : drain(t))
			if (d.tag == s2c::CHAT_BROADCAST && d.body == chat) saw = true;
		both = both && saw;
	}
	if (!expect(both, "TextChatServer fans channel 10 / slot 0xFF to both players")) return false;
	// TextChatPlayerByName reaches only the named slot; CmdEchoPlayer is channel 14.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "TextChatPlayer", "ByName", {"P2", "you"});
	if (!expect(out.handled && drain(f.transports[0]).empty(),
			"TextChatPlayerByName sends nothing to the other slot"))
		return false;
	std::vector<ns::Datagram> to_p2 = drain(f.transports[1]);
	const std::vector<uint8_t> private_chat = {10, 0xFF, 'y', 'o', 'u', 0};
	if (!expect(to_p2.size() == 1 && to_p2[0].body == private_chat, "TextChatPlayerByName reaches P2"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "CmdEchoPlayer", "ByIndex", {"2", "echo"});
	to_p2 = drain(f.transports[1]);
	if (!expect(out.handled && to_p2.size() == 1 && to_p2[0].body[0] == 14,
			"CmdEchoPlayerByIndex is channel 14 to slot 2"))
		return false;
	// "*02" and an ambiguous / unknown name.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "TextChatPlayer", "ByName", {"*02", "star"});
	if (!expect(out.handled && drain(f.transports[1]).size() == 1, "*NN resolves the slot index")) return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "TextChatPlayer", "ByName", {"P9", "lost"});
	if (!expect(!out.handled, "an unknown callsign resolves nothing")) return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "PuntPlayer", "", {"P2"});
	if (!expect(!out.handled && !f.conn(1).host_disconnect_sent, "a targeted verb without a suffix is a no-op"))
		return false;
	// SetServerName / SetServerMsg / SetMPReset change the config and ask for a save.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "SetServerName", "", {"Renamed Host"});
	if (!expect(out.handled && out.config_changed && f.ctx.config.server_name == "Renamed Host" &&
					f.ctx.np_protocol.session_name == "Renamed Host",
			"SetServerName renames the session and the wire name"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "setservermsg", "", {"Welcome"});
	if (!expect(out.handled && f.ctx.config.custom_text == "Welcome", "SetServerMsg is case-insensitive"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "SetMPReset", "", {"3"});
	if (!expect(out.handled && f.ctx.config.multiplayer_reset == 3, "SetMPReset stores its argument"))
		return false;
	// Earthquake / TimeOfDay / Lightning reach the world and the wire.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "Earthquake", "", {"10"});
	if (!expect(out.handled && f.world.weather.quake_ticks == 60u, "Earthquake 10 = 60 quake ticks"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "Earthquake", "", {"99"});
	if (!expect(f.world.weather.quake_ticks == 240u, "Earthquake clamps to 40 seconds")) return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "TimeOfDay", "", {"0630"});
	if (!expect(out.handled && f.world.weather.tod_fixed24 == 390u * 0x44444u, "TimeOfDay 0630 = minute 390"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "TimeOfDay", "", {"2400"});
	if (!expect(f.world.weather.tod_fixed24 == 720u * 0x44444u, "an out-of-range TimeOfDay is noon"))
		return false;
	for (auto &t : f.transports) (void)drain(t);
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "Lightning", "", {});
	const std::vector<uint8_t> flash = {'S', 'E', 'T', 'F', 'L', 'A', 'S', 'H', '1', ' ', '1', '6', 0};
	bool flashed = out.handled;
	for (auto &t : f.transports) {
		bool saw = false;
		for (const ns::Datagram &d : drain(t))
			if (d.tag == s2c::TEXT_COMMAND && d.body == flash) saw = true;
		flashed = flashed && saw;
	}
	if (!expect(flashed, "Lightning broadcasts the SETFLASH1 16 text command")) return false;
	// KillPlayer zeroes the health and queues the death.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "KillPlayer", "ByIndex", {"2"});
	const w::Entity *p2 = f.world.registry.get(f.players[1]);
	if (!expect(out.handled && p2 != nullptr && p2->health == 0 && f.world.round_sim.deaths.size() == 1 &&
					f.world.round_sim.deaths[0].victim == f.players[1],
			"KillPlayerByIndex kills slot 2"))
		return false;
	// EndMission ends the round with the named winner and shortens the linger.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "EndMission", "", {"Red"});
	if (!expect(out.handled && f.world.match.outcome().ended && f.ctx.round_end_linger_override_ticks == 620,
			"EndMission Red ends the round with the 620-tick linger pending"))
		return false;
	// PuntPlayer on a remote punts it; on the host's own slot it stops hosting.
	HostFixture g(2, 1);
	out = inmatch::Server_ExecuteServerCommand(g.ctx, &g.world, "PuntPlayer", "ByName", {"P2"});
	if (!expect(out.handled && g.conn(1).host_disconnect_sent && !g.conn(0).host_disconnect_sent,
			"PuntPlayerByName punts the named remote"))
		return false;
	g.conn(0).link.mode = ns::TransportMode::Loopback;
	out = inmatch::Server_ExecuteServerCommand(g.ctx, &g.world, "PuntPlayer", "ByIndex", {"1"});
	if (!expect(out.handled && out.stop_hosting && !g.conn(0).host_disconnect_sent,
			"PuntPlayer on the host's own slot asks the shell to stop hosting"))
		return false;
	// ReloadPlayer is not modeled.
	out = inmatch::Server_ExecuteServerCommand(g.ctx, &g.world, "ReloadPlayer", "ByName", {"P1"});
	return expect(!out.handled, "ReloadPlayer is not modeled");
}

// The ServerCommand gates are per verb, as retail's arms test them: every verb but SetMPReset
// needs the authority with its hosted session up; the player-targeted verbs and ChangeTeam /
// SwapTeam also need the player table (a World here); a verb that reads an argument it cannot
// default needs its token; SetMPReset tests its token alone and so runs on any receiver.
// [orig: CNapiGameSession_HandleServerCommand @0x4D22F0 — PuntPlayer @0x4D23C0..0x4D23E7,
//  TextChatServer @0x4D2584..0x4D259F, TextChatPlayer @0x4D263E..0x4D2665, CmdEchoPlayer
//  @0x4D2785..0x4D27AC, KillPlayer @0x4D28CC..0x4D28F3, Earthquake @0x4D2AAA..0x4D2ABC,
//  Lightning @0x4D2B45..0x4D2B57, TimeOfDay @0x4D2BCB..0x4D2BDD, SetServerName
//  @0x4D2CD4..0x4D2CEF, SetServerMsg @0x4D2D69..0x4D2D84, SetMPReset @0x4D2E12, ReloadPlayer
//  @0x4D2E65..0x4D2E8C, DisarmPlayer @0x4D2FAF..0x4D2FD6, Cycle / EndMission / GameOver
//  @0x4D30DE..0x4D30F0, ChangeTeam / SwapTeam @0x4D31EA..0x4D3211]
bool check_server_command_gates() {
	struct Row {
		const char *verb;
		const char *suffix;
		std::vector<std::string> args;
		bool hosting; // needs is_authority and the hosted session
		bool table;   // needs the player table (or, here, a World to act on)
		bool modeled; // false for ReloadPlayer / DisarmPlayer
	};
	const std::vector<Row> rows = {
			{"PuntPlayer", "ByIndex", {"2"}, true, true, true},
			{"TextChatServer", "", {"hi"}, true, false, true},
			{"TextChatPlayer", "ByIndex", {"2", "you"}, true, true, true},
			{"CmdEchoPlayer", "ByIndex", {"2", "echo"}, true, true, true},
			{"KillPlayer", "ByIndex", {"2"}, true, true, true},
			{"ChangeTeam", "ByIndex", {"2"}, true, true, true},
			{"SwapTeam", "ByIndex", {"2"}, true, true, true},
			{"Cycle", "", {}, true, true, true},
			{"EndMission", "", {"Red"}, true, true, true},
			{"GameOver", "", {}, true, true, true},
			{"Earthquake", "", {}, true, true, true},
			{"Lightning", "", {}, true, true, true},
			{"TimeOfDay", "", {}, true, true, true},
			{"SetServerName", "", {"Gated"}, true, false, true},
			{"SetServerMsg", "", {"Gated"}, true, false, true},
			{"SetMPReset", "", {"4"}, false, false, true},
			{"ReloadPlayer", "ByIndex", {"2"}, true, true, false},
			{"DisarmPlayer", "ByIndex", {"2"}, true, true, false},
	};
	// The receiver's state per pass: {authority, in session, a World}.
	struct State {
		bool authority, session, world;
		const char *what;
	};
	const State states[] = {
			{true, true, true, "the hosting authority with its match"},
			{false, true, true, "a receiver that is not the authority"},
			{true, false, true, "an authority whose hosted session is down"},
			{false, false, false, "no authority, no session, no World"},
			{true, true, false, "the hosting authority with no player table"},
	};
	for (const State &s : states) {
		for (const Row &row : rows) {
			HostFixture f(2, 1);
			f.ctx.is_authority = s.authority ? 1 : 0;
			f.ctx.is_in_session = s.session ? 1 : 0;
			f.ctx.config.multiplayer_reset = 0;
			const inmatch::ServerCommandOutcome out = inmatch::Server_ExecuteServerCommand(
					f.ctx, s.world ? &f.world : nullptr, row.verb, row.suffix, row.args);
			const bool want = row.modeled && (!row.hosting || (s.authority && s.session)) &&
			                  (!row.table || s.world);
			if (!expect(out.handled == want, "each verb runs exactly under its own gates")) {
				std::fprintf(stderr, "  %s%s under %s: handled %d, want %d\n", row.verb, row.suffix, s.what,
				             int(out.handled), int(want));
				return false;
			}
		}
	}
	// SetMPReset outside the authority and the session still stores its argument and asks for
	// the save (atol, then Game_SaveConfig) [orig: @0x4D2E1B..0x4D2E2D].
	HostFixture peer(0, 1);
	peer.ctx.is_authority = 0;
	peer.ctx.is_in_session = 0;
	inmatch::ServerCommandOutcome out =
			inmatch::Server_ExecuteServerCommand(peer.ctx, nullptr, "SetMPReset", "", {"7"});
	if (!expect(out.handled && out.config_changed && peer.ctx.config.multiplayer_reset == 7,
			"SetMPReset runs on a receiver that is neither the authority nor in session"))
		return false;
	// Its one gate is the token count [orig: `cmp edi, 1; jle` @0x4D2E12].
	out = inmatch::Server_ExecuteServerCommand(peer.ctx, nullptr, "SetMPReset", "", {});
	if (!expect(!out.handled && !out.config_changed && peer.ctx.config.multiplayer_reset == 7,
			"SetMPReset without its argument is the no-op tail"))
		return false;
	// The numbers are the CRT atol's, 32-bit on every host: a value past the int32 range
	// saturates (an LP64 strtol narrowed "4294967298" to slot 2 and "4294967296" to 0), and an
	// index past the slot capacity is no slot rather than a byte that wraps onto one.
	// [orig: _atol @0x76AB0A; PlayerState_GetByIndex @0x500850..0x500861]
	HostFixture wide(2, 1);
	out = inmatch::Server_ExecuteServerCommand(wide.ctx, &wide.world, "PuntPlayer", "ByIndex",
			{"4294967298"});
	if (!expect(!out.handled && !wide.conn(0).host_disconnect_sent && !wide.conn(1).host_disconnect_sent,
			"PuntPlayerByIndex 4294967298 saturates to INT32_MAX and punts nobody"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(wide.ctx, &wide.world, "PuntPlayer", "ByIndex", {"258"});
	if (!expect(!out.handled && !wide.conn(1).host_disconnect_sent,
			"PuntPlayerByIndex 258 is past the slot capacity and does not wrap onto slot 2"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(wide.ctx, &wide.world, "SetMPReset", "", {"4294967296"});
	if (!expect(out.handled && wide.ctx.config.multiplayer_reset == INT32_MAX,
			"SetMPReset 4294967296 saturates to INT32_MAX, a nonzero mpreset"))
		return false;
	out = inmatch::Server_ExecuteServerCommand(wide.ctx, &wide.world, "SetMPReset", "", {"-4294967296"});
	if (!expect(out.handled && wide.ctx.config.multiplayer_reset == INT32_MIN,
			"SetMPReset -4294967296 saturates to INT32_MIN"))
		return false;
	// The string verbs' token gate holds under the hosting gate too.
	HostFixture host(2, 1);
	out = inmatch::Server_ExecuteServerCommand(host.ctx, &host.world, "SetServerName", "", {});
	if (!expect(!out.handled, "SetServerName without its argument is the no-op tail")) return false;
	out = inmatch::Server_ExecuteServerCommand(host.ctx, &host.world, "TextChatServer", "", {});
	return expect(!out.handled, "TextChatServer without its text is the no-op tail");
}

// --------------------------------------------------------------------------
// The admin ChangeTeam / SwapTeam verbs, Server_ChangeEntityTeam and the
// team-change list a late joiner's C2S 0x29 walk reads back as S2C 0x51.
// [orig: loc_4D31EA; Server_ChangeEntityTeam @0x518D70;
//  Server_SendPlayerStateAndSquad @0x518B40; NapiNPServerMsg_0x029 @0x514F10]
// --------------------------------------------------------------------------

bool check_change_team() {
	HostFixture f(2, 1);
	f.ctx.config.game_type = 0x10000u; // TDM: team 2 takes side B
	for (size_t i = 0; i < 2; ++i) {
		f.conn(i).assigned_team = static_cast<uint8_t>(1 + i);
		f.conn(i).assigned_team_valid = true;
	}
	inmatch::NapiNPConnection &p1 = f.conn(0);
	p1.char_vars.char_id[0] = 0x0201;
	p1.char_vars.char_id[1] = 0x0302;
	p1.char_vars.avatar[0] = 3;
	p1.char_vars.avatar[1] = 4;
	p1.reply.score_delta_sound_value = 9;
	p1.link.armory_reuse_seconds = 5;
	w::MatchRules rules;
	rules.game_type = 0x10000u;
	f.world.match.configure(rules);
	f.world.match.upsert_player({f.players[0], 1, "P1"});
	w::MatchPlayer *row = f.world.match.player(f.players[0]);
	if (!expect(row != nullptr, "P1 has a roster row")) return false;
	row->stats[w::MatchStats::kDeaths] = 3;
	row->script_vars[0] = 5;
	row->script_vars[16] = 7;
	for (auto &t : f.transports) (void)drain(t);

	inmatch::ServerCommandOutcome out =
			inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "ChangeTeam", "ByName", {"P1"});
	const w::Entity *e = f.world.registry.get(f.players[0]);
	bool ok = expect(out.handled && e->team == 2 && p1.assigned_team == 2,
			"ChangeTeamByName swaps team 1 to 2 on the entity and the slot");
	ok = expect(e->minimap_net_id == 0x0302 && e->anim_slot == 4,
			"the NetId and animSlot come from side B's character vars") && ok;
	ok = expect(row->stats[w::MatchStats::kDeaths] == 0 &&
					row->stats[w::MatchStats::kRoundMarker] == 1 &&
					row->script_vars[16] == 0 && row->script_vars[0] == 5,
			"the stats restart with field 35 = 1 and only script var 16 clears") && ok;
	ok = expect(p1.reply.score_delta_sound_value == 0 && p1.link.armory_reuse_seconds == 0,
			"the score-sound cache and the armory cooldown clear") && ok;
	TeamAssign assigned;
	assigned.entity_handle = f.players[0].packed;
	assigned.team = 2;
	assigned.net_id = 0x0302;
	assigned.anim_slot = 4;
	const std::vector<uint8_t> team_assign = encode_team_assign(assigned);
	const std::string text = "Changing team....";
	std::vector<uint8_t> chat = {10, 0xFF};
	chat.insert(chat.end(), text.begin(), text.end());
	chat.push_back(0);
	const std::vector<ns::Datagram> to_p1 = drain(f.transports[0]);
	std::vector<uint8_t> tags;
	for (const ns::Datagram &d : to_p1) tags.push_back(d.tag);
	ok = expect(tags == std::vector<uint8_t>({s2c::SQUAD_JOIN, s2c::SQUAD_ORDER, s2c::SQUAD_ORDER,
					s2c::TEAM_ASSIGN, s2c::CHAT_BROADCAST}),
			"the changed player gets 0x71, the 0x72 pair, 0x50, then the chat") && ok;
	if (tags.size() == 5) {
		ok = expect(to_p1[0].body == std::vector<uint8_t>({0xFF, 1}) &&
						to_p1[1].body == std::vector<uint8_t>({0, 0}) &&
						to_p1[2].body == std::vector<uint8_t>({1, 0}) &&
						to_p1[3].body == team_assign && to_p1[4].body == chat,
				"0x71 [0xFF][slot], 0x72 [0][\"\"] / [1][\"\"], the 0x50 record, channel 10 chat") && ok;
	}
	const std::vector<ns::Datagram> to_p2 = drain(f.transports[1]);
	ok = expect(to_p2.size() == 2 && to_p2[0].tag == s2c::SQUAD_JOIN &&
					to_p2[1].tag == s2c::TEAM_ASSIGN && to_p2[1].body == team_assign,
			"the new teammate gets 0x71 and 0x50 only") && ok;
	ok = expect(f.ctx.team_change_entities.size() == 1 &&
					f.ctx.team_change_entities[0] == f.players[0],
			"the player joins the team-change list") && ok;

	// SwapTeam swaps back to side A; the list keeps one entry.
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "SwapTeam", "ByIndex", {"1"});
	ok = expect(out.handled && e->team == 1 && p1.assigned_team == 1 &&
					e->minimap_net_id == 0x0201 && e->anim_slot == 3 &&
					f.ctx.team_change_entities.size() == 1,
			"SwapTeamByIndex swaps back to side A") && ok;

	// A team-3 player keeps its team but still gets the chat.
	f.world.registry.get(f.players[1])->team = 3;
	for (auto &t : f.transports) (void)drain(t);
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "ChangeTeam", "ByName", {"P2"});
	const std::vector<ns::Datagram> to_p3 = drain(f.transports[1]);
	ok = expect(out.handled && f.world.registry.get(f.players[1])->team == 3 &&
					to_p3.size() == 1 && to_p3[0].tag == s2c::CHAT_BROADCAST,
			"a team-3 player keeps its team and still gets the chat") && ok;
	// A target that is not in the game is a no-op.
	f.conn(1).phase = inmatch::ConnectionPhase::Joined;
	out = inmatch::Server_ExecuteServerCommand(f.ctx, &f.world, "ChangeTeam", "ByName", {"P2"});
	ok = expect(!out.handled, "a target outside the game is a no-op") && ok;
	f.conn(1).phase = inmatch::ConnectionPhase::InMatch;

	// C2S 0x29 [0] reads the list back as S2C 0x51 [0][the 0x50 record] to
	// the sender; an index past the list draws nothing; a new round clears it.
	TeamAssign now;
	now.entity_handle = f.players[0].packed;
	now.team = 1;
	now.net_id = 0x0201;
	now.anim_slot = 3;
	std::vector<ProtocolMessage> replies = f.dispatch(1, c2s::TEAM_SPAWN_ACK, {0, 0});
	ok = expect(replies.size() == 1 && replies[0].tag == s2c::TEAM_CHANGE_CONFIRM &&
					replies[0].payload == encode_team_change_confirm(0, now),
			"C2S 0x29 [0] draws the 0x51 record to the sender") && ok;
	replies = f.dispatch(1, c2s::TEAM_SPAWN_ACK, {1, 0});
	ok = expect(replies.empty(), "an index past the list draws nothing") && ok;
	inmatch::Server_InitNewRoundState(f.ctx);
	replies = f.dispatch(1, c2s::TEAM_SPAWN_ACK, {0, 0});
	return expect(f.ctx.team_change_entities.empty() && replies.empty(),
			"a new round empties the team-change list") && ok;
}

// --------------------------------------------------------------------------
// The NovaWorld ticket wait: ClientPlayerEnterRequest per joiner.
// --------------------------------------------------------------------------

bool check_player_enter_hook() {
	HostFixture f(1);
	ns::UdpSessionTransport t_a(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport t_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport t_c(ns::UdpSessionTransport::Role::Host);
	auto joined = [&](uint32_t id, ns::UdpSessionTransport &t, uint16_t port) {
		inmatch::NapiNPConnection conn = conn_fixture::make_conn(
				id, 1, &t, ns::TransportMode::Client, w::EntityHandle{}, false);
		conn.phase = inmatch::ConnectionPhase::Joined;
		conn.peer = PeerAddr{0x0100007Fu, port};
		conn.join_validated_host_ms = 0;
		conn.join_identity_pairs = {{"PCID", "42"}, {"PUBJOINTICKET", "T-" + std::to_string(id)}};
		f.ctx.np_protocol.connection_list.push_back(std::move(conn));
	};
	joined(20, t_a, 4000);
	joined(21, t_b, 4001);
	joined(22, t_c, 4002);
	auto conn_by_id = [&](uint32_t id) -> inmatch::NapiNPConnection & {
		for (inmatch::NapiNPConnection &c : f.ctx.np_protocol.connection_list)
			if (c.connection_id == id) return c;
		return f.ctx.np_protocol.connection_list.front();
	};
	std::vector<inmatch::NapiNPServerCtx::PlayerEnterRequest> requests;
	f.ctx.on_player_enter_request = [&](const inmatch::NapiNPServerCtx::PlayerEnterRequest &r) {
		requests.push_back(r);
	};
	// GetLocalAddress: "PUB" on the NovaWorld transport in session.
	f.ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	f.ctx.is_in_session = 1;
	f.ctx.novaworld_join_tickets_armed = true;
	f.ctx.np_protocol.host_run_duration_ms = 1000;
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	if (!expect(requests.size() == 3, "an armed host announces each validated joiner exactly once")) return false;
	if (!expect(requests[0].connection_id == 20 && requests[0].peer.port == 4000 &&
					requests[0].join_ticket == "T-20",
			"the request carries the id, the UDP source and the <localaddr>JOINTICKET"))
		return false;
	if (!expect(conn_by_id(20).player_enter_pending(), "the joiner waits in state 4")) return false;
	// Success admits; a failure punts with NWPENTERFAIL; the third times out.
	if (!expect(inmatch::Server_ApplyPlayerEnterResult(f.ctx, 20, true, 0) &&
					!conn_by_id(20).player_enter_pending() && !conn_by_id(20).host_disconnect_sent,
			"a successful result releases the hold"))
		return false;
	if (!expect(inmatch::Server_ApplyPlayerEnterResult(f.ctx, 21, false, 6001) &&
					conn_by_id(21).host_disconnect_sent,
			"a failed result punts the joiner"))
		return false;
	if (!expect(!inmatch::Server_ApplyPlayerEnterResult(f.ctx, 21, true, 0),
			"a result for a settled joiner is refused"))
		return false;
	f.ctx.np_protocol.host_run_duration_ms = 0x1D4C0u;
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	if (!expect(!conn_by_id(22).host_disconnect_sent, "exactly 120 s is not yet the ticket deadline"))
		return false;
	f.ctx.np_protocol.host_run_duration_ms = 0x1D4C1u;
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	if (!expect(conn_by_id(22).host_disconnect_sent, "past 120 s the ticket wait reaps")) return false;
	// Without the arm the announced state is punted at once and a fresh joiner
	// waits for its 120 s validation deadline.
	ns::UdpSessionTransport t_d(ns::UdpSessionTransport::Role::Host);
	joined(23, t_d, 4003);
	conn_by_id(23).join_validated_host_ms = f.ctx.np_protocol.host_run_duration_ms;
	f.ctx.novaworld_join_tickets_armed = false;
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	if (!expect(!conn_by_id(23).host_disconnect_sent && !conn_by_id(23).player_enter_requested &&
					requests.size() == 3,
			"an unarmed host neither announces nor reaps a fresh joiner"))
		return false;
	f.ctx.np_protocol.host_run_duration_ms += 0x1D4C1u;
	inmatch::Server_CheckPlayerTimeouts(f.ctx);
	return expect(conn_by_id(23).host_disconnect_sent, "the unarmed validation deadline reaps");
}

// --------------------------------------------------------------------------
// The visible-players table: C2S 0x23 -> S2C 0x4C, and the S2C 0x4D fan.
// --------------------------------------------------------------------------

std::vector<VisiblePlayers::Entry> snapshot_of(const std::vector<ProtocolMessage> &replies) {
	for (const ProtocolMessage &m : replies) {
		if (m.tag != s2c::VISIBLE_PLAYERS) continue;
		VisiblePlayers v;
		decode_visible_players(m.payload.data(), m.payload.size(), v);
		return v.entries;
	}
	return {};
}

bool check_visible_players_snapshot() {
	// Teams alternate 1, 2, 1, 2 over slots 1..4.
	HostFixture f(4);
	f.ctx.config.game_type = 0x10000; // team deathmatch
	std::vector<ProtocolMessage> replies = f.dispatch(0, c2s::VISIBLE_PLAYERS_REQUEST, {});
	std::vector<VisiblePlayers::Entry> entries = snapshot_of(replies);
	bool ok = expect(entries.size() == 2, "a team game lists the requester's team");
	ok = expect(entries.size() == 2 && entries[0].slot == 1 && entries[1].slot == 3 &&
			entries[0].entity_handle == f.players[0].packed &&
			entries[1].entity_handle == f.players[2].packed,
			"entries in slot order, each {slot id, packed handle}") && ok;
	// Slot order, not list order [orig: the g_PlayerSlots walk @0x5063c0].
	std::swap(f.ctx.np_protocol.connection_list[0], f.ctx.np_protocol.connection_list[2]);
	entries = snapshot_of(f.dispatch(2, c2s::VISIBLE_PLAYERS_REQUEST, {}));
	ok = expect(entries.size() == 2 && entries[0].slot == 1 && entries[1].slot == 3,
			"the snapshot walks slot order whatever the connection order") && ok;
	std::swap(f.ctx.np_protocol.connection_list[0], f.ctx.np_protocol.connection_list[2]);
	// A spectator slot drops out [orig: +100567 @0x506410].
	f.conn(2).link.spectator = true;
	entries = snapshot_of(f.dispatch(0, c2s::VISIBLE_PLAYERS_REQUEST, {}));
	ok = expect(entries.size() == 1 && entries[0].slot == 1,
			"a spectator is never listed") && ok;
	f.conn(2).link.spectator = false;
	// Outside the team types the requester reads as team 255: itself alone
	// [orig: @0x50637c..0x50638c; the spawn-time bit @0x43c546..0x43c54f].
	f.ctx.config.game_type = 0x00000;
	entries = snapshot_of(f.dispatch(1, c2s::VISIBLE_PLAYERS_REQUEST, {}));
	ok = expect(entries.size() == 1 && entries[0].slot == 2,
			"a free-for-all snapshot holds the requester alone") && ok;
	// The listen host's own slot is listed only when the host plays.
	f.ctx.config.game_type = 0x10000;
	f.conn(2).link.mode = ns::TransportMode::Loopback;
	f.ctx.is_mp_session_peer = 0;
	entries = snapshot_of(f.dispatch(0, c2s::VISIBLE_PLAYERS_REQUEST, {}));
	ok = expect(entries.size() == 1, "a serve-only host's own slot is skipped") && ok;
	f.ctx.is_mp_session_peer = 1;
	entries = snapshot_of(f.dispatch(0, c2s::VISIBLE_PLAYERS_REQUEST, {}));
	ok = expect(entries.size() == 2, "a playing host's own slot is listed") && ok;
	// An unbound requester gets count 0 [orig: @0x50635a..0x5063ac].
	f.conn(0).link.owned_entity = w::EntityHandle{};
	replies = f.dispatch(0, c2s::VISIBLE_PLAYERS_REQUEST, {});
	ok = expect(replies.size() == 1 && replies[0].tag == s2c::VISIBLE_PLAYERS &&
			replies[0].payload.size() == 1 && replies[0].payload[0] == 0,
			"an inactive requester is answered with count 0") && ok;
	return ok;
}

bool check_spawn_slot_notice_fan() {
	HostFixture f(3);
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);
	inmatch::fan_spawn_slot_notice(f.ctx.np_protocol.connection_list, f.conn(1));
	bool ok = true;
	for (size_t i = 0; i < 3; ++i) {
		const std::vector<ns::Datagram> out = drain(f.transports[i]);
		const size_t n = count_tag(out, s2c::SPAWN_SLOT_NOTICE);
		if (i == 1) {
			ok = expect(n == 0, "the joined slot's copy rides its bundle, not the fan") && ok;
		} else {
			ok = expect(n == 1 && out[0].body.size() == 1 && out[0].body[0] == 2,
					"every other in-game slot hears the joined slot id") && ok;
		}
	}
	return ok;
}

// --------------------------------------------------------------------------
// The emote: C2S 0x14 -> S2C 0x2D near the sender, the 2 s cooldown.
// --------------------------------------------------------------------------

bool check_emote_request() {
	HostFixture f(3);
	// Players at x = 0, 10, 20; the third moves out of range.
	f.world.registry.get(f.players[2])->position.x = 150.0f;
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);
	EmoteRequest req;
	req.value = 3;
	std::vector<ProtocolMessage> replies =
			f.dispatch(0, c2s::EMOTE_REQUEST, encode_emote_request(req));
	bool ok = expect(replies.size() == 1 && replies[0].tag == s2c::EMOTE_BROADCAST &&
			!replies[0].reliable,
			"the sender hears its own emote, unreliable");
	if (!replies.empty()) {
		const std::vector<uint8_t> body = {3, static_cast<uint8_t>(f.players[0].slot()), 0, 0};
		ok = expect(replies[0].payload == body, "the body is {emote, pool-0 index, 0, 0}") && ok;
	}
	ok = expect(count_tag(drain(f.transports[1]), s2c::EMOTE_BROADCAST) == 1,
			"a player within 100 units hears it") && ok;
	ok = expect(count_tag(drain(f.transports[2]), s2c::EMOTE_BROADCAST) == 0,
			"a player past 100 units on an axis does not") && ok;
	ok = expect(f.conn(0).link.emote_cooldown_seconds == 2, "the cooldown re-arms to 2") && ok;
	replies = f.dispatch(0, c2s::EMOTE_REQUEST, encode_emote_request(req));
	ok = expect(replies.empty() && count_tag(drain(f.transports[1]), s2c::EMOTE_BROADCAST) == 0,
			"a cooling sender sends nothing") && ok;
	// The 1 Hz maintenance counts the cooldown down [orig: @0x51e028].
	for (int second = 0; second < 2; ++second) {
		do {
			inmatch::Server_TickUpdate(f.ctx);
		} while (!f.world.match.periodic_second());
	}
	ok = expect(f.conn(0).link.emote_cooldown_seconds == 0,
			"two periodic seconds clear the cooldown") && ok;
	// A dead sender, and a spectator, send nothing [orig: @0x501e33 / @0x501e55].
	f.world.registry.get(f.players[0])->engine_flags |= w::kEntityFlagDead;
	ok = expect(f.dispatch(0, c2s::EMOTE_REQUEST, encode_emote_request(req)).empty(),
			"a dead sender is silent") && ok;
	f.world.registry.get(f.players[0])->engine_flags &= ~w::kEntityFlagDead;
	f.conn(0).link.spectator = true;
	ok = expect(f.dispatch(0, c2s::EMOTE_REQUEST, encode_emote_request(req)).empty(),
			"a spectator is silent") && ok;
	return ok;
}

// --------------------------------------------------------------------------
// The radio call: C2S 0x13 -> S2C 0x6D by the rule table, the latch, the
// designation and its S2C 0x6B, the 4 s cooldown.
// --------------------------------------------------------------------------

size_t count_radio(ns::UdpSessionTransport &t) {
	return count_tag(drain(t), s2c::TRACKED_PLAYER_VOICE);
}

bool check_radio_call_rules() {
	// [orig: g_RadioCallRules @0x840C20; RadioCall_FindRuleByKey @0x5BFE50]
	const inmatch::RadioCallRule *medic = inmatch::radio_call_find_rule("rad_medic2");
	bool ok = expect(medic != nullptr && medic->type == 2 && medic->range_units == 8000,
			"the lookup ignores case");
	const inmatch::RadioCallRule *rifle = inmatch::radio_call_find_rule("RAD_RIFLE2");
	ok = expect(rifle != nullptr && rifle->type == -1 && rifle->designation_seconds == 30 &&
			rifle->designation_mode == 3, "the last searched row marks for 30 s") && ok;
	ok = expect(inmatch::radio_call_find_rule("RAD_1") == nullptr,
			"the type-0 row ends the walk uncompared") && ok;
	ok = expect(inmatch::radio_call_find_rule("RAD_SNIPER1") == nullptr &&
			inmatch::radio_call_find_rule("RAD_MP_TDM1") == nullptr,
			"rows past the terminator are never reached") && ok;
	// The table ages every tick; the life that reaches 0 frees its owner, a
	// zero-life row keeps its owner [orig: @0x51e4a0..0x51e4af].
	inmatch::ServerDesignationTable table{};
	const int32_t point[3] = {1, 2, 3};
	inmatch::Server_RegisterDesignation(table, w::EntityHandle::make(0, 4), point, 2, 0, 3);
	inmatch::Server_RegisterDesignation(table, w::EntityHandle::make(0, 5), point, 0, 0, 3);
	inmatch::Server_RegisterDesignation(table, w::EntityHandle::make(0, 4), point, 5, 0, 3);
	ok = expect(table[0].remaining_ticks == 5 && table[1].owner == w::EntityHandle::make(0, 5),
			"an owner refreshes its own row; another takes the next free one") && ok;
	for (int i = 0; i < 5; ++i) inmatch::Server_TickDesignations(table);
	ok = expect(!table[0].owner.valid() && table[0].remaining_ticks == 0,
			"the aged-out row frees its owner") && ok;
	ok = expect(table[1].owner.valid(), "a zero-life row keeps its owner") && ok;
	return ok;
}

bool check_radio_call_request() {
	HostFixture f(4); // teams 1, 2, 1, 2 at x = 0, 10, 20, 30
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);
	w::Entity *sender = f.world.registry.get(f.players[0]);
	w::Entity *mate = f.world.registry.get(f.players[2]);
	RadioCallRequest req;

	// Call 1 builds RAD_1, which no rule reaches: the sender's team.
	req.value = 1;
	std::vector<ProtocolMessage> replies =
			f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	bool ok = expect(replies.size() == 1 && replies[0].tag == s2c::TRACKED_PLAYER_VOICE &&
			!replies[0].reliable, "the sender hears its own call, unreliable");
	if (!replies.empty()) {
		const std::vector<uint8_t> body = {1, uint8_t(f.players[0].slot()), 0xFF, 0xFF};
		ok = expect(replies[0].payload == body,
				"the body is {call, pool-0 index, no location}") && ok;
	}
	ok = expect(count_radio(f.transports[2]) == 1, "a teammate hears it") && ok;
	ok = expect(count_radio(f.transports[1]) == 0 && count_radio(f.transports[3]) == 0,
			"the other team does not") && ok;
	ok = expect(f.conn(0).link.radio_call_cooldown_seconds == 4, "the cooldown re-arms to 4") && ok;
	ok = expect(f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req)).empty() &&
			count_radio(f.transports[2]) == 0, "a cooling sender sends nothing") && ok;
	// The 1 Hz maintenance counts it down [orig: @0x51e045].
	for (int second = 0; second < 4; ++second) {
		do {
			inmatch::Server_TickUpdate(f.ctx);
		} while (!f.world.match.periodic_second());
	}
	ok = expect(f.conn(0).link.radio_call_cooldown_seconds == 0,
			"four periodic seconds clear the cooldown") && ok;
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);

	// Call 6 is RAD_6: type 1, a control seat within 500 units (2-D), and the
	// sender's ride-request latch.
	req.value = 6;
	replies = f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(replies.size() == 1 && count_radio(f.transports[2]) == 0,
			"a teammate on foot is not a driver") && ok;
	ok = expect(sender->radio_request == 1 && sender->radio_request_seconds == 30,
			"call 6 raises the sender's latch for 30 seconds") && ok;
	// The body's 64-tick window ages the latch [orig: @0x4b467a..0x4b469d].
	for (int tick = 0; tick < 64; ++tick) inmatch::Server_TickUpdate(f.ctx);
	ok = expect(sender->radio_request == 1 && sender->radio_request_seconds == 29,
			"one 64-tick window takes one second off the latch") && ok;
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);
	mate->mount_type = w::SeatType::Driver;
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	(void)f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(count_radio(f.transports[2]) == 1, "a driving teammate in range hears it") && ok;
	mate->position.x = 600.0f;
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	(void)f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(count_radio(f.transports[2]) == 0, "past 500 units it does not") && ok;
	mate->position.x = 20.0f;
	mate->mount_type = w::SeatType::None;

	// Call 9 of a medic outside a zone game is RAD_MEDIC1: the dead in range.
	f.ctx.config.game_type = 0x10001;
	sender->player_class = 5;
	req.value = 9;
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	(void)f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(sender->radio_request == 0, "any other call clears the latch") && ok;
	ok = expect(count_radio(f.transports[2]) == 0, "a living teammate needs no medic") && ok;
	mate->engine_flags |= w::kEntityFlagDead;
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	(void)f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(count_radio(f.transports[2]) == 1, "a downed teammate hears the medic") && ok;
	mate->engine_flags &= ~w::kEntityFlagDead;

	// Call 7 is RAD_7: every teammate, and a 30 s mark 1000 units down the
	// sender's view (no collision world: the far point) [orig: @0x514668..0x51470c].
	req.value = 7;
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	(void)f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req));
	ok = expect(count_radio(f.transports[2]) == 1, "RAD_7 reaches the whole team") && ok;
	const inmatch::ServerDesignation &mark = f.ctx.designations[0];
	ok = expect(mark.owner == f.players[0] && mark.remaining_ticks == 62 * 30 && mark.mode == 3 &&
			mark.radius_q16 == 0, "the call marks its point for 30 s, mode 3") && ok;
	const double dx = double(mark.point[0]) - double(w::to_fixed(sender->position.x));
	const double dy = double(mark.point[1]) - double(w::to_fixed(sender->position.y));
	ok = expect(std::abs(std::sqrt(dx * dx + dy * dy) - 1000.0 * 65536.0) < 2.0 * 65536.0,
			"the mark lies 1000 units down the view") && ok;
	// The next overlay visit, at the head of the tick, sends the team's marks
	// as S2C 0x6B before the tail ages them [orig: @0x5188d6 before @0x51e496].
	const int32_t life_at_send = mark.remaining_ticks;
	for (ns::UdpSessionTransport &t : f.transports) (void)drain(t);
	for (inmatch::NapiNPConnection &conn : f.ctx.np_protocol.connection_list)
		conn.reply.minimap_overlay_cooldown = 0;
	inmatch::Server_TickUpdate(f.ctx);
	const auto overlays = [&](size_t i) {
		std::vector<MinimapOverlayBatch> out;
		for (const ns::Datagram &d : drain(f.transports[i])) {
			if (d.tag != s2c::MINIMAP_OVERLAY) continue;
			MinimapOverlayBatch batch;
			if (decode_minimap_overlay_batch(d.body.data(), d.body.size(), batch))
				out.push_back(batch);
		}
		return out;
	};
	const std::vector<MinimapOverlayBatch> own = overlays(2);
	ok = expect(own.size() == 1 && own[0].entries.size() == 1 &&
			own[0].entries[0].handle == f.players[0].packed && own[0].entries[0].type == 3 &&
			own[0].entries[0].lifetime_s == life_at_send / 62 && own[0].entries[0].height == 0,
			"the mark's team sees it: {owner, point, seconds, mode 3, 0}") && ok;
	ok = expect(overlays(1).empty() && overlays(3).empty(), "the other team sees nothing") && ok;
	ok = expect(mark.remaining_ticks == life_at_send - 1, "the tick's tail aged the mark") && ok;
	// A round init clears the table [orig: @0x51cb95..0x51cba5].
	inmatch::Server_InitNewRoundState(f.ctx);
	ok = expect(!f.ctx.designations[0].owner.valid() && f.ctx.designations[0].remaining_ticks == 0,
			"the new-round init clears the marks") && ok;

	// A dead sender, and a spectator, are silent [orig: @0x514365 / @0x51437c].
	f.conn(0).link.radio_call_cooldown_seconds = 0;
	sender->engine_flags |= w::kEntityFlagDead;
	ok = expect(f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req)).empty(),
			"a dead sender is silent") && ok;
	sender->engine_flags &= ~w::kEntityFlagDead;
	f.conn(0).link.spectator = true;
	ok = expect(f.dispatch(0, c2s::RADIO_CALL_REQUEST, encode_radio_call_request(req)).empty(),
			"a spectator is silent") && ok;
	return ok;
}

} // namespace

int main() {
	bool ok = check_ping_ring_and_strikes();
	ok = check_ping_dispatch_return_leg() && ok;
	ok = check_client_quality_store_and_resend() && ok;
	ok = check_host_quality_window() && ok;
	ok = check_host_novaworld_exit() && ok;
	ok = check_chat_helpers() && ok;
	ok = check_chat_routing() && ok;
	ok = check_loaded_model_reply_stamp() && ok;
	ok = check_vehicle_spawn_availability() && ok;
	ok = check_spectator_respawn_request() && ok;
	ok = check_join_leave_lines() && ok;
	ok = check_medic_revive_transaction() && ok;
	ok = check_medic_revive_gates() && ok;
	ok = check_medic_heal_transaction() && ok;
	ok = check_session_ping_codec() && ok;
	ok = check_server_commands() && ok;
	ok = check_server_command_gates() && ok;
	ok = check_change_team() && ok;
	ok = check_player_enter_hook() && ok;
	ok = check_visible_players_snapshot() && ok;
	ok = check_spawn_slot_notice_fan() && ok;
	ok = check_emote_request() && ok;
	ok = check_radio_call_rules() && ok;
	ok = check_radio_call_request() && ok;
	if (ok) std::printf("OK\n");
	return ok ? 0 : 1;
}
