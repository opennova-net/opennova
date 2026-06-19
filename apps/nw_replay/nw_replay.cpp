// nw_replay — plays a captured NovaWorld session at Godot game-client instances.
//
// One process is "the network": it loads ONE pcap of a multi-player session
// (1 host + N clients, captured on one machine), partitions it by UDP port into
// roles (role 0 = host, roles 1..N = clients), waits for that many Godot
// instances to dial in, assigns each a role, and then forwards each role its raw
// captured datagrams — paced off the pcap timestamps on one shared clock so all
// roles stay in sync. The bytes on the wire are the EXACT captured UDP payloads
// (NAPI envelope onward); the Godot instance decodes them with the same libs it
// would use against a real server, so cutting over to real multiplayer is just
// pointing the client at a server address instead of this tool.
//
// What each role receives:
//   - a client role (port P): every session datagram with src==P or dst==P
//     (its own bidirectional flow, incl. its ClientAuth/ServerAuth handshake so
//     the in-engine decoder recovers SCRK, and its clean C2S 0x0C own-uplink).
//   - the host role: ALL session datagrams — the host is the server and sees the
//     whole world (its own player has no C2S uplink; it comes from the S2C 0x0A
//     broadcast, exactly as build_per_participant_world's host view reconstructs).
//
// CLI:
//   nw_replay <capture.pcapng> [--mission <bms>] [--control-port N]
//             [--port-base N] [--instances N] [--speed X] [--max-gap-ms N]
//             [--start-timeout-ms N] [--announce-file PATH]
//             [--print-roles] [--validate] [--no-dial] [--loop]
//
// --print-roles : print the role partition and exit (no sockets).
// --validate    : decode each role's stream (CaptureDecoder) and print the
//                 message/role counts that role would render (no sockets).
// --no-dial     : skip dial-in; stream each role to 127.0.0.1:(port_base+role)
//                 immediately (for a standalone UDP sink — no Godot needed).

#include "nw_replay_partition.h"

#include "net_sockets.h"
#include "pcap_reader.h"

#include <novaworld/wire_capture.h>
#include <novaworld/replay_timeline.h>
#include <novaworld/ingame_decode.h>
#include <def/def.h>
#include <scr/scr.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
using namespace std::chrono;

namespace {

// --- control protocol (replay-only scaffolding for anonymous dial-in) --------
// The DATA plane is pure captured bytes; only role assignment is bespoke, and it
// vanishes at real-MP cut-over (a real client does the gate/session handshake
// instead). HELLO: instance -> replay. ASSIGN: replay -> instance.
constexpr uint8_t HELLO_MAGIC[4] = {'N', 'W', 'R', 'H'};
constexpr uint8_t ASSIGN_MAGIC[4] = {'N', 'W', 'R', 'A'};

bool is_hello(const uint8_t *p, int n) {
	return n >= 4 && std::memcmp(p, HELLO_MAGIC, 4) == 0;
}

std::vector<uint8_t> build_assign(int role_index, bool is_host, int session_port,
                                  const std::string &mission) {
	std::vector<uint8_t> b(ASSIGN_MAGIC, ASSIGN_MAGIC + 4);
	b.push_back(uint8_t(role_index));
	b.push_back(is_host ? 1 : 0);
	b.push_back(uint8_t(session_port & 0xFF));
	b.push_back(uint8_t((session_port >> 8) & 0xFF));
	b.push_back(uint8_t(std::min<size_t>(mission.size(), 255)));
	for (size_t i = 0; i < mission.size() && i < 255; ++i) b.push_back(uint8_t(mission[i]));
	return b;
}

struct Args {
	std::string pcap;
	std::string mission;
	std::string items; // items.def for --validate motion check
	std::string announce_file;
	uint16_t control_port = 42000;
	uint16_t port_base = 42001; // role K listens on port_base + K (in --no-dial)
	int instances = -1;         // expected dial-ins; -1 => role count
	double speed = 1.0;
	int max_gap_ms = 250;
	int start_timeout_ms = 30000;
	bool print_roles = false;
	bool validate = false;
	bool no_dial = false;
	bool loop = false;
};

bool parse_args(int argc, char **argv, Args &a) {
	for (int i = 1; i < argc; ++i) {
		const std::string s = argv[i];
		auto next = [&](const char *what) -> const char * {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "missing value for %s\n", what);
				return nullptr;
			}
			return argv[++i];
		};
		if (s == "--mission") { const char *v = next("--mission"); if (!v) return false; a.mission = v; }
		else if (s == "--items") { const char *v = next("--items"); if (!v) return false; a.items = v; }
		else if (s == "--announce-file") { const char *v = next("--announce-file"); if (!v) return false; a.announce_file = v; }
		else if (s == "--control-port") { const char *v = next("--control-port"); if (!v) return false; a.control_port = uint16_t(std::atoi(v)); }
		else if (s == "--port-base") { const char *v = next("--port-base"); if (!v) return false; a.port_base = uint16_t(std::atoi(v)); }
		else if (s == "--instances") { const char *v = next("--instances"); if (!v) return false; a.instances = std::atoi(v); }
		else if (s == "--speed") { const char *v = next("--speed"); if (!v) return false; a.speed = std::atof(v); }
		else if (s == "--max-gap-ms") { const char *v = next("--max-gap-ms"); if (!v) return false; a.max_gap_ms = std::atoi(v); }
		else if (s == "--start-timeout-ms") { const char *v = next("--start-timeout-ms"); if (!v) return false; a.start_timeout_ms = std::atoi(v); }
		else if (s == "--print-roles") a.print_roles = true;
		else if (s == "--validate") a.validate = true;
		else if (s == "--no-dial") a.no_dial = true;
		else if (s == "--loop") a.loop = true;
		else if (!s.empty() && s[0] == '-') { std::fprintf(stderr, "unknown flag %s\n", s.c_str()); return false; }
		else if (a.pcap.empty()) a.pcap = s;
		else { std::fprintf(stderr, "unexpected arg %s\n", s.c_str()); return false; }
	}
	if (a.speed <= 0.0) a.speed = 1.0;
	return !a.pcap.empty();
}

