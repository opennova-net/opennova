// nw-lister - advertise one game-server listing on a NovaWorld master, acting
// as a Joint Operations host without running the game.
//
// Flow (retail order, docs/net/novaworld-net-re.md):
//   gate probe (UDP 7597, "jop:cus2", GATEAPI cipher)  -> GATE VARs
//   NWU lobby session to UDPNOVAWORLD: ClientHello -> ServerHello -> ClientAuth
//     -> ServerSessionInit -> ClientConnected -> ServerStartVerify
//     -> ClientRequestVerifyResult(Cookie) -> ServerVerifyResult  = VALIDATED
//   [HTTP] nwprepare -> NWStart -> NWLogin POST -> NWLogin poll   (account login)
//   [HTTP] jop_2_host1.htm -> NWHost.dll (relay) -> NWHost.dll -> [HOSTKEY=...&]
//   ClientHostRequest(CurrentlyHosting=0, VarCheck=1; Cookie, HostSetup, Host, PlayerList)
//     -> ServerHostResult (Success, MsgCode, Rid, HostCommands{HostRequiresJoinTicket, GSID})
//   every 1860 ticks (~29.76 s): ClientHostUpdate(Host, PlayerList) + plaintext
//     status blob to POSTIPADDRESS:POSTIPPORT; ClientHostPlayerAdded/Removed on
//     roster changes; header-only 0x43 keepalives per the negotiated CS interval
//   stop: ClientStopHosting + ClientGoodBye (0x46)
//
// Protocol/crypto come from OpenNova (engine/net); this file owns sockets,
// timing, the HTTP transport and logging.

#include "http_client.h"
#include "listing.h"
#include "log.h"
#include "udp.h"

#include <base/io/log.h>
#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/napi/tlv.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/http_login.h>
#include <net/novaworld/lobby_update.h>
#include <net/novaworld/lobby_vars.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>

#include "socket_compat.h" // gethostname

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace opennova::lister;
using opennova::ClientSession;
using opennova::ClientVar;
using opennova::HostPlayerSlot;
using opennova::NapiMessage;

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
HANDLE g_done = nullptr; // set once StopHosting + GoodBye have gone out

BOOL WINAPI on_console_ctrl(DWORD type) {
	g_stop = true;
	// Closing the window / logoff / shutdown kills the process as soon as this
	// returns; hold it (Windows allows ~5 s) so the main loop can deregister.
	if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_done)
		WaitForSingleObject(g_done, 4500);
	return TRUE;
}
#else
void on_stop_signal(int) { g_stop = true; }
#endif

// Free-running millisecond counter for the AppId / PCIDKey seeds (retail feeds
// GetTickCount there).
uint32_t tick_count() {
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

struct Options {
	std::string listing_path;
	std::string credentials_path;
	std::string master_host = "127.0.0.1";
	uint16_t gate_port = opennova::GATE_DEFAULT_PORT;
	std::string bind_ip = "0.0.0.0";
	uint16_t bind_port = 0;       // NW session socket source port (0 = ephemeral)
	bool dry_run = false;
	std::string log_path;
	std::string login = "auto";   // auto | always | never
	bool allow_public = false;
	double refresh_s = static_cast<double>(opennova::SESSION_HOST_INFO_REFRESH_TICKS) / 62.5; // 29.76 s
	int json_poll_ms = 2000;
	std::string update_mode = "full"; // full | delta
	int run_seconds = 0;          // 0 = until Ctrl+C / stop file
	std::string stop_file;
	bool reconnect = true;
	bool verbose = false;
	bool send_status_blob = true;
};

void usage() {
	std::fprintf(stderr,
	    "nw-lister - list one server on a NovaWorld master (no game required)\n\n"
	    "usage: nw-lister --listing FILE.json [options]\n"
	    "  --credentials FILE     KEY=VALUE file with NOVAWORLD_USER / NOVAWORLD_PASS\n"
	    "  --master-host HOST     gate host (default 127.0.0.1)\n"
	    "  --master-gate-port N   gate UDP port (default %u)\n"
	    "  --allow-public         permit non-127.0.0.0/8 destinations (live master)\n"
	    "  --login auto|always|never  HTTP account login + NWHost HOSTKEY (default auto:\n"
	    "                         only when credentials are given)\n"
	    "  --dry-run              build and print every statement, send nothing\n"
	    "  --log FILE             append the log to FILE as well as stderr\n"
	    "  --refresh-seconds S    ClientHostUpdate/status cadence (default 29.76 = 1860 ticks)\n"
	    "  --update-mode full|delta  full Host/PlayerList each refresh (default) or retail dirty delta\n"
	    "  --json-poll-ms N       listing re-read interval for roster changes (default 2000)\n"
	    "  --no-status-blob       skip the plaintext POSTIPPORT heartbeat\n"
	    "  --bind-ip IP --bind-port N  local address for the session socket\n"
	    "  --run-seconds N        stop cleanly after N seconds (testing)\n"
	    "  --stop-file PATH       stop cleanly when PATH appears (testing)\n"
	    "  --no-reconnect         exit instead of re-registering after a lost session\n"
	    "  --verbose              dump every statement tree\n",
	    static_cast<unsigned>(opennova::GATE_DEFAULT_PORT));
}

bool parse_args(int argc, char **argv, Options &o) {
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto next = [&](std::string &dst) {
			if (i + 1 >= argc) return false;
			dst = argv[++i];
			return true;
		};
		std::string v;
		if (a == "--listing") { if (!next(o.listing_path)) return false; }
		else if (a == "--credentials") { if (!next(o.credentials_path)) return false; }
		else if (a == "--master-host") { if (!next(o.master_host)) return false; }
		else if (a == "--master-gate-port") { if (!next(v)) return false; o.gate_port = static_cast<uint16_t>(std::atoi(v.c_str())); }
		else if (a == "--bind-ip") { if (!next(o.bind_ip)) return false; }
		else if (a == "--bind-port") { if (!next(v)) return false; o.bind_port = static_cast<uint16_t>(std::atoi(v.c_str())); }
		else if (a == "--dry-run") o.dry_run = true;
		else if (a == "--log") { if (!next(o.log_path)) return false; }
		else if (a == "--login") { if (!next(o.login)) return false; }
		else if (a == "--allow-public") o.allow_public = true;
		else if (a == "--refresh-seconds") { if (!next(v)) return false; o.refresh_s = std::atof(v.c_str()); }
		else if (a == "--json-poll-ms") { if (!next(v)) return false; o.json_poll_ms = std::atoi(v.c_str()); }
		else if (a == "--update-mode") { if (!next(o.update_mode)) return false; }
		else if (a == "--run-seconds") { if (!next(v)) return false; o.run_seconds = std::atoi(v.c_str()); }
		else if (a == "--stop-file") { if (!next(o.stop_file)) return false; }
		else if (a == "--no-reconnect") o.reconnect = false;
		else if (a == "--no-status-blob") o.send_status_blob = false;
		else if (a == "--verbose" || a == "-v") o.verbose = true;
		else if (a == "--help" || a == "-h") return false;
		else {
			std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
			return false;
		}
	}
	if (o.listing_path.empty()) return false;
	if (o.login != "auto" && o.login != "always" && o.login != "never") return false;
	if (o.update_mode != "full" && o.update_mode != "delta") return false;
	if (o.refresh_s < 1.0) o.refresh_s = 1.0;
	return true;
}

