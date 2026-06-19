// nw_replay — plays a captured NovaWorld session at a Godot spectator.
//
// "The network" stand-in: load ONE pcap, find the host's authoritative S2C
// broadcast, and stream those raw datagrams to a connected spectator -- paced off
// the pcap timestamps. The bytes on the wire are the EXACT captured UDP payloads
// (NAPI envelope onward); the spectator decodes them with the same libs it would
// use against a real server, and reads the map name off the wire (S2C 0x7B). So
// cutting over to real multiplayer is just pointing the client at a server.
//
// The spectator registers by sending any datagram to the control port; nw_replay
// records its address and streams the world to it. One session is streamed (the
// representative client's S2C broadcast), which decodes cleanly on the spectator
// even though the hop rewrites the UDP ports.
//
// CLI:
//   nw_replay <capture.pcapng> [--control-port N] [--speed X] [--max-gap-ms N]
//             [--spectator-timeout-ms N] [--loop]
//             [--print-roles] [--validate [--items <items.def>]]
//
// --print-roles : print the host/client port partition and exit (no sockets).
// --validate    : decode each role's stream + (with --items) report the motion
//                 that role would render; no sockets.

#include "nw_replay_partition.h"

#include "net_sockets.h"
#include "pcap_reader.h"

#include <def/def.h>
#include <novaworld/ingame_decode.h>
#include <novaworld/replay_timeline.h>
#include <novaworld/wire_capture.h>
#include <scr/scr.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
using namespace std::chrono;

namespace {

struct Args {
	std::string pcap;
	std::string items; // items.def for the --validate motion check
	uint16_t control_port = 42000;
	double speed = 1.0;
	int max_gap_ms = 250;
	int spectator_timeout_ms = 120000;
	bool print_roles = false;
	bool validate = false;
	bool loop = false;
};

bool parse_args(int argc, char **argv, Args &a) {
	for (int i = 1; i < argc; ++i) {
		const std::string s = argv[i];
		auto next = [&](const char *what) -> const char * {
			if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); return nullptr; }
			return argv[++i];
		};
		if (s == "--items") { const char *v = next("--items"); if (!v) return false; a.items = v; }
		else if (s == "--control-port") { const char *v = next("--control-port"); if (!v) return false; a.control_port = uint16_t(std::atoi(v)); }
		else if (s == "--speed") { const char *v = next("--speed"); if (!v) return false; a.speed = std::atof(v); }
		else if (s == "--max-gap-ms") { const char *v = next("--max-gap-ms"); if (!v) return false; a.max_gap_ms = std::atoi(v); }
		else if (s == "--spectator-timeout-ms") { const char *v = next("--spectator-timeout-ms"); if (!v) return false; a.spectator_timeout_ms = std::atoi(v); }
		else if (s == "--print-roles") a.print_roles = true;
		else if (s == "--validate") a.validate = true;
		else if (s == "--loop") a.loop = true;
		else if (!s.empty() && s[0] == '-') { std::fprintf(stderr, "unknown flag %s\n", s.c_str()); return false; }
		else if (a.pcap.empty()) a.pcap = s;
		else { std::fprintf(stderr, "unexpected arg %s\n", s.c_str()); return false; }
	}
	if (a.speed <= 0.0) a.speed = 1.0;
	return !a.pcap.empty();
}

void print_roles(const replay::Roles &r) {
	std::printf("roles: host=:%d", r.host_port);
	for (size_t i = 0; i < r.client_ports.size(); ++i)
		std::printf("  client[%zu]=:%d", i + 1, r.client_ports[i]);
	std::printf("  (%d total)\n", r.role_count());
}

// The representative client session whose S2C broadcast is the authoritative
// world a spectator renders (the host has no own C2S uplink; one client's S2C
// broadcast is the whole world). The first client port.
int representative_port(const replay::Roles &roles) {
	return roles.client_ports.empty() ? 0 : roles.client_ports.front();
}

// Does datagram `d` belong to the spectator stream? The representative session's
// S2C broadcast only -- a SINGLE session, so the spectator's in-engine decoder
// keys it cleanly even though the replay hop rewrites the UDP ports.
bool is_spectator_datagram(const net::PcapDatagram &d, const replay::Roles &roles) {
	const replay::SessionKind k = replay::classify_session(d.payload);
	return replay::is_server_kind(k) && d.dstport == representative_port(roles);
}

// Per-role datagram subset (used only by --validate): role 0 = the spectator/host
// S2C stream, roles 1..N = each client's full bidirectional session.
std::vector<CaptureDatagram> role_stream(const std::vector<net::PcapDatagram> &pkts,
                                         const replay::Roles &roles, int role_index) {
	std::vector<CaptureDatagram> caps;
	for (const auto &d : pkts) {
		bool keep = false;
		if (role_index == 0) {
			keep = is_spectator_datagram(d, roles);
		} else {
			const replay::SessionKind k = replay::classify_session(d.payload);
			if (k != replay::SessionKind::NotSession) {
				const int sp = replay::is_server_kind(k) ? d.dstport : d.srcport;
				keep = (sp == roles.client_ports[role_index - 1]);
			}
		}
		if (keep) caps.push_back({d.frame_index, d.srcport, d.dstport, d.payload});
	}
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
		    "usage: nw_replay <capture.pcapng> [--control-port N] [--speed X]\n"
		    "       [--max-gap-ms N] [--spectator-timeout-ms N] [--loop]\n"
		    "       [--print-roles] [--validate [--items <items.def>]]\n");
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

	// Spectator stream: the representative session's S2C broadcast, in capture
	// order, with timestamps for pacing.
	struct PlanItem {
		size_t pkt_index;
		uint64_t ts_nanos;
	};
	std::vector<PlanItem> playlist;
	playlist.reserve(pkts.size());
	for (size_t i = 0; i < pkts.size(); ++i)
		if (is_spectator_datagram(pkts[i], roles))
			playlist.push_back({i, pkts[i].ts_nanos});
	std::printf("spectator stream: %zu datagrams (representative session :%d S2C)\n",
	            playlist.size(), representative_port(roles));

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

	// Wait for the spectator to register (any datagram announces its address).
	std::printf("waiting for a spectator on 127.0.0.1:%d ...\n", args.control_port);
	std::fflush(stdout);
	net::Endpoint spectator;
	bool have = false;
	const auto deadline = steady_clock::now() + milliseconds(args.spectator_timeout_ms);
	while (!have && steady_clock::now() < deadline) {
		uint8_t buf[64];
		net::Endpoint from;
		const int n = net::udp_recv_from(sock.get(), buf, sizeof(buf), from, 200);
		if (n <= 0) continue;
		spectator = from;
		have = true;
		std::printf("spectator %s connected\n", net::endpoint_to_string(from).c_str());
		std::fflush(stdout);
	}
	if (!have) {
		std::fprintf(stderr, "nw_replay: no spectator connected within %d ms\n",
		             args.spectator_timeout_ms);
		net::shutdown();
		return 1;
	}

	// Paced playback to the spectator.
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
			net::udp_send_to(sock.get(), spectator, payload.data(), payload.size());
			sent++;
		}
		std::printf("playback complete (%zu datagrams)\n", sent);
		std::fflush(stdout);
	} while (args.loop);

	net::shutdown();
	return 0;
}