// Map a session datagram's session_port -> role index (0 = host).
struct RolePlan {
	replay::Roles roles;
	std::map<int, int> client_role; // client port -> role index (1..N)
	int role_of_session(int session_port) const {
		if (session_port == roles.host_port) return 0; // (host-side port; unusual)
		auto it = client_role.find(session_port);
		return it == client_role.end() ? -1 : it->second;
	}
};

RolePlan build_plan(const replay::Roles &roles) {
	RolePlan p;
	p.roles = roles;
	for (size_t i = 0; i < roles.client_ports.size(); ++i)
		p.client_role[roles.client_ports[i]] = int(i) + 1;
	return p;
}

void print_roles(const replay::Roles &r) {
	std::printf("roles: host=:%d", r.host_port);
	for (size_t i = 0; i < r.client_ports.size(); ++i)
		std::printf("  client[%zu]=:%d", i + 1, r.client_ports[i]);
	std::printf("  (%d total)\n", r.role_count());
}

// The representative client session whose S2C broadcast the host role renders.
// The host has no own C2S uplink — it spectates the authoritative world, and in a
// co-op capture one client's S2C broadcast IS the whole world. The first client.
int representative_port(const replay::Roles &roles) {
	return roles.client_ports.empty() ? 0 : roles.client_ports.front();
}

// Should datagram `d` be delivered to role `role_index`?
//  - role 0 (host): the representative session's S2C broadcast only.
//  - role k>0 (client): that client's full bidirectional session.
// ONE session per role is the invariant that keeps the in-engine decode correct
// after the replay->Godot hop loses the original UDP ports (multi-session to one
// instance would collide SCRK keys; single-session decodes cleanly because each
// direction keys to its own scrk independently of the constant hop ports).
bool datagram_for_role(const net::PcapDatagram &d, const replay::Roles &roles,
                       int role_index) {
	const replay::SessionKind k = replay::classify_session(d.payload);
	if (k == replay::SessionKind::NotSession) return false;
	const int sp = replay::is_server_kind(k) ? d.dstport : d.srcport;
	if (role_index == 0)
		return replay::is_server_kind(k) && sp == representative_port(roles);
	return sp == roles.client_ports[role_index - 1];
}

// Per-role datagram subset, used by --validate (mirrors live forwarding).
std::vector<CaptureDatagram> role_stream(const std::vector<net::PcapDatagram> &pkts,
                                         const replay::Roles &roles, int role_index) {
	std::vector<CaptureDatagram> caps;
	for (const auto &d : pkts)
		if (datagram_for_role(d, roles, role_index))
			caps.push_back({d.frame_index, d.srcport, d.dstport, d.payload});
	return caps;
}

// Minimal items.def -> wire_id->EntityClass loader (the class table needed to
// walk S2C 0x0A). Mirrors nw_pp's load_items_def class half.
std::map<uint16_t, EntityClass> load_class_table(const std::string &path) {
	std::map<uint16_t, EntityClass> tbl;
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return tbl;
	const std::streamsize n = f.tellg();
	if (n <= 0) return tbl;
	std::vector<uint8_t> raw(static_cast<size_t>(n));
	f.seekg(0);
	if (!f.read(reinterpret_cast<char *>(raw.data()), n)) return tbl;
	const uint8_t *plain = raw.data();
	size_t plain_size = raw.size();
	std::vector<uint8_t> dec;
	if (scr_is_scr(raw.data(), raw.size())) {
		dec.resize(raw.size());
		size_t out = dec.size();
		if (scr_decrypt_buf(raw.data(), raw.size(), dec.data(), &out, SCR_KEY_JO_DFX2) != 0)
			return tbl;
		dec.resize(out);
		plain = dec.data();
		plain_size = dec.size();
	}
	DefItemsFile items{};
	if (def_parse_items_memory(plain, plain_size, &items) != 0) return tbl;
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &it = items.entries[i];
		const int wire = it.id - 100000;
		if (wire < 0 || wire >= 0x10000) continue;
		EntityClass c = class_from_tag(it.ai_function);
		if (c == EntityClass::Unknown) c = class_from_tag(it.move_function);
		if (c != EntityClass::Unknown) tbl[uint16_t(wire)] = c;
	}
	def_free_items(&items);
	return tbl;
}

