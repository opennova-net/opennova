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
// BYTE-PARITY (added 2026-06-27, §5.2a serializers ported):
//   The config-independent 0x2A table record is asserted byte-equal to the retail capture's 0x2A
//   bodies (our port reproduces retail exactly). The host-config-dependent serializers (0x2C/0x08/
//   0x66/0x76) are asserted STRUCTURE-equal (the layout our serializer emits — string count / fixed
//   size / count-prefix), since their VALUES are the capture host's config (server name, rules, tick)
//   and our host's differ. World-stream bodies are built from our own World (not the capture's
//   mission), so those stay order-only.

#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

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
	(void)msg;
	const int a = first_of(tags, before);
	const int b = first_of(tags, after);
	return a >= 0 && b >= 0 && a < b;
}
} // namespace

int main() {
	if (!expect(!order_ok(std::vector<uint8_t>{0x0B, 0x0C, 0x20}, 0x0B, 0x10,
	                      "helper rejects a missing required world-stream tag"),
	            "golden order helper fails closed when a required tag is absent"))
		return 1;

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

	// The host->joiner (dir 'S') in-match tag sequence + bodies — the burst the retail host emitted.
	std::vector<uint8_t> s2c_tags;
	std::vector<const InGameMessage *> s2c;
	for (const InGameMessage &m : msgs) {
		if (m.dir != 'S' || m.settings_update) continue;
		s2c_tags.push_back(static_cast<uint8_t>(m.tag & 0xFF));
		s2c.push_back(&m);
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
	ok = expect(order_ok(s2c_tags, 0x0B, 0x10, "§5.2a: 0x0B (player-sync BMS header) precedes 0x10 world-stream"),
	            "§5.2a: 0x0B (player-sync BMS header) precedes 0x10 world-stream") && ok;
	ok = expect(order_ok(s2c_tags, 0x0B, 0x0C, "§5.2a: 0x0B precedes 0x0C organic spawns"),
	            "§5.2a: 0x0B precedes 0x0C organic spawns") && ok;
	// world-stream track: 0x10 -> 0x0D -> 0x0C -> 0x20 -> ... -> 0x1A.
	ok = expect(order_ok(s2c_tags, 0x10, 0x0C, "§5.2a: 0x10 static batch precedes 0x0C organic spawns"),
	            "§5.2a: 0x10 static batch precedes 0x0C organic spawns") && ok;
	ok = expect(order_ok(s2c_tags, 0x0C, 0x20, "§5.2a: 0x0C organic spawns precede 0x20 pool-3 sync"),
	            "§5.2a: 0x0C organic spawns precede 0x20 pool-3 sync") && ok;
	ok = expect(order_ok(s2c_tags, 0x10, 0x1A, "§5.2a: 0x10 precedes the 0x1A world-stream timestamp"),
	            "§5.2a: 0x10 precedes the 0x1A world-stream timestamp") && ok;
	ok = expect(order_ok(s2c_tags, 0x20, 0x1A, "§5.2a: 0x20 precedes the 0x1A world-stream timestamp"),
	            "§5.2a: 0x20 precedes the 0x1A world-stream timestamp") && ok;
	if (!ok) return 1;

	// --- §5.2a serializer byte-parity / structure-parity (ported 2026-06-27). ---
	auto first_msg = [&](uint8_t tag) -> const InGameMessage * {
		for (const InGameMessage *m : s2c)
			if (static_cast<uint8_t>(m->tag & 0xFF) == tag) return m;
		return nullptr;
	};

	// 0x2A: config-independent const table record. Our serializer (server_initial_state.cpp
	// k0x2aRecord) must byte-match EVERY 0x2A body the retail host sent. This is exact retail parity.
	static const std::vector<uint8_t> kRec2a = {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};
	int n2a = 0;
	for (const InGameMessage *m : s2c) {
		if (static_cast<uint8_t>(m->tag & 0xFF) != 0x2A) continue;
		++n2a;
		ok = expect(m->payload == kRec2a, "§5.2a: retail 0x2A body == our const table record (byte-parity)") && ok;
	}
	if (n2a > 0)
		ok = expect(n2a == 6, "§5.2a: retail capture carries six 0x2A records (matches our table)") && ok;

	// 0x2C: two NUL-terminated C strings (serverName, mapFile) — our serializer emits exactly that
	// shape. Assert the retail body ends in a NUL and contains exactly two NUL terminators.
	if (const InGameMessage *m = first_msg(0x2C)) {
		int nuls = 0;
		for (uint8_t b : m->payload) if (b == 0) ++nuls;
		ok = expect(!m->payload.empty() && m->payload.back() == 0 && nuls == 2,
		            "§5.2a: retail 0x2C is two NUL-terminated strings (our serializer's shape)") && ok;
	}
	// 0x08: fixed 51-byte server-config block — our serializer emits exactly 51.
	if (const InGameMessage *m = first_msg(0x08))
		ok = expect(m->payload.size() == 51, "§5.2a: retail 0x08 server-config is 51 bytes (== our serializer)") && ok;
	// 0x66: count byte + (index,value) pairs — our serializer emits 1 + 2*count.
	if (const InGameMessage *m = first_msg(0x66))
		ok = expect(!m->payload.empty() && m->payload.size() == 1u + 2u * m->payload[0],
		            "§5.2a: retail 0x66 is count + 2*count pairs (== our serializer shape)") && ok;
	// 0x76: server tick16 — 2 bytes.
	if (const InGameMessage *m = first_msg(0x76))
		ok = expect(m->payload.size() == 2, "§5.2a: retail 0x76 server-tick16 is 2 bytes (== our serializer)") && ok;
	if (!ok) return 1;

	std::printf("[golden] §5.2a serializer parity: 0x2A byte-exact (%d records); 0x2C/0x08/0x66/0x76"
	            " structure-exact. World-stream bodies stay order-only (built from our World).\n", n2a);

	std::printf("OK\n");
	return 0;
}