// ----------------------------------------------------------------------------
// Statement / packet description for the log.

// Cookie-list names whose values are printed; every other Cookie value is masked
// (login session tags, account ids, machine fingerprints).
const std::set<std::string> kPlainCookie = {"CountryName", "Language", "TimeZoneBias", "MyInstalledExpBits",
                                            "NWCDKIID", "NWCDKIIDEXP1", "VER1", "VER2", "GT", "CC",
                                            "USEJUNCTION", "EXPBITS", "NWPF", "NWPF2"};

std::string printable(const std::string &s) {
	std::string out;
	for (unsigned char c : s) {
		if (c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
		else {
			char b[8];
			std::snprintf(b, sizeof(b), "\\x%02X", c);
			out += b;
		}
	}
	return out;
}

void dump_tree(const NapiMessage &m, int depth, std::string &out, const std::string &list_ctx) {
	const std::string pad(static_cast<size_t>(depth * 2), ' ');
	std::string list = list_ctx;
	std::string var_name;
	for (const auto &f : m.fields) {
		if (f.name == "VarList") list = opennova::field_to_string(f);
		if (f.name == "VarName") var_name = opennova::field_to_string(f);
	}
	out += pad + m.name;
	for (const auto &f : m.fields) {
		std::string v = opennova::field_to_string(f);
		const bool cookie_val = list == "Cookie" && f.name == "VarValue" && !kPlainCookie.count(var_name);
		const bool hostkey_val = f.name == "VarValue" && (var_name == "HostKey");
		if (cookie_val || hostkey_val) v = mask_value(v);
		out += " " + f.name + "=\"" + printable(v) + "\"";
	}
	out += "\n";
	for (const auto &c : m.children) dump_tree(c, depth + 1, out, list);
}

std::string statement_summary(const NapiMessage &m) {
	std::string s = m.name;
	std::vector<std::string> lists;
	for (const auto &c : m.children) {
		if (c.name == "ClientVarList" || c.name == "ServerVarList") {
			std::string ln;
			for (const auto &f : c.fields)
				if (f.name == "VarList") ln = opennova::field_to_string(f);
			lists.push_back(ln + "[" + std::to_string(c.children.size()) + "]");
		}
	}
	if (!m.fields.empty()) {
		s += " {";
		bool first = true;
		for (const auto &f : m.fields) {
			s += (first ? "" : ", ") + f.name + "=" + printable(opennova::field_to_string(f));
			first = false;
		}
		s += "}";
	}
	for (const auto &l : lists) s += " " + l;
	return s;
}

const char *opcode_name(uint8_t op) {
	switch (op) {
	case 0x41: return "0x41 ClientHello";
	case 0x42: return "0x42 ClientAuth";
	case 0x43: return "0x43 ClientSession";
	case 0x44: return "0x44 ClientResendList";
	case 0x46: return "0x46 ClientGoodBye";
	case 0x81: return "0x81 ServerHello";
	case 0x82: return "0x82 ServerSessionInit";
	case 0x83: return "0x83 ServerSession";
	case 0x84: return "0x84 ServerResendList";
	case 0x86: return "0x86 ServerGoodBye";
	default: return "op?";
	}
}

// Decode one NW datagram (either direction) for the log: opcode, seq/ack and the
// contained statement names. `scrk` is the key that encrypted the inner region.
std::vector<NapiMessage> describe_datagram(const std::vector<uint8_t> &dg, const std::string &scrk,
                                           std::string &line) {
	std::vector<NapiMessage> containers;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!opennova::nw_decode_inbound(dg.data(), dg.size(), op, body)) {
		line = "undecodable datagram (" + std::to_string(dg.size()) + "B)";
		return containers;
	}
	line = std::string(opcode_name(op)) + " " + std::to_string(dg.size()) + "B";
	if ((op == 0x43 || op == 0x83) && !scrk.empty()) {
		opennova::SessionSequencing scratch;
		opennova::ProtocolPacketHeader hdr;
		std::vector<opennova::ProtocolMessage> msgs;
		if (!opennova::deframe_session_packet(scratch, opennova::SessionCrypto{{}, scrk, 0, std::nullopt},
		                                      body.data(), body.size(), hdr, msgs)) {
			line += " (inner decode failed)";
			return containers;
		}
		line += " seq=" + std::to_string(hdr.seq_num) + " ack=" + std::to_string(hdr.ack_count);
		if (msgs.empty()) line += " [header-only ack/keepalive]";
		for (const auto &pm : msgs) {
			if (pm.flags.settings_update) {
				char b[48];
				std::snprintf(b, sizeof(b), " [settings tag 0x%03X %uB]", pm.full_tag, pm.length);
				line += b;
				continue;
			}
			if (pm.full_tag != 0) continue;
			if (pm.flags.frag_cont || pm.flags.frag_end) {
				line += " [fragment " + std::to_string(pm.length) + "B]";
				continue;
			}
			std::vector<NapiMessage> cs;
			size_t consumed = 0;
			if (opennova::napi_stream_decode(pm.payload.data(), pm.payload.size(), cs, &consumed) == 0) {
				for (auto &c : cs) {
					line += " <" + statement_summary(c) + ">";
					containers.push_back(std::move(c));
				}
			}
		}
	}
	return containers;
}

