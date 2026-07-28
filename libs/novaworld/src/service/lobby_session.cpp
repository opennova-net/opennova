#include <novaworld/lobby_session.h>

#include <novaworld/host_repository.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>
#include <io/log.h>

namespace opennova {

namespace {

std::string field_to_string(const NapiField *f) {
	if (!f || f->data.empty()) return {};
	const char *p = reinterpret_cast<const char *>(f->data.data());
	size_t n = f->data.size();
	while (n > 0 && p[n - 1] == '\0') --n;
	return std::string(p, n);
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
// Mirrors onnet's _find_app_id_in_container.
std::string find_app_id_recursive(const NapiMessage &msg) {
	if (msg.name == "ClientVar") {
		std::string vname, vvalue;
		for (const auto &f : msg.fields) {
			if (f.name == "VarName")  vname  = field_to_string(&f);
			else if (f.name == "VarValue") vvalue = field_to_string(&f);
		}
		if (vname == "AppId" && !vvalue.empty()) return vvalue;
	}
	for (const auto &child : msg.children) {
		auto found = find_app_id_recursive(child);
		if (!found.empty()) return found;
	}
	return {};
}

std::string default_sess_id() {
	// 32-char hex token (matches onnet's secrets.token_hex(16)).
	static thread_local std::mt19937_64 gen{std::random_device{}()};
	std::uniform_int_distribution<uint64_t> pick;
	uint64_t a = pick(gen);
	uint64_t b = pick(gen);
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

	uint32_t app_int = 0;
	try {
		if (!app_id.empty()) app_int = static_cast<uint32_t>(std::stoul(app_id));
	} catch (...) {}

	static thread_local std::mt19937_64 gen{std::random_device{}()};
	const uint64_t r = std::uniform_int_distribution<uint64_t>{}(gen);

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

uint32_t default_rid(const std::string &app_id) {
	uint32_t suffix = 0;
	try {
		if (!app_id.empty()) {
			suffix = static_cast<uint32_t>(std::stoul(app_id)) & 0x0FFFu;
		}
	} catch (...) {}
	if (suffix == 0) {
		static thread_local std::mt19937 gen{std::random_device{}()};
		suffix = std::uniform_int_distribution<uint32_t>{0, 0x0FFFu}(gen);
	}
	return 0x0A000000u | suffix;
}

int parse_int_safe(const std::string &s) {
	try {
		auto slash = s.find('/');
		return std::stoi(slash == std::string::npos ? s : s.substr(0, slash), nullptr, 10);
	} catch (...) {
		return 0;
	}
}

std::string first_value(const std::map<std::string, std::string> &a,
                        const std::map<std::string, std::string> &b,
                        std::initializer_list<const char *> keys) {
	for (const char *key : keys) {
		auto it = a.find(key);
		if (it != a.end() && !it->second.empty()) return it->second;
		it = b.find(key);
		if (it != b.end() && !it->second.empty()) return it->second;
	}
	return {};
}

std::string yn_value(const std::string &v, const char *fallback) {
	if (v.empty()) return fallback;
	if (v == "1" || v == "Y" || v == "y" || v == "true" || v == "TRUE") return "Y";
	if (v == "0" || v == "N" || v == "n" || v == "false" || v == "FALSE") return "N";
	return v;
}

void refresh_gsb_fields(LobbyState &state,
                        const std::map<std::string, std::string> &host_setup,
                        const std::map<std::string, std::string> &host_info) {
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
}

} // namespace

std::map<std::string, std::map<std::string, std::string>>
extract_var_lists(const NapiMessage &container) {
	std::map<std::string, std::map<std::string, std::string>> out;
	for (const auto &child : container.children) {
		if (child.name != "ClientVarList") continue;
		const auto list_name = field_to_string(find_field(child, "VarList"));
		if (list_name.empty()) continue;
		auto &entries = out[list_name];
		for (const auto &entry : child.children) {
			if (entry.name != "ClientVar") continue;
			const auto vname  = field_to_string(find_field(entry, "VarName"));
			const auto vvalue = field_to_string(find_field(entry, "VarValue"));
			if (!vname.empty()) entries[vname] = vvalue;
		}
	}
	return out;
}

LobbySession::LobbySession()
	: sess_id_gen_(default_sess_id),
	  gsid_gen_(default_gsid),
	  rid_gen_(default_rid) {}

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
	// Retail sends ClientHostPlayerAdded immediately after a successful host
	// registration (the host itself counts as the first player); onnet doesn't
	// reply either (no entry in onnet/onnw/novaworldudp.py). Bump player_count
	// optimistically so /api/hosts reflects the joined player even before the
	// next ClientHostUpdate arrives. (G.6 lifecycle.)
	else if (name == "ClientHostPlayerAdded") {
		state.player_count = std::max(1, state.player_count + 1);
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] player_added rid=%u players=%d/%d",
		            state.rid, state.player_count, state.max_players);
		return {{}, "ClientHostPlayerAdded"};
	}
	else if (name == "ClientHostPlayerRemoved") {
		state.player_count = std::max(0, state.player_count - 1);
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] player_removed rid=%u players=%d/%d",
		            state.rid, state.player_count, state.max_players);
		return {{}, "ClientHostPlayerRemoved"};
	}
	else if (name == "ClientStopHosting") {
		const uint32_t rid = state.rid;
		state.hosting = false;
		state.player_count = 0;
		if (db_ && rid != 0) {
			try { hostdb::remove_host_by_rid(*db_, rid); }
			catch (const std::exception &e) {
				opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[lobby] WARN stop-host remove: %s", e.what());
			}
		}
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] stopped rid=%u reason=ClientStopHosting", rid);
		return {{}, "ClientStopHosting"};
	}
	else if (name == "ClientStopPlaying") {
		state.play_state.clear();
		return {{}, "ClientStopPlaying"};
	}
	return LobbyDispatchResult{{}, std::string("unknown:") + name};
}

