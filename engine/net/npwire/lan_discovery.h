#pragma once

#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig (the proxy-assisted join)
#include <net/npwire/peer_addr.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {

struct TitleIdentity; // net/title/title_identity.h

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
	// The session record a join from this row connects with: the 0x81 HK the
	// 0x42 echoes and its SF (a server password is set).
	uint32_t host_key = 0;
	bool password_required = false;

	bool operator==(const LanDiscoveryServer &o) const {
		return server_name == o.server_name && session_id == o.session_id &&
		       expansion == o.expansion && gametype == o.gametype &&
		       server_flags == o.server_flags &&
		       current_players == o.current_players && max_players == o.max_players &&
		       host_key == o.host_key && password_required == o.password_required;
	}
	bool operator!=(const LanDiscoveryServer &o) const { return !(*this == o); }
};

// Build a complete, ready-to-send NWU datagram containing the same NP/NAPI
// 0x41 title identity as the direct-peer join path (`title` complete:
// make_title_client_hello). The caller owns broadcast/socket policy. `client_index` is the CI the replies
// echo; it must be non-zero (the announce builder omits a zero CI entirely).
// `target_ip` / `target_port` are the datagram's own destination, which the
// announce writes as EIP / EPN, each only when nonzero: the address in
// PeerAddr's packing (0xFFFFFFFF for the LAN broadcast) and the port as a plain
// number, so an enumerator sends each port its own datagram.
// [orig: NapiNPSession_SendAnnouncePacket @0x61fa00 - EIP @0x61fcf5..0x61fd19,
//  EPN @0x61fd23..0x61fd47, the SendTo to the same pair @0x61fdd3..0x61fdde;
//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - one announce per port
//  @0x629241..0x629268]
std::vector<uint8_t> build_lan_discovery_probe(const TitleIdentity &title, uint32_t client_index,
                                               uint32_t target_ip, uint32_t target_port);

// Decode a complete NWU datagram and project a 0x81 ServerHello into the LAN
// browser model. `client_index` is the browse window's CI: retail resolves the
// reply's CI against its enumerator and drops a reply for a CI it did not
// issue (an absent CI resolves as 0 and never matches a live enumerator).
// Returns false for malformed packets, any opcode other than ServerHello, a
// foreign CI, or a game identity other than `title`'s; `out` is changed only
// on success.
bool parse_lan_discovery_reply(const TitleIdentity &title, const uint8_t *data, size_t size,
                               uint32_t client_index, LanDiscoveryServer &out);

// The connection index a browse's announces carry (LanDiscoveryBrowser::begin's
// `client_index`). Every browse ours runs creates its client connection twice: the
// screen's own create, then InitTransportConnection's, which destroys the first
// before anything is sent on it and creates again; each create steps the game
// protocol's connection counter, so the browse's CI (and the join's that goes on
// with it, D-NET-521) is the counter's second step. (The LAN screen's search under
// connect type 1 skips the screen's create and creates once, @0x558367 -> @0x5583a9;
// ours never searches under type 1.)
// [orig: UI_EnumerateAndJoinSession @0x569f50 - CNapiGameSession_CreateServerConnection
//  @0x56a011, then CNapiGameSession_InitTransportConnection @0x56a097; the LAN
//  screen's search - the create @0x55837e (connect type 2) / @0x55836e (type 3), then
//  InitTransportConnection @0x5583b0; InitTransportConnection @0x4c9e10 - its create
//  @0x4c9e16; CNapiGameSession_CreateServerConnection - the destroy @0x4c9d7d, the
//  create @0x4c9d9c; CNapiNPConnection_Create @0x62b282 steps the counter]
uint32_t next_browse_connection_index();

// The witnessed browse window: the LAN screen's state machine re-enables
// LAN_SEARCH (MP_SEARCH) once GetTickCount() - search_start > 0x7530
// (30000 ms) — strictly greater, so the window is still open at exactly
// 30000. [orig: UI_ProcessLANSessionStateMachine @0x558de0, the 0x7530
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
// The per-pump walk over the discovered sessions examines at most 32 entries,
// so a browse window never lists more than 32 hosts.
// [orig: UI_ProcessLANSessionStateMachine @0x558de0, the `session_count >= 32`
// break @0x5593b5]
inline constexpr size_t kLanBrowseMaxSessions = 32;

