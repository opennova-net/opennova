#include <net/novaworld/lobby_session.h>

#include <net/novaworld/host_repository.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <utility>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

namespace opennova {

namespace {

std::string field_to_string(const NapiField *f) {
	return f ? opennova::field_to_string(*f) : std::string();
}

const NapiField *find_field(const NapiMessage &msg, const std::string &name) {
	for (const auto &f : msg.fields) {
		if (f.name == name) return &f;
	}
	return nullptr;
}

// Add a `ClientVar`-style child (VarName + VarValue + VarFNum) to a parent
// container (a "ServerVarList" emitted as part of HostCommands etc.).
void append_server_var(NapiMessage &parent,
                       const std::string &name,
                       const std::string &value,
                       const std::string &fnum = "0") {
	NapiMessage var;
	var.name = "ServerVar";
	var.fields.push_back({"VarName",  std::vector<uint8_t>(name.begin(), name.end())});
	var.fields.push_back({"VarValue", std::vector<uint8_t>(value.begin(), value.end())});
	var.fields.push_back({"VarFNum",  std::vector<uint8_t>(fnum.begin(), fnum.end())});
	parent.children.push_back(std::move(var));
}

void set_field(NapiMessage &msg, const std::string &name, const std::string &value) {
	msg.fields.push_back({name, std::vector<uint8_t>(value.begin(), value.end())});
}

// Look for an "AppId" ClientVar nested anywhere under the container, in
// case the field is buried inside a nested ClientVarList we didn't peel out.
std::string find_app_id_recursive(const NapiMessage &msg) {
	if (msg.name == "ClientVar") {
		std::string vname, vvalue;
		for (const auto &f : msg.fields) {
			if (f.name == "VarName")  vname  = field_to_string(&f);
			else if (f.name == "VarValue") vvalue = field_to_string(&f);
		}
		if (strutil::iequals(vname, "AppId") && !vvalue.empty()) return vvalue;
	}
	for (const auto &child : msg.children) {
		auto found = find_app_id_recursive(child);
		if (!found.empty()) return found;
	}
	return {};
}

std::string default_sess_id() {
	// 32-char hex token (policy: the service mints the SessIdString), from the
	// OS CSPRNG (base/os_random).
	const uint64_t a = os_random_u64();
	const uint64_t b = os_random_u64();
	std::ostringstream os;
	os << std::hex << std::setfill('0') << std::setw(16) << a << std::setw(16) << b;
	return os.str();
}

std::string default_gsid(const std::string &app_id) {
	using clock = std::chrono::system_clock;
	const auto now = clock::now();
	const auto t = clock::to_time_t(now);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count() % 1000000;

	// A non-numeric AppId keeps the GSID's app field 0.
	const uint32_t app_int = static_cast<uint32_t>(strutil::parse_ulong(app_id).value_or(0));

	const uint64_t r = os_random_u64(); // the OS CSPRNG (base/os_random)

	char buf[80];
	std::snprintf(buf, sizeof(buf),
	              "GSID-10-%08x-%04d%02d%02d%02d%02d%02d%03lld-%016llx",
	              app_int,
	              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	              tm.tm_hour, tm.tm_min, tm.tm_sec,
	              static_cast<long long>(micros / 1000),
	              static_cast<unsigned long long>(r));
	return std::string(buf);
}

// The decimal before any '/' ("3/16" is 3); 0 when it is no number or out of
// int's range.
int parse_int_safe(const std::string &s) {
	const auto slash = s.find('/');
	return strutil::parse_int(slash == std::string::npos ? s : s.substr(0, slash)).value_or(0);
}

std::string first_value(const VarList &a, const VarList &b,
                        std::initializer_list<const char *> keys) {
	for (const char *key : keys) {
		if (auto v = var_value(a, key); !v.empty()) return v;
		if (auto v = var_value(b, key); !v.empty()) return v;
	}
	return {};
}

std::string yn_value(const std::string &v, const char *fallback) {
	if (v.empty()) return fallback;
	if (v == "1" || v == "Y" || v == "y" || v == "true" || v == "TRUE") return "Y";
	if (v == "0" || v == "N" || v == "n" || v == "false" || v == "FALSE") return "N";
	return v;
}

// Fold the host-reported Host/HostSetup vars into the browser-row fields.
// Column names follow the retail Host list [orig: Lobby_UpdateServerInfo
// @0x4fe8c0]; the alternate spellings keep earlier OpenNova hosts readable.
void refresh_gsb_fields(LobbyState &state,
                        const VarList &host_setup,
                        const VarList &host_info) {
	if (auto v = first_value(host_info, host_setup, {"GameType", "GameTypeName", "GameMode"}); !v.empty()) state.game_type = v;
	if (auto v = first_value(host_info, host_setup, {"MissionName", "Mission", "MapName"}); !v.empty()) state.mission_name = v;
	if (auto v = first_value(host_info, host_setup, {"Country", "CountryCode"}); !v.empty()) state.country = v;
	if (auto v = first_value(host_info, host_setup, {"Password", "Passworded", "RequiresPassword"}); !v.empty()) state.password = yn_value(v, "N");
	if (auto v = first_value(host_info, host_setup, {"Locked", "Private"}); !v.empty()) state.locked = yn_value(v, "N");
	if (auto v = first_value(host_info, host_setup, {"Dedicated"}); !v.empty()) state.dedicated = yn_value(v, "Y");
	if (auto v = first_value(host_info, host_setup, {"Stat", "Stats", "StatsEnabled"}); !v.empty()) state.stat = yn_value(v, "N");
	if (auto v = first_value(host_info, host_setup, {"Exp", "EXP", "Expansion"}); !v.empty()) state.exp = v;
	if (auto v = first_value(host_info, host_setup, {"Expbits", "ExpBits", "EXPBITS"}); !v.empty()) state.exp_bits = v;
	if (auto v = first_value(host_info, host_setup, {"VER1", "Ver1", "Version1"}); !v.empty()) state.ver1 = v;
	if (auto v = first_value(host_info, host_setup, {"Joicon2", "JOICON2"}); !v.empty()) state.joicon2 = v;
	if (auto v = first_value(host_info, host_setup, {"TimeLeft"}); !v.empty()) state.time_left = v;
	if (auto v = first_value(host_info, host_setup, {"TimeOfDay"}); !v.empty()) state.time_of_day = v;
	if (auto v = first_value(host_info, host_setup, {"Msg"}); !v.empty()) state.msg = v;
	if (auto v = first_value(host_info, host_setup, {"Mod"}); !v.empty()) state.mod = v;
	if (auto v = first_value(host_info, host_setup, {"Age"}); !v.empty()) state.age = v;
	if (auto v = first_value(host_info, host_setup, {"PBServer", "PBSERVER"}); !v.empty()) state.pb_server = v;
	if (auto v = first_value(host_info, host_setup, {"LevelRange"}); !v.empty()) state.level_range = v;
	if (auto v = first_value(host_info, host_setup, {"BBMode"}); !v.empty()) state.bb_mode = v;
	if (auto v = first_value(host_info, host_setup, {"Skins"}); !v.empty()) state.skins = yn_value(v, "N");
	if (auto v = first_value(host_info, host_setup, {"Tracers"}); !v.empty()) state.tracers = yn_value(v, "Y");
	if (auto v = first_value(host_info, host_setup, {"PIX"}); !v.empty()) state.pix = v;
}

// The host's own keys ride the Host list on both ClientHostRequest and
// ClientHostUpdate (the same list object is serialized by both
// [orig: CNapiGameSession_SendHostRequest @0x4d37e0 / SendHostUpdate
// @0x4d3860]); PCIDKey rotates every refresh, so the latest value wins.
void refresh_host_keys(LobbyState &state, const VarList &host_info) {
	if (var_has(host_info, "HostKey")) state.host_key = var_value(host_info, "HostKey");
	if (var_has(host_info, "PCIDKey")) state.pcid_key = var_value(host_info, "PCIDKey");
}

// The joinable game endpoint. No retail list carries the host's own reachable
// address: the Host list's Port is the literal "-1" [orig: Lobby_UpdateServerInfo
// @0x4fef6d], a dedicated host publishes no slot 0, and a listen host's slot-0
// PlayerIpAndPort is its loopback connection's unset address [orig:
// Server_PlayerAdd @0x51d45c formats the slot's connection ip:port] (D-NET-346).
// What a retail host does give the service is the source of its NWU datagrams,
// which is its game socket: the lobby session rides the game's NP manager and
// its one socket [orig: CNapiGameSession_InitNPConnection @0x4d3be0
// @0x4d3c6b..0x4d3c89; CNapiNetwork_OpenTransportSocket @0x4c6a40, the
// NovaWorld quad @0x4c6af6..0x4c6b08]. Service policy for the stored endpoint:
// a positive Port (an OpenNova-only override, a host behind a port map), else
// the observed UDP source of the registration.
void refresh_host_endpoint(LobbyState &state, const VarList &host_info,
                           const std::string &remote_ip, uint16_t remote_port) {
	if (state.host_ip.empty()) state.host_ip = remote_ip;
	if (const int port = parse_int_safe(var_value(host_info, "Port")); port > 0) {
		state.host_port = port;
	} else if (state.host_port == 0) {
		state.host_port = remote_port;
	}
}

// One ClientHostPlayerAdded / ClientHostPlayerRemoved statement's params:
// PlayerNumber is the slot the five vars are keyed by in the PlayerList
// [orig: CNapiGameSession_SendPlayerAdded @0x4cfec0 — PlayerNumber, PlayerName,
//  PlayerIpAndPort, PlayerPCID, PlayerTeam, PlayerType; SendPlayerRemoved
//  @0x4d01a0 — PlayerNumber only].
HostRosterSlot roster_slot_from_statement(const NapiMessage &msg) {
	HostRosterSlot s;
	for (const auto &f : msg.fields) {
		const auto value = field_to_string(&f);
		if      (strutil::iequals(f.name, "PlayerNumber"))    s.slot = parse_int_safe(value);
		else if (strutil::iequals(f.name, "PlayerName"))      s.player_name = value;
		else if (strutil::iequals(f.name, "PlayerIpAndPort")) s.ip_and_port = value;
		else if (strutil::iequals(f.name, "PlayerPCID"))      s.pcid = value;
		else if (strutil::iequals(f.name, "PlayerTeam"))      s.team = value;
		else if (strutil::iequals(f.name, "PlayerType"))      s.type = value;
	}
	return s;
}

enum class HostRowWrite { Upsert, Update };

// The host row and its roster in one transaction, so a reader's snapshot
// (db::ReadSnapshot, as /api/hosts and the GSB feed take) never holds the row
// beside a roster its player count disagrees with, or a roster caught between
// replace_roster's delete and its last insert. A failure is logged and rolls
// back both; the lobby keeps serving from its in-memory state.
void persist_host(opennova::db::Database &db, HostRowWrite write,
                  const hostdb::HostRow &row, const LobbyState &state, const char *what) {
	try {
		opennova::db::Transaction tx(db);
		if (write == HostRowWrite::Upsert) hostdb::upsert_host(db, row);
		else                               hostdb::update_host(db, row);
		if (state.rid != 0) hostdb::replace_roster(db, state.rid, state.roster);
		tx.commit();
	} catch (const std::exception &e) {
		opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[lobby] WARN %s: %s", what, e.what());
	}
}

} // namespace

bool var_has(const VarList &list, std::string_view name, int fnum) {
	for (const auto &e : list) {
		if (e.fnum == fnum && strutil::iequals(e.name, name)) return true;
	}
	return false;
}

std::string var_value(const VarList &list, std::string_view name, int fnum) {
	for (const auto &e : list) {
		if (e.fnum == fnum && strutil::iequals(e.name, name)) return e.value;
	}
	return {};
}

VarLists extract_var_lists(const NapiMessage &container) {
	VarLists out;
	for (const auto &child : container.children) {
		if (child.name != "ClientVarList") continue;
		const auto list_name = field_to_string(find_field(child, "VarList"));
		if (list_name.empty()) continue;
		auto &entries = out[list_name];
		for (const auto &entry : child.children) {
			if (entry.name != "ClientVar") continue;
			VarEntry var;
			var.fnum  = parse_int_safe(field_to_string(find_field(entry, "VarFNum")));
			var.name  = field_to_string(find_field(entry, "VarName"));
			var.value = field_to_string(find_field(entry, "VarValue"));
			if (!var.name.empty()) entries.push_back(std::move(var));
		}
	}
	return out;
}

// A PlayerList's vars onto `roster`, slot by VarFNum: a ClientHostUpdate's list
// carries only the vars whose values changed (below), so a slot it leaves out
// keeps its row; a slot leaves with ClientHostPlayerRemoved.
void apply_player_list(std::vector<HostRosterSlot> &roster, const VarList &player_list) {
	auto slot_for = [&roster](int fnum) -> HostRosterSlot & {
		for (auto &s : roster) if (s.slot == fnum) return s;
		roster.push_back({});
		roster.back().slot = fnum;
		return roster.back();
	};
	for (const auto &e : player_list) {
		if (strutil::iequals(e.name, "PlayerName"))           slot_for(e.fnum).player_name = e.value;
		else if (strutil::iequals(e.name, "PlayerIpAndPort")) slot_for(e.fnum).ip_and_port = e.value;
		else if (strutil::iequals(e.name, "PlayerPCID"))      slot_for(e.fnum).pcid = e.value;
		else if (strutil::iequals(e.name, "PlayerTeam"))      slot_for(e.fnum).team = e.value;
		else if (strutil::iequals(e.name, "PlayerType"))      slot_for(e.fnum).type = e.value;
	}
}

std::vector<HostRosterSlot> roster_from_player_list(const VarList &player_list) {
	std::vector<HostRosterSlot> roster;
	apply_player_list(roster, player_list);
	return roster;
}

// A ClientHostUpdate's list into the kept one by (VarFNum, VarName): a retail
// host's update carries only the vars whose values changed since the last one
// (its whole list only right after the host result), so the service keeps the
// union [orig: CNapiVarEntry_SetValue @0x630590 dirties a var only on a changed
// value; NapiStatement_SerializeVarList @0x4d0660 writes `includeAll || dirty`;
// CNapiGameSession_SendHostUpdate @0x4d3860].
void merge_var_list(VarList &kept, const VarList &update) {
	for (const VarEntry &e : update) {
		auto it = std::find_if(kept.begin(), kept.end(), [&e](const VarEntry &k) {
			return k.fnum == e.fnum && strutil::iequals(k.name, e.name);
		});
		if (it != kept.end()) it->value = e.value;
		else kept.push_back(e);
	}
}

LobbySession::LobbySession()
	: sess_id_gen_(default_sess_id),
	  gsid_gen_(default_gsid) {}

// policy: the host id is minted here and only here. It is the u32 the
// browser row carries and the `rid=` the join URL echoes, so two live hosts
// must never share one. The host's AppId cannot serve: retail draws it per
// session from (GetTickCount + rand) % 9000 + 1000 [orig:
// CNapiNetwork_RandomizeTimeout @0x4c4d9a, sole caller
// CNapiGameSession_BuildHostVarLists @0x4d0c6a], so two retail hosts can
// present the same value. The table is cleared at boot, so a per-process
// monotonic suffix under the 0x0A prefix is unique for the run.
uint32_t LobbySession::mint_rid() {
	uint32_t suffix = next_rid_suffix_++ & 0x00FFFFFFu;
	if (suffix == 0) suffix = next_rid_suffix_++ & 0x00FFFFFFu;
	return 0x0A000000u | suffix;
}

void LobbySession::end_hosting(LobbyState &state, const char *reason) {
	const uint32_t rid = state.rid;
	state.hosting = false;
	state.player_count = 0;
	state.roster.clear();
	if (db_ && rid != 0) {
		try { hostdb::remove_host_by_rid(*db_, rid); }
		catch (const std::exception &e) {
			opennova::io::logf(opennova::io::LogLevel::kWarn,
	"[lobby] WARN stop-host remove: %s", e.what());
		}
	}
	opennova::io::logf(opennova::io::LogLevel::kInfo,
	"[lobby] stopped rid=%u reason=%s", rid, reason);
}

LobbyDispatchResult LobbySession::dispatch(const NapiMessage &inner_message,
                                           LobbyState &state,
                                           const std::string &remote_ip,
                                           uint16_t remote_port) {
	const auto &name = inner_message.name;
	if      (name == "ClientConnected")             return handle_client_connected(inner_message, state);
	else if (name == "ClientRequestVerifyResult")   return handle_client_request_verify_result(inner_message, state);
	else if (name == "ClientHostRequest")           return handle_client_host_request(inner_message, state, remote_ip, remote_port);
	else if (name == "ClientHostUpdate")            return handle_client_host_update(inner_message, state, remote_ip);
	else if (name == "ClientPlayRequest")           return handle_client_play_request(inner_message, state);
	else if (name == "ClientGLSVSSRequest")         return handle_client_glsvss_request(inner_message, state);
	else if (name == "ClientPlayerEnterRequest") {
		NapiMessage reply;
		reply.name = "ServerPlayerEnterResult";
		std::string connection_id, ip_field, port_field;
		for (const auto &f : inner_message.fields) {
			const auto value = field_to_string(&f);
			if (f.name == "ConnectionId") {
				connection_id = value;
				set_field(reply, "ConnectionID", value);
			} else {
				if (f.name == "IpAddress")  ip_field = value;
				if (f.name == "PortNumber") port_field = value;
				set_field(reply, f.name, value);
			}
		}
		if (connection_id.empty()) set_field(reply, "ConnectionID", "0");
		set_field(reply, "Success", "1");
		set_field(reply, "MsgCode", "0");
		// Surface the joiner's reported dcb + game endpoint so the host can stamp
		// it into the in-match 0x0C entity_flags (see LobbyDispatchResult docs).
		LobbyDispatchResult out{{std::move(reply)}, "ClientPlayerEnterRequest"};
		out.has_player_enter = true;
		out.player_connection_id = static_cast<uint32_t>(std::strtoul(
				connection_id.empty() ? "0" : connection_id.c_str(), nullptr, 10));
		out.player_ip_field = ip_field;
		out.player_game_port = static_cast<uint16_t>(std::strtoul(
				port_field.empty() ? "0" : port_field.c_str(), nullptr, 10));
		return out;
	}
	// A retail host sends ClientHostPlayerAdded after every player join and
	// ClientHostPlayerRemoved on every disconnect, in session state 6
	// [orig: CNapiGameSession_SendPlayerAdded @0x4cfec0 (PlayerNumber,
	//  PlayerName, PlayerIpAndPort, PlayerPCID, PlayerTeam, PlayerType) /
	//  SendPlayerRemoved @0x4d01a0 (PlayerNumber only)]; neither expects a
	// reply. The statement is the per-slot roster delta between two
	// ClientHostUpdates: it lands in the roster and the Players column follows
	// the roster size, so /api/hosts and the GSB reflect the join before the
	// next refresh. No slot names the host's game endpoint (refresh_host_endpoint).
	else if (name == "ClientHostPlayerAdded" || name == "ClientHostPlayerRemoved") {
		const HostRosterSlot slot = roster_slot_from_statement(inner_message);
		auto it = std::find_if(state.roster.begin(), state.roster.end(),
				[&slot](const HostRosterSlot &s) { return s.slot == slot.slot; });
		if (name == "ClientHostPlayerAdded") {
			if (it != state.roster.end()) *it = slot;
			else state.roster.push_back(slot);
		} else {
			if (it != state.roster.end()) state.roster.erase(it);
			// The slot's vars leave the kept PlayerList with it: the host's
			// later updates carry only changed vars, never the removal.
			auto pl = state.last_host_update.find("PlayerList");
			if (pl != state.last_host_update.end()) {
				VarList &vars = pl->second;
				vars.erase(std::remove_if(vars.begin(), vars.end(),
						[&slot](const VarEntry &e) { return e.fnum == slot.slot; }), vars.end());
			}
		}
		state.player_count = static_cast<int>(state.roster.size());
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] %s rid=%u slot=%d players=%d/%d",
		            name == "ClientHostPlayerAdded" ? "player_added" : "player_removed",
		            state.rid, slot.slot, state.player_count, state.max_players);
		if (db_ && state.rid != 0) {
			persist_host(*db_, HostRowWrite::Update,
			             hostdb::row_from_lobby(state, remote_ip, /*peer_port=*/0), state,
			             "active_hosts roster update");
		}
		return {{}, name};
	}
	else if (name == "ClientStopHosting") {
		end_hosting(state, "ClientStopHosting");
		return {{}, "ClientStopHosting"};
	}
	else if (name == "ClientStopPlaying") {
		state.play_state.clear();
		return {{}, "ClientStopPlaying"};
	}
	// An empty statement with no params and no reply path in the client:
	// the builder [orig: CNapiGameSession_SendUpdateVars @0x4d05e0] has no
	// caller in the retail binary, so the statement is accepted and logged
	// rather than surfaced as unknown.
	else if (name == "ClientUpdateVars") {
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] update_vars rid=%u (no-op)", state.rid);
		return {{}, "ClientUpdateVars"};
	}
	return LobbyDispatchResult{{}, std::string("unknown:") + name};
}

