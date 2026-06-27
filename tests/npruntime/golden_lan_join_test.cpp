// P3 golden — validate the §5.2a initial-state burst ORDER against the retail LAN host/join capture
// (.scratch/golden/retail-lan-host-join.pcapng). Env-gated on NW_GOLDEN_LAN_JOIN with a DEFAULT_*
// fallback; skips cleanly when the gitignored golden is absent (no committed derived oracle).
//
// WHAT THIS PROVES (the structural P3 bar, honestly scoped):
//   Decoded host->joiner S2C stream — the burst a RETAIL host emits — appears in the §5.2a order our
//   P3 machine (server_initial_state.cpp) reproduces: the player-sync bundle (0x0B BMS header)
//   precedes the world-stream batches, and the world-stream batches land in 0x10 -> 0x0C -> 0x20 ->
//   0x1A order. This is the ground-truth cross-check of our ordering model. (Our host's OWN emitted
//   order is asserted field-for-field by npruntime_initial_state_burst.)
//
// WHAT IS DEFERRED (documented, NOT faked green):
//   Full-datagram body byte-parity is NOT asserted — the ~8 unwitnessed §5.2a serializers
//   (0x2C/0x08/0x2A/0x66/0x76/0x45/0x7E/0x1A) are a tracked grill follow-up, and our world-stream
//   bodies are built from our own World (not the capture's mission), so they would not byte-match.
//   The observed sequence + the deferred-tag set are printed for the record.

#include <novaworld/wire_capture.h>

#include "pcap_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#ifndef DEFAULT_LAN_JOIN_PCAP
#define DEFAULT_LAN_JOIN_PCAP ""
#endif

namespace {
using namespace opennova;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// first-occurrence index of `tag` in the ordered S2C tag list, or -1 if absent.
int first_of(const std::vector<uint8_t> &tags, uint8_t tag) {
	for (size_t i = 0; i < tags.size(); ++i)
		if (tags[i] == tag) return static_cast<int>(i);
	return -1;
}

// Assert §5.2a relative order for two tags ONLY when both are present (robust to a capture that omits
// a tag): first(before) must precede first(after).
bool order_ok(const std::vector<uint8_t> &tags, uint8_t before, uint8_t after, const char *msg) {
	const int a = first_of(tags, before);
	const int b = first_of(tags, after);
	if (a < 0 || b < 0) return true; // one absent — nothing to assert
	return expect(a < b, msg);
}
} // namespace

int main() {
	std::string path;
	if (const char *env = std::getenv("NW_GOLDEN_LAN_JOIN"); env && *env)
		path = env;
	else
		path = DEFAULT_LAN_JOIN_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (path.empty() || !net::read_pcap_udp_file(path, pkts)) {
		std::printf("[skip] golden LAN host/join capture not found (set NW_GOLDEN_LAN_JOIN) — '%s'\n",
		            path.c_str());
		return 0; // skip clean — CI stays green without the gitignored golden
	}

	// Outer-decode the whole capture (recovers each side's SCRK from the handshake as it streams by).
	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);

	// The host->joiner (dir 'S') in-match tag sequence — the burst the retail host emitted.
	std::vector<uint8_t> s2c_tags;
	for (const InGameMessage &m : msgs) {
		if (m.dir != 'S' || m.settings_update) continue;
		s2c_tags.push_back(static_cast<uint8_t>(m.tag & 0xFF));
	}
	if (!expect(!s2c_tags.empty(),
	            "golden decoded a host->joiner S2C stream (handshake present so SCRK recovered)"))
		return 1;

	// Print the observed sequence + a per-tag count for the record.
	std::printf("[golden] host->joiner S2C in-match tags (%zu):", s2c_tags.size());
	for (uint8_t t : s2c_tags) std::printf(" %02X", t);
	std::printf("\n");

	// --- §5.2a order assertions (only for tags actually present in this capture). ---
	bool ok = true;
	// player-sync bundle (0x0B BMS header) precedes the world-stream batches.
	ok = order_ok(s2c_tags, 0x0B, 0x10, "§5.2a: 0x0B (player-sync BMS header) precedes 0x10 world-stream") && ok;
	ok = order_ok(s2c_tags, 0x0B, 0x0C, "§5.2a: 0x0B precedes 0x0C organic spawns") && ok;
	// world-stream track: 0x10 -> 0x0D -> 0x0C -> 0x20 -> ... -> 0x1A.
	ok = order_ok(s2c_tags, 0x10, 0x0C, "§5.2a: 0x10 static batch precedes 0x0C organic spawns") && ok;
	ok = order_ok(s2c_tags, 0x0C, 0x20, "§5.2a: 0x0C organic spawns precede 0x20 pool-3 sync") && ok;
	ok = order_ok(s2c_tags, 0x10, 0x1A, "§5.2a: 0x10 precedes the 0x1A world-stream timestamp") && ok;
	ok = order_ok(s2c_tags, 0x20, 0x1A, "§5.2a: 0x20 precedes the 0x1A world-stream timestamp") && ok;
	if (!ok) return 1;

	// --- Deferred-gap note (NOT asserted): the unwitnessed §5.2a serializers our host omits. ---
	const uint8_t deferred[] = {0x2C, 0x08, 0x2A, 0x66, 0x76, 0x45, 0x7E, 0x1A};
	std::printf("[golden] DEFERRED §5.2a serializers (our host emits nothing pending the grill wave;"
	            " present in this retail capture =");
	for (uint8_t t : deferred) {
		if (first_of(s2c_tags, t) >= 0) std::printf(" %02X", t);
	}
	std::printf(")\n");
	std::printf("[golden] body byte-parity DEFERRED (our world-stream bodies are built from our own"
	            " World, not the capture's mission). §5.2a tag ORDER validated above.\n");

	std::printf("OK\n");
	return 0;
}