uint32_t now_ms() {
	static const auto t0 = std::chrono::steady_clock::now();
	return static_cast<uint32_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

std::string ip4(const std::array<uint8_t, 4> &ip) {
	return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
	       std::to_string(ip[3]);
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// Stable per-machine fingerprint seed (the verify Cookie's NWPSSK/NWUSID are
// retail hardware tokens; the master only records them).
uint32_t machine_seed() {
	char name[256] = {};
	gethostname(name, sizeof(name) - 1);
	uint32_t h = 2166136261u;
	for (const char *c = name; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
	return h;
}

// ----------------------------------------------------------------------------

class Lister {
public:
	explicit Lister(Options o) : opt_(std::move(o)) {}

	int run();

private:
	enum class Outcome { Stopped, Lost, Fatal };

	bool load_listing_now(bool initial);
	Outcome session_once();
	bool gate_probe();
	bool handshake();
	bool http_login_and_hostkey();
	bool host_request();
	Outcome hosting_loop();
	void stop_hosting_and_goodbye();

	void pump(int wait_ms);
	void send_nw(const std::vector<uint8_t> &dg, const char *what);
	void send_nw_one(const std::vector<uint8_t> &dg, const char *what);
	std::vector<std::vector<uint8_t>> fragment_if_needed(const std::vector<uint8_t> &dg);
	void drain_notices();
	bool should_stop() const;

	std::vector<std::pair<std::string, std::string>> identity_vars() const;
	std::vector<std::pair<std::string, std::string>> cookie_vars() const;
	opennova::HostRegistration current_reg() const;
	std::vector<HostPlayerSlot> roster() const { return listing_.players; }
	void send_host_update(bool full);
	void send_status_blob();
	void sync_roster(const std::vector<HostPlayerSlot> &before, const std::vector<HostPlayerSlot> &after);

	opennova::LoginResult http_drive(opennova::LobbyHttpFlow &flow, opennova::LoginResult r);
	HttpResponse http_get_logged(const std::string &url);

	void dry_run();

	Options opt_;
	Credentials creds_;
	Listing listing_;
	std::filesystem::file_time_type listing_mtime_{};
	std::string listing_raw_;

	UdpSocket gate_sock_, nw_sock_;
	Addr gate_addr_, nw_addr_, post_addr_;
	bool have_post_ = false;
	opennova::GateResponse gate_;
	std::unique_ptr<ClientSession> session_;
	uint32_t ci_ = 0, ck_ = 0;
	std::mt19937 rng_{std::random_device{}()};

	bool want_login_ = false;
	bool logged_in_ = false;
	opennova::CookieJar jar_;  // the browser jar after login/NWHost
	std::string host_key_;
	uint32_t app_id_ = 0;
	opennova::SessionIdRing pcid_ring_;
	uint32_t hosting_started_ms_ = 0;
	uint32_t run_started_ms_ = 0;
	std::vector<ClientVar> last_host_, last_players_;
	bool session_lost_ = false;
	bool host_result_seen_ = false;
};

bool Lister::should_stop() const {
	if (g_stop) return true;
	if (opt_.run_seconds > 0 && now_ms() - run_started_ms_ > static_cast<uint32_t>(opt_.run_seconds) * 1000u)
		return true;
	if (!opt_.stop_file.empty()) {
		std::error_code ec;
		if (std::filesystem::exists(opt_.stop_file, ec)) return true;
	}
	return false;
}

bool Lister::load_listing_now(bool initial) {
	std::error_code ec;
	const auto mt = std::filesystem::last_write_time(opt_.listing_path, ec);
	if (!initial && !ec && mt == listing_mtime_) return false;
	Listing fresh;
	std::string err;
	if (!load_listing(opt_.listing_path, fresh, err)) {
		logf("[listing] %s: %s (keeping the previous listing)", initial ? "load failed" : "re-read failed",
		     err.c_str());
		return false;
	}
	listing_mtime_ = mt;
	listing_ = std::move(fresh);
	std::string names;
	for (const auto &p : listing_.players) names += (names.empty() ? "" : ", ") + p.player_name;
	logf("[listing] %s: name='%s' map='%s' type=%s players=%zu/%d [%s] exp=%s password=%d",
	     initial ? "loaded" : "changed", listing_.reg.server_name.c_str(), listing_.reg.mission_name.c_str(),
	     listing_.reg.game_type.c_str(), listing_.players.size(), listing_.reg.max_players, names.c_str(),
	     listing_.reg.expansion.c_str(), listing_.reg.password ? 1 : 0);
	return true;
}

std::vector<std::pair<std::string, std::string>> Lister::identity_vars() const {
	opennova::LobbyIdentityParams p;
	p.client_index = ci_;
	p.client_key = ck_;
	p.country = listing_.reg.country_name.empty() ? "United Kingdom" : listing_.reg.country_name;
	p.language = listing_.reg.language.empty() ? "English" : listing_.reg.language;
	p.tz_bias = std::to_string(listing_.reg.tz_bias);
	p.my_installed_exp_bits = listing_.installed_exp_bits;
	const uint32_t seed = machine_seed();
	p.nwpssk = opennova::az_fingerprint(seed ^ 0x5053534Bu, opennova::kNwpsskLen);
	p.nwusid = opennova::az_fingerprint(seed ^ 0x55534944u, opennova::kNwusidLen);
	return opennova::make_lobby_identity_vars(p);
}

// The Cookie var-list every Cookie-bearing statement serializes: the browser jar
// (after login/NWHost) with the locale trio overlaid, else the identity set -
// LobbyHttpFlow::session_cookie_vars' rule. NWUID empty = echo the SessionInit's.
std::vector<std::pair<std::string, std::string>> Lister::cookie_vars() const {
	const auto ids = identity_vars();
	if (!logged_in_) return ids;
	opennova::CookieJar c = jar_;
	for (const auto &kv : ids) {
		const bool locale = kv.first == "CountryName" || kv.first == "Language" || kv.first == "TimeZoneBias";
		if (!c.find(kv.first) || locale) {
			c.set(kv.first, (kv.first == "NWUID" && kv.second.empty() && session_) ? session_->server_nwuid()
			                                                                     : kv.second);
		}
	}
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto &n : c.names()) out.emplace_back(n, *c.find(n));
	return out;
}

opennova::HostRegistration Lister::current_reg() const {
	opennova::HostRegistration r = listing_.reg;
	r.lobby_name = !listing_.lobby_name_override.empty() ? listing_.lobby_name_override
	               : !gate_.lobby_name.empty()            ? gate_.lobby_name
	                                                      : std::string("jop_2_consumer");
	r.app_id = app_id_;
	r.host_key = host_key_;
	r.pcid_key = pcid_ring_.current();
	r.player_count = listing_.player_count_override >= 0 ? listing_.player_count_override
	                                                     : static_cast<int>(listing_.players.size());
	r.uptime_ms = hosting_started_ms_ ? now_ms() - hosting_started_ms_ : 0;
	return r;
}

// Retail's CNapiNPConnection_QueueMessage @0x628640 splits any message larger than
// SESSION_MESSAGE_CHUNK_BYTES (1300) into fragments: first 0x04 (FRAG_CONT), mid 0x06,
// final 0x02 (FRAG_END), one packet each, consecutive sequence numbers. OpenNova's
// ClientSession frames a lobby container as ONE record (a 4.3 KB ClientHostRequest
// with the full cookie jar), which a 4 KB receive buffer truncates. We re-frame the
// datagram it built: read its seq/ack, split the payload, frame each fragment under
// seq, seq+1, ... and then advance ClientSession's own counter past the extra
// packets with discarded header-only frames (retention is off on the lobby link).
std::vector<std::vector<uint8_t>> Lister::fragment_if_needed(const std::vector<uint8_t> &dg) {
	std::vector<std::vector<uint8_t>> out;
	constexpr size_t kChunk = opennova::SESSION_MESSAGE_CHUNK_BYTES;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!session_ || dg.size() <= kChunk + 100 || !opennova::nw_decode_inbound(dg.data(), dg.size(), op, body) ||
	    op != 0x43) {
		out.push_back(dg);
		return out;
	}
	opennova::SessionSequencing scratch;
	opennova::ProtocolPacketHeader hdr;
	std::vector<opennova::ProtocolMessage> msgs;
	if (!opennova::deframe_session_packet(scratch, opennova::SessionCrypto{{}, session_->client_scrk(), 0, std::nullopt},
	                                      body.data(), body.size(), hdr, msgs) ||
	    msgs.size() != 1 || msgs[0].payload.size() <= kChunk) {
		out.push_back(dg);
		return out;
	}
	const std::vector<uint8_t> &payload = msgs[0].payload;
	const size_t n = (payload.size() + kChunk - 1) / kChunk;
	opennova::SessionSequencing seq;
	seq.next_outbound_seq = hdr.seq_num;
	seq.last_inbound_seq = hdr.ack_count;
	const std::string scrk = session_->client_scrk();
	for (size_t i = 0; i < n; ++i) {
		const size_t off = i * kChunk;
		const size_t len = std::min(kChunk, payload.size() - off);
		opennova::ProtocolMessage pm;
		pm.tag = msgs[0].tag;
		pm.full_tag = msgs[0].full_tag;
		pm.flags.len16 = len > 0xFF;
		pm.flags.len8 = !pm.flags.len16;
		pm.flags.frag_cont = i + 1 < n;   // first + mid
		pm.flags.frag_end = i > 0;        // mid + final
		pm.flags.raw = static_cast<uint8_t>((pm.flags.len16 ? 0x40 : 0x20) | (pm.flags.frag_cont ? 0x04 : 0) |
		                                    (pm.flags.frag_end ? 0x02 : 0));
		pm.length = static_cast<uint32_t>(len);
		pm.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
		                  payload.begin() + static_cast<std::ptrdiff_t>(off + len));
		std::vector<uint8_t> framed;
		if (!opennova::frame_session_packet(seq, opennova::SessionCrypto{scrk, {}, session_->server_key()}, {pm}, framed)) {
			logf("[tx] fragment framing failed; sending the unsplit datagram");
			out.clear();
			out.push_back(dg);
			return out;
		}
		out.push_back(opennova::nw_encode_outbound(0x43, std::move(framed)));
	}
	for (size_t i = 1; i < n; ++i) (void)session_->build_heartbeat(); // consume seq+1 .. seq+n-1
	logf("[tx] message %zuB split into %zu fragments (seq %u..%u)", payload.size(), n, hdr.seq_num,
	     hdr.seq_num + static_cast<uint32_t>(n) - 1);
	return out;
}

