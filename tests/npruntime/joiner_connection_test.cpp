// Drives the real np::JoinerConnection (the client mirror) against the real np server legs
// in-process — no sockets, each side's framing is the other's inbound. P2 port of
// tests/novaworld/joiner_session_test.cpp. Proves the D.0 self-identification loop end-to-end:
//
//   handshake (0x41/0x42) -> Driving
//   a decoy organic-spawn (wrong name) does NOT match
//   pump() the in-match spawn-gate burst (0x37/0x09/0x22/0x2F+0x2F+0x0B), with tick_connections
//     climbing entity_batch_count between stages, -> PeerSpawned
//   the test mimics NovaSimulation's PeerSpawned reaction: net.admit_peer -> wire handle H, then a
//     named S2C 0x0C organic-spawn (entity_name == PeerSpawned.peer_name)
//   the joiner NAME-MATCHES the record -> InMatch + self handle H == admitted handle
//   joiner.frame_c2s_uplink(H, ...) -> server surfaces PeerC2SInMatch carrying the 0x0C
//   that 0x0C, fed through the REAL NetSystem receive path, SNAPs the peer entity
//
// The wire handle H and the host-World handle are the SAME value here (as in production via
// NovaSimulation): the test admits the peer at PeerSpawned and stamps that handle into the
// organic-spawn record's slot_id, so the joiner adopts it and the uplink resolves to that entity.

#include <npruntime/joiner_connection.h>
#include <npruntime/napi_np_protocol.h>

#include <novaworld/ingame_decode.h>
#include <novaworld/ingame_encode.h>

#include <netsim/connection.h>
#include <netsim/net_system.h>
#include <netsim/udp_session_transport.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
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

const np::HostAcceptEvent *find_event(const np::HandleResult &r, np::HostAcceptEvent::Kind kind) {
	for (const auto &e : r.events) {
		if (e.kind == kind) return &e;
	}
	return nullptr;
}

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	return s;
}

