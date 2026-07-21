// P5 — np::ClientRuntime (the headless Client_ProcessNetworkFrame role), always-on:
//
//  (A) Full in-process round-trip — client_runtime <-> the REAL np server legs <-> Server_TickUpdate
//      + the production apply_in_match_c2s consumer + the NetClientView S2C fold:
//        handshake (0x41/0x42) via ClientRuntime.start()/receive()/Client_ProcessNetworkFrame()
//        -> the spawn-gate burst (driven from the per-frame client role) -> PeerSpawned
//        -> the owner binds the joiner connection's transport + streams a NAMED organic-spawn
//        -> client name-matches -> InMatch (learns wire handle H)
//        -> each frame: ClientRuntime emits a framed C2S 0x0C -> handle_server_datagram surfaces
//           PeerC2SInMatch -> apply_in_match_c2s deliver_c2s's it onto the connection's transport
//           -> Server_TickUpdate drains+SNAPs the entity + fans an S2C 0x0A
//        -> the owner reframes that 0x0A as a 0x83 -> ClientRuntime folds it into ClientState.
//      Asserts the peer SNAPs to the uplink AND the client's ClientState reflects the server's 0x0A
//      (exactly one SNAP per 0x0C). This is the P5 e2e bar and the FIRST coverage of NetClientView
//      fold + the production PeerC2SInMatch consumer (joiner_connection_test does neither).
//
//  (B) Host-as-client (D-NET-121/122) — the SP listen-server host's OWN loopback view: Server_TickUpdate
//      fans the host loopback a per-frame 0x0A (is_in_match) anchored to the host player's owned_entity
//      (NOT the dvxi5 fallback), and a HostClient ClientRuntime folds it off the loopback. Guards that
//      the host's own local view is no longer starved and anchors correctly.

#include <npruntime/client_runtime.h>
#include <npruntime/host_session.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/server_spawn.h>
#include <npruntime/server_tick.h>

#include "host_test_setup.h"

#include <netsim/connection.h>
#include <netsim/idatagram_socket.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/vehicle_attach.h>
#include <world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
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

struct NullDatagramSocket final : ns::IDatagramSocket {
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

struct HostPumpHookProbe {
	np::HostOwner *owner = nullptr;
	w::World *world = nullptr;
	w::AiSystem *ai = nullptr;
	PeerAddr peer{};
	int calls = 0;
	w::EntityHandle spawned{};
	bool saw_spawned_connection = false;
	bool saw_live_entity = false;
	bool saw_ai_component = false;
	bool saw_before_logic = false;
	bool saw_before_fan = false;
};

void observe_host_before_server_tick(void *opaque) {
	auto &probe = *static_cast<HostPumpHookProbe *>(opaque);
	++probe.calls;
	for (np::NapiNPConnection &conn : probe.owner->ctx.np_protocol.connection_list) {
		if (!(conn.peer == probe.peer)) continue;
		probe.spawned = conn.link.owned_entity;
		probe.saw_spawned_connection =
				conn.burst.spawned && conn.phase == np::ConnectionPhase::Spawned;
		probe.saw_live_entity = probe.spawned.valid() &&
				probe.world->registry.get(probe.spawned) != nullptr;
		probe.saw_ai_component = probe.spawned.valid() &&
				probe.ai->for_handle(probe.spawned) != nullptr;
		probe.saw_before_logic = probe.world->logic_tick == 0;
		probe.saw_before_fan = conn.link.s2c_phase == 0;
		return;
	}
}

struct FirstLogicTickProbe final : w::ISystem {
	HostPumpHookProbe *hook = nullptr;
	int ticks = 0;
	bool first_tick_saw_hook = false;
	bool first_tick_saw_player = false;

	const char *name() const override { return "host-before-server-tick-order"; }
	void tick(w::World &world, const w::TickContext &) override {
		++ticks;
		if (ticks != 1) return;
		first_tick_saw_hook = hook != nullptr && hook->calls == 1;
		first_tick_saw_player = hook != nullptr && hook->spawned.valid() &&
				world.registry.get(hook->spawned) != nullptr;
	}
};

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	return s;
}