void Lister::send_nw(const std::vector<uint8_t> &dg_in, const char *what) {
	if (dg_in.empty()) return;
	const auto parts = fragment_if_needed(dg_in);
	if (parts.size() > 1) {
		for (const auto &p : parts) send_nw_one(p, what);
		return;
	}
	send_nw_one(parts.front(), what);
}

void Lister::send_nw_one(const std::vector<uint8_t> &dg, const char *what) {
	if (dg.empty()) return;
	std::string line;
	const auto cs = describe_datagram(dg, session_ ? session_->client_scrk() : std::string(), line);
	logf("[tx] %s -> %s%s%s", line.c_str(), nw_addr_.str().c_str(), what ? "  # " : "", what ? what : "");
	if (log_verbose()) {
		for (const auto &c : cs) {
			std::string t;
			dump_tree(c, 2, t, "");
			vlogf("[tx] statement tree:\n%s", t.c_str());
		}
	}
	if (!nw_sock_.send_to(nw_addr_, dg)) logf("[tx] send failed");
}

void Lister::pump(int wait_ms) {
	if (!session_) return;
	std::vector<uint8_t> buf;
	Addr from;
	int waited = 0;
	for (;;) {
		const int n = nw_sock_.recv_from(buf, from, waited == 0 ? wait_ms : 0);
		waited = 1;
		if (n <= 0) break;
		session_->set_clock_ms(now_ms());
		std::string line;
		const auto cs = describe_datagram(buf, session_->server_scrk(), line);
		const auto before = session_->state();
		std::vector<std::vector<uint8_t>> replies;
		const bool ok = session_->handle_datagram(buf.data(), buf.size(), replies);
		// The ServerSessionInit (0x82) carries the server SCRK the 0x83s use; decode
		// again now if the first pass could not.
		logf("[rx] %s <- %s state %d->%d", line.c_str(), from.str().c_str(), static_cast<int>(before),
		     static_cast<int>(session_->state()));
		for (const auto &c : cs) {
			if (log_verbose()) {
				std::string t;
				dump_tree(c, 2, t, "");
				vlogf("[rx] statement tree:\n%s", t.c_str());
			}
			if (c.name == "ServerHostResult") {
				// Log every field of the registration result.
				std::string t;
				dump_tree(c, 2, t, "");
				logf("[host] ServerHostResult (all fields):\n%s", t.c_str());
			}
		}
		for (const auto &r : replies) send_nw(r, "reply");
		if (!ok) {
			logf("[session] protocol error: %s", session_->last_error().c_str());
			session_lost_ = true;
			return;
		}
	}
	session_->set_clock_ms(now_ms());
	std::vector<std::vector<uint8_t>> periodic;
	session_->process_periodic_update(periodic);
	session_->pump_send_intervals(periodic);
	for (const auto &dg : periodic) send_nw(dg, "periodic");
	if (session_->state() == ClientSession::State::Closed && !session_lost_) {
		const auto &ev = session_->disconnect_event();
		logf("[session] closed by peer/reap: dc=%u dpc=%u dstr=%s ddstr=%s", ev.dc, ev.dpc, ev.dstr.c_str(), ev.ddstr.c_str());
		session_lost_ = true;
	}
	// Anything arriving on the gate socket after the probe (not expected).
	if (gate_sock_.is_open()) {
		while (gate_sock_.recv_from(buf, from, 0) > 0)
			vlogf("[rx] %zuB on the gate socket from %s (ignored)", buf.size(), from.str().c_str());
	}
	drain_notices();
}

void Lister::drain_notices() {
	if (!session_) return;
	using K = ClientSession::Notice::Kind;
	for (const auto &n : session_->take_notices()) {
		switch (n.kind) {
		case K::HostResult: {
			host_result_seen_ = true;
			const auto &f = n.fields;
			logf("[host] ServerHostResult Success=%d MsgCode=%d MsgParam1=%d MsgParam2=%d", f.success, f.msg_code,
			     f.msg_param1, f.msg_param2);
			logf("[host]   GSID=%s HostRequiresJoinTicket=%d", session_->host_gsid().c_str(),
			     session_->host_requires_join_ticket());
			if (!f.success)
				logf("[host] REJECTED: %s", opennova::novaworld_host_error_tag(f.msg_code).c_str());
			break;
		}
		case K::StopHosting:
			logf("[host] ServerStopHosting MsgCode=%d (%s)", n.fields.msg_code, n.msg_key.c_str());
			session_lost_ = true;
			break;
		case K::LeaveNovaWorld:
			logf("[host] ServerLeaveNovaWorld (punted) MsgCode=%d", n.fields.msg_code);
			session_lost_ = true;
			break;
		case K::Command:
			logf("[host] ServerCommand verb=%s args=%zu (not acted on: no game behind this listing)",
			     opennova::server_command_verb_name(n.command.verb), n.command.args.size());
			break;
		case K::PlayerEnterResult:
			logf("[host] ServerPlayerEnterResult conn=%u success=%d", n.player_enter.connection_id,
			     n.player_enter.success);
			break;
		default:
			logf("[host] notice kind=%d", static_cast<int>(n.kind));
			break;
		}
	}
}

