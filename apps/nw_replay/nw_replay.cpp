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

#include "nw_replay_cli.h"
#include "nw_replay_partition.h"

#include "net_sockets.h"
#include <pcapio/pcap_reader.h>

#include <def/def.h>
#include <npwire/ingame_decode.h>
#include <npwire/replay_timeline.h>
#include <npwire/wire_capture.h>
#include <scr/scr.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
using namespace std::chrono;

namespace {

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
	replay::Args args;
	std::string parse_error;
	if (!replay::parse_args(argc, argv, args, parse_error)) {
		std::fprintf(stderr, "nw_replay: %s\n", parse_error.c_str());
		std::fprintf(stderr,
		    "usage: nw_replay <capture.pcapng> [--control-port N] [--speed X]\n"
		    "       [--max-gap-ms N] [--spectator-timeout-ms N] [--loop]\n"
		    "       [--print-roles] [--validate [--items <items.def>]]\n");
		return 2;
	}

	// --validate needs every datagram in memory to decode per-role; small captures
	// only (it loads the whole file).
	if (args.validate) {
		std::vector<net::PcapDatagram> pkts;
		if (!net::read_pcap_udp_file(args.pcap, pkts)) {
			std::fprintf(stderr, "nw_replay: failed to read pcap %s\n", args.pcap.c_str());
			return 1;
		}
		std::printf("loaded %zu datagrams from %s\n", pkts.size(), args.pcap.c_str());
		const replay::Roles roles = replay::partition_roles(pkts);
		if (!roles.ok()) {
			std::fprintf(stderr, "nw_replay: no NovaWorld session flows found\n");
			return 1;
		}
		print_roles(roles);
		return run_validate(pkts, roles, args.items);
	}

	// Pass 1 (streaming): partition + capture span with flat memory. Early-exit once
	// a stable host+client partition is found, so a multi-GB capture isn't fully
	// scanned twice (the join handshake — incl. the representative client — is early).
	replay::RoleTally tally;
	uint64_t tmin = UINT64_MAX, tmax = 0;
	size_t scanned = 0;
	if (!net::stream_pcap_udp_file(args.pcap, [&](const net::PcapDatagram &d) -> bool {
		    tally.add(d);
		    ++scanned;
		    if (d.ts_nanos) {
			    if (d.ts_nanos < tmin) tmin = d.ts_nanos;
			    if (d.ts_nanos > tmax) tmax = d.ts_nanos;
		    }
		    if (scanned >= 100000 && (scanned % 10000) == 0 && tally.finish().ok())
			    return false; // partition stable — stop scanning
		    return true;
	    })) {
		std::fprintf(stderr, "nw_replay: failed to read pcap %s\n", args.pcap.c_str());
		return 1;
	}
	std::printf("scanned %zu datagrams from %s\n", scanned, args.pcap.c_str());
	if (tmax >= tmin && tmin != UINT64_MAX)
		std::printf("capture span (scanned): %.3f s\n", double(tmax - tmin) / 1.0e9);

	const replay::Roles roles = tally.finish();
	if (!roles.ok()) {
		std::fprintf(stderr, "nw_replay: no NovaWorld session flows found in the "
		                     "capture (host + at least one client expected)\n");
		return 1;
	}
	print_roles(roles);
	if (args.print_roles) return 0;
	std::printf("spectator stream: representative session :%d S2C\n",
	            representative_port(roles));

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

	// Pass 2 (streaming): paced playback of the representative session's S2C to the
	// spectator, read straight off disk — flat memory even for a multi-GB capture.
	do {
		const auto wall0 = steady_clock::now();
		const replay::PlaybackReport report = replay::run_playback(
		    [&](const std::function<bool(const net::PcapDatagram &)> &on) {
			    return net::stream_pcap_udp_file(args.pcap, on);
		    },
		    [&](const net::PcapDatagram &d) { return is_spectator_datagram(d, roles); },
		    [&](const net::PcapDatagram &d) {
			    return net::udp_send_to(sock.get(), spectator, d.payload.data(),
			                            d.payload.size());
		    },
		    [&](int64_t ns) { std::this_thread::sleep_until(wall0 + nanoseconds(ns)); },
		    args.speed, args.max_gap_ms);

		// Report what actually happened. Previously both the capture read and
		// every send were discarded, so a mid-file read error or a dead
		// spectator socket still printed "playback complete" and exited 0.
		if (!report.read_ok) {
			std::fprintf(stderr, "playback FAILED: could not read %s (%zu datagram(s) sent)\n",
			             args.pcap.c_str(), report.sent);
			net::shutdown();
			return 1;
		}
		if (report.send_failures > 0) {
			std::fprintf(stderr, "playback FAILED: %zu send error(s) after %zu datagram(s)\n",
			             report.send_failures, report.sent);
			net::shutdown();
			return 1;
		}
		std::printf("playback complete (%zu datagrams)\n", report.sent);
		std::fflush(stdout);
	} while (args.loop);

	net::shutdown();
	return 0;
}