// ClientConnected -> the empty ServerStartVerify challenge.
LobbyDispatchResult LobbySession::handle_client_connected(const NapiMessage &, LobbyState &) {
	NapiMessage reply;
	reply.name = "ServerStartVerify";
	return {{std::move(reply)}, "ClientConnected"};
}

// ClientRequestVerifyResult -> Success=1, SessIdString token, embedded
// ServerVarList(VarList="ConnectCommands").
LobbyDispatchResult LobbySession::handle_client_request_verify_result(
		const NapiMessage &, LobbyState &state) {
	if (state.sess_id_string.empty()) {
		state.sess_id_string = sess_id_gen_();
	}

	NapiMessage reply;
	reply.name = "ServerVerifyResult";
	set_field(reply, "Success", "1");
	set_field(reply, "SessIdString", state.sess_id_string);

	NapiMessage var_list;
	var_list.name = "ServerVarList";
	set_field(var_list, "VarList", "ConnectCommands");
	reply.children.push_back(std::move(var_list));

	return {{std::move(reply)}, "ClientRequestVerifyResult"};
}

// ClientHostRequest carries four ClientVarLists — Cookie (the login cookie
// jar plus CountryName/Language/TimeZoneBias), HostSetup, Host and
// PlayerList [orig: CNapiGameSession_SendHostRequest @0x4d37b4..0x4d37f6] —
// plus CurrentlyHosting (0 on a fresh host, 1 on a re-host) and VarCheck.
// Neither Cookie nor CurrentlyHosting gates admission here. The reply is
// ServerHostResult with Rid and the HostCommands ServerVarList (GSID,
// HostRequiresJoinTicket), which the host stores
// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d59d0].
LobbyDispatchResult LobbySession::handle_client_host_request(
		const NapiMessage &msg, LobbyState &state,
		const std::string &remote_ip, uint16_t remote_port) {
	auto var_lists = extract_var_lists(msg);
	const VarList &host_setup = var_lists["HostSetup"];
	const VarList &host_info  = var_lists["Host"];
	if (host_setup.empty() && host_info.empty()) {
		return {{}, "ClientHostRequest:missing-var-lists"};
	}

	std::string app_id = var_value(host_setup, "AppId");
	if (app_id.empty()) app_id = var_value(host_info, "AppId");
	if (app_id.empty()) app_id = find_app_id_recursive(msg);

	std::string lobby_name = var_value(host_setup, "LobbyName");
	if (lobby_name.empty()) lobby_name = var_value(host_info, "LobbyName");

	if (state.gsid.empty()) state.gsid = gsid_gen_(app_id);
	if (state.rid == 0)     state.rid  = rid_gen_ ? rid_gen_() : mint_rid();
	state.game = lobby_name;
	state.app_id = app_id;
	state.hosting = true;

	state.roster = roster_from_player_list(var_lists["PlayerList"]);
	// The kept lists the updates merge into (the Cookie, the login's, is not kept).
	state.last_host_update.clear();
	for (const char *list : {"Host", "HostSetup", "PlayerList"}) {
		if (var_lists.count(list)) state.last_host_update[list] = var_lists[list];
	}
	refresh_host_endpoint(state, host_info, remote_ip, remote_port);
	// Reflection override (dev/NAT): force a locally reachable host endpoint so
	// joiners can connect, instead of the observed docker-gateway source.
	if (!reflect_ip_.empty()) state.host_ip   = reflect_ip_;
	if (reflect_port_ != 0)   state.host_port = reflect_port_;

	if (var_has(host_info, "ServerName")) state.server_name = var_value(host_info, "ServerName");
	else if (var_has(host_setup, "ServerName")) state.server_name = var_value(host_setup, "ServerName");
	if (var_has(host_info, "Players"))    state.player_count = parse_int_safe(var_value(host_info, "Players"));
	if (var_has(host_setup, "MaxPlayers")) state.max_players = parse_int_safe(var_value(host_setup, "MaxPlayers"));
	else if (var_has(host_info, "MaxPlayers")) state.max_players = parse_int_safe(var_value(host_info, "MaxPlayers"));
	if (var_has(host_info, "Region"))       state.region = var_value(host_info, "Region");
	else if (var_has(host_info, "Country")) state.region = var_value(host_info, "Country");
	else if (state.region.empty())          state.region = "us";
	refresh_host_keys(state, host_info);
	refresh_gsb_fields(state, host_setup, host_info);
	if (state.player_count == 0 && !state.roster.empty()) {
		state.player_count = static_cast<int>(state.roster.size());
	}

	{
		std::string host_keys;
		for (const auto &e : host_info) { host_keys += e.name; host_keys += ' '; }
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host-addr request observed=%s:%u Port=%s stored=%s:%d Host{ %s}",
		            remote_ip.c_str(), static_cast<unsigned>(remote_port),
		            var_has(host_info, "Port") ? var_value(host_info, "Port").c_str() : "(absent)",
		            state.host_ip.c_str(), state.host_port, host_keys.c_str());
	}

	NapiMessage reply;
	reply.name = "ServerHostResult";
	set_field(reply, "Success",   "1");
	set_field(reply, "MsgCode",   "0");
	set_field(reply, "MsgParam1", "0");
	set_field(reply, "MsgParam2", "17");
	set_field(reply, "Rid",       std::to_string(state.rid));

	NapiMessage host_commands;
	host_commands.name = "ServerVarList";
	set_field(host_commands, "VarList", "HostCommands");
	append_server_var(host_commands, "HostRequiresJoinTicket", "0");
	append_server_var(host_commands, "GSID", state.gsid);
	reply.children.push_back(std::move(host_commands));

	opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] started rid=%u gsid=%s server_name='%s' host=%s:%d game=%s app_id=%s pcid_key=%zuB roster=%zu",
	            state.rid, state.gsid.c_str(), state.server_name.c_str(),
	            state.host_ip.c_str(), state.host_port, state.game.c_str(),
	            state.app_id.c_str(), state.pcid_key.size(), state.roster.size());

	// Persist so /jop_2.gsb + /api/hosts query SQL rather than the in-memory
	// snapshot. db_ is null in tests.
	if (db_) {
		persist_host(*db_, HostRowWrite::Upsert,
		             hostdb::row_from_lobby(state, remote_ip, remote_port), state,
		             "active_hosts upsert");
	}

	return {{std::move(reply)}, "ClientHostRequest"};
}