bool Lister::gate_probe() {
	if (!resolve_checked(opt_.master_host, opt_.gate_port, gate_addr_, "gate")) return false;
	auto inner = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t sz = 0;
	if (opennova::napi_envelope_encode(inner.data(), inner.size(), probe.data(), probe.size(), &sz) != 0) {
		logf("[gate] envelope encode failed");
		return false;
	}
	probe.resize(sz);
	const uint32_t t0 = now_ms();
	uint32_t last = 0;
	bool first = true;
	std::vector<uint8_t> buf;
	Addr from;
	while (!should_stop()) {
		if (first || now_ms() - last > opennova::SESSION_GATE_PROBE_RETRY_MS) {
			logf("[tx] gate probe tag=%s %zuB -> %s", opennova::GATE_PROBE_TAG_JOINTOPS, probe.size(),
			     gate_addr_.str().c_str());
			gate_sock_.send_to(gate_addr_, probe);
			last = now_ms();
			first = false;
		}
		if (now_ms() - t0 > opennova::SESSION_GATE_PROBE_TIMEOUT_MS) {
			logf("[gate] no response in %u ms (%s)", opennova::SESSION_GATE_PROBE_TIMEOUT_MS,
			     opennova::novaworld_gate_error_tag(0, false).c_str());
			return false;
		}
		if (gate_sock_.recv_from(buf, from, 200) <= 0) continue;
		std::vector<uint8_t> in(buf.size());
		size_t isz = 0;
		if (opennova::napi_envelope_decode(buf.data(), buf.size(), in.data(), in.size(), &isz) != 0) {
			logf("[rx] gate: bad envelope (%zuB)", buf.size());
			continue;
		}
		in.resize(isz);
		opennova::GateResponse g;
		if (!opennova::gate_response_decrypt_and_parse(in.data(), in.size(), g)) {
			logf("[rx] gate: undecodable response");
			continue;
		}
		gate_ = g;
		logf("[rx] gate response %zuB <- %s: %d VARs", buf.size(), from.str().c_str(), g.var_count);
		logf("[gate]   LOBBYNAME=%s UDPNOVAWORLD=%s POSTIPADDRESS=%s POSTIPPORT=%u", g.lobby_name.c_str(),
		     g.udp_novaworld.c_str(), ip4(g.post_ip).c_str(), g.post_port);
		logf("[gate]   STARTUPURL=%s UDPCODE1=%s UDPCODE2=%s USEJUNCTION=%d CLEARJUNCTION=%d",
		     g.startup_url.c_str(), g.udp_code1.c_str(), g.udp_code2.c_str(), g.use_junction, g.clear_junction);
		logf("[gate]   REFLECTED=%s:%u MET=%s:%u '%s' GLSVSS='%s'", ip4(g.reflected_ip).c_str(), g.reflected_port,
		     g.met_ip.c_str(), g.met_port, g.met_label.c_str(), g.glsvss_request.c_str());
		std::string h;
		uint16_t p = 0;
		if (!opennova::parse_host_port(g.udp_novaworld, h, p)) {
			logf("[gate] UDPNOVAWORLD malformed");
			return false;
		}
		if (!resolve_checked(h, p, nw_addr_, "UDPNOVAWORLD")) return false;
		have_post_ = false;
		if (g.post_port != 0 && ip4(g.post_ip) != "0.0.0.0") {
			Addr pa;
			if (resolve_checked(ip4(g.post_ip), static_cast<uint16_t>(g.post_port), pa, "POSTIPADDRESS")) {
				post_addr_ = pa;
				have_post_ = true;
			}
		}
		return true;
	}
	return false;
}

bool Lister::handshake() {
	ClientSession::Config cfg;
	cfg.client_index = ci_;
	cfg.client_key = ck_;
	// Retail identity block for the lobby connection (CO/AP/BDAT are read but
	// not validated; NVS/PN/PV1/PG are validated and keep OpenNova's defaults).
	cfg.co = "NovaLogic Inc, Calabasas CA U.S.A.";
	cfg.ap = "JOINTOPS.EXE";
	cfg.bdat = "Jul 21 2009 18:54:41";
	opennova::NovaWorldJoinCu cu;
	cu.application = "Jointops.exe";
	cu.gate_tag = cfg.na;
	cu.met_tag = gate_.met_label;
	cu.udp_code1 = gate_.udp_code1;
	cu.udp_code2 = gate_.udp_code2;
	cfg.cu_vars = opennova::make_novaworld_join_cu(cu);
	cfg.glsvss_request = gate_.glsvss_request;
	cfg.glsvss_rims_ms = gate_.glsvss_rims;
	cfg.glsvss_agrms_ms = gate_.glsvss_agrms;
	cfg.cookie_vars = [this]() { return cookie_vars(); };
	session_ = std::make_unique<ClientSession>(cfg);
	session_->set_clock_ms(now_ms());
	session_lost_ = false;
	send_nw(session_->start(), "start handshake");
	const uint32_t t0 = now_ms();
	uint32_t last = now_ms();
	while (!should_stop() && !session_lost_) {
		pump(50);
		if (session_->is_verified()) {
			logf("[session] VALIDATED (SessIdString=%s, NWUID=%s, web domain=%s, CS timeout=%d idle=%d ms)",
			     mask_value(session_->sess_id_string(), 6).c_str(), mask_value(session_->server_nwuid(), 6).c_str(),
			     session_->server_web_domain().c_str(), session_->connection_settings().timeout_ms,
			     session_->connection_settings().idle_send_interval_ms);
			return true;
		}
		if (session_->state() == ClientSession::State::Error) {
			logf("[session] error: %s", session_->last_error().c_str());
			return false;
		}
		if (now_ms() - t0 > opennova::SESSION_CONNECT_TIMEOUT_MS) {
			logf("[session] handshake timeout (state %d)", static_cast<int>(session_->state()));
			return false;
		}
		if (now_ms() - last > opennova::SESSION_CONNECT_RETRANSMIT_MS) {
			last = now_ms();
			send_nw(session_->retransmit_stage_datagram(), "stage retransmit");
		}
	}
	return false;
}

