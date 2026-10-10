#pragma once

#include <net/napi/tlv.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {
namespace db { class Database; }

// One entry of a client-sent var list, in wire order. `fnum` is the retail
// VarFNum: 0 for a plain host/setup variable, the player-slot index for the
// indexed PlayerList entries, so N players keep N distinct
// (fnum, "PlayerName") entries instead of collapsing onto one key.
// [orig: NapiStatement_SerializeVarList @0x4d0660 writes VarFNum/VarName/VarValue
//  per node; Server_PlayerAdd @0x51d441..0x51d4aa writes PlayerName /
//  PlayerIpAndPort / PlayerPCID / PlayerTeam / PlayerType keyed by the slot]
struct VarEntry {
	int fnum = 0;
	std::string name;
	std::string value;
};
using VarList = std::vector<VarEntry>;
// Top-level keys are VarList names (e.g. "HostSetup"); values keep the wire order.
using VarLists = std::map<std::string, VarList>;

// Lookup helpers over a VarList. `var_value` returns the value of the first
// entry matching (fnum, name) or an empty string; `var_has` reports presence.
// Names compare case-insensitively (retail's CNapiVarList_FindByTypeAndName
// walks Napi_StrCaseEqual).
bool var_has(const VarList &list, std::string_view name, int fnum = 0);
std::string var_value(const VarList &list, std::string_view name, int fnum = 0);

// One player slot the host reported in its PlayerList (the five retail
// per-slot vars, grouped by VarFNum).
struct HostRosterSlot {
	int slot = 0;                 // VarFNum
	std::string player_name;      // PlayerName
	std::string ip_and_port;      // PlayerIpAndPort
	std::string pcid;             // PlayerPCID
	std::string team;             // PlayerTeam
	std::string type;             // PlayerType
};

// Per-connection lobby state. Persists across protocol messages on the
// same UDP session (keyed by Connection.id in the standalone server).
struct LobbyState {
	// Set during handle_client_request_verify_result.
	std::string sess_id_string;     // hex token returned in ServerVerifyResult

	// Populated when the client transitions into hosting mode
	// (ClientHostRequest sets these; ClientHostUpdate refreshes them).
	bool hosting = false;
	std::string gsid;               // game-server identifier returned in HostCommands
	uint32_t    rid = 0;            // numeric room/host id returned in ServerHostResult.Rid
	std::string game;               // LobbyName from HostSetup (e.g. "jop_2_consumer")
	std::string host_ip;            // the observed UDP source (retail advertises no address)
	int         host_port = 0;      // Host.Port when > 0, else the observed UDP source port
	std::string host_key;           // Host.HostKey (request or update)
	std::string pcid_key;           // Host.PCIDKey (request or update; rotates every refresh)
	std::string server_name;
	int         player_count = 0;
	int         max_players = 0;
	std::string region;
	std::string app_id;             // from HostSetup.AppId (string form)
	std::string game_type;
	std::string mission_name;
	std::string country;
	std::string password = "N";
	std::string locked = "N";
	std::string dedicated = "Y";
	std::string stat = "N";
	std::string exp;
	std::string exp_bits;
	std::string ver1;
	std::string joicon2;
	// The remaining Host columns the retail browser row carries verbatim
	// [orig: Lobby_UpdateServerInfo @0x4fe8c0 writes TimeLeft @0x4fedc5, Msg
	//  @0x4fef46, Age @0x4ff033, TimeOfDay @0x4ff153, LevelRange @0x4ff251,
	//  BBMode @0x4ff297, PBServer @0x4ff43c, Skins @0x4fed52, Tracers @0x4feec2,
	//  Mod @0x4feee8]. Empty until the host reports them.
	std::string time_left;
	std::string time_of_day;
	std::string msg;
	std::string mod;
	std::string age;
	std::string pb_server;
	std::string level_range;
	std::string bb_mode;
	std::string skins;
	std::string tracers;
	std::string pix;
	// The host's PlayerList, one entry per reported slot (VarFNum-keyed).
	std::vector<HostRosterSlot> roster;

	// The host's Host / HostSetup / PlayerList as its request set them and each
	// ClientHostUpdate's changed vars merged in (by VarFNum and VarName): the
	// current lists, for /api/lobbies introspection.
	VarLists last_host_update;
	VarLists play_state;
};

struct LobbyDispatchResult {
	// Reply containers, each suitable to be wrapped in a ProtocolMessage
	// (full_msg_type=0, msg_type=0, LEN8/LEN16 chosen per payload size)
	// by the caller. Empty when the message generates no reply
	// (e.g. ClientHostUpdate is a one-way broadcast).
	std::vector<NapiMessage> reply_containers;

	// Diagnostic label — the message name we dispatched ("ClientConnected",
	// "ClientHostRequest", or "unknown:<name>"). Logged by the listener.
	std::string label;

	// Set on ClientPlayerEnterRequest: the joiner reports its own dcb
	// (ConnectionId = its NapiNPConnection.unk_18) tagged with its GAME
	// connection endpoint (IpAddress / PortNumber fields). The host correlates
	// these to the in-match peer and stamps `ConnectionId` into that peer's S2C
	// 0x0C organic-spawn `entity_flags` (entity+0x78), which the retail client
	// matches against its own connection+0x18 in Player_FindLocalPlayerEntity
	// @0x4e0090. [orig: CNapiGameSession_SendPlayEnterRequest @0x4d02a0 reads
	// ConnectionId/IpAddress/PortNumber from the connection's +0x18/+0x30/+0x34]
	bool        has_player_enter = false;
	uint32_t    player_connection_id = 0; // the joiner's dcb
	std::string player_ip_field;          // IpAddress field, verbatim (decimal string)
	uint16_t    player_game_port = 0;     // PortNumber field
};