// ClientHostUpdate is silent: the changed vars merge into the kept Host /
// HostSetup lists and the roster (merge_var_list says why), and every column
// is re-read from the merged lists.
LobbyDispatchResult LobbySession::handle_client_host_update(
		const NapiMessage &msg, LobbyState &state, const std::string &remote_ip) {
	VarLists update = extract_var_lists(msg);
	for (const char *list : {"Host", "HostSetup", "PlayerList"}) {
		if (update.count(list)) merge_var_list(state.last_host_update[list], update[list]);
	}
	const VarList &host_vars  = state.last_host_update["Host"];
	const VarList &host_setup = state.last_host_update["HostSetup"];
	refresh_host_keys(state, host_vars);
	{
		std::string update_keys;
		for (const auto &e : host_vars) { update_keys += e.name; update_keys += ' '; }
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host update vars Host{ %s} host_key=%zuB pcid_key=%zuB",
		            update_keys.c_str(), state.host_key.size(), state.pcid_key.size());
	}
	if (var_has(host_vars, "ServerName"))      state.server_name = var_value(host_vars, "ServerName");
	if (var_has(host_vars, "Players"))         state.player_count = parse_int_safe(var_value(host_vars, "Players"));
	if (var_has(host_vars, "MaxPlayers"))      state.max_players  = parse_int_safe(var_value(host_vars, "MaxPlayers"));
	else if (var_has(host_setup, "MaxPlayers")) state.max_players = parse_int_safe(var_value(host_setup, "MaxPlayers"));
	if (var_has(host_vars, "Region"))          state.region       = var_value(host_vars, "Region");
	else if (var_has(host_vars, "Country"))    state.region       = var_value(host_vars, "Country");
	refresh_gsb_fields(state, host_setup, host_vars);
	if (update.count("PlayerList")) apply_player_list(state.roster, update["PlayerList"]);

	refresh_host_endpoint(state, host_vars, remote_ip, /*remote_port=*/0);
	// Reflection override (dev/NAT) — same as the host-request path.
	if (!reflect_ip_.empty()) state.host_ip   = reflect_ip_;
	if (reflect_port_ != 0)   state.host_port = reflect_port_;

	opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] update rid=%u players=%d/%d server_name='%s' stored=%s:%d roster=%zu",
	            state.rid, state.player_count, state.max_players,
	            state.server_name.c_str(), state.host_ip.c_str(), state.host_port,
	            state.roster.size());

	// Keep the DB row in sync. No peer_port in the update handler signature;
	// pass 0 — the row already exists from the initial upsert with the right
	// peer addr, and update_host() doesn't rely on peer_port for the WHERE.
	if (db_) {
		persist_host(*db_, HostRowWrite::Update,
		             hostdb::row_from_lobby(state, remote_ip, /*peer_port=*/0), state,
		             "active_hosts update");
	}

	return {{}, "ClientHostUpdate"};
}

