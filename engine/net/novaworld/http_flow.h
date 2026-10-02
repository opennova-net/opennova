#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <net/novaworld/gsb.h>        // GsbResponse / gsb_parse_response (reused)
#include <net/novaworld/http_login.h> // CookieJar / LoginFormField / build_login_post_body / parse_joi_connection_string

#include <net/novacrypto/epask.h> // EpaskParams / epask_to_string / epask_from_string

// P7 Part 2 — the NovaWorld lobby HTTP orchestration, moved out of the Godot binding so the binding
// becomes a pure HTTPRequest pump (ADR 0010 / .agents/README.md: protocol/framing/sequencing in libs,
// sockets + HTTP transport + signals in Godot). This owns the EPASK login chain, the GSB server-browser
// fetch, and the NWJoin handshake — the URL builders, the cookie jar, and the LoginStep / JoinStep state
// machines — reusing the existing byte helpers (epask, build_login_post_body, the GSB/joi parsers).
// Godot-free: no HTTPRequest, no Variant. The owner ships each HttpRequestSpec over its transport and
// feeds the (code, headers, body) back in.
//
// Faithful port of novaworld_client.cpp's login()/send_login_post()/on_login_request_completed(),
// gsb_url()/request_server_list()/on_gsb_request_completed(), join()/on_join_request_completed(), and
// http_base()/request_headers()/merge_response_cookies(). The URL/.dll/form-field literals are
// byte-preserved [orig: the retail jop_2_* template + NWLogin/NWStart/NWJoin query strings].
namespace opennova {

// --- the transport seam (the owner maps these onto its HTTP client) ---
enum class HttpMethod { Get, Post };

struct HttpRequestSpec {
	HttpMethod method = HttpMethod::Get;
	std::string url;
	std::vector<std::string> headers; // "Header: value" lines (Content-Type, Cookie)
	std::string body;                 // form body (POST); empty otherwise
	bool valid = false;               // false when there is no request to issue
};

// The lobby context the HTTP machines read — filled by the owner from the gate response + the NWU
// session leg (the UDP side stays in the binding/ClientSession; here we only consume its outputs).
struct LobbyHttpContext {
	std::string startup_url;  // gate "UNPS" StartupUrl ("[domainname]/..." templated, or concrete for OpenNova)
	std::string post_ip;      // gate POST host (fallback http_base)
	std::string post_port;    // gate POST port
	std::string web_domain;   // NovaworldWebDomainNameAndPortNumber from the SessionInit ("[domainname]" fill)
	std::string server_nwuid; // ServerAuth nwuid (the empty-NWUID identity cookie substitute)
	std::string locale;       // OS locale ("en_US"); the [CC] region is derived from it (injected, not read here)
	std::vector<std::pair<std::string, std::string>> identity_vars; // the NW-S5 "Cookie" identity set (seeded as cookies)
};

// --- login ---
struct LoginResult {
	enum class Kind { NeedRequest, Succeeded, Failed };
	Kind kind = Kind::Failed;
	HttpRequestSpec request;  // valid when kind == NeedRequest
	std::string nwhandle;     // when Succeeded
	std::string pcid;         // when Succeeded (may be empty)
	std::string reason;       // when Failed
};

// --- join ---
struct JoinResult {
	enum class Kind { NeedRequest, Resolved, Failed };
	Kind kind = Kind::Failed;
	HttpRequestSpec request;  // when NeedRequest
	std::string host_ip;      // when Resolved: the decoded-NK endpoint (NK is the only dial authority)
	uint16_t host_port = 0;   // when Resolved
	// The .joi lobby number. Nonzero means the retail transport dials the
	// LAN-discovered endpoint for this session instead of host_ip/host_port
	// and reports the number as the session var "Lan". 0 when absent.
	// [orig: CNapiGameSession_InitTransportConnection @0x4c9e6c; ConnectOrHost @0x4d5418]
	int ln = 0;               // when Resolved
	// The proxy-join triple, verbatim from the .joi: NI/NP are the proxy
	// node's ip/port and BK its cookie. With all three present (plus the NK
	// relay pair) retail installs the proxy config and sends the 48-byte
	// rendezvous datagram every enumerator tick. Empty when the .joi carries
	// none. [orig: InitTransportConnection @0x4ca051..0x4ca0c7 installs the
	//  six proxy fields; CNapiNPConnection_SendPingPacket @0x61f8c0]
	std::string ni;           // when Resolved
	std::string np;           // when Resolved
	std::string bk;           // when Resolved
	// The game-session APPID join token (decimal) recovered from the .joi CK —
	// the value the NovaWorld host validates (code 9). "0" for LAN / a bare .joi.
	std::string app_id = "0"; // when Resolved
	// The CD identity cookie: every PUB* cookie the login/NWJoin Set-Cookie'd,
	// packed [name\0][value\0], relayed in the C2S 0x00 JOIN. Empty when the jar
	// carries no PUB* cookie (a bare .joi / unauthenticated). Host code 23/24/25.
	std::vector<uint8_t> cd_cookie; // when Resolved
	std::string reason;       // when Failed
};

// The single stateful lobby HTTP flow. One per session; the owner sets the context, then drives login /
// GSB / join. The cookie jar is SHARED across all three (login's NWHANDLE/PCID ride GSB + join).
class LobbyHttpFlow {
public:
	void set_context(LobbyHttpContext ctx) { ctx_ = std::move(ctx); }
	const LobbyHttpContext &context() const { return ctx_; }
	void reset(); // start()/stop() equivalent: clears the jar + both machines (keeps the context)