// A 1-record S2C 0x0C organic-spawn body the joiner name-matches (the owner's PeerSpawned reaction,
// mirroring NovaSimulation / joiner_connection_test).
std::vector<uint8_t> make_organic_spawn(uint16_t slot_id, const std::string &name, int32_t x,
                                        int32_t y, int32_t z, int32_t orient, uint8_t team,
                                        uint16_t net_id) {
	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord rec;
	rec.slot_id = slot_id;
	rec.has_body = true;
	rec.item_type_id = 0x14B9;
	rec.entity_name = name;
	rec.pos_x = x;
	rec.pos_y = y;
	rec.pos_z = z;
	rec.orientation = orient;
	rec.team = team;
	rec.net_id = net_id;
	batch.records.push_back(rec);
	return encode_organic_spawn_batch(batch);
}

// ---------------------------------------------------------------------------------------------------
// (A) Full in-process round-trip: a remote joiner via ClientRuntime.
// ---------------------------------------------------------------------------------------------------
bool run_roundtrip() {
	const PeerAddr peer{0x0100007Fu, 30000}; // 127.0.0.1:30000
	const std::string kName = "JoinerOne";

	np::NapiNPServerCtx ctx;
	// HostClient listen host (mode 3). local_client = nullptr: no host loopback connection in this
	// run — the only connection is the joiner, keeping the round-trip focused (the host loopback path
	// is run (B)). The host advertises this HK; ClientRuntime echoes it so the join HK gate passes.
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        0x0FE0E112u);

	// The host's authoritative World. ctx.world set BEFORE the join so tick_connections'
	// Server_ProcessPendingPlayerSpawns spawns the joiner's pool-0 entity (binding conn.link.owned_entity).
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	ctx.world = &world;
	// The host's own local player (sets cached.local_player, which apply_player_intent refuses to snap).
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host's own player spawned (cached.local_player)")) return false;

	np::ClientRuntime client(ClientSession::Config::jointoperations(), kName);

	uint32_t tick = 1;
	bool spawned = false;
	std::string spawn_name;
	auto note_event = [&](const np::HostAcceptEvent &e) {
		if (e.kind == np::HostAcceptEvent::Kind::PeerSpawned && !spawned) {
			spawned = true;
			spawn_name = e.peer_name;
		}
	};
	// Dispatch one client->host datagram and feed every host reply (ServerHello/ServerAuth/0x83) back
	// into the client's recv FIFO.
	auto pump_host = [&](std::vector<uint8_t> dg) {
		if (dg.empty()) return;
		np::HandleResult r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		for (const np::HostAcceptEvent &e : r.events) note_event(e);
		for (const std::vector<uint8_t> &o : r.outbound) client.receive(o.data(), o.size());
	};

	// --- 1) Handshake + spawn-gate burst, driven entirely from the per-frame client role ---
	pump_host(client.start()); // ClientHello -> ServerHello (queued back to the client)
	for (int f = 0; f < 120 && !spawned; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) pump_host(std::move(d));
		// Several host ticks per frame so entity_batch_count climbs through world streaming (F3) and the
		// spawn gate opens; ship the burst replies back to the client.
		for (int k = 0; k < 6; ++k) {
			for (np::TickOut &t : np::tick_connections(ctx, 300, tick++)) {
				for (const np::HostAcceptEvent &e : t.events) note_event(e);
				for (const std::vector<uint8_t> &o : t.outbound) client.receive(o.data(), o.size());
			}
		}
	}
	if (!expect(spawned, "the spawn-gate burst trips PeerSpawned")) return false;
	if (!expect(spawn_name == kName, "PeerSpawned carries the joiner's ClientHello.co name")) return false;
	if (!expect(np::connection_spawned(ctx, peer), "host marks the peer spawned")) return false;

	// --- 2) Owner's PeerSpawned reaction: bind the connection's transport (the pipeline already bound
	//        owned_entity) + stream a NAMED organic-spawn so the client name-matches. ---
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	w::EntityHandle Hh{};
	for (np::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) {
			c.link.transport = &udp_host;
			c.link.mode = ns::TransportMode::Client;
			Hh = c.link.owned_entity;
		}
	}
	if (!expect(Hh.valid(), "joiner entity spawned + owned_entity bound by the spawn pipeline")) return false;
	if (!expect(Hh != host_h, "joiner is a distinct entity from the host's own player")) return false;

	const w::Entity *je0 = world.registry.get(Hh);
	const uint16_t net_id = je0 ? je0->net_id : 0;
	const int32_t sx = w::to_fixed(70.0), sy = w::to_fixed(25.0), sz = w::to_fixed(56.0);
	{
		std::vector<uint8_t> body = make_organic_spawn(Hh.packed, spawn_name, sx, sy, sz, 0x40000000, 2, net_id);
		std::vector<uint8_t> sdg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0C, body, sdg), "host frames the named 0x0C"))
			return false;
		client.receive(sdg.data(), sdg.size());
		client.Client_ProcessNetworkFrame(tick++); // fold + name-match
	}
	if (!expect(client.in_match() && client.self_handle() == Hh.packed,
	            "client reached InMatch via the name-match; H == the host wire handle")) return false;
	if (!expect(client.deployed(), "client is deployed on the spawn name-match (the 0x0C gate)")) return false;

	// --- 3) In-match per-frame loop: client 0x0C -> apply_in_match_c2s -> Server_TickUpdate -> 0x0A fold ---
	PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF;
	up.pos_x = w::to_fixed(100.0);
	up.pos_y = w::to_fixed(200.0);
	up.pos_z = w::to_fixed(-50.0);
	up.heading = 0x2000; // -> mission yaw 45
	up.pitch = 0x0100;

	std::size_t staged = 0;
	for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(up, tick)) {
		np::HandleResult r = np::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (const np::HostAcceptEvent &e : r.events) staged += np::apply_in_match_c2s(ctx, e);
	}
	if (!expect(staged == 1, "exactly one C2S 0x0C staged via apply_in_match_c2s")) return false;

	np::Server_TickUpdate(ctx); // drain (SNAP) -> run_logic_tick -> emit per-connection 0x0A

	const w::Entity *je = world.registry.get(Hh);
	if (!expect(je != nullptr, "joiner entity present after the tick")) return false;
	if (!expect(je->position.x == static_cast<float>(w::from_fixed(up.pos_x)) &&
	                    je->position.y == static_cast<float>(w::from_fixed(up.pos_y)) &&
	                    je->position.z == static_cast<float>(w::from_fixed(up.pos_z)),
	            "joiner entity SNAPped to the C2S 0x0C uplink pose")) return false;
	// The host's own player was NOT touched by the joiner's uplink (entity-scoped owner gate).
	const w::Entity *ho = world.registry.get(host_h);
	if (!expect(ho && ho->position.x == 0.0f && ho->position.y == 0.0f && ho->position.z == 0.0f,
	            "host's own player pose unchanged by the peer's 0x0C")) return false;

	// Reframe the host's emitted S2C 0x0A (identity-framed on the transport outbound) as a 0x83 and
	// fold it into the client's ClientState.
	std::vector<uint8_t> raw;
	bool got_0a = false;
	while (udp_host.pop_outbound(raw)) {
		if (raw.empty() || raw[0] != 0x0A) continue;
		std::vector<uint8_t> inner(raw.begin() + 1, raw.end());
		std::vector<uint8_t> dg83;
		if (np::frame_in_match_s2c(ctx, peer, 0x0A, inner, dg83)) {
			client.receive(dg83.data(), dg83.size());
			got_0a = true;
		}
	}
	if (!expect(got_0a, "Server_TickUpdate emitted an S2C 0x0A for the joiner connection")) return false;
	client.Client_ProcessNetworkFrame(tick++); // fold the 0x0A into ClientState

	// The client's decoded view reflects the server's 0x0A: its anchor IS the joiner's post-SNAP
	// position (emit_connection_s2c anchors to owned_entity), tying 0x0C-in -> 0x0A-out -> ClientState.
	if (!expect(client.state().anchor_x == w::to_fixed(je->position.x) &&
	                    client.state().anchor_y == w::to_fixed(je->position.y) &&
	                    client.state().anchor_z == w::to_fixed(je->position.z),
	            "client ClientState anchor == joiner's post-SNAP position (0x0A folded)")) return false;
	if (!expect(client.state().frames_applied >= 1, "client folded at least one 0x0A frame")) return false;

	// Exactly one SNAP per 0x0C: a second tick with no new uplink drained nothing more.
	if (!expect(udp_host.inbound_pending() == 0, "the connection's C2S queue is drained")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (B) Host-as-client (D-NET-121/122): the host's own loopback view anchors to its player, not dvxi5.
// ---------------------------------------------------------------------------------------------------
bool run_host_as_client() {
	np::NapiNPServerCtx ctx;
	ns::LoopbackChannel host_loop; // the in-process channel the host emits its own S2C onto
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        0x0FE0E112u, &host_loop);

	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	ctx.world = &world;

	// Spawn the host's own player through the real pipeline (type-2 loopback -> spawn_player ->
	// cached.local_player; binds conn.link.owned_entity = the host player, the D-NET-121 anchor).
	const int spawned = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(spawned == 1, "the host's own player spawned via the pipeline")) return false;

	// Find the host loopback connection; confirm its owned_entity is the host player + mark it in-match.
	// (In production burst.spawned latches when the host loopback's §5.2a burst completes — covered by
	// npruntime_initial_state_burst; here we set it directly to exercise the per-frame emit path.)
	np::NapiNPConnection *self = nullptr;
	for (np::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.type == 2) self = &c;
	}
	if (!expect(self != nullptr, "host loopback connection present")) return false;
	if (!expect(self->link.owned_entity.valid(), "host loopback owned_entity bound to its player")) return false;
	if (!expect(self->link.transport == &host_loop, "host loopback transport is the in-process channel"))
		return false;
	self->burst.spawned = true; // in-match (the is_in_match predicate) — §5.2a-complete in production

	// Move the host player to a DISTINCT, non-dvxi5 position so the anchor check is meaningful.
	const w::EntityHandle hp = self->link.owned_entity;
	w::Entity *he = world.registry.get(hp);
	if (!expect(he != nullptr, "host player entity present")) return false;
	he->position = w::Vec3{static_cast<float>(w::from_fixed(w::to_fixed(420.0))),
	                       static_cast<float>(w::from_fixed(w::to_fixed(-37.0))),
	                       static_cast<float>(w::from_fixed(w::to_fixed(910.0)))};

	np::Server_TickUpdate(ctx); // fans a per-frame 0x0A to the host loopback (is_in_match), anchored to hp

	np::ClientRuntime host_view(host_loop); // HostClient role: recv-fold only, 0x0C suppressed
	if (!expect(host_view.is_authority(), "host-as-client runtime is authority (no 0x0C)")) return false;
	std::vector<std::vector<uint8_t>> out = host_view.Client_ProcessNetworkFrame();
	if (!expect(out.empty(), "host-as-client emits no C2S (is_authority gate)")) return false;

	const w::Entity *he2 = world.registry.get(hp);
	if (!expect(host_view.state().frames_applied >= 1, "host-as-client folded its own 0x0A")) return false;
	if (!expect(host_view.state().anchor_x == w::to_fixed(he2->position.x) &&
	                    host_view.state().anchor_y == w::to_fixed(he2->position.y) &&
	                    host_view.state().anchor_z == w::to_fixed(he2->position.z),
	            "host-as-client anchor == host player position (D-NET-121: owned_entity, not dvxi5)"))
		return false;
	// Explicitly assert it is NOT the dvxi5 fallback (the bug D-NET-121 guards).
	if (!expect(host_view.state().anchor_x != static_cast<int32_t>(0xfe56f854u),
	            "host-as-client anchor is not the dvxi5 fallback")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (C) Production HostOwner startup: the host-local client must receive the load-time pool stream
// before its first compact frame. A mounted pool-0 organic can precede its pool-1 ewep carrier in
// registry order, while the no-callback carrier has no live 0x0A body. Its 0x0D spawn is therefore
// the only faithful source for the carrier row used to lift the child's local compact pose.
// ---------------------------------------------------------------------------------------------------
bool run_host_startup_seeds_mounted_no_callback_carrier() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::AiSystem ai;
	world.ai = &ai;

	w::Entity infantry;
	infantry.kind = w::EntityKind::Organic;
	infantry.item_id = 5311; // US02 organic in the production Godot witness
	infantry.net_class_code = static_cast<uint8_t>(EntityClass::Infantry);
	const w::EntityHandle infantry_h = world.registry.spawn(0, infantry);
	if (!expect(infantry_h.valid() && ai.attach(infantry_h) >= 0,
	            "mounted startup fixture spawns pool-0 infantry first")) return false;

	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.item_id = 1294; // B50Cal ewep
	gun.position = {0.0f, 8.0f, 0.0f};
	gun.yaw = 45;
	gun.net_class_code = static_cast<uint8_t>(EntityClass::NoNetworkCallback);
	w::Seat gunner;
	gunner.type = w::SeatType::Gunner;
	gunner.bone_index = 6; // B50Cal.3di USRP row 6 is the witnessed Usegun bone
	gun.seats.push_back(gunner);
	const w::EntityHandle gun_h = world.registry.spawn_from(1, 0, gun);
	if (!expect(gun_h.valid() && gun_h.packed == 0x1000,
	            "B50Cal occupies the first pool-1 carrier handle")) return false;
	if (!expect(w::entity_process_vehicle_attach(world, infantry_h, gun_h, 6),
	            "infantry attaches to the B50Cal gunner bone")) return false;
	w::Entity *infantry_live = world.registry.get(infantry_h);
	const w::Entity *gun_live = world.registry.get(gun_h);
	if (!expect(infantry_live != nullptr && gun_live != nullptr,
	            "mounted startup fixture entities resolve")) return false;
	w::pose_mounted_occupant(*infantry_live, *gun_live, gun_live->seats[0]);

	ns::LoopbackChannel host_loop;
	np::HostOwner owner;
	owner.host_loopback = &host_loop;
	owner.ctx.world = &world;
	np::HostConfig cfg;
	cfg.config.server_name = "SINGLEPLAYERGAME";
	cfg.config.max_players = 1;
	cfg.socket_mode = np::SocketMode::Socketless;
	cfg.serve_and_play = true;
	np::start_host_session(owner, cfg);

	// Match NovaSimulation's startup order: construct/install the client view after host bring-up,
	// then fold the queued initial stream and first whole-world compact frame together.
	np::Server_TickUpdate(owner.ctx);
	np::ClientRuntime host_view(host_loop);
	host_view.view().set_item_class_resolver([](uint16_t type_id) {
		if (type_id == 0x14B9u) return EntityClass::Player;
		if (type_id == 5311u) return EntityClass::Infantry;
		if (type_id == 1294u) return EntityClass::NoNetworkCallback;
		return EntityClass::Unknown;
	});
	host_view.Client_ProcessNetworkFrame();

	const ns::ClientEntityState *carrier = host_view.view().state().find(gun_h.packed);
	if (!expect(carrier != nullptr && carrier->type_id == 1294,
	            "production host startup streams the B50Cal 0x0D carrier row")) return false;
	if (!expect(carrier->x == w::to_fixed(gun_live->position.x) &&
	                    carrier->y == w::to_fixed(gun_live->position.y) &&
	                    carrier->z == w::to_fixed(gun_live->position.z),
	            "host-local carrier row retains its absolute spawn pose")) return false;
	const ns::ClientEntityState *child = host_view.view().state().find(infantry_h.packed);
	if (!expect(child != nullptr && child->carrier_handle == gun_h.packed &&
	                    child->mount_bone == 6,
	            "mounted child compact retains its B50Cal carrier and raw Usegun bone"))
		return false;
	if (!expect(child->x == carrier->x && child->y == carrier->y && child->z == carrier->z,
	            "host-local mounted child lifts through the load-time no-callback carrier")) return false;

	const np::NapiNPConnection *self = nullptr;
	for (const np::NapiNPConnection &conn : owner.ctx.np_protocol.connection_list)
		if (conn.type == 2) self = &conn;
	if (!expect(self != nullptr && self->burst.spawned && self->burst.entity_batch_count > 0,
	            "host startup reaches in-match through the real initial-state burst")) return false;
	return true;
}