int run_validate(const std::vector<net::PcapDatagram> &pkts, const replay::Roles &roles,
                 const std::string &items_path) {
	std::map<uint16_t, EntityClass> class_tbl;
	if (!items_path.empty()) {
		class_tbl = load_class_table(items_path);
		std::printf("class table: %zu entries from %s\n", class_tbl.size(), items_path.c_str());
	}
	auto class_of = [&](uint16_t t) {
		auto it = class_tbl.find(t);
		return it == class_tbl.end() ? EntityClass::Unknown : it->second;
	};
	for (int r = 0; r < roles.role_count(); ++r) {
		const std::vector<CaptureDatagram> caps = role_stream(pkts, roles, r);
		const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);
		int s2c = 0, c2s = 0;
		std::map<int, int> per_session;
		for (const auto &m : msgs) {
			(m.dir == 'S' ? s2c : c2s)++;
			per_session[m.session]++;
		}
		std::printf("role %d (%s :%d): %zu datagrams -> %zu msgs (S2C=%d C2S=%d) "
		            "across %zu session(s)\n",
		            r, r == 0 ? "host" : "client",
		            r == 0 ? roles.host_port : roles.client_ports[r - 1],
		            caps.size(), msgs.size(), s2c, c2s, per_session.size());
		if (!items_path.empty()) {
			const ReplayTimeline tl = build_replay_timeline(msgs, class_of);
			int moving = 0, total_fu = 0;
			for (const auto &e : tl.entities) {
				int fu = 0;
				for (const auto &s : e.track)
					if (s.source == ReplaySampleSource::FrameUpdate) fu++;
				if (fu > 0) moving++;
				total_fu += fu;
			}
			std::printf("        timeline: %zu entities, %d with motion, %d frameupdate samples\n",
			            tl.entities.size(), moving, total_fu);
		}
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	Args args;
	if (!parse_args(argc, argv, args)) {
		std::fprintf(stderr,
		    "usage: nw_replay <capture.pcapng> [--mission <bms>] [--control-port N]\n"
		    "       [--port-base N] [--instances N] [--speed X] [--max-gap-ms N]\n"
		    "       [--start-timeout-ms N] [--announce-file PATH]\n"
		    "       [--print-roles] [--validate] [--no-dial] [--loop]\n");
		return 2;
	}

	std::vector<net::PcapDatagram> pkts;
	if (!net::read_pcap_udp_file(args.pcap, pkts)) {
		std::fprintf(stderr, "nw_replay: failed to read pcap %s\n", args.pcap.c_str());
		return 1;
	}
	std::printf("loaded %zu datagrams from %s\n", pkts.size(), args.pcap.c_str());
	{
		uint64_t tmin = UINT64_MAX, tmax = 0;
		for (const auto &d : pkts)
			if (d.ts_nanos) {
				if (d.ts_nanos < tmin) tmin = d.ts_nanos;
				if (d.ts_nanos > tmax) tmax = d.ts_nanos;
			}
		if (tmax >= tmin && tmin != UINT64_MAX)
			std::printf("capture span: %.3f s\n", double(tmax - tmin) / 1.0e9);
	}

	const replay::Roles roles = replay::partition_roles(pkts);
	if (!roles.ok()) {
		std::fprintf(stderr, "nw_replay: no NovaWorld session flows found in the "
		                     "capture (host + at least one client expected)\n");
		return 1;
	}
	print_roles(roles);

	if (args.print_roles) return 0;
	if (args.validate) return run_validate(pkts, roles, args.items);

	const RolePlan plan = build_plan(roles);
	const int role_count = roles.role_count();

	const int rep_port = representative_port(roles);

	// Precompute the paced send plan: every session datagram in capture order,
	// classified once (its owning client session port + direction) so the hot
	// send loop never re-decodes the envelope.
	struct PlanItem {
		size_t pkt_index;
		int session_port;
		bool is_server;
		uint64_t ts_nanos;
	};
	std::vector<PlanItem> playlist;
	playlist.reserve(pkts.size());
	for (size_t i = 0; i < pkts.size(); ++i) {
		const replay::SessionKind k = replay::classify_session(pkts[i].payload);
		if (k == replay::SessionKind::NotSession) continue; // noise
		const bool srv = replay::is_server_kind(k);
		const int sp = srv ? pkts[i].dstport : pkts[i].srcport;
		playlist.push_back({i, sp, srv, pkts[i].ts_nanos});
	}
	std::printf("playlist: %zu session datagrams\n", playlist.size());

	if (net::startup() != 0) {
		std::fprintf(stderr, "nw_replay: socket startup failed\n");
		return 1;
	}
	net::ScopedSocket sock(net::udp_bind(args.control_port));
	if (!sock.is_valid()) {
		std::fprintf(stderr, "nw_replay: failed to bind control port %d\n", args.control_port);
		net::shutdown();
		return 1;
	}

	// role index -> connected instance endpoint.
	std::vector<net::Endpoint> role_ep(role_count);
	std::vector<bool> role_connected(role_count, false);

	if (args.no_dial) {
		for (int r = 0; r < role_count; ++r) {
			role_ep[r] = net::Endpoint{{127, 0, 0, 1}, uint16_t(args.port_base + r)};
			role_connected[r] = true;
		}
		std::printf("--no-dial: streaming role K to 127.0.0.1:%d+K\n", args.port_base);
	} else {
		const int want = args.instances > 0 ? std::min(args.instances, role_count) : role_count;
		std::printf("waiting for %d instance(s) to dial in on control port %d "
		            "(announce endpoint: 127.0.0.1:%d)\n", want, args.control_port,
		            args.control_port);
		if (!args.announce_file.empty()) {
			std::ofstream f(args.announce_file);
			if (f) f << "{\"endpoint\":\"127.0.0.1:" << args.control_port << "\",\"roles\":"
			         << role_count << "}\n";
		}
		const auto deadline = steady_clock::now() + milliseconds(args.start_timeout_ms);
		int connected = 0;
		while (connected < want && steady_clock::now() < deadline) {
			uint8_t buf[64];
			net::Endpoint from;
			const int n = net::udp_recv_from(sock.get(), buf, sizeof(buf), from, 200);
			if (n <= 0) continue;
			if (!is_hello(buf, n)) continue;
			const int role = connected; // assign in connection order; role 0 = host
			role_ep[role] = from;
			role_connected[role] = true;
			const bool is_host = (role == 0);
			const int session_port = is_host ? roles.host_port : roles.client_ports[role - 1];
			const std::vector<uint8_t> assign =
			    build_assign(role, is_host, session_port, args.mission);
			net::udp_send_to(sock.get(), from, assign.data(), assign.size());
			std::printf("  instance %s -> role %d (%s :%d)\n",
			            net::endpoint_to_string(from).c_str(), role,
			            is_host ? "host" : "client", session_port);
			connected++;
		}
		if (connected == 0) {
			std::fprintf(stderr, "nw_replay: no instances dialed in within %d ms\n",
			             args.start_timeout_ms);
			net::shutdown();
			return 1;
		}
		if (connected < role_count)
			std::fprintf(stderr, "nw_replay: WARNING only %d/%d roles claimed — the "
			                     "unclaimed role(s) won't be shown\n", connected, role_count);
	}

	// --- paced playback ------------------------------------------------------
	do {
		const auto wall0 = steady_clock::now();
		uint64_t prev_ts = playlist.empty() ? 0 : playlist.front().ts_nanos;
		double sched_ns = 0.0;
		size_t sent = 0;
		for (const auto &item : playlist) {
			const uint64_t gap = item.ts_nanos >= prev_ts ? item.ts_nanos - prev_ts : 0;
			double gap_ms = double(gap) / 1.0e6 / args.speed;
			if (gap_ms > double(args.max_gap_ms)) gap_ms = double(args.max_gap_ms);
			sched_ns += gap_ms * 1.0e6;
			prev_ts = item.ts_nanos;
			std::this_thread::sleep_until(wall0 + nanoseconds(int64_t(sched_ns)));

			const std::vector<uint8_t> &payload = pkts[item.pkt_index].payload;
			// The host instance (role 0) renders the representative session's S2C
			// broadcast (single session -> clean decode -> authoritative world).
			if (role_connected[0] && item.is_server && item.session_port == rep_port)
				net::udp_send_to(sock.get(), role_ep[0], payload.data(), payload.size());
			// The owning client instance gets its full bidirectional session.
			const int cr = plan.role_of_session(item.session_port);
			if (cr > 0 && cr < role_count && role_connected[cr])
				net::udp_send_to(sock.get(), role_ep[cr], payload.data(), payload.size());
			sent++;
		}
		std::printf("playback complete (%zu datagrams)\n", sent);
	} while (args.loop);

	net::shutdown();
	return 0;
}
