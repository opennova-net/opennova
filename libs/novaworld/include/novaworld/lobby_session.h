#pragma once

#include <napi/tlv.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace opennova {
namespace db { class Database; }

// Per-connection lobby state. Persists across protocol messages on the
// same UDP session (keyed by Connection.id in the standalone server).
//
// Mirrors the Python `session.host_state` and `session.play_state`
// dictionaries in onnet/onnw/novaworldudp.py.
struct LobbyState {
	// Set during handle_client_request_verify_result.
	std::string sess_id_string;     // hex token returned in ServerVerifyResult

	// Populated when the client transitions into hosting mode
	// (ClientHostRequest sets these; ClientHostUpdate refreshes them).
	bool hosting = false;
	std::string gsid;               // game-server identifier returned in HostCommands
	uint32_t    rid = 0;            // numeric room/host id returned in ServerHostResult.Rid
	std::string game;               // LobbyName from HostSetup (e.g. "jop_2_consumer")
	std::string host_ip;            // from HostInfo.ServerIP, falls back to remote addr
	int         host_port = 0;
	std::string host_key;           // from ClientHostUpdate
	std::string pcid_key;           // from ClientHostUpdate
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

	// Last full var-lists snapshot — useful for /api/lobbies introspection.
	std::map<std::string, std::map<std::string, std::string>> last_host_update;
	std::map<std::string, std::map<std::string, std::string>> play_state;
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
// the nested-map form onnet's _extract_var_lists / _client_var_list_to_dict
// produce. Top-level keys are VarList names (e.g. "HostSetup"); inner
// maps are VarName -> VarValue (NUL-trimmed ASCII).
std::map<std::string, std::map<std::string, std::string>>
extract_var_lists(const NapiMessage &container);

class LobbySession {
public:
	LobbySession();

	// Optional DB handle for host-state persistence (Phase I.2). When
	// non-null, ClientHostRequest / ClientHostUpdate / ClientHostPlayerAdded /
	// ClientHostPlayerRemoved upsert/delete `active_hosts` + `host_players`
	// rows; null leaves the dispatcher purely in-memory (used by tests
	// that don't bring up sqlite).
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

	// Dispatch one inbound lobby message. `inner_message` is the outer
	// container's first child (the actual ClientConnected / ClientHostRequest /
	// etc.). `state` is the caller-owned per-connection state.
	LobbyDispatchResult dispatch(const NapiMessage &inner_message,
	                             LobbyState &state,
	                             const std::string &remote_ip,
	                             uint16_t remote_port);

	// Allow tests to inject deterministic id generation.
	using IdGenerator = std::function<std::string()>;
	void set_sess_id_generator(IdGenerator g) { sess_id_gen_ = std::move(g); }
	void set_gsid_generator(std::function<std::string(const std::string&)> g) {
		gsid_gen_ = std::move(g);
	}
	void set_rid_generator(std::function<uint32_t(const std::string&)> g) {
		rid_gen_ = std::move(g);
	}

private:
	LobbyDispatchResult handle_client_connected(const NapiMessage &msg, LobbyState &state);
	LobbyDispatchResult handle_client_request_verify_result(const NapiMessage &msg, LobbyState &state);
	LobbyDispatchResult handle_client_host_request(const NapiMessage &msg, LobbyState &state,
	                                               const std::string &remote_ip, uint16_t remote_port);
	LobbyDispatchResult handle_client_host_update(const NapiMessage &msg, LobbyState &state,
	                                              const std::string &remote_ip);
	LobbyDispatchResult handle_client_play_request(const NapiMessage &msg, LobbyState &state);

	IdGenerator sess_id_gen_;
	std::function<std::string(const std::string&)> gsid_gen_;
	std::function<uint32_t(const std::string&)> rid_gen_;
	opennova::db::Database *db_ = nullptr;  // optional, null in tests
	std::string reflect_ip_;                // ONNET_CLIENT_REFLECT_IP (empty=unset)
	uint16_t    reflect_port_ = 0;          // client NovaWorld session port (0=unset)
};

} // namespace opennova
