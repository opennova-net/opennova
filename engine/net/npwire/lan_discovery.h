#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::np {

// Godot- and socket-free projection of the retail fields carried by one LAN
// host's NP/NAPI 0x81 ServerHello.
struct LanDiscoveryServer {
	std::string server_name;
	std::string session_id;
	std::string expansion;
	uint32_t gametype = 0;
	uint32_t server_flags = 0;
	uint32_t current_players = 0;
	uint32_t max_players = 0;

	bool operator==(const LanDiscoveryServer &o) const {
		return server_name == o.server_name && session_id == o.session_id &&
		       expansion == o.expansion && gametype == o.gametype &&
		       server_flags == o.server_flags &&
		       current_players == o.current_players && max_players == o.max_players;
	}
	bool operator!=(const LanDiscoveryServer &o) const { return !(*this == o); }
};

// Build a complete, ready-to-send NWU datagram containing the same NP/NAPI
// 0x41 JointOperations identity as the existing direct-peer join path. The
// caller owns broadcast/socket policy.
std::vector<uint8_t> build_lan_discovery_probe(uint32_t client_index);

// Decode a complete NWU datagram and project a 0x81 ServerHello into the LAN
// browser model. Returns false for malformed packets or any opcode other than
// ServerHello; `out` is changed only on success.
bool parse_lan_discovery_reply(const uint8_t *data, size_t size, LanDiscoveryServer &out);

// The witnessed browse window: the LAN screen's state machine re-enables
// LAN_SEARCH (MP_SEARCH) once GetTickCount() - search_start > 0x7530
// (30000 ms). [orig: UI_ProcessLANSessionStateMachine @0x558de0, the 0x7530
// gate @0x55933a]
inline constexpr double kLanBrowseWindowSeconds = 30.0;
// Retail re-announces while enumerating every 3000 ms — discovery is a
// cadence, not a single burst. The pump's interval select is (+37 ? 3000 :
// enum+20), and +37 IS the enumerator identity: the 0x41 announce builder
// skips the player-count TLV exactly when +37 is set, matching the captured
// client probe shape (which carries none), and the golden LAN capture shows
// the ~3 s re-probe cadence live. [orig: CNapiNPConnection_PumpEnumeratorAndSend
// @0x6290c0 interval select; NapiNPSession_SendAnnouncePacket @0x61fa00 +37
// player-count gate]
inline constexpr double kLanAnnounceIntervalSeconds = 3.0;

// One discovered host: the reply's source endpoint plus its projected hello.
struct LanDiscoveryRow {
	std::string host_ip;
	int port = 0;
	LanDiscoveryServer server;
};

enum class LanRowChange : uint8_t {
	kNone = 0, // filtered, unparsable, or an unchanged re-announce
	kAdded,
	kUpdated,
};

// The LAN browse window over the retail game-server UDP range: one probe
// identity per window (retail keeps its connection identity across the
// enumerator's re-announce pumps, so every burst repeats the same bytes),
// the 30-second window, the 3-second re-announce cadence, the reply filter
// (the browsed port range, a usable source address, a retail-identity 0x81)
// and the endpoint-keyed upsert whose rows are live state (player count,
// mission rotation) refreshed in place. The embedder owns the socket: it
// sends `probe()` to every port in the range on begin and whenever `advance`
// reports an announce due, feeds every received datagram to `accept_reply`,
// and stops when `advance` returns false.
class LanDiscoveryBrowser {
public:
	// Begins a window; false (nothing changes) on an invalid port range.
	bool begin(uint32_t client_index, int port_min, int port_max);
	void stop();
	bool browsing() const { return browsing_; }
	int port_min() const { return port_min_; }
	int port_max() const { return port_max_; }
	const std::vector<uint8_t> &probe() const { return probe_; }

	// One outer frame: false once the window has expired (the browse is
	// stopped); `announce_due` is set when a re-announce burst falls due this
	// frame (the announce clock restarts from zero, not from the overshoot).
	bool advance(double delta_seconds, bool &announce_due);

	// A datagram received from (source_ip, source_port) while browsing.
	LanRowChange accept_reply(const uint8_t *data, size_t size,
	                          const std::string &source_ip, int source_port);

	// The discovered hosts in first-seen order.
	const std::vector<LanDiscoveryRow> &servers() const { return servers_; }

private:
	std::vector<uint8_t> probe_;
	std::vector<LanDiscoveryRow> servers_;
	std::unordered_map<std::string, size_t> index_by_endpoint_;
	int port_min_ = 0;
	int port_max_ = 0;
	double browse_elapsed_s_ = 0.0;
	double announce_elapsed_s_ = 0.0;
	bool browsing_ = false;
};

} // namespace opennova::np
