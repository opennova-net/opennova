#pragma once

#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig (the proxy-assisted join)
#include <net/npwire/peer_addr.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
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
// The per-pump walk over the listed sessions examines at most 32 of them, and the
// row table it appends to holds 32 rows (0x23D00 bytes of 0x11E8-byte rows).
// [orig: UI_ProcessLANSessionStateMachine @0x558de0, the `session_count >= 32`
// break @0x5593b5, the row stride 0x11E8 @0x5593db; UI_StartLANSessionSearch
// @0x558300 - the table's memset(.., 0x23D00) @0x5583C1]
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

// One row of the LAN list: the session's address and the hello it was discovered
// with, copied from its list node when the row was appended, and the key the row
// table holds, the session record's id (a record replaced or expired leaves its row;
// LanDiscoveryBrowser::advance's walk).
struct LanDiscoveryRow {
	std::string host_ip;
	int port = 0;
	LanDiscoveryServer server;
	uint32_t record_id = 0;
};

// The browse's session-record lines in _connectlog.txt (each written as ConnectLog's
// `"<time> : <text>"` while the log is on): the enumerator's removed and discovered
// callbacks, with the record's address (Napi_FormatAddress's "a.b.c.d:port",
// peer_addr_to_string) and its session name (the record's SN copy, +508).
// [orig: CNapiNetwork_OnSessionRemoved @0x4C68C0 (Napi_FormatAddress @0x4C693D, the
//  append @0x4C6975); CNapiNetwork_OnSessionDiscovered @0x4C8470 (Napi_FormatAddress
//  @0x4C8631, the append @0x4C8663)]
std::string net_session_removed_text(const PeerAddr &at, const std::string &session_name);
std::string net_session_discovered_text(const PeerAddr &at, const std::string &session_name);

// The browse's session-record lifetime: while the enumerator runs, a record no 0x81
// has stamped for longer than this goes [orig: CNapiGameSession_InitTransportConnection
// @0x4C9E10 - enumerator+0x18 = 0x7530 @0x4C9FB4; CNapiNPConnection_PumpStateMachine
// @0x6292E0 - `now - stamp > lifetime` @0x62945E..0x629468].
inline constexpr double kLanSessionRecordLifetimeSeconds = 30.0;

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

// The LAN browse window over the retail game-server UDP range: one connection
// identity per window (retail keeps its connection identity across the
// enumerator's re-announce pumps, so every burst repeats the same bytes), one
// announce per port carrying that port and the enumerate address as its EIP /
// EPN, the 30-second window, the 3-second re-announce cadence, the reply filter
// (the browsed port range, a usable source address, our CI, a retail-identity
// 0x81), the enumerator's session records and the list's row walk. The embedder
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
//  the row found among its entries (NapiNPEnumerator_FindTimerById, the call @0x4CA1EE, its
//  result stored @0x4ca1f8)]
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - the port walk
//  @0x629241..0x629268, each announce to (enumerator+4, port)]
//
// The session records (D-NET-NEW-L8-8): every 0x81 the reply filter takes is the
// enumerator's record for its sender. None yet: a new record (its id the next of the
// enumerator's counter, stamped on the browse clock) and the discovered callback. The
// same HK and SN: stamped again in place, no callback. Anything else: the record goes
// (the removed callback) and a new one comes (the discovered callback). The discovered
// callback lists a session (its list node, a copy of the record as discovered) only
// when its P2 admits the browse's connect type, and only then writes NET SESSION
// DISCOVERED; the removed callback frees the node if there is one and writes NET
// SESSION .. HAS BEEN REMOVED whatever the channel. A record goes the same way when it
// expires while the enumerator runs and at every drain (drain_records, begin).
// [orig: CNapiGameSession_InitTransportConnection @0x4C9E10 - the discovered callback
//  enumerator+0x1C @0x4C9FCB, the removed callback +0x20 @0x4C9FE2; Nwu_HandleServerHello
//  @0x626D20 - NapiNPEnumerator_FindSessionByAddress (the call @0x627667), the HK test
//  @0x627917, String_ExactMatch over the SN @0x627928, the stamp in place @0x627E34;
//  a sender's first record NapiNPSession_Create (the call @0x62767E), its callback
//  @0x6278D7..0x627903; the replace NapiNPTimer_Destroy (the call @0x627941) ->
//  NapiNPSession_Create (the call @0x627947), stamped again @0x627BA6, its callback
//  @0x627BAC..0x627BD4; NapiNPSession_Create @0x624540 - the stamp +0xB0C @0x6245AE,
//  the id counter @0x624704..0x624710;
//  NapiNPTimer_Destroy @0x621E50 - the removed callback @0x621E5D..0x621E6F;
//  CNapiNetwork_OnSessionDiscovered @0x4C8470 - NetPacket_ValidateChannelType
//  @0x4C84A3..0x4C84BA, the node by the record's id @0x4C84CB, alloc @0x4C84D5;
//  CNapiNetwork_OnSessionRemoved @0x4C68C0 - the node @0x4C68F8..0x4C690D]
class LanDiscoveryBrowser {
public:
	// Begins a window under `connect_type` (kLanConnectType*): only a session whose
	// P2 admits it lists (lan_session_admits_connect_type). `enumerate_ip` is the
	// destination every announce goes to, in PeerAddr's packing (0xFFFFFFFF for the
	// LAN broadcast, the resolved address on a direct browse; 0 writes no EIP).
	// `title` is the identity the announces carry and the replies must match (complete,
	// outliving the window). The search drains the enumerator's records first
	// (drain_records) and starts its row table empty. False (nothing changes) on an
	// invalid port range or a zero client index.
	// [orig: UI_StartLANSessionSearch @0x558300 - CNapiNetwork_DrainProtocolTimers
	//  (the call @0x55830F), the row table cleared @0x5583C1 / @0x5583C9;
	//  UI_EnumerateAndJoinSession @0x569F50 - the drain (the call @0x56A08C);
	//  CNapiGameSession_InitTransportConnection @0x4c9e10 - -1 on a LAN browse
	//  @0x4c9eab, the resolved address on a direct one @0x4c9eb5..0x4c9ec3, stored
	//  to enumerator+4 @0x4c9f0b]
	bool begin(const TitleIdentity &title, uint32_t client_index, uint32_t enumerate_ip,
	           int port_min, int port_max, int connect_type);
	// Stops the enumerator; its records and the rows stay (the window's end).
	// [orig: UI_ProcessLANSessionStateMachine @0x558de0 -
	//  CNapiNetwork_DrainPendingDataTransfers (the call @0x559341) stops the enumerator
	//  alone]
	void stop();
	// Every session record goes, the enumerator's list from its head, each through the
	// removed callback (its line); the rows stay. The search, the LAN screen's
	// activation and the enumerate-and-join each drain first.
	// [orig: CNapiNetwork_DrainProtocolTimers @0x4C6DE0 -> NapiNPEnumerator_DrainTimerList
	//  @0x622140 (NapiNPTimer_Destroy over the head, the call @0x62215E); the callers
	//  UI_StartLANSessionSearch @0x55830F, UI_InitLANMultiplayerScreen @0x5569C7,
	//  UI_EnumerateAndJoinSession @0x56A08C]
	void drain_records();
	// The connect log's line writer: the embedder's _connectlog.txt, which writes while
	// the log is on (ConnectLog::line); none writes nothing. Kept across begin/stop.
	void set_connect_log(std::function<void(const std::string &)> line) {
		connect_log_ = std::move(line);
	}
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