// The network connect type a browse runs under (g_GameConfigState.networkConnectType):
// the NovaWorld menu is 1, the LAN screen 2 [orig: UI_InitLANMultiplayerScreen
// @0x5569e1], the internet (direct-address) type 3.
inline constexpr int kLanConnectTypeNovaWorld = 1;
inline constexpr int kLanConnectTypeLan = 2;
inline constexpr int kLanConnectTypeInternet = 3;

// Whether a discovered session's P2 flag word (CNapiServerConfig_BuildFlags) admits a
// browse of `connect_type`: 0x400 (a NovaWorld-transport host) needs type 1, else 0x100
// (LAN) type 2, else 0x200 (internet) type 3; a word with none of the three admits any
// type. The word is read as the 16-bit value the original takes.
// [orig: NetPacket_ValidateChannelType @0x4c49b0; BuildFlags' transport bits
//  @0x4c4de4 / @0x4c4df1 / @0x4c4dfe]
bool lan_session_admits_connect_type(int connect_type, uint32_t server_flags);

// One datagram of an enumerator burst, in send order: the proxy rendezvous to
// the game node (`rendezvous`, sent to `node`), or the announce to the browse
// target at `port`.
struct LanBurstDatagram {
	bool rendezvous = false;
	PeerAddr node{};
	int port = 0;
	std::vector<uint8_t> bytes;
};

// One discovered host: the reply's source endpoint plus its projected hello.
struct LanDiscoveryRow {
	std::string host_ip;
	int port = 0;
	LanDiscoveryServer server;
};

// The LAN list's row text for a listed session, bytes in the host's code page:
// "<name> - <expansion> (<players>/<max>)" when the session advertises an
// expansion variant, else "<name> (<players>/<max>)". The list walks the
// session record the 0x81 translate fills (SN -> +32, NP -> +580, MP -> +584,
// SUS2 -> +1104); the row's copy holds the expansion's first 31 bytes, the test
// is its first byte, and the row keeps the text's first 255 bytes
// [orig: CNapiNetwork_OnSessionDiscovered @0x4c8470;
//  UI_ProcessLANSessionStateMachine @0x558de0 -- the record copy
//  Napi_CopyString(.., +0x450, 0x20) @0x559448..0x559458, the first-byte test
//  @0x559471, sprintf("%s - %s (%ld/%ld)") @0x559493 / ("%s (%ld/%ld)") @0x5594b9
//  over +0x20 and the counts +0x244 / +0x248, strncpy(row, text, 0xFF)
//  @0x5594cc..0x5594dc, UIList_AddRow @0x559516].
std::string lan_discovery_row_label(const std::string &server_name, const std::string &expansion,
		int32_t players, int32_t max_players);

enum class LanRowChange : uint8_t {
	kNone = 0, // filtered, unparsable, or a repeat sighting of a listed host
	kAdded,
};

