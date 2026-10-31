#include "lister.h"

#include "console_log.h"
#include "destination_policy.h"

#include <base/io/log.h>
#include <base/io/strutil.h>
#include <net/napi/session.h>
#include <net/novacrypto/cdkey.h>
#include <net/novaworld/connect_or_host.h>
#include <net/novaworld/lobby_identity.h>
#include <runtime/world/tick_count_clock.h> // frame_tick_count_ms (a GET's nocachefix tick)

#include <chrono>
#include <cstring>
#include <optional>
#include <utility>

namespace opennova::serve {

using io::LogLevel;

namespace {

// Whether a web page that failed the flow is the service's answer: a page it served (200, a
// content refusal) or a client error it chose (4xx). A request that never reached it, a page its
// web host failed (5xx) or one moved elsewhere (3xx) is a fault, which a second listing retries.
bool answered_by_service(const net::HttpReply &reply) {
	return reply.transport_ok && (reply.code == 200 || (reply.code >= 400 && reply.code < 500));
}

// The gloss on the host request's NWEC tags: what each says in plain words, and the service's
// MsgCode name behind it [orig: the NWUSERVERMSGCODE_ name table, {code, name} pairs from
// @0x7CB960; ConnectOrHost's MsgCode switch @0x4D5207 picks the tag] (null for the listing's own
// fault). The plain words are a second listing's reported reason (Lister::failure).
struct HostTagGloss {
	const char *text;
	const char *msg_code;
};
HostTagGloss host_tag_gloss(const std::string &tag) {
	if (tag == "NWEC51")
		return { "the request was not queued", nullptr };
	if (tag == "NWEC52")
		return { "the service did not answer within 60 s", nullptr };
	if (tag == "NWEC53")
		return { "the service wants a server name", "MISSINGSERVERNAME" };
	if (tag == "NWEC54")
		return { "the service rejected the hosting options", "REJECTEDHOSTINGINFO" };
	if (tag == "NWEC55")
		return { "the service rejected the server name", "BADSERVERNAME" };
	if (tag == "NWEC56")
		return { "the service counts too many servers running", "TOOMANYSERVERSRUNNING" };
	if (tag == "NWEC57")
		return { "the service did not accept the server name", "SERVERNAMENOTACCEPTED" };
	if (tag == "NWEC60")
		return { "the service did not accept the server message", "MSGNOTACCEPTED" };
	return { "another refusal, or the session left before the service answered", nullptr };
}

// The operator's gloss on a host request's tag: the plain words with the MsgCode name.
std::string host_tag_meaning(const std::string &tag) {
	const HostTagGloss gloss = host_tag_gloss(tag);
	return gloss.msg_code != nullptr ? std::string(gloss.text) + " (" + gloss.msg_code + ")"
									 : std::string(gloss.text);
}

// An NWEC tag ("NWEC57"), as the connect and the host request report one: the tag a second
// listing's state carries beside its reason (a session's other failure texts are none).
bool is_nwec_tag(const std::string &text) {
	if (text.size() < 5 || text.size() > 8 || text.compare(0, 4, "NWEC") != 0)
		return false;
	for (std::size_t i = 4; i < text.size(); ++i)
		if (text[i] < '0' || text[i] > '9')
			return false;
	return true;
}

// The hosting leg's own refusals ahead of its request (host_leg_refusal's tags), in plain words.
const char *host_leg_meaning(const char *tag) {
	if (std::strcmp(tag, NWEC_SESSION_NOT_SET_UP) == 0)
		return "the session was not set up";
	if (std::strcmp(tag, NWEC_WRONG_SESSION_STATE) == 0)
		return "the session was not in a state to host";
	if (std::strcmp(tag, NWEC_HOST_GATE_NOT_OK) == 0)
		return "the gate named no lobby to host in";
	return "the session could not host";
}

// The operator's gloss on the connect's NWEC tags this driver reports (the game shows menutxt's
// text; these lines are the lister's own).
const char *connect_tag_meaning(const std::string &tag) {
	if (tag == "NWEC15")
		return "the gate did not answer";
	if (tag == "NWEC25")
		return "the gate named no session server and lacks its POST address or port";
	if (tag == "NWEC40")
		return "the connect poll timed out";
	if (tag == "NWEC42")
		return "no session server answered within 20 s";
	if (tag == "NWEC43")
		return "the session server refused the join";
	if (tag == "NWEC45")
		return "the session closed before its verify";
	if (tag == "NWEC46")
		return "the session was not verified within 20 s";
	if (tag == "NWEC48")
		return "the session was not verified";
	return nullptr;
}

constexpr int kHttpTimeoutMs = 15000;

std::string ip4(const std::array<uint8_t, 4> &ip) {
	return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
	       std::to_string(ip[3]);
}

// A stable machine id for the stand-in NWPSSK / NWUSID where retail's inputs do not read.
std::string stable_identity() {
	const std::string host = net::local_host_name();
	return host.empty() ? std::string("opennova-serve") : "opennova-serve|" + host;
}

// The strings the Host list's Region, TimeLeft, Y/N and TimeOfDay tokens resolve to until the
// embedder hands the role its mounted gametext's (opennova-serve's make_host_lobby_text): retail's
// English Gametext.bin (NovaWorld STRNOVA07..12, the TimeOfDay section).
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — STRNOVA07/08/09 @0x4fea56.., STRNOVA10 @0x4fed8d,
//  STRNOVA11/12 @0x4fec2c.., the TimeOfDay quintet @0x4ff09b..0x4ff138]
HostLobbyText english_lobby_text() {
	HostLobbyText text;
	text.yes = "Y";
	text.no = "N";
	text.no_time_limit = "NA";
	text.region = {"Jungle", "Desert", "Snow"};
	text.time_of_day = {"Unknown", "Dawn", "Day", "Dusk", "Night"};
	return text;
}

} // namespace

LobbyMachineTokens machine_tokens(const FingerprintSerialMix &mix) {
	RetailMachineInputs machine;
	if (read_retail_machine_inputs(machine))
		return make_retail_machine_tokens(machine, mix);
	return fallback_machine_tokens(stable_identity(), mix);
}

Lister::Lister(ListerOptions options, ListingSource &source, IDatagramSocket *session_socket) :
		options_(std::move(options)),
		log_prefix_(options_.label.empty() ? std::string() : options_.label + " "),
		source_(source),
		shared_session_socket_(session_socket),
		lobby_(
				[this]() {
					NwuLobbySession::Hooks hooks;
					hooks.on_gate_response = [this](const GateResponse &gate) {
						std::string addresses;
						for (const std::string &address : gate.udp_novaworld)
							addresses += (addresses.empty() ? "" : ", ") + address;
						io::logf(LogLevel::kInfo, "%s[gate] lobby %s, NovaWorld %s", who(),
								gate.lobby_name.c_str(), addresses.c_str());
					};
					hooks.cookie_vars = [this]() { return cookie_vars(); };
					hooks.on_fatal = [this](const std::string &message) {
						const char *meaning = connect_tag_meaning(message);
						io::logf(LogLevel::kError, "%s[session] failed: %s%s%s%s", who(),
								message.c_str(), meaning != nullptr ? " (" : "",
								meaning != nullptr ? meaning : "", meaning != nullptr ? ")" : "");
						// A refused join (CR 0) keeps the 0x82's failure fields on the connection.
						if (const ClientSession *session = lobby_.session();
								session != nullptr && message == "NWEC43")
							io::logf(LogLevel::kError,
									"%s[session] the 0x82 refused: JFC %u JFP %u JFS \"%s\"", who(),
									session->join_fail_code(), session->join_fail_param(),
									session->join_fail_string().c_str());
						// The session server's refusal is its decision; the other tags are a
						// gate or a session that did not answer.
						end(kExitFailed, /*refused=*/message == "NWEC43",
								meaning != nullptr ? meaning : "the NovaWorld session failed",
								is_nwec_tag(message) ? message : std::string());
					};
					hooks.on_soft_error = [this](const std::string &message) {
						io::logf(LogLevel::kDebug, "%s[gate] %s", who(), message.c_str());
					};
					// A gate naming no UDPNOVAWORLD leaves no NWU session to register the host on.
					hooks.on_sessionless = [this]() {
						io::logf(LogLevel::kError,
								"%s[gate] the gate named no NovaWorld session server (UDPNOVAWORLD)",
								who());
						end(kExitFailed, /*refused=*/false,
								"the gate named no NovaWorld session server");
					};
					// The gate and connect lines to the server's _connectlog.txt.
					hooks.connect_log_on = [this]() {
						return options_.connect_log != nullptr && options_.connect_log->enabled();
					};
					hooks.connect_log = [this](const std::string &text) {
						if (options_.connect_log != nullptr) options_.connect_log->line(text);
					};
					return hooks;
				}(),
				[this]() {
					NwuLobbySession::Environment env; // random_u32 unset: the OS CSPRNG
					env.resolve_ipv4 = [this](const std::string &host, PeerAddr &out) {
						net::Endpoint endpoint;
						if (!resolve_destination(
									host, endpoint, options_.allow_public, "NovaWorld host"))
							return false;
						out.ip = net::NetDatagramSocket::to_peer(endpoint).ip;
						return true;
					};
					env.gate_local_port = [this]() { return gate_port_; };
					// The command-line host's gate wait (CNapiGateManager_WaitForGateResponse).
					env.command_line_host = true;
					return env;
				}()),
		role_(lobby_, [this]() {
			NwuHostRole::Hooks hooks;
			hooks.on_hosting = [this]() { on_listed(); };
			hooks.inout_line = [this](const std::string &text) {
				if (options_.server_logs != nullptr)
					inmatch::server_logs_inout_line(*options_.server_logs, text);
			};
			hooks.on_failed = [this](const std::string &tag) {
				io::logf(LogLevel::kError, "%s[host] the host request failed: %s (%s)", who(),
						tag.c_str(), host_tag_meaning(tag).c_str());
				source_.on_host_result(false, tag);
				// NWEC53..58 and NWEC60 are the service's MsgCode refusals (too many servers, the
				// name not accepted, ...; NWEC58 one outside the table). NWEC51 (not queued), NWEC52
				// (no reply in 60 s) and NWEC58 over no word (0: the session left state 5/6 with
				// no answer, a reap or a reconnect) are faults, not its decision.
				const ClientSession *session = lobby_.session();
				const bool no_word = session == nullptr || session->server_msg_code() == 0;
				end(kExitFailed, /*refused=*/tag != "NWEC51" && tag != "NWEC52" &&
								!(tag == "NWEC58" && no_word),
						host_tag_gloss(tag).text, is_nwec_tag(tag) ? tag : std::string());
			};
			// ServerStopHosting drops the session to verified and sends nothing; the session
			// stays up, pumped, until the match's NovaWorld exit (12) over the dropped word
			// reaches the router, whose network-type reset sends the goodbye burst with no
			// ClientStopHosting (the embedder's stop at that exit). The lister finishes then,
			// with the service's word as its outcome (D-NET-993).
			// [orig: CNapiGameSession_HandleServerMessage @0x4D1C50 - SetState(4) @0x4D1D29,
			//  the word 1 @0x4D1D4F, no send; Game_ProcessMainFrame @0x5263F0 - the 62-frame
			//  check @0x52654F..0x52657C; PostMenu_RouteMissionExit @0x568460 - `cmp eax, 0Ch`
			//  @0x56854F / jz @0x568552 -> loc_5686CB, SetNetworkType(0) (the call @0x5686E1);
			//  CNapiNetwork_SetNetworkType @0x4C4A50 - ResetToDisconnected (the call @0x4C4A7D);
			//  CNapiGameSession_ResetToDisconnected @0x4D0890 - CNapiNPConnection_RequestDisconnect
			//  @0x61E0F0 (the call @0x4D08A1)]
			hooks.on_stopped = [this](const std::string &key) {
				io::logf(LogLevel::kError, "%s[host] the service stopped the hosting (%s)", who(),
						key.c_str());
				source_.on_host_result(false, key);
				stopped_by_service_ = true;
			};
			hooks.on_command = [this](const ServerCommand &command) {
				source_.on_command(command);
			};
			hooks.on_player_enter_result = [this](const ClientSession::PlayerEnterResult &result) {
				source_.on_player_enter_result(result);
			};
			return hooks;
		}()) {
	role_.set_lobby_text(english_lobby_text());
}

Lister::~Lister() {
	source_.stop();
	if (http_.valid()) http_.wait();
}

bool Lister::start() {
	if (!source_.start(exit_code_))
		return false;
	// The gate worker's socket binds on every interface over the block's mpgateserverlocalport
	// quad, the first free port of its scan (D-NET-978); the session socket is the host's.
	// [orig: CNapiGateManager_WaitForConnect @0x4CE7D2 (the bind address 0), @0x4CE7DE..0x4CE810
	//  (the quad into the context); CNapiGateManager_CreateAndBindSocket @0x633770 - the address
	//  and the quad into the bind @0x6337D4..0x633824, NapiUdpSocket_CreateAndBind @0x633830]
	for (const uint16_t port : options_.gate_ports) {
		gate_udp_ = net::ScopedSocket(net::udp_bind(port, &gate_port_, 0));
		if (gate_udp_.is_valid()) break;
	}
	// A probe whose bind finds no port fails its start: the gate wait writes its CONFIGURING and
	// STARTING lines ahead of the connect machine, then logs the manager's state, which the
	// machine left at 2 (its fall back to -6 is outside the state setter's -5..3); no probe
	// leaves (NwuLobbySession::gate_start_failed).
	// [orig: CNapiGateManager_WaitForConnect @0x4CE816..0x4CE84D ("GATE CONFIGURING GATE
	//  MANAGER."), @0x4CE855..0x4CE88C ("GATE STARTING GATE SERVICE."), the machine @0x4CE89D,
	//  the failure @0x4CE8A4, "%s : GATE GATE SERVICE START FAILED [%ld]." @0x4CE8D2 of the state
	//  +0x20 @0x4CE8C2, NapiFile_AppendLine @0x4CE8E7; CNapiGateManager_ProcessConnectStateMachine
	//  @0x633E3C (state 2), the bind @0x633E43, SetState(-6) @0x633E64]
	if (!gate_udp_.is_valid()) (void)lobby_.gate_start_failed();
	if (shared_session_socket_ == nullptr) session_udp_ = net::ScopedSocket(net::udp_bind(0));
	if (!gate_udp_.is_valid() || (shared_session_socket_ == nullptr && !session_udp_.is_valid())) {
		io::logf(LogLevel::kError, "%s[net] cannot bind the UDP sockets", who());
		exit_code_ = kExitNetwork;
		return false;
	}
	gate_socket_ = std::make_unique<net::NetDatagramSocket>(gate_udp_.get());
	IDatagramSocket *session = shared_session_socket_;
	if (session == nullptr) {
		session_socket_ = std::make_unique<net::NetDatagramSocket>(session_udp_.get());
		session = session_socket_.get();
	}
	lobby_.set_protocol(options_.protocol, options_.gate_tag);
	lobby_.open(*gate_socket_, *session);
	lobby_.probe(options_.master_host, options_.master_gate_port);
	io::logf(LogLevel::kInfo, "%s[gate] probing %s:%u as %s (%s)", who(),
			options_.master_host.c_str(), options_.master_gate_port, options_.gate_tag.c_str(),
			options_.allow_public ? "public destinations allowed"
								  : "NovaLogic's NovaWorld refused");
	return true;
}

bool Lister::tick(uint32_t now_ms) {
	if (phase_ == Phase::Done) return false;
	// The session's clock starts with the lister: the gate probe went out at 0 (start), so the
	// embedder's wall clock reads relative to the first pass, else a wall clock past the probe's
	// deadline ends the probe before its reply can land.
	if (!clock_started_) {
		clock_started_ = true;
		clock_origin_ = now_ms;
	}
	now_ms -= clock_origin_;
	now_ms_ = now_ms;
	lobby_.tick(now_ms);
	if (ClientSession *session = lobby_.session()) {
		for (const ClientSession::Notice &notice : session->take_notices()) {
			if (role_.handle_notice(notice)) continue;
			if (notice.kind == ClientSession::Notice::Kind::LeaveNovaWorld) {
				io::logf(LogLevel::kError, "%s[session] the service ended the session (MsgCode %d)",
				         who(), notice.fields.msg_code);
				end(kExitStoppedByService, /*refused=*/false, "the service ended the session");
			}
		}
	}
	role_.tick(now_ms);
	if (!ending_ && phase_ == Phase::Connecting && lobby_.session_verified()) on_verified();
	if (!ending_ && http_.valid() && http_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		on_http_reply(http_.get());
	}
	if (!ending_)
		refresh_listing(false);
	if (ending_) teardown(/*stop_hosting=*/true);
	return phase_ != Phase::Done;
}

void Lister::publish_server_info(int32_t session_round_count) {
	if (phase_ != Phase::Hosting || ending_) return;
	refresh_listing(true);
	role_.publish_mission_start(session_round_count);
}

void Lister::stop() {
	if (phase_ == Phase::Done) return;
	io::logf(LogLevel::kInfo, "%s[main] stopping", who());
	end(kExitStopped);
	teardown(/*stop_hosting=*/true);
}

void Lister::disconnect() {
	if (phase_ == Phase::Done) return;
	io::logf(LogLevel::kInfo, "%s[main] disconnecting", who());
	end(kExitStopped);
	teardown(/*stop_hosting=*/false);
}

// ServerHostResult success: the listing is up. A fresh host's result publishes nothing: its first
// full Host list, HostKey included, goes with its mission start's update, which the embedder runs
// at each one (publish_server_info, D-NET-465).
// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d5c52..0x4d5c58]
void Lister::on_listed() {
	phase_ = Phase::Hosting;
	// The host request's wait ended: the menu's next frame opens the HOSTKEY page's refresh
	// (engine: LobbyHttpFlow::on_host_response's Resolved request carries the witness). A failed
	// request ends the lister, so only the listed one fetches it.
	if (host_page_.valid && !http_.valid()) {
		ship(host_page_);
		host_page_in_flight_ = true;
	}
	host_page_ = HttpRequestSpec{};
	io::logf(LogLevel::kInfo, "%s[host] listed: '%s', %zu player(s)", who(),
			registration().server_name.c_str(), role_.roster().size());
	source_.on_host_result(true, std::string());
}

// Verified: the account login first when there is one (the hosting page needs it), else the host
// request straight away.
void Lister::on_verified() {
	io::logf(LogLevel::kInfo, "%s[session] verified at %s", who(),
			peer_addr_to_string(lobby_.nw_peer()).c_str());
	if (!options_.credentials.present()) {
		io::logf(LogLevel::kInfo, "%s[http] no account: hosting without a HOSTKEY", who());
		request_host();
		return;
	}
	sync_flow_context();
	const LoginResult first = flow_.login(options_.credentials.user, options_.credentials.pass);
	if (first.kind != LoginResult::Kind::NeedRequest) {
		io::logf(LogLevel::kError, "%s[http] login failed: %s", who(), first.reason.c_str());
		// Nothing a later try would change.
		end(kExitFailed, /*refused=*/true, "the sign-in could not start");
		return;
	}
	phase_ = Phase::LoggingIn;
	ship(first.request);
}

void Lister::on_http_reply(const net::HttpReply &reply) {
	if (!reply.transport_ok) io::logf(LogLevel::kWarn, "%s[http] %s", who(), reply.error.c_str());
	// The HOSTKEY page's refresh (on_listed): its answer settles the flow's host leg.
	if (host_page_in_flight_) {
		host_page_in_flight_ = false;
		flow_.on_host_response(reply.transport_ok, reply.code, reply.headers, reply.body);
		return;
	}
	if (phase_ == Phase::LoggingIn) {
		const LoginResult r = flow_.on_login_response(reply.transport_ok, reply.code, reply.headers, reply.body);
		if (r.kind == LoginResult::Kind::NeedRequest) {
			ship(r.request);
			return;
		}
		if (r.kind == LoginResult::Kind::Failed) {
			io::logf(LogLevel::kError, "%s[http] login failed: %s", who(), r.reason.c_str());
			// A login the service answered and refused is its decision; a request that never
			// reached it, or a page its web host failed (5xx) or moved (3xx), is not.
			const bool answered = answered_by_service(reply);
			end(kExitFailed, /*refused=*/answered,
					answered ? "the service refused the sign-in"
							 : "the sign-in page did not answer");
			return;
		}
		add_log_secret(r.nwhandle);
		io::logf(LogLevel::kInfo, "%s[http] logged in", who());
		sync_flow_context();
		// The Host's jar entries carry the registration's Exp (g_ExpansionName).
		const HostKeyResult first = flow_.host(registration().expansion);
		if (first.kind != HostKeyResult::Kind::NeedRequest) {
			io::logf(LogLevel::kError, "%s[http] the hosting page failed: %s", who(),
					first.reason.c_str());
			end(kExitFailed, /*refused=*/true, "the hosting page could not start");
			return;
		}
		phase_ = Phase::FetchingHostKey;
		ship(first.request);
		return;
	}
	if (phase_ == Phase::FetchingHostKey) {
		const HostKeyResult r = flow_.on_host_response(reply.transport_ok, reply.code, reply.headers, reply.body);
		if (r.kind == HostKeyResult::Kind::NeedRequest) {
			ship(r.request);
			return;
		}
		if (r.kind == HostKeyResult::Kind::Failed) {
			io::logf(LogLevel::kError, "%s[http] the hosting page failed: %s", who(),
					r.reason.c_str());
			const bool answered = answered_by_service(reply);
			end(kExitFailed, /*refused=*/answered,
					answered ? "the service refused the hosting page"
							 : "the hosting page did not answer");
			return;
		}
		host_key_ = r.host_key;
		io::logf(LogLevel::kInfo, "%s[http] HOSTKEY %s", who(), mask_value(host_key_, 6).c_str());
		// The HOSTKEY page's refresh goes out once the host request's wait ends (on_listed).
		host_page_ = r.request;
		request_host();
	}
}

// One HTTP exchange on a worker thread, so the session keeps pumping while it runs (the game's
// HTTPRequest nodes run beside its frame the same way).
void Lister::ship(const HttpRequestSpec &spec) {
	// The session tags the last reply set ride the next URL: mask them before it is logged.
	for (const char *tag : {"LOGINSESSIONTAG", "NWJOINSESSIONTAG", "NWHANDLE"}) {
		if (const std::string *value = flow_.cookies().find(tag)) add_log_secret(*value);
	}
	const bool allow_public = options_.allow_public;
	const char *method = spec.method == HttpMethod::Post ? "POST" : "GET";
	io::logf(LogLevel::kDebug, "%s[http] %s %s", who(), method, spec.url.c_str());
	http_ = std::async(std::launch::async, [spec, method, allow_public]() {
		// One resolve per request: the connect uses the address the nocachefix pair reads.
		// [orig: CNapiWinsock_InitFromUrl @0x658DA0 - CNapiWinsock_ResolveHostAsync called
		//  @0x658E4B into the request's +0x30 word, which CUIBrowser_SendHTTPRequest @0x658840
		//  reads for the pair @0x6588A3 and the connect reads @0x6597F3]
		std::optional<net::Endpoint> resolved;
		const auto resolve = [allow_public, &resolved](const std::string &host, net::Endpoint &out) {
			if (!resolved) {
				net::Endpoint server{};
				if (!resolve_destination(host, server, allow_public, "web host")) return false;
				resolved = server;
			}
			out.ip = resolved->ip;
			return true;
		};
		// A menu-browser GET takes its nocachefix pair once its server resolved: the tick at the
		// send and that server's address (browser_cache_bust_url, D-NET-1251).
		std::string url = spec.url;
		if (spec.cache_bust) {
			const std::size_t scheme = url.find("://");
			const std::size_t host_at = scheme == std::string::npos ? 0 : scheme + 3;
			const std::size_t host_end = url.find_first_of(":/", host_at);
			const std::string host = url.substr(host_at,
					host_end == std::string::npos ? std::string::npos : host_end - host_at);
			net::Endpoint server{};
			if (!resolve(host, server)) {
				net::HttpReply refused;
				refused.error = "cannot reach " + host;
				return refused;
			}
			url = browser_cache_bust_url(url, world::frame_tick_count_ms(), server.ip);
		}
		// The menu browser's Host line leads the flow's lines: the URL's host with no port
		// (browser_request_host, D-NET-1329), where the transport would write the authority.
		std::vector<std::string> headers{"Host: " + browser_request_host(spec.url)};
		headers.insert(headers.end(), spec.headers.begin(), spec.headers.end());
		return net::http_exchange(method, url, headers, spec.body, kHttpTimeoutMs, resolve);
	});
}

// The hosting leg of ConnectOrHost: its session and gate checks, then the request; the roster
// follows it (the request clears the role's), and its players ride the first full PlayerList
// (ClientHostPlayerAdded goes only in state 6).
void Lister::request_host() {
	const ClientSession *session = lobby_.session();
	const GateResponse &gate = lobby_.gate_response();
	const char *refusal = host_leg_refusal(session != nullptr, session ? session->session_flags() : 0u,
	                                       true, gate.lobby_name);
	if (refusal != nullptr) {
		io::logf(LogLevel::kError, "%s[host] cannot host: %s", who(), refusal);
		// Only the gate's reply naming no lobby to host in is the service's word; a session not
		// set up or out of state (a reconnect during the web leg) is a fault.
		end(kExitFailed, /*refused=*/std::strcmp(refusal, NWEC_HOST_GATE_NOT_OK) == 0,
				host_leg_meaning(refusal), refusal);
		return;
	}
	phase_ = Phase::Requesting;
	refresh_listing(true);
	if (!role_.request(registration(), options_.taken_app_id)) return; // on_failed finished it
	sync_roster();
	io::logf(LogLevel::kInfo, "%s[host] ClientHostRequest for '%s' on %s", who(),
	         registration().server_name.c_str(), gate.lobby_name.c_str());
}

// The flow reads the gate response and the session's SessionInit; both are fixed once the
// session is verified, so re-syncing before each leg cannot move the web base mid-login.
void Lister::sync_flow_context() {
	const GateResponse &gate = lobby_.gate_response();
	LobbyHttpContext ctx;
	ctx.protocol = options_.protocol;
	ctx.gate_tag = options_.gate_tag;
	ctx.startup_url = gate.startup_url;
	ctx.post_ip = ip4(gate.post_ip);
	ctx.post_port = std::to_string(gate.post_port);
	if (const ClientSession *session = lobby_.session()) {
		ctx.web_domain = session->server_web_domain();
		ctx.server_nwuid = session->server_nwuid();
	}
	ctx.identity_vars = identity_vars_;
	// The jar's gate-reply fields: the junction pair and the locale three's switch (D-NET-1253).
	ctx.use_junction = lobby_.use_junction();
	ctx.extended_metrics = options_.extended_metrics;
	flow_.set_context(std::move(ctx));
}

// The verify Cookie: the machine's identity set, then the browser jar once the login filled it.
std::vector<std::pair<std::string, std::string>> Lister::cookie_vars() {
	if (identity_vars_.empty()) {
		read_locale_identity(identity_);
		// NWPSSK under the served protocol's serial term (its session part is complete).
		const LobbyMachineTokens tokens = machine_tokens(*options_.protocol->nwpssk_mix);
		identity_.nwpssk = tokens.nwpssk;
		identity_.nwusid = tokens.nwusid;
		identity_.nwhwi = options_.hardware_identity;
		// The CD-key ids of the record the server's boot read filled, for a protocol that keeps one
		// (LobbyIdentityParams::nwcdkiid carries the witness).
		if (options_.protocol->cd_key != nullptr) {
			identity_.nwcdkiid = cdkey::process_key_info().keys.cdkiid;
			identity_.nwcdkiidexp1 = cdkey::process_key_info().keys.exp1;
		}
		identity_vars_ = make_lobby_identity_vars(identity_);
	}
	sync_flow_context();
	return flow_.session_cookie_vars();
}

// The source's changes: the columns ride the host role's next refresh, the roster goes out at once.
void Lister::refresh_listing(bool force) {
	if (phase_ != Phase::Requesting && phase_ != Phase::Hosting)
		return;
	if (!source_.refresh(now_ms_, force))
		return;
	role_.set_columns(registration());
	sync_roster();
}

// The source's columns plus what the session owns: the gate's LobbyName, the HOSTKEY, the leg's
// MaxPlayers clamp and the machine's locale, which the host role sends as the CountryName / Lang /
// TZB trio when the gate's METEXT asks for it (D-NET-347).
HostRegistration Lister::registration() const {
	HostRegistration r = source_.registration();
	r.lobby_name = lobby_.gate_response().lobby_name;
	r.host_key = host_key_;
	r.max_players = host_leg_max_players(r.max_players, !r.listen_host, *options_.game_title);
	r.country_name = identity_.country;
	r.language = identity_.language;
	r.tz_bias = strutil::parse_int(identity_.tz_bias).value_or(0);
	return r;
}

// ClientHostPlayerRemoved for every slot that left or now holds another player, then
// ClientHostPlayerAdded for every slot that arrived or changed player (Server_HandlePlayerDisconnect /
// Server_PlayerAdd per slot); a kept player's team or type changes its vars alone, as a team
// change or a loadout does (NwuHostRole::sync_roster).
void Lister::sync_roster() {
	role_.sync_roster(source_.wanted_roster(role_.roster()));
}

void Lister::end(int code, bool refused, std::string why, std::string tag) {
	if (ending_ || phase_ == Phase::Done) return;
	ending_ = true;
	refused_ = refused;
	failure_ = std::move(why);
	failure_tag_ = std::move(tag);
	exit_code_ = stopped_by_service_ ? kExitStoppedByService : code;
}

// Close the session: with `stop_hosting` (stop) the ClientStopHosting statement leaves on one
// send pump first and the connection's disconnect burst follows (NovaWorldClient::stop's
// teardown); without it (disconnect) the burst alone, as a process exit's atexit teardown sends
// it. The burst is RequestDisconnect's: a connection the server accepted (state 5, verifying or
// verified) sends it, one still in its Hello / Auth legs nothing (D-NET-974)
// [orig: CNapiNPConnection_RequestDisconnect @0x61E0F0 - state 5 -> SetState(6)
//  @0x61E0F3..0x61E0FA; CNapiNPConnection_TeardownActiveConnection's state-5 burst
//  @0x62549E..0x6254D3].
// The NovaWorld leg goes first: a console close leaves the process a few seconds.
void Lister::teardown(bool stop_hosting) {
	if (phase_ == Phase::Done) return;
	phase_ = Phase::Done;
	if (lobby_.is_open()) {
		// A refused or failed host request left the role idle: no ClientStopHosting, the burst
		// alone, as retail's ResetToDisconnected sends it [orig: Game_HostMultiplayerSession
		//  @0x4A65A0 - CNapiGameSession_ResetToDisconnected @0x4A6718 / @0x4A674F].
		const bool stop_sent = stop_hosting && role_.phase() != NwuHostRole::Phase::Idle &&
				lobby_.session() != nullptr;
		if (stop_hosting) {
			role_.stop();
			lobby_.flush();
		}
		if (ClientSession *session = lobby_.session(); session != nullptr) {
			std::vector<std::vector<uint8_t>> out;
			session->request_disconnect(out);
			for (const std::vector<uint8_t> &dg : out) lobby_.send(dg);
			if (!out.empty())
				io::logf(LogLevel::kInfo, "%s[session] %s", who(),
				         stop_sent ? "ClientStopHosting and the goodbye sent" : "the goodbye sent");
		}
		lobby_.close();
	}
	source_.stop();
}

} // namespace opennova::serve