	// One outer frame after its replies (accept_reply), in retail's order: the
	// protocol pump's re-announce (`announce_due`; the announce clock restarts from
	// zero, not from the overshoot) and record expiry, then the window's gate, then the
	// list's walk, which runs on the window's last frame too. False once the window has
	// expired (the browse is stopped; never for an unwindowed browse); a re-announce due
	// on that last frame is still reported.
	// [orig: UI_ProcessLANSessionStateMachine @0x558de0 -
	//  CNapiNetwork_PumpTransportAndProtocol (the call @0x559324), the gate @0x55933a,
	//  the walk @0x559384..0x559546; CNapiNetwork_PumpTransportAndProtocol @0x4C6E00 -
	//  the manager's receive @0x4C6E2B ahead of the protocol's pump @0x4C6E3D;
	//  CNapiNPConnection_PumpFlags @0x629780 - the announce @0x6297B7 ahead of the state
	//  machine @0x6297C3]
	bool advance(double delta_seconds, bool &announce_due);

	// A datagram received from `from` while browsing: a reply the filter takes is its
	// sender's session record (the class comment).
	void accept_reply(const uint8_t *data, size_t size, const PeerAddr &from);

	// The LAN list's rows in the order the walk appended them.
	const std::vector<LanDiscoveryRow> &servers() const { return servers_; }
	// The enumerator's session records, listed or not.
	size_t record_count() const { return records_.size(); }

private:
	// One session record: its id, its sender, the HK and SN the next 0x81 is
	// compared with, its last stamp on the browse clock, and whether the discovered
	// callback listed it (its node), with the node's copy of the record as discovered.
	// (The in-place update's refreshed fields have no reader here: the row is the
	// node's copy.)
	struct Record {
		uint32_t id = 0;
		PeerAddr sender{};
		uint32_t host_key = 0;
		std::string name; // the record's SN copy (+0x1FC)
		double stamp_s = 0.0;
		bool listed = false;
		LanDiscoveryServer node;
	};
	void create_record(const PeerAddr &from, LanDiscoveryServer server);
	void destroy_record(size_t index);
	void expire_records();
	void walk_rows();
	void connect_log(const std::string &text) const;

	std::vector<Record> records_;
	std::vector<LanDiscoveryRow> servers_;
	std::function<void(const std::string &)> connect_log_;
	const TitleIdentity *title_ = nullptr;
	ProxyRendezvousConfig proxy_{};
	uint32_t client_index_ = 0;
	uint32_t enumerate_ip_ = 0;
	uint32_t record_ids_ = 0; // the enumerator's id counter (proto+92)
	int connect_type_ = kLanConnectTypeLan;
	int port_min_ = 0;
	int port_max_ = 0;
	double browse_elapsed_s_ = 0.0;
	double announce_elapsed_s_ = 0.0;
	bool browsing_ = false;
	bool windowed_ = true;
};

} // namespace opennova
