// PeerAddr: the octet packing and the two renderings.
//
// These are not cosmetic helpers. The dotted-quad string is a DB key: the
// novaworld host_players rows are written at add_player time and deleted by
// remove_player_by_peer, and a second differently-derived spelling of the same
// address silently leaks rows. The packing direction is equally load-bearing —
// retail's _connectlog.txt reads the four payload bytes positionally, so the
// other endianness prints "1.0.0.127" for 127.0.0.1 (verified 2026-04-27).
#include <npwire/peer_addr.h>

#include <array>
#include <cstdio>
#include <string>

using opennova::PeerAddr;
using opennova::peer_addr_from_octets;
using opennova::peer_addr_ip_to_string;
using opennova::peer_addr_to_string;

static int failures = 0;

static void check(bool cond, const char *what) {
	if (!cond) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
}

static void check_str(const std::string &got, const char *want, const char *what) {
	if (got != want) {
		std::printf("FAIL: %s — got '%s', want '%s'\n", what, got.c_str(), want);
		++failures;
	}
}

int main() {
	// Octet 0 lands in the LOW byte: 127.0.0.1 -> 0x0100007F.
	const PeerAddr local = peer_addr_from_octets({127, 0, 0, 1}, 32768);
	check(local.ip == 0x0100007Fu, "127.0.0.1 packs octet 0 into the low byte");
	check(local.port == 32768, "port carried");
	check_str(peer_addr_ip_to_string(local), "127.0.0.1", "loopback renders low->high");
	check_str(peer_addr_to_string(local), "127.0.0.1:32768", "loopback with port");

	// Every octet distinct: catches any transposition the symmetric cases miss.
	const PeerAddr asym = peer_addr_from_octets({10, 20, 30, 40}, 7597);
	check(asym.ip == 0x281E140Au, "asymmetric quad packs low->high");
	check_str(peer_addr_ip_to_string(asym), "10.20.30.40", "asymmetric quad renders in order");
	check_str(peer_addr_to_string(asym), "10.20.30.40:7597", "asymmetric quad with port");

	// Full-range octets: no sign extension, no zero padding, no truncation.
	const PeerAddr edge = peer_addr_from_octets({255, 0, 128, 1}, 65535);
	check_str(peer_addr_ip_to_string(edge), "255.0.128.1", "edge octets render exactly");
	check_str(peer_addr_to_string(edge), "255.0.128.1:65535", "max port renders exactly");

	// Round trip: the packer and the renderer are inverses over the quad.
	for (uint32_t a = 0; a < 256u; a += 37u) {
		for (uint32_t d = 0; d < 256u; d += 53u) {
			const PeerAddr p = peer_addr_from_octets(
					{static_cast<uint8_t>(a), 1, 2, static_cast<uint8_t>(d)}, 1);
			const std::string want = std::to_string(a) + ".1.2." + std::to_string(d);
			check_str(peer_addr_ip_to_string(p), want.c_str(), "round trip");
		}
	}

	// The label the connection layer keys sessions by is ip + ':' + port, with
	// no padding — npruntime's peer_session_id is exactly this string.
	const PeerAddr zero{};
	check_str(peer_addr_to_string(zero), "0.0.0.0:0", "zero address renders unpadded");

	if (failures == 0) std::printf("peer_addr_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