// witness: onnw/novaworldudp.py:114-120 — empty container response
LobbyDispatchResult LobbySession::handle_client_connected(const NapiMessage &, LobbyState &) {
	NapiMessage reply;
	reply.name = "ServerStartVerify";
	return {{std::move(reply)}, "ClientConnected"};
}

// witness: onnw/novaworldudp.py:122-137 — Success=1, SessIdString token,
// embedded ServerVarList(VarList="ConnectCommands").
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

// witness: onnw/novaworldudp.py:139-245 — extracts HostSetup + Host var
// lists, resolves AppId/LobbyName, mints gsid+rid, persists host state,
// returns ServerHostResult with HostCommands(GSID, HostRequiresJoinTicket).
LobbyDispatchResult LobbySession::handle_client_host_request(
		const NapiMessage &msg, LobbyState &state,
		const std::string &remote_ip, uint16_t remote_port) {
	auto var_lists = extract_var_lists(msg);
	auto host_setup = var_lists["HostSetup"];
	auto host_info  = var_lists["Host"];
	if (host_setup.empty() && host_info.empty()) {
		return {{}, "ClientHostRequest:missing-var-lists"};
	}

	std::string app_id = host_setup.count("AppId") ? host_setup["AppId"]
	                   : host_info.count("AppId")  ? host_info["AppId"]
	                                               : find_app_id_recursive(msg);

	std::string lobby_name = host_setup.count("LobbyName") ? host_setup["LobbyName"]
	                                                       : std::string();

	if (state.gsid.empty()) state.gsid = gsid_gen_(app_id);
	if (state.rid == 0)     state.rid  = rid_gen_(app_id);
	state.game = lobby_name;
	state.app_id = app_id;
	state.hosting = true;

	state.host_ip = host_info.count("ServerIP") ? host_info["ServerIP"]
	             : (state.host_ip.empty() ? remote_ip : state.host_ip);
	if (host_info.count("ServerPortNumber")) {
		state.host_port = parse_int_safe(host_info["ServerPortNumber"]);
	} else if (host_setup.count("ServerPortNumber")) {
		state.host_port = parse_int_safe(host_setup["ServerPortNumber"]);
	} else if (state.host_port == 0) {
		state.host_port = remote_port;
	}
	// Reflection override (dev/NAT): force a locally reachable host endpoint so
	// joiners can connect, instead of the observed docker-gateway source.
	// [cf onnw/novaworldudp.py:186,274 — ONNET_CLIENT_REFLECT_IP/PORT]
	if (!reflect_ip_.empty()) state.host_ip   = reflect_ip_;
	if (reflect_port_ != 0)   state.host_port = reflect_port_;

	state.server_name  = host_info.count("ServerName") ? host_info["ServerName"] : state.server_name;
	state.player_count = host_info.count("Players")    ? parse_int_safe(host_info["Players"]) : state.player_count;
	state.max_players  = host_setup.count("MaxPlayers") ? parse_int_safe(host_setup["MaxPlayers"]) : state.max_players;
	state.region       = host_info.count("Region") ? host_info["Region"]
	                  : host_info.count("Country") ? host_info["Country"]
	                                               : (state.region.empty() ? std::string("us") : state.region);
	refresh_gsb_fields(state, host_setup, host_info);

	// Diagnostic for the host/join "stuck on Enumerating" symptom: show what
	// endpoint the server OBSERVED for the host (UDP source) vs. what the host
	// ADVERTISED (Host.ServerIP / ServerPortNumber) vs. what we STORED. Under a
	// docker-bridge dev setup the observed source is the proxy gateway
	// (172.x:ephemeral) and retail usually advertises neither, so the stored
	// endpoint is unreachable by a joiner. Host networking lets the server see
	// the real client endpoint (the prod path). [cf onnw/novaworldudp.py:182-194]
	{
		std::string host_keys;
		for (const auto &kv : host_info) { host_keys += kv.first; host_keys += ' '; }
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host-addr request observed=%s:%u advertised ServerIP=%s "
		            "ServerPortNumber(Host)=%s ServerPortNumber(Setup)=%s stored=%s:%d Host{ %s}",
		            remote_ip.c_str(), static_cast<unsigned>(remote_port),
		            host_info.count("ServerIP") ? host_info["ServerIP"].c_str() : "(absent)",
		            host_info.count("ServerPortNumber") ? host_info["ServerPortNumber"].c_str() : "(absent)",
		            host_setup.count("ServerPortNumber") ? host_setup["ServerPortNumber"].c_str() : "(absent)",
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
		"[lobby] started rid=%u gsid=%s server_name='%s' host=%s:%d game=%s app_id=%s",
	            state.rid, state.gsid.c_str(), state.server_name.c_str(),
	            state.host_ip.c_str(), state.host_port, state.game.c_str(),
	            state.app_id.c_str());

	// Phase I.2: persist to DB so /jop_2.gsb + /api/hosts query SQL
	// rather than the in-memory snapshot. db_ is null in tests.
	if (db_) {
		// Diagnostic (join-failure triage): a ClientHostRequest re-sent AFTER a
		// PCIDKey-bearing update would, via INSERT OR REPLACE, wipe the stored
		// pcid_key back to empty (onnet's upsert preserves it). Watch this go
		// non-empty -> empty across requests.
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host request upsert rid=%u pcid_key=%zuB host_key=%zuB",
		            state.rid, state.pcid_key.size(), state.host_key.size());
		try {
			hostdb::upsert_host(*db_,
				hostdb::row_from_lobby(state, remote_ip, remote_port));
		} catch (const std::exception &e) {
			opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[lobby] WARN active_hosts upsert: %s", e.what());
		}
	}

	return {{std::move(reply)}, "ClientHostRequest"};
}