	// The derived base "http://host[:port]" for the lobby HTTP endpoint (empty until the gate replies).
	// (see godot/src/network/novaworld_client.cpp http_base())
	std::string http_base() const;
	// The concrete login prepare URL with the six gate-markup tokens filled, each matched
	// case-insensitively: [DOMAINNAME] [VER1] [VER2] [CC] [GT] [PRODUCTCODE].
	// [orig: Mission_DeobfuscateDescription @0x4cdaa0 (a misnomer: the URL token substituter)]
	std::string resolve_startup_url() const;

	// --- EPASK login chain ---
	// Begin: returns the prepare GET (NeedRequest) or Failed (no startup_url). [orig: login()]
	LoginResult login(const std::string &username, const std::string &password);
	// Feed a completed login response; advances PREPARE -> (NWSTART) -> POST -> POLL. [orig:
	// on_login_request_completed()]. `transport_ok` is the owner's "the HTTP call itself succeeded"
	// (Godot RESULT_SUCCESS); `code` is the HTTP status (must be 200).
	LoginResult on_login_response(bool transport_ok, int code, const std::vector<std::string> &response_headers,
	                              const std::vector<uint8_t> &body);
	bool login_active() const { return login_step_ != LoginStep::Idle; }

	// --- GSB server browser ---
	// Build the GSB fetch (jop_2.gsb?a=1) with the current cookies; invalid when no base URL yet.
	// [orig: gsb_url() + request_server_list()]
	HttpRequestSpec gsb_request() const;
	// Parse a completed GSB response into `out` (reuses gsb_parse_response). Returns false on a failed
	// fetch / parse. Does NOT merge Set-Cookie (the retail GSB leg doesn't). [orig: on_gsb_request_completed()]
	bool on_gsb_response(bool transport_ok, int code, const std::vector<uint8_t> &body, GsbResponse &out);

	// --- NWJoin handshake ---
	// Begin: returns the first NWJoin GET (NeedRequest) or Failed. [orig: join()]
	JoinResult join(uint32_t rid);
	// Feed a completed join response; FIRST reads NWJOINSESSIONTAG -> the ?rid=&tag= GET, SECOND parses
	// the .joi -> {host, port}. [orig: on_join_request_completed()]
	JoinResult on_join_response(bool transport_ok, int code, const std::vector<std::string> &response_headers,
	                            const std::vector<uint8_t> &body);
	bool join_active() const { return join_step_ != JoinStep::Idle; }

	// Exposed for the owner's cookie diagnostics / tests.
	const CookieJar &cookies() const { return jar_; }
	// Current browser cookies plus the locale/initial identity for UDP statements.
	std::vector<std::pair<std::string, std::string>> session_cookie_vars() const;

private:
	enum class LoginStep { Idle, Prepare, NwStart, Post, Poll };
	enum class JoinStep { Idle, First, Second };

	std::vector<std::string> request_headers(bool form_content_type) const; // Content-Type + Cookie lines
	void merge_response_cookies(const std::vector<std::string> &response_headers);
	void seed_identity_cookies();
	HttpRequestSpec login_post_request();     // build /NWLogin.dll POST (the 13-field form)
	std::string gsb_url() const;
	std::string nwlogin_poll_url() const;     // /NWLogin.dll?tag=<LOGINSESSIONTAG>

	LobbyHttpContext ctx_;
	CookieJar jar_;
	EpaskParams epask_;
	LoginStep login_step_ = LoginStep::Idle;
	int login_poll_count_ = 0;
	std::string login_user_, login_pass_;
	JoinStep join_step_ = JoinStep::Idle;
	uint32_t join_rid_ = 0;
};

} // namespace opennova
