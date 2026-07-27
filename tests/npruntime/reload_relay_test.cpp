// D-NET-142 — the C2S 0x25 reload-request relay. The dispatch handler must broadcast S2C 0x49
// (the SAME [u16 handle][u16 weaponSlotCombo] body, rebuilt per ADR 0003) onto EVERY in-match
// connection's transport INCLUDING the requester [orig: NapiNPServerMsg_HandleReloadRequest
// @0x514DF0 -> two NapiNPServer_SendFiltered @0x4C87E0 sends], because the client's 0x49 apply is
// the only place its clip refills (§5.58). The 0x80 phase bit is transient: without the echo the
// empty clip returns to idle and auto-reload requests again — the retail-join v15 defect.
//
// Coverage: three in-match connections (the host's own type-2 loopback + two type-1 remotes) each
// pop exactly one 0x49 with the relayed body; the direct reply list carries NO 0x49 (transports
// are the broadcast path); a pre-spawn connection receives nothing; a malformed short 0x25 is
// dropped without any send.

#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_message_dispatch.h>

#include <netsim/connection.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/protocol_message.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = {0, 0, 0};
	s.net_id = net_id;
	return s;
}

np::NapiNPConnection make_conn(uint32_t id, int type, ns::ISessionTransport *t,
                               ns::TransportMode mode, w::EntityHandle owned, bool spawned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? np::ConnectionPhase::InMatch : np::ConnectionPhase::New;
	return c;
}

// Pop exactly one staged S2C datagram from a transport and assert it is the relayed 0x49.
bool pops_one_relayed_49(ns::ISessionTransport &t, bool udp_raw,
                         uint16_t expected_handle, uint16_t expected_combo,
                         const char *who) {
	uint8_t tag = 0;
	std::vector<uint8_t> body;
	if (udp_raw) {
		auto &udp = static_cast<ns::UdpSessionTransport &>(t);
		std::vector<uint8_t> raw;
		if (!expect(udp.pop_outbound(raw), "an S2C datagram was staged")) return false;
		if (!expect(raw.size() == 5, "staged datagram is tag + 4-B body")) return false;
		tag = raw[0];
		body.assign(raw.begin() + 1, raw.end());
		if (!expect(!udp.pop_outbound(raw), "exactly one staged S2C datagram")) return false;
	} else {
		ns::Datagram dg;
		if (!expect(t.client_recv(dg), "an S2C datagram was staged (loopback)")) return false;
		tag = dg.tag;
		body = dg.body;
		ns::Datagram extra;
		if (!expect(!t.client_recv(extra), "exactly one staged S2C datagram (loopback)"))
			return false;
	}
	if (!expect(tag == 0x49, "staged tag is S2C 0x49")) return false;
	WeaponReload r;
	size_t consumed = 0;
	if (!expect(decode_weapon_reload(body.data(), body.size(), r, consumed) && consumed == 4,
	            "relayed 0x49 body decodes (4 B)"))
		return false;
	if (!expect(r.entity_handle == expected_handle && r.reload_param == expected_combo,
	            "relayed body carries the request's handle + weaponSlotCombo")) {
		std::fprintf(stderr, "  (%s: handle=0x%04x combo=0x%04x)\n", who, r.entity_handle,
		             r.reload_param);
		return false;
	}
	return true;
}

} // namespace

int main() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle ha = w::spawn_player(world, player_spawn(0xFFF0));
	const w::EntityHandle hb = w::spawn_remote_player(world, player_spawn(0xFFF1));
	const w::EntityHandle hc = w::spawn_remote_player(world, player_spawn(0xFFF2));
	if (!expect(ha.valid() && hb.valid() && hc.valid(), "three players spawned")) return 1;

	// Host's own loopback + two remote peers in-match, one straggler still handshaking.
	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_d(ns::UdpSessionTransport::Role::Host);

	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_conn(1, 2, &loop, ns::TransportMode::Loopback, ha, true));
	roster.push_back(make_conn(3, 1, &udp_b, ns::TransportMode::Client, hb, true));
	roster.push_back(make_conn(4, 1, &udp_c, ns::TransportMode::Client, hc, true));
	roster.push_back(make_conn(5, 1, &udp_d, ns::TransportMode::Client, {}, false)); // pre-spawn

	// Requester = remote B. C2S 0x25 [u16 live addressed handle][u16 combo=0x00C3].
	const WeaponReload request{hb.packed, 0x00C3};
	const std::vector<uint8_t> req_body = encode_weapon_reload(request);
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(0x25, req_body));
	std::vector<ProtocolMessage> replies =
			np::dispatch_session_replies(np::GameConfig{}, roster[1], msgs, 100, roster, &world);

	// The broadcast rides the transports, never the requester's direct reply list.
	for (const ProtocolMessage &m : replies)
		if (!expect(m.tag != 0x49, "no 0x49 in the direct replies (broadcast path only)")) return 1;

	if (!pops_one_relayed_49(loop, /*udp_raw=*/false, hb.packed, 0x00C3, "loopback")) return 1;
	if (!pops_one_relayed_49(udp_b, /*udp_raw=*/true, hb.packed, 0x00C3, "requester")) return 1;
	if (!pops_one_relayed_49(udp_c, /*udp_raw=*/true, hb.packed, 0x00C3, "peer C")) return 1;
	{
		std::vector<uint8_t> raw;
		if (!expect(!udp_d.pop_outbound(raw), "pre-spawn connection receives nothing")) return 1;
	}

	// Malformed (short) 0x25 -> dropped, no sends.
	std::vector<ProtocolMessage> bad;
	bad.push_back(make_protocol_message(0x25, {0x05, 0x10}));
	replies = np::dispatch_session_replies(np::GameConfig{}, roster[1], bad, 101, roster, &world);
	{
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		if (!expect(!udp_b.pop_outbound(raw) && !udp_c.pop_outbound(raw) && !loop.client_recv(dg),
		            "short 0x25 dropped without any send"))
			return 1;
	}

	// A syntactically valid request still needs a live addressed pool slot.
	std::vector<ProtocolMessage> stale;
	stale.push_back(make_protocol_message(
			0x25, encode_weapon_reload(WeaponReload{0x1005, 0x00C3})));
	replies = np::dispatch_session_replies(
			np::GameConfig{}, roster[1], stale, 102, roster, &world);
	{
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		if (!expect(!udp_b.pop_outbound(raw) && !udp_c.pop_outbound(raw) && !loop.client_recv(dg),
		            "an unconfigured/stale addressed slot is not relayed"))
			return 1;
	}

	// Retail's requester dead-mark gate precedes the addressed-entity relay.
	w::Entity *requester_entity = world.registry.get(hb);
	if (!expect(requester_entity != nullptr, "requester remains live in the registry")) return 1;
	requester_entity->health = 0;
	replies = np::dispatch_session_replies(
			np::GameConfig{}, roster[1], msgs, 103, roster, &world);
	{
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		if (!expect(!udp_b.pop_outbound(raw) && !udp_c.pop_outbound(raw) && !loop.client_recv(dg),
		            "a dead requester cannot relay a reload"))
			return 1;
	}

	std::printf("OK\n");
	return 0;
}