// witness: onnw/novaworldudp.py:247-296 — silent: parse var lists, refresh
// host_key/pcid_key/players/etc into state. No reply.
LobbyDispatchResult LobbySession::handle_client_host_update(
		const NapiMessage &msg, LobbyState &state, const std::string &remote_ip) {
	state.last_host_update = extract_var_lists(msg);
	const auto host_vars = state.last_host_update["Host"];
	if (host_vars.count("HostKey"))         state.host_key  = host_vars.at("HostKey");
	if (host_vars.count("PCIDKey"))         state.pcid_key  = host_vars.at("PCIDKey");
	{
		// Diagnostic (join-failure triage): does the host's ClientHostUpdate
		// actually carry PCIDKey? If pcid_key stays empty here, the join emits
		// an empty PUBPCID and the retail client reports "login info invalid or
		// expired". Dump the Host var keys + the resolved key lengths.
		std::string update_keys;
		for (const auto &kv : host_vars) { update_keys += kv.first; update_keys += ' '; }
		opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host update vars Host{ %s} host_key=%zuB pcid_key=%zuB",
		            update_keys.c_str(), state.host_key.size(), state.pcid_key.size());
	}
	if (host_vars.count("ServerIP"))        state.host_ip   = host_vars.at("ServerIP");
	if (host_vars.count("ServerPortNumber"))state.host_port = parse_int_safe(host_vars.at("ServerPortNumber"));
	if (host_vars.count("ServerName"))      state.server_name = host_vars.at("ServerName");
	if (host_vars.count("Players"))         state.player_count = parse_int_safe(host_vars.at("Players"));
	else if (host_vars.count("CurrentPlayers")) state.player_count = parse_int_safe(host_vars.at("CurrentPlayers"));
	if (host_vars.count("MaxPlayers"))      state.max_players  = parse_int_safe(host_vars.at("MaxPlayers"));
	if (host_vars.count("Region"))          state.region       = host_vars.at("Region");
	else if (host_vars.count("Country"))    state.region       = host_vars.at("Country");
	const auto host_setup = state.last_host_update["HostSetup"];
	refresh_gsb_fields(state, host_setup, host_vars);

	if (state.host_ip.empty()) state.host_ip = remote_ip;
	// Reflection override (dev/NAT) — same as the host-request path.
	if (!reflect_ip_.empty()) state.host_ip   = reflect_ip_;
	if (reflect_port_ != 0)   state.host_port = reflect_port_;

	opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] update rid=%u players=%d/%d server_name='%s'",
	            state.rid, state.player_count, state.max_players,
	            state.server_name.c_str());
	opennova::io::logf(opennova::io::LogLevel::kInfo,
		"[lobby] host-addr update observed=%s advertised ServerIP=%s "
	            "ServerPortNumber=%s stored=%s:%d",
	            remote_ip.c_str(),
	            host_vars.count("ServerIP") ? host_vars.at("ServerIP").c_str() : "(absent)",
	            host_vars.count("ServerPortNumber") ? host_vars.at("ServerPortNumber").c_str() : "(absent)",
	            state.host_ip.c_str(), state.host_port);

	// Phase I.2: keep DB row in sync. We don't have peer_port in the
	// update handler signature; pass 0 — the row already exists from the
	// initial upsert with the right peer addr, and update_host() doesn't
	// rely on peer_port for the WHERE clause.
	if (db_) {
		try {
			hostdb::update_host(*db_,
				hostdb::row_from_lobby(state, remote_ip, /*peer_port=*/0));
		} catch (const std::exception &e) {
			opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[lobby] WARN active_hosts update: %s", e.what());
		}
	}

	return {{}, "ClientHostUpdate"};
}

// witness: onnw/novaworldudp.py:298-315 — captures play_state, returns
// ServerPlayResult with empty PlayCommands ServerVarList.
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

} // namespace opennova