// ---------------------------------------------------------------------------------------------------
// (D) Host-owner registration boundary: a player created by tick_connections must be visible to
// adapter registration before its first authoritative body tick and per-connection 0x0A fan.
// ---------------------------------------------------------------------------------------------------
bool run_host_pump_hook_observes_remote_before_first_tick() {
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);

	np::HostOwner owner;
	np::test::bring_up_host(owner.ctx, np::ConnectionMode::HostClient,
			np::SocketMode::Socketless, 0x0FE0E112u);
	owner.ctx.world = &world;

	const PeerAddr peer{0x0100007Fu, 31000};
	np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<ns::UdpSessionTransport>(
			ns::UdpSessionTransport::Role::Host);

	np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.connection_id = np::kFirstJoinerDcb;
	conn.player_name = "LateJoiner";
	conn.self_id_seen = true;
	conn.phase = np::ConnectionPhase::PendingSpawn;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = ns::TransportMode::Client;
	conn.reply.roster_pushed = true;
	conn.burst.sync_state = 4;
	conn.burst.world_stream_phase = 8;
	conn.burst.loadout_received = true;
	conn.burst.entity_batch_count = 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	owner.ctx.np_protocol.next_connection_id = np::kFirstJoinerDcb + 1;

	HostPumpHookProbe hook{&owner, &world, &ai, peer};
	FirstLogicTickProbe logic_probe;
	logic_probe.hook = &hook;
	world.add_system(&logic_probe);

	NullDatagramSocket sock;
	np::host_session_pump(owner, sock, &observe_host_before_server_tick, &hook);

	np::NapiNPConnection *remote = nullptr;
	for (np::NapiNPConnection &candidate : owner.ctx.np_protocol.connection_list) {
		if (candidate.peer == peer) remote = &candidate;
	}
	if (!expect(hook.calls == 1, "pre-Server_TickUpdate hook runs exactly once")) return false;
	if (!expect(hook.saw_spawned_connection && hook.saw_live_entity && hook.saw_ai_component,
			"hook observes the remote spawned and AI-attached by tick_connections")) return false;
	if (!expect(hook.saw_before_logic && hook.saw_before_fan,
			"hook runs before the remote's first logic tick and 0x0A fan")) return false;
	if (!expect(logic_probe.ticks == 1 && logic_probe.first_tick_saw_hook &&
				logic_probe.first_tick_saw_player,
			"first authoritative logic tick observes completed registration")) return false;
	if (!expect(world.logic_tick == 1, "host pump advances exactly one logic tick")) return false;
	if (!expect(remote != nullptr && remote->link.s2c_phase == 1,
			"the first authoritative 0x0A fan follows registration")) return false;
	return true;
}

} // namespace

int main() {
	const bool ok = run_roundtrip() && run_host_as_client() &&
	                run_host_startup_seeds_mounted_no_callback_carrier() &&
	                run_host_pump_hook_observes_remote_before_first_tick();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