HttpResponse Lister::http_get_logged(const std::string &url) {
	std::vector<std::string> headers;
	for (const auto &l : jar_.cookie_header_lines()) headers.push_back("Cookie: " + l);
	logf("[http] GET %s (%zu cookies)", url.c_str(), headers.size());
	HttpResponse r = http_request(false, url, headers, "");
	std::vector<std::string> sc;
	std::vector<std::string> names;
	for (const auto &h : r.headers) {
		if (lower(h.substr(0, 11)) == "set-cookie:") {
			std::string v = h.substr(11);
			while (!v.empty() && v.front() == ' ') v.erase(0, 1);
			sc.push_back(v);
			names.push_back(v.substr(0, v.find('=')));
		}
	}
	if (!sc.empty()) jar_.merge_set_cookie_values(sc);
	std::string nl;
	for (const auto &n : names) nl += " " + n;
	logf("[http]   -> %s %d, %zuB body, Set-Cookie:%s", r.transport_ok ? "HTTP" : r.error.c_str(), r.code,
	     r.body.size(), nl.c_str());
	return r;
}

opennova::LoginResult Lister::http_drive(opennova::LobbyHttpFlow &flow, opennova::LoginResult r) {
	using K = opennova::LoginResult::Kind;
	int steps = 0;
	while (r.kind == K::NeedRequest && !should_stop() && steps++ < 20) {
		const auto &q = r.request;
		const bool post = q.method == opennova::HttpMethod::Post;
		// Never log the POST body: NAME/PASSWORD ride it (EPASK-encrypted).
		std::string path = q.url;
		logf("[http] %s %s (%zu header lines%s)", post ? "POST" : "GET", path.c_str(), q.headers.size(),
		     post ? (", form body " + std::to_string(q.body.size()) + "B, fields EPASK-encrypted").c_str() : "");
		HttpResponse resp = http_request(post, q.url, q.headers, q.body);
		std::string names;
		for (const auto &h : resp.headers)
			if (lower(h.substr(0, 11)) == "set-cookie:") {
				std::string v = h.substr(11);
				while (!v.empty() && v.front() == ' ') v.erase(0, 1);
				names += " " + v.substr(0, v.find('='));
			}
		logf("[http]   -> %s %d, %zuB body, Set-Cookie:%s", resp.transport_ok ? "HTTP" : resp.error.c_str(),
		     resp.code, resp.body.size(), names.c_str());
		pump(0); // keep the lobby session serviced between HTTP legs
		r = flow.on_login_response(resp.transport_ok, resp.code, resp.headers, resp.body);
	}
	return r;
}

// Extract the first META refresh URL of an HTML page ("" when none).
std::string meta_refresh_url(const std::string &html) {
	const std::string l = lower(html);
	size_t at = l.find("http-equiv=\"refresh\"");
	if (at == std::string::npos) return "";
	size_t u = l.find("url=", at);
	if (u == std::string::npos) return "";
	u += 4;
	std::string out;
	if (u < html.size() && (html[u] == '"' || html[u] == '\'')) {
		const char q = html[u++];
		const size_t e = html.find(q, u);
		out = html.substr(u, e == std::string::npos ? std::string::npos : e - u);
	} else {
		size_t e = u;
		while (e < html.size() && html[e] != '"' && html[e] != '>' && !std::isspace(static_cast<unsigned char>(html[e])))
			++e;
		out = html.substr(u, e - u);
	}
	return out;
}

bool Lister::http_login_and_hostkey() {
	opennova::LobbyHttpFlow flow;
	opennova::LobbyHttpContext ctx;
	ctx.startup_url = gate_.startup_url;
	ctx.post_ip = ip4(gate_.post_ip);
	ctx.post_port = std::to_string(gate_.post_port);
	ctx.web_domain = session_->server_web_domain();
	ctx.server_nwuid = session_->server_nwuid();
	ctx.locale = "en_GB";
	ctx.identity_vars = identity_vars();
	flow.set_context(ctx);
	const std::string base = flow.http_base();
	logf("[http] web base %s (startupurl %s)", base.c_str(), flow.resolve_startup_url().c_str());
	if (base.empty()) {
		logf("[http] no web base from the gate/SessionInit");
		return false;
	}
	auto r = http_drive(flow, flow.login(creds_.user, creds_.pass));
	if (r.kind != opennova::LoginResult::Kind::Succeeded) {
		logf("[http] LOGIN FAILED: %s", r.reason.c_str());
		return false;
	}
	log_add_secret(r.nwhandle);
	logf("[http] LOGIN OK: NWHANDLE=%s PCID=%s", mask_value(r.nwhandle, 2).c_str(), mask_value(r.pcid, 2).c_str());
	jar_ = flow.cookies();
	logged_in_ = true;
	if (const std::string *t = jar_.find("LOGINSESSIONTAG")) log_add_secret(*t);

	// The retail "Host" link: jop_2_host1.htm meta-refreshes to NWHost.dll, whose
	// relay page refreshes back to NWHost.dll; the success page's <TITLE> carries
	// [HOSTKEY=...&], which the game parses (parse_connection_query_string
	// @0x54dfb0: HOSTKEY= trimmed at '&' then ']').
	std::string url = base + "/jop_2_host1.htm";
	const std::string fallback = base +
	    "/NWHost.dll?needexpkey=jop_2_key2err.htm&success=jop_2_host2.htm&failure=jop_2_main.htm"
	    "&relay=jop_2_relay.htm&msgbase=jop_2_msg.htm&nodb=jop_2_nodb.htm&start=jop_2_login.htm&pfid=28";
	for (int hop = 0; hop < 8 && !should_stop(); ++hop) {
		HttpResponse resp = http_get_logged(url);
		pump(0);
		const std::string html(resp.body.begin(), resp.body.end());
		if (!resp.transport_ok || resp.code != 200) {
			if (hop == 0) {
				url = fallback;
				continue;
			}
			logf("[http] NWHost leg failed (HTTP %d)", resp.code);
			return false;
		}
		const size_t hk = html.find("HOSTKEY=");
		if (hk != std::string::npos) {
			std::string v = html.substr(hk + 8);
			v = v.substr(0, v.find('&'));
			v = v.substr(0, v.find(']'));
			while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
			host_key_ = v;
			logf("[http] HOSTKEY obtained: %s", mask_value(host_key_, 6).c_str());
			if (const std::string *t = jar_.find("NWJOINSESSIONTAG")) log_add_secret(*t);
			return true;
		}
		std::string next = meta_refresh_url(html);
		if (next.empty()) {
			const std::string *tag = jar_.find("NWJOINSESSIONTAG");
			if (hop > 0 && tag && !tag->empty()) next = "NWHost.dll?tag=" + *tag;
		}
		if (next.empty()) {
			// Message pages render the error into @MESSAGE@/@GENERIC@.
			const size_t m = lower(html).find("<ib3_subst");
			logf("[http] NWHost leg: no refresh and no HOSTKEY; page excerpt: %s",
			     m == std::string::npos ? "(none)" : printable(html.substr(m, 240)).c_str());
			return false;
		}
		if (next.compare(0, 7, "http://") != 0) next = base + "/" + (next[0] == '/' ? next.substr(1) : next);
		url = next;
	}
	logf("[http] NWHost leg: no HOSTKEY after redirects");
	return false;
}