// Build a 1-record S2C 0x0C organic-spawn body the joiner can name-match.
std::vector<uint8_t> make_organic_spawn(uint16_t slot_id, const std::string &name,
                                        int32_t x, int32_t y, int32_t z, int32_t orient,
                                        uint8_t team, uint16_t net_id) {
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

bool run() {
	const PeerAddr peer{0x0100007Fu, 30000}; // 127.0.0.1:30000
	const std::string kName = "JoinerOne";

	np::NapiNPServerCtx ctx;
	np::configure_session_runtime(ctx);
	np::JoinerConnection joiner(ClientSession::Config::jointoperations(), kName);

	// The host's live sim World — where the joiner's entity lives and where its C2S 0x0C uplink is
	// read-applied (this is NovaSimulation's job in production).
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host's own player spawned (sets cached.local_player)")) return false;

	ns::NetSystem net;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	const std::size_t conn =
			net.add_connection(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});

	uint32_t tick = 1;

	// --- 1) Handshake: ClientHello -> ServerHello -> ClientAuth -> ServerAuth ---
	{
		auto dg = joiner.start();
		if (!expect(joiner.phase() == np::JoinerConnection::Phase::Hello, "joiner -> Hello")) return false;
		auto rh = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), tick++);
		if (!expect(rh.outbound.size() == 1, "0x41 -> one ServerHello")) return false;

		auto pr = joiner.handle_datagram(rh.outbound[0].data(), rh.outbound[0].size());
		if (!expect(joiner.phase() == np::JoinerConnection::Phase::Auth, "joiner -> Auth")) return false;
		if (!expect(pr.outbound.size() == 1, "0x81 -> one ClientAuth")) return false;

		auto ra = np::handle_server_datagram(ctx, peer, pr.outbound[0].data(), pr.outbound[0].size(), tick++);
		if (!expect(ra.outbound.size() == 1, "0x42 -> one ServerAuth")) return false;

		auto pr2 = joiner.handle_datagram(ra.outbound[0].data(), ra.outbound[0].size());
		if (!expect(joiner.phase() == np::JoinerConnection::Phase::Driving, "joiner -> Driving (cr==1)"))
			return false;
		if (!expect(pr2.outbound.empty(), "ServerAuth produces no auto-reply (pump drives the burst)"))
			return false;
	}

	// --- 2) NEGATIVE: a decoy organic-spawn with a different name must NOT match ---
	{
		auto body = make_organic_spawn(0x0002, "SomeoneElse", w::to_fixed(1.0), 0, 0, 0, 1, 0xFFF2);
		std::vector<uint8_t> ddg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0C, body, ddg),
		            "host frames the decoy 0x0C")) return false;
		auto pr = joiner.handle_datagram(ddg.data(), ddg.size());
		if (!expect(!pr.reached_in_match && !joiner.in_match() && !joiner.has_self_handle(),
		            "decoy name does not flip the joiner to InMatch")) return false;
	}

	// --- 3) Drive the in-match spawn-gate burst (pump stages + host ticks) ---
	auto drive_stage = [&](int stage_ticks, uint32_t tick_base) {
		for (auto &d : joiner.pump(tick)) np::handle_server_datagram(ctx, peer, d.data(), d.size(), tick++);
		for (int i = 0; i < stage_ticks; ++i)
			np::tick_connections(ctx, 300, tick_base + static_cast<uint32_t>(i));
	};
	drive_stage(40, 1000); // 0x37 mission request -> climb entity_batch_count via streaming
	drive_stage(6, 2000);  // 0x09 transition marker
	drive_stage(1, 2500);  // 0x22 player-sync ack

	// stage 3 = the 0x2F/0x2F/0x0B burst that trips the spawn gate -> PeerSpawned.
	const np::HostAcceptEvent *spawn = nullptr;
	np::HostJoinerPose spawn_pose;
	std::string spawn_name;
	for (auto &d : joiner.pump(tick)) {
		auto r = np::handle_server_datagram(ctx, peer, d.data(), d.size(), 3000);
		if (const np::HostAcceptEvent *ev = find_event(r, np::HostAcceptEvent::Kind::PeerSpawned)) {
			spawn = ev;
			spawn_pose = ev->pose;
			spawn_name = ev->peer_name;
		}
	}
	if (!expect(spawn != nullptr, "spawn-gate burst trips PeerSpawned")) return false;
	if (!expect(np::connection_spawned(ctx, peer), "host marks the peer spawned")) return false;
	if (!expect(spawn_name == kName,
	            "PeerSpawned carries the joiner's ClientHello.co player name (Step 1 plumbing)"))
		return false;

	// --- 4) Mimic NovaSimulation's PeerSpawned reaction: admit the peer (-> H), then stream a
	//        NAMED organic-spawn so the joiner name-matches. ---
	const w::EntityHandle Hh = net.admit_peer(
			world, conn,
			player_spawn(w::Vec3{static_cast<float>(w::from_fixed(spawn_pose.pos_x)),
			                     static_cast<float>(w::from_fixed(spawn_pose.pos_y)),
			                     static_cast<float>(w::from_fixed(spawn_pose.pos_z))},
			             0, 0xFFF1));
	if (!expect(Hh.valid() && Hh != host_h, "joiner admitted at a distinct handle")) return false;

	const w::Entity *spawned = world.registry.get(Hh);
	const uint16_t net_id = spawned ? spawned->net_id : 0;
	// Distinctive spawn pose to assert the joiner caches it (the host streams the admitted entity's
	// pose; here a known value proves the field round-trip).
	const int32_t sx = w::to_fixed(70.0), sy = w::to_fixed(25.0), sz = w::to_fixed(56.0);
	const int32_t sorient = 0x40000000;
	{
		auto body = make_organic_spawn(Hh.packed, spawn_name, sx, sy, sz, sorient, 2, net_id);
		std::vector<uint8_t> sdg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0C, body, sdg),
		            "host frames the named organic-spawn 0x0C")) return false;
		auto pr = joiner.handle_datagram(sdg.data(), sdg.size());
		if (!expect(pr.reached_in_match, "the named record flips the joiner to InMatch")) return false;
	}
	if (!expect(joiner.in_match(), "joiner reached InMatch via the name-match")) return false;
	if (!expect(joiner.has_self_handle() && joiner.self_handle() == Hh.packed,
	            "joiner learned H == the host-admitted wire handle")) return false;
	if (!expect(joiner.spawn_pose().pos_x == sx && joiner.spawn_pose().pos_y == sy &&
	                    joiner.spawn_pose().pos_z == sz && joiner.spawn_pose().orientation == sorient,
	            "joiner cached the organic-spawn pose")) return false;
	if (!expect(joiner.spawn_pose().team == 2 && joiner.spawn_pose().net_id == net_id,
	            "joiner cached team + net_id from the record")) return false;

	// --- 5) The joiner sims its player and sends a C2S 0x0C uplink stamped with H ---
	PlayerExtendedUplink up;
	up.vehicle_handle = 0xFFFF;
	up.pos_x = w::to_fixed(100.0);
	up.pos_y = w::to_fixed(200.0);
	up.pos_z = w::to_fixed(-50.0);
	up.heading = 0x2000; // -> mission yaw 45
	up.pitch = 0x0100;
	auto uplink_dg = joiner.frame_c2s_uplink(joiner.self_handle(), 0x14B9, up);
	auto ru = np::handle_server_datagram(ctx, peer, uplink_dg.data(), uplink_dg.size(), 4000);
	const np::HostAcceptEvent *c2s = find_event(ru, np::HostAcceptEvent::Kind::PeerC2SInMatch);
	if (!expect(c2s != nullptr, "joiner uplink surfaces PeerC2SInMatch")) return false;
	if (!expect(c2s->in_match_c2s.size() == 1 && c2s->in_match_c2s[0].tag == 0x0C,
	            "PeerC2SInMatch carries the one 0x0C uplink")) return false;

	// --- 6) Faithful apply: feed the surfaced 0x0C through the REAL NetSystem path (what
	//        NovaSimulation does: push [tag][body] into the peer's transport, then NetSystem::tick
	//        drains + apply_player_intent SNAPs the entity). ---
	{
		std::vector<uint8_t> framed;
		framed.push_back(0x0C);
		framed.insert(framed.end(), c2s->in_match_c2s[0].payload.begin(),
		              c2s->in_match_c2s[0].payload.end());
		udp_host.push_inbound(framed);
		w::TickContext tctx;
		tctx.world = &world;
		tctx.is_authority = true;
		net.tick(world, tctx);
		if (!expect(udp_host.inbound_pending() == 0, "host drained the joiner connection")) return false;

		const w::Entity *je = world.registry.get(Hh);
		if (!expect(je != nullptr, "joiner entity present")) return false;
		if (!expect(je->position.x == static_cast<float>(w::from_fixed(up.pos_x)) &&
		                    je->position.y == static_cast<float>(w::from_fixed(up.pos_y)) &&
		                    je->position.z == static_cast<float>(w::from_fixed(up.pos_z)),
		            "joiner entity SNAPped to the uplink pose")) return false;
		if (!expect(je->yaw == 45, "joiner yaw = 90 - BAM/deg (== 45)")) return false;
		const w::AiEntity *jae = ai.for_handle(Hh);
		if (!expect(jae != nullptr && jae->net_is_remote_peer,
		            "joiner AiEntity marked net-snapped (remote peer)")) return false;

		// The host's own player was NOT touched by the joiner's uplink.
		const w::Entity *he = world.registry.get(host_h);
		if (!expect(he->position.x == 0.0f && he->position.y == 0.0f && he->position.z == 0.0f,
		            "host's own player pose unchanged")) return false;
	}

	return true;
}

} // namespace

int main() {
	const bool ok = run();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