// The LAN browse window over the retail game-server UDP range: one connection
// identity per window (retail keeps its connection identity across the
// enumerator's re-announce pumps, so every burst repeats the same bytes), one
// announce per port carrying that port and the enumerate address as its EIP /
// EPN, the 30-second window, the 3-second re-announce cadence, the reply filter
// (the browsed port range, a usable source address, our CI, a retail-identity
// 0x81), the 32-row cap, and first-sighting rows: retail's row table keys on
// the session id and a repeat sighting adds and updates nothing, so a row
// keeps the counts it was first seen with for the whole window. The embedder
// owns the socket: it sends `burst()` on begin and whenever `advance` reports
// an announce due, feeds every received datagram to `accept_reply`, and stops
// when `advance` returns false. That socket is the client's one transport socket
// for its network type, and the join from a discovered row goes on with it and
// with the enumerator's connection (D-NET-NEW-R3-3): retail opens one transport
// socket per mode and never re-opens it between the browse and the join, the join
// takes the ctx's one client connection, and the NovaWorld lobby protocol rides the
// same manager and socket (net/npwire/datagram_demux.h).
// [orig: CNapiNetwork_SetTransportMode @0x4c8750 - the same mode returns at once
//  @0x4c8766, the one open @0x4c8796; CNapiNetwork_StartClientConnection @0x4ca160 -
//  the ctx's one client connection @0x4ca1c2, its enumerator stopped @0x4ca1d8 and
//  the row found among its entries (NapiNPEnumerator_FindTimerById, the call @0x4CA1EE)]
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 — the key hit @0x5593c4
//  falls through to the next node, the miss @0x5593e3 appends @0x5593fa..0x559521;
//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the port walk
//  @0x629241..0x629268, each announce to (enumerator+4, port)]
class LanDiscoveryBrowser {
public:
	// Begins a window under `connect_type` (kLanConnectType*): only a session whose
	// P2 admits it lists (lan_session_admits_connect_type). `enumerate_ip` is the
	// destination every announce goes to, in PeerAddr's packing (0xFFFFFFFF for the
	// LAN broadcast, the resolved address on a direct browse; 0 writes no EIP).
	// `title` is the identity the announces carry and the replies must match (complete,
	// outliving the window). False (nothing changes) on an invalid port range or a zero
	// client index.
	// [orig: CNapiGameSession_InitTransportConnection @0x4c9e10 - -1 on a LAN browse
	//  @0x4c9eab, the resolved address on a direct one @0x4c9eb5..0x4c9ec3, stored
	//  to enumerator+4 @0x4c9f0b]
	bool begin(const TitleIdentity &title, uint32_t client_index, uint32_t enumerate_ip,
	           int port_min, int port_max, int connect_type);
	void stop();
	// Whether the window expires (the LAN screen's 30-second search, the default).
	// The join's enumerate-and-join runs without one: its loop ends only on the
	// first discovered session or the player's abort (D-NET-703). Kept across
	// begin/stop; set before begin.
	// [orig: UI_EnumerateAndJoinSession @0x569f50 - the loop @0x56a0c1..0x56a0f9,
	//  no time limit; UI_ProcessLANSessionStateMachine @0x558de0 - the 0x7530 gate
	//  @0x55933a]
	void set_windowed(bool windowed) { windowed_ = windowed; }
	bool windowed() const { return windowed_; }
	bool browsing() const { return browsing_; }
	int port_min() const { return port_min_; }
	int port_max() const { return port_max_; }
	uint32_t client_index() const { return client_index_; }
	uint32_t enumerate_ip() const { return enumerate_ip_; }
	// The announce to `port`: this window's CI, with the enumerate address and the
	// port as its EIP / EPN (build_lan_discovery_probe).
	std::vector<uint8_t> probe(int port) const;

	// The proxy-assisted NovaWorld join's six fields (the .joi NI/NP/BK next to NK),
	// installed on the enumerator with its target; a disabled config (any field
	// unset, LAN) sends no rendezvous. Kept across begin/stop: the embedder installs
	// the target's fields before the window begins.
	// [orig: CNapiGameSession_InitTransportConnection @0x4c9e10 - the install
	//  @0x4ca040..0x4ca0c7]
	void set_proxy(const ProxyRendezvousConfig &proxy) { proxy_ = proxy; }
	const ProxyRendezvousConfig &proxy() const { return proxy_; }
	// One burst of the enumerator pump, in send order: with all six proxy fields
	// set, the 48-byte rendezvous to the game node first, then one announce per
	// port of the range. The embedder sends a burst on begin and whenever
	// `advance` reports an announce due; a join from a row stops the enumerator,
	// so no rendezvous follows it (D-NET-712).
	// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the enumerator
	//  gate @0x6290e3 (enumerator+44), the six-field gate @0x629122..0x62914f,
	//  CNapiNPConnection_SendPingPacket @0x61F8C0 called @0x629159 ahead of the
	//  announce walk @0x6291c6..0x629268; CNapiNetwork_StartClientConnection
	//  @0x4ca160 - NapiTimer_Stop(enumerator) @0x4ca1d8 clears enumerator+44]
	std::vector<LanBurstDatagram> burst() const;

	// One outer frame: false once the window has expired (the browse is
	// stopped; never for an unwindowed browse); `announce_due` is set when a
	// re-announce burst falls due this frame (the announce clock restarts from
	// zero, not from the overshoot).
	bool advance(double delta_seconds, bool &announce_due);

	// A datagram received from (source_ip, source_port) while browsing.
	LanRowChange accept_reply(const uint8_t *data, size_t size,
	                          const std::string &source_ip, int source_port);

	// The discovered hosts in first-seen order.
	const std::vector<LanDiscoveryRow> &servers() const { return servers_; }

private:
	std::vector<LanDiscoveryRow> servers_;
	std::unordered_map<std::string, size_t> index_by_endpoint_;
	const TitleIdentity *title_ = nullptr;
	ProxyRendezvousConfig proxy_{};
	uint32_t client_index_ = 0;
	uint32_t enumerate_ip_ = 0;
	int connect_type_ = kLanConnectTypeLan;
	int port_min_ = 0;
	int port_max_ = 0;
	double browse_elapsed_s_ = 0.0;
	double announce_elapsed_s_ = 0.0;
	bool browsing_ = false;
	bool windowed_ = true;
};

} // namespace opennova