bool Lister::host_request() {
	// AppId: (GetTickCount + rand) % 9000 + 1000, once per registration.
	app_id_ = opennova::make_session_app_id(now_ms() + tick_count(), static_cast<int>(rng_() & 0x7FFF));
	pcid_ring_ = opennova::SessionIdRing{};
	pcid_ring_.advance(tick_count(), static_cast<int>(rng_() & 0x7FFF));
	host_result_seen_ = false;
	const auto reg = current_reg();
	const auto dg = session_->build_host_request(reg, /*currently_hosting=*/0);
	if (dg.empty()) {
		logf("[host] could not build ClientHostRequest (session state)");
		return false;
	}
	logf("[host] ClientHostRequest: AppId=%u LobbyName=%s CurrentlyHosting=0 VarCheck=1 (HostKey %s)", app_id_,
	     reg.lobby_name.c_str(), host_key_.empty() ? "none - not logged in" : "rides the Host list updates");
	send_nw(dg, "StartHosting");
	const uint32_t t0 = now_ms();
	while (!should_stop() && !session_lost_) {
		pump(50);
		if (host_result_seen_) {
			if (session_->host_state() == ClientSession::HostState::Established) return true;
			return false;
		}
		if (now_ms() - t0 > opennova::SESSION_CONNECT_TIMEOUT_MS) {
			logf("[host] no ServerHostResult in 60 s (%s); sending ClientStopHosting", opennova::NWEC_HOST_TIMEOUT);
			send_nw(session_->build_stop_hosting(), "host poll timeout");
			return false;
		}
	}
	return false;
}

void Lister::send_host_update(bool full) {
	const auto reg = current_reg();
	const auto host = opennova::make_host_var_list(reg, listing_.text, /*full=*/true);
	const auto players = opennova::make_player_list(roster());
	const bool delta = !full && opt_.update_mode == "delta";
	const auto h = delta ? opennova::dirty_client_vars(last_host_, host) : host;
	const auto p = delta ? opennova::dirty_client_vars(last_players_, players) : players;
	if (h.empty() && p.empty()) return;
	logf("[host] ClientHostUpdate %s: Host[%zu] PlayerList[%zu] Players=%d map='%s' PCIDKey=%u", delta ? "delta" : "full",
	     h.size(), p.size(), reg.player_count, reg.mission_name.c_str(), reg.pcid_key);
	send_nw(session_->build_host_update(h, p), "refresh");
	last_host_ = host;
	last_players_ = players;
}

void Lister::send_status_blob() {
	if (!opt_.send_status_blob || !have_post_) return;
	if (gate_.use_junction) return; // retail skips the POST leg on the junction bypass
	const auto blob = opennova::make_host_status_blob(current_reg(), listing_.text, roster());
	const auto dg = opennova::lobby_update_build_datagram(gate_, blob);
	if (dg.empty()) return;
	std::string text = opennova::lobby_update_build(blob);
	if (!host_key_.empty()) {
		const std::string san = opennova::lobby_sanitize_value(host_key_);
		for (size_t pos; (pos = text.find(san)) != std::string::npos;) text.replace(pos, san.size(), mask_value(host_key_, 6));
	}
	logf("[tx] status blob (Lobby_UpdateServerInfo) %zuB -> %s", dg.size(), post_addr_.str().c_str());
	vlogf("[tx] status blob text: %s", printable(text).c_str());
	gate_sock_.send_to(post_addr_, dg);
}

void Lister::sync_roster(const std::vector<HostPlayerSlot> &before, const std::vector<HostPlayerSlot> &after) {
	auto find = [](const std::vector<HostPlayerSlot> &v, int slot) -> const HostPlayerSlot * {
		for (const auto &p : v)
			if (p.slot == slot) return &p;
		return nullptr;
	};
	for (const auto &p : before) {
		const HostPlayerSlot *q = find(after, p.slot);
		if (!q || q->player_name != p.player_name) {
			logf("[host] ClientHostPlayerRemoved slot=%d (%s)", p.slot, p.player_name.c_str());
			send_nw(session_->build_host_player_removed(p.slot), "roster");
		}
	}
	for (const auto &p : after) {
		const HostPlayerSlot *q = find(before, p.slot);
		if (!q || q->player_name != p.player_name || q->ip_and_port != p.ip_and_port || q->team != p.team) {
			logf("[host] ClientHostPlayerAdded slot=%d name='%s'", p.slot, p.player_name.c_str());
			send_nw(session_->build_host_player_added(p), "roster");
		}
	}
}

Lister::Outcome Lister::hosting_loop() {
	hosting_started_ms_ = now_ms();
	last_host_.clear();
	last_players_.clear();
	// Registered (retail state 6): full Host list at once, each roster slot
	// announced, first POST heartbeat.
	send_host_update(/*full=*/true);
	for (const auto &p : roster()) {
		logf("[host] ClientHostPlayerAdded slot=%d name='%s'", p.slot, p.player_name.c_str());
		send_nw(session_->build_host_player_added(p), "initial roster");
	}
	send_status_blob();
	const uint32_t refresh_ms = static_cast<uint32_t>(opt_.refresh_s * 1000.0);
	uint32_t last_refresh = now_ms();
	uint32_t last_poll = now_ms();
	while (!should_stop()) {
		pump(50);
		if (session_lost_) return Outcome::Lost;
		if (now_ms() - last_poll >= static_cast<uint32_t>(opt_.json_poll_ms)) {
			last_poll = now_ms();
			const auto before = roster();
			if (load_listing_now(false)) sync_roster(before, roster());
		}
		if (now_ms() - last_refresh >= refresh_ms) {
			last_refresh = now_ms();
			load_listing_now(false);
			pcid_ring_.advance(tick_count(), static_cast<int>(rng_() & 0x7FFF));
			send_host_update(/*full=*/false);
			send_status_blob();
		}
	}
	return Outcome::Stopped;
}

void Lister::stop_hosting_and_goodbye() {
	if (!session_) return;
	if (session_->is_verified() || session_->host_state() == ClientSession::HostState::Established) {
		if (session_->host_state() == ClientSession::HostState::Established ||
		    session_->host_state() == ClientSession::HostState::Requested) {
			logf("[host] ClientStopHosting");
			send_nw(session_->build_stop_hosting(), "stop");
			pump(300); // let the server ack it
		}
		logf("[session] ClientGoodBye");
		send_nw(session_->build_goodbye(), "goodbye");
	}
	session_.reset();
}