// ClientPlayRequest -> ServerPlayResult with an empty PlayCommands list.
LobbyDispatchResult LobbySession::handle_client_play_request(
		const NapiMessage &msg, LobbyState &state) {
	state.play_state = extract_var_lists(msg);

	NapiMessage reply;
	reply.name = "ServerPlayResult";
	set_field(reply, "Success",   "1");
	set_field(reply, "MsgCode",   "0");
	set_field(reply, "MsgParam1", "0");
	set_field(reply, "MsgParam2", "34");

	NapiMessage play_commands;
	play_commands.name = "ServerVarList";
	set_field(play_commands, "VarList", "PlayCommands");
	reply.children.push_back(std::move(play_commands));

	return {{std::move(reply)}, "ClientPlayRequest"};
}

// ClientGLSVSSRequest {GLSVSSRequest, Cookie list} [orig:
// CNapiGameSession_SendGLSVSSRequest @0x4d3a70/@0x4d3a98] -> ServerGLSVSSResults.
// The consumer walks the reply's params for one named "GLSVSSResults"
// (<= 1024 bytes) and, only when found, installs it as the CHARGLSVSSDATA
// form field and reloads the character data
// [orig: CNapiGameSession_HandleGLSVSSResults @0x4d33be..0x4d3416]; with
// nothing configured the param is omitted so the client no-ops.
LobbyDispatchResult LobbySession::handle_client_glsvss_request(
		const NapiMessage &msg, LobbyState &state) {
	opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] glsvss request rid=%u GLSVSSRequest='%s'",
	            state.rid, field_to_string(find_field(msg, "GLSVSSRequest")).c_str());
	NapiMessage reply;
	reply.name = "ServerGLSVSSResults";
	if (!glsvss_results_.empty()) {
		set_field(reply, "GLSVSSResults", glsvss_results_.substr(0, 1023));
	}
	return {{std::move(reply)}, "ClientGLSVSSRequest"};
}

} // namespace opennova