// Utility: peel "ClientVarList" children out of a parsed Container into
// VarLists. Top-level keys are VarList names (e.g. "HostSetup"); each list
// keeps every ClientVar in wire order with its VarFNum, so indexed entries
// (the PlayerList) survive intact.
VarLists extract_var_lists(const NapiMessage &container);

// Group a PlayerList into per-slot roster entries (one HostRosterSlot per
// distinct VarFNum, in first-seen order).
std::vector<HostRosterSlot> roster_from_player_list(const VarList &player_list);

class LobbySession {
public:
	LobbySession();

	// Optional DB handle for host-state persistence (Phase I.2). When
	// non-null, ClientHostRequest / ClientHostUpdate / ClientHostPlayerAdded /
	// ClientHostPlayerRemoved upsert/delete `active_hosts` + `host_players`
	// rows; null leaves the dispatcher purely in-memory (used by tests
	// that don't bring up sqlite). The connection belongs to the thread that
	// calls dispatch() (Database is single-threaded; the NW UDP listener hands
	// over the one its receive thread leases).
	void set_database(opennova::db::Database *db) { db_ = db; }

	// Reflection override for the advertised game-host endpoint (dev/NAT). When
	// set, ClientHostRequest / ClientHostUpdate force host_ip / host_port to
	// these instead of trusting the observed UDP source (the docker gateway
	// behind a bridge), so joiners get a reachable endpoint. `port` is the
	// client's NovaWorld session port (game.cfg mpnovaworldport, 32768). Empty
	// ip / 0 port == unset (prod default — real source observation).
	void set_reflect_endpoint(std::string ip, uint16_t port) {
		reflect_ip_ = std::move(ip);
		reflect_port_ = port;
	}

	// The GLSVSSResults string a ClientGLSVSSRequest is answered with. Empty
	// (the default) answers with a ServerGLSVSSResults that carries no
	// GLSVSSResults param, which the retail consumer treats as a no-op
	// [orig: CNapiGameSession_HandleGLSVSSResults @0x4d3380 acts only when the
	//  param is found].
	void set_glsvss_results(std::string results) { glsvss_results_ = std::move(results); }

	// Dispatch one inbound lobby message. `inner_message` is the outer
	// container's first child (the actual ClientConnected / ClientHostRequest /
	// etc.). `state` is the caller-owned per-connection state.
	LobbyDispatchResult dispatch(const NapiMessage &inner_message,
	                             LobbyState &state,
	                             const std::string &remote_ip,
	                             uint16_t remote_port);

	// The connection's hosting ends: it leaves the browser (hosting cleared, its
	// roster and player count dropped, its active_hosts row removed) and keeps
	// its RID / GSID for a later re-host. The host's own ClientStopHosting runs
	// it, and so does the service's ServerStopHosting, which leaves a stock host
	// verified (state 4) with no statement back [orig:
	// CNapiGameSession_HandleServerMessage @0x4d1c50]. `reason` is the log tag.
	void end_hosting(LobbyState &state, const char *reason);

	// Allow tests to inject deterministic id generation.
	using IdGenerator = std::function<std::string()>;
	void set_sess_id_generator(IdGenerator g) { sess_id_gen_ = std::move(g); }
	void set_gsid_generator(std::function<std::string(const std::string&)> g) {
		gsid_gen_ = std::move(g);
	}
	// The host RID is minted by the service alone (see mint_rid); tests may
	// pin it. The generator takes no input on purpose: nothing the host sends
	// is a uniqueness source (its AppId is a per-session random in
	// [1000, 9999] [orig: CNapiNetwork_RandomizeTimeout @0x4c4d80]).
	void set_rid_generator(std::function<uint32_t()> g) { rid_gen_ = std::move(g); }

private:
	LobbyDispatchResult handle_client_connected(const NapiMessage &msg, LobbyState &state);
	LobbyDispatchResult handle_client_request_verify_result(const NapiMessage &msg, LobbyState &state);
	LobbyDispatchResult handle_client_host_request(const NapiMessage &msg, LobbyState &state,
	                                               const std::string &remote_ip, uint16_t remote_port);
	LobbyDispatchResult handle_client_host_update(const NapiMessage &msg, LobbyState &state,
	                                              const std::string &remote_ip);
	LobbyDispatchResult handle_client_play_request(const NapiMessage &msg, LobbyState &state);
	LobbyDispatchResult handle_client_glsvss_request(const NapiMessage &msg, LobbyState &state);
	uint32_t mint_rid();

	IdGenerator sess_id_gen_;
	std::function<std::string(const std::string&)> gsid_gen_;
	std::function<uint32_t()> rid_gen_;     // null == mint_rid()
	uint32_t next_rid_suffix_ = 1;          // policy: monotonic per process
	std::string glsvss_results_;
	opennova::db::Database *db_ = nullptr;  // optional, null in tests
	std::string reflect_ip_;                // ONNET_CLIENT_REFLECT_IP (empty=unset)
	uint16_t    reflect_port_ = 0;          // client NovaWorld session port (0=unset)
};

} // namespace opennova