Lister::Outcome Lister::session_once() {
	ci_ = static_cast<uint32_t>(rng_());
	ck_ = static_cast<uint32_t>(rng_());
	logged_in_ = false;
	host_key_.clear();
	jar_ = opennova::CookieJar{};
	if (!gate_sock_.open("0.0.0.0", 0) || !nw_sock_.open(opt_.bind_ip, opt_.bind_port)) {
		logf("[net] socket bind failed");
		return Outcome::Fatal;
	}
	logf("[net] gate socket :%u, session socket %s:%u", gate_sock_.local_port(), opt_.bind_ip.c_str(),
	     nw_sock_.local_port());
	Outcome out = Outcome::Lost;
	do {
		if (!gate_probe()) break;
		if (!handshake()) break;
		if (want_login_) {
			if (!http_login_and_hostkey()) {
				out = Outcome::Fatal;
				break;
			}
		} else {
			logf("[http] account login skipped (--login %s%s)", opt_.login.c_str(),
			     creds_.present() ? "" : ", no credentials");
		}
		if (!host_request()) break;
		logf("[host] REGISTERED: listing is live (refresh every %.2f s, update mode %s)", opt_.refresh_s,
		     opt_.update_mode.c_str());
		out = hosting_loop();
	} while (false);
	if (should_stop()) out = Outcome::Stopped;
	stop_hosting_and_goodbye();
	gate_sock_.close();
	nw_sock_.close();
	return out;
}

void Lister::dry_run() {
	logf("[dry-run] nothing is sent; statements are built exactly as they would be");
	ci_ = 0x11111111u;
	ck_ = 0x22222222u;
	gate_.lobby_name = "jop_2_consumer";
	app_id_ = opennova::make_session_app_id(tick_count(), 1234);
	pcid_ring_.advance(tick_count(), 4321);
	if (want_login_) host_key_ = "HOSTKEY-FROM-NWHOST.DLL";
	auto inner = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::string hex;
	for (uint8_t b : inner) {
		char t[4];
		std::snprintf(t, sizeof(t), "%02X", b);
		hex += t;
	}
	logf("[dry-run] gate probe -> %s:%u: tag '%s' GATEAPI-ciphered = %s (+4B CRC envelope)", opt_.master_host.c_str(),
	     opt_.gate_port, opennova::GATE_PROBE_TAG_JOINTOPS, hex.c_str());
	logf("[dry-run] NWU session: 0x41 ClientHello -> 0x42 ClientAuth (CU: Application, BuildDateAndTime, Debug, "
	     "CountryName, Language, TimeZoneBias, GateTag, MetTag, UdpCode1, UdpCode2, MaxPacketSize) -> "
	     "0x43 ClientConnected -> 0x43 ClientRequestVerifyResult");
	std::vector<ClientVar> cookie;
	for (const auto &kv : identity_vars()) cookie.push_back({0, kv.first, kv.second});
	NapiMessage verify;
	verify.name = "ClientRequestVerifyResult";
	verify.fields.push_back({"SessIdString", {}});
	verify.children.push_back(opennova::make_client_var_list("Cookie", cookie));
	auto show = [](const char *label, const NapiMessage &m) {
		std::string t;
		dump_tree(m, 1, t, "");
		logf("[dry-run] %s: %s\n%s", label, statement_summary(m).c_str(), t.c_str());
	};
	show("verify", verify);
	if (want_login_) {
		logf("[dry-run] HTTP: GET <startupurl> (nwprepare.dll) -> GET NWStart.dll -> POST NWLogin.dll (13 fields, "
		     "EPASK-encrypted) -> GET NWLogin.dll poll -> GET jop_2_host1.htm -> NWHost.dll -> NWHost.dll -> HOSTKEY");
		logf("[dry-run] after login the Cookie var-list is the browser cookie jar (+ locale overlay)");
	}
	const auto reg = current_reg();
	show("host request", opennova::make_host_request(reg, cookie, 0));
	hosting_started_ms_ = now_ms();
	const auto host = opennova::make_host_var_list(reg, listing_.text, true);
	const auto players = opennova::make_player_list(roster());
	show("host update (full)", opennova::make_client_host_update(host, players));
	for (const auto &p : roster())
		show("player added", opennova::make_client_host_player_added(p.slot, p.player_name, p.ip_and_port, p.pcid,
		                                                              p.team, p.type));
	if (!roster().empty()) show("player removed", opennova::make_client_host_player_removed(roster().front().slot));
	const auto blob = opennova::make_host_status_blob(reg, listing_.text, roster());
	logf("[dry-run] status blob -> POSTIPADDRESS:POSTIPPORT every %.2f s:\n  %s", opt_.refresh_s,
	     printable(opennova::lobby_update_build(blob)).c_str());
	show("stop hosting", opennova::make_client_stop_hosting());
	logf("[dry-run] then 0x46 ClientGoodBye");
}

int Lister::run() {
	run_started_ms_ = now_ms();
	if (!load_listing_now(true)) return 2;
	if (!opt_.credentials_path.empty()) {
		std::string err;
		if (!load_credentials(opt_.credentials_path, creds_, err)) {
			logf("[creds] %s", err.c_str());
			return 2;
		}
		log_add_secret(creds_.user);
		log_add_secret(creds_.pass);
		logf("[creds] loaded: user %s, password %s", creds_.user.empty() ? "MISSING" : "present (masked)",
		     creds_.pass.empty() ? "MISSING" : "present (masked)");
	}
	want_login_ = opt_.login == "always" || (opt_.login == "auto" && creds_.present());
	if (opt_.login == "always" && !creds_.present()) {
		logf("[creds] --login always needs NOVAWORLD_USER and NOVAWORLD_PASS");
		return 2;
	}
	if (opt_.dry_run) {
		dry_run();
		return 0;
	}
	net_set_allow_public(opt_.allow_public);
	logf("[net] destination policy: %s", opt_.allow_public ? "PUBLIC ALLOWED (--allow-public)" : "loopback only");
	if (!net_startup()) return 3;
	int rc = 0;
	int backoff_s = 15;
	while (!should_stop()) {
		const Outcome o = session_once();
		if (o == Outcome::Stopped) break;
		if (o == Outcome::Fatal || !opt_.reconnect) {
			rc = o == Outcome::Fatal ? 4 : 5;
			break;
		}
		logf("[main] session lost; re-registering in %d s", backoff_s);
		for (int i = 0; i < backoff_s * 10 && !should_stop(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		backoff_s = backoff_s < 240 ? backoff_s * 2 : 240;
	}
	logf("[main] stopped (rc=%d)", rc);
	net_shutdown();
	return rc;
}

} // namespace

int main(int argc, char **argv) {
	Options opt;
	if (!parse_args(argc, argv, opt)) {
		usage();
		return 1;
	}
	if (!opt.log_path.empty()) log_open_file(opt.log_path);
	log_set_verbose(opt.verbose);
	// Route OpenNova's engine diagnostics into our (redacting) log.
	opennova::io::set_log_sink([](opennova::io::LogLevel level, const char *msg) {
		if (level == opennova::io::LogLevel::kDebug && !log_verbose()) return;
		logf("[opennova] %s", msg);
	});
#ifdef _WIN32
	g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#else
	std::signal(SIGINT, on_stop_signal);
	std::signal(SIGTERM, on_stop_signal);
#endif
	logf("nw-lister starting: master %s:%u%s", opt.master_host.c_str(), opt.gate_port, opt.dry_run ? " (dry run)" : "");
	Lister lister(opt);
	const int rc = lister.run();
#ifdef _WIN32
	SetEvent(g_done);
#endif
	return rc;
}
