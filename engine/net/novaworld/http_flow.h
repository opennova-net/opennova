#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <net/novaworld/gsb.h>        // GsbResponse / gsb_parse_response (reused)
#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig (proxy_rendezvous_from_join)
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
// http_base()/request_headers()/merge_response_cookies(). The URL/.dll shapes are byte-preserved,
// their page names, pfid, version pair and product code the title's (TitleIdentity::pages / pfid /
// ver1 / ver2 / product_code) [orig: the retail jop_2_* template + NWJoin query strings; NWStart's
// is the start page's refresh, followed as it stands, and the NWLogin form the sign-in page's own,
// posted as it stands (ib3_form_inputs)].
namespace opennova {

struct TitleIdentity; // net/title/title_identity.h

// --- the transport seam (the owner maps these onto its HTTP client) ---
enum class HttpMethod { Get, Post };

struct HttpRequestSpec {
	HttpMethod method = HttpMethod::Get;
	std::string url;
	std::vector<std::string> headers; // "Header: value" lines (Content-Type, Cookie)
	std::string body;                 // form body (POST); empty otherwise
	bool valid = false;               // false when there is no request to issue
	// The menu browser's GET: once the transport resolved the server, the URL takes its
	// nocachefix pair (browser_cache_bust_url) before it goes out. A form post takes none.
	bool cache_bust = false;
};

// A menu-browser GET's URL as it goes out: "?nocachefix=" (or "&nocachefix=" behind a query)
// and "%d%d" of the process tick (GetTickCount's) and the resolved server's IPv4 address, its
// four octets in network order read as one little-endian int (207.178.209.204 prints
// -858672433). A URL with no path goes out as it is (D-NET-NEW-L9-31).
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - the query test `cmp dword ptr [ebp+1Ch], 0`
//  @0x65889F, the request's +0x30 word read @0x6588A3, GetTickCount @0x6588AA / @0x6588F6,
//  "?nocachefix=%d%d" (0x7E1E28) pushed @0x6588B5, "&nocachefix=%d%d" (0x7E1DFC) pushed
//  @0x658901, the path's length test (String_SafeLen called @0x658910), "GET / HTTP/1.0" with no
//  pair @0x658942; the request's +0x30 word the server's address: CNapiWinsock_InitFromUrl
//  @0x658DA0 called @0x659797 over the request's +0x28 (@0x65978D) - CNapiWinsock_ResolveHostAsync
//  called @0x658E4B into its +8, read as the connect's address (`lea eax, [esi+30h]` @0x6597F3,
//  CNapiWinsock_ConnectNonBlocking @0x667600 called @0x6597F7)]
std::string browser_cache_bust_url(const std::string &url, uint32_t tick_ms,
		const std::array<uint8_t, 4> &server_ip);

// The URL a page's META refresh names, as the menu browser's parser reads it ("" when none):
// the last tag the element table takes as META whose HTTP-EQUIV is REFRESH, whole and
// case-insensitively, and which has a CONTENT; in that CONTENT, "URL=" sought from its first ';',
// one leading quote skipped, the copy to a quote or the end. A refresh with no ';' or no "URL="
// names none (retail copies an uninitialized buffer there). (D-NET-NEW-L9-34)
// [orig: CUIHTMLParser_ParseMetaRefreshTag @0x655620; XML_ParseAttributes @0x655410;
//  CUIHTMLParser_ParseTag @0x655E20]
std::string meta_refresh_url(const std::string &page);

// The template a page names, as the menu browser's parser reads it (none when it names none): the
// FILE of the last tag the element table takes as IB3_TEMPLATE that has one, its value as the
// attribute parser reads it (an empty one included). The browser fetches it, combined with the
// page's URL, through its cache (browser_cache.h) once the page is processed.
// [orig: the IB3_TEMPLATE handler @0x6558C0 (the table row @0x84C148 + 3 * 12) - a closing tag
//  passed over @0x6558C0..0x6558C5, the FILE lookup (XML_FindAttributeValue called @0x6558DD), the
//  old copy freed @0x6558E6..0x6558F6 and the value's copy to the parser's +8 @0x655902..0x65592C;
//  CUIHTMLParser_BuildContentNodeFromHTML @0x656080 - the copy to the node's +0xC @0x6563F1;
//  UI_ProcessWebResponseContent @0x63D450 - the test @0x63D63C, CUIBrowser_AsyncFetchURLContent
//  called @0x63D6D1 over the page's URL (the scene's +0x34, set @0x63D599)]
std::optional<std::string> page_template_file(const std::string &page);

// The inputs of a page's IB3_FORM `form` as the menu browser's form post sends them (name, value),
// in its order (D-NET-NEW-L9-45): each INPUT the element table takes inside an IB3_FORM whose VALUE
// reads (atol) as `form` is linked at the head of the form's list, so the page's LAST input comes
// first. An IB3_FORM with no VALUE keeps the form in force, a closing IB3_FORM ends it while one is
// open, and an INPUT outside one reads nothing (a comment's "<!--" is the META element, which the
// skip to its first '>' ends, so a commented INPUT is none). A missing NAME or VALUE reads
// "(null)", as the script pass's sprintf writes it. The page's template places the form's controls
// (its @IB3_FORM_<n>@) after its own edits of that form, which post first.
// [orig: CUIHTMLParser_ParseTag @0x655E20; CUIHTMLParser_HandleIb3FormTag @0x655960 (the IB3_FORM
//  handler); XML_PropertyNodeAdd @0x655C60 (the INPUT handler); UI_ProcessScriptBuffer @0x63CBE0;
//  CWnd_BuildFormFieldQueryString @0x657760 - the control's form test @0x6577AE..0x6577BE, the
//  value's EPASK_EncryptField called @0x657980, the children walked in order @0x657A80..0x657A97]
std::vector<std::pair<std::string, std::string>> ib3_form_inputs(const std::string &page,
		int32_t form);

// The lobby context the HTTP machines read — filled by the owner from the gate response + the NWU
// session leg (the UDP side stays in the binding/ClientSession; here we only consume its outputs).
struct LobbyHttpContext {
	// The title the session speaks (complete: resolve_title_identity) and its gate manager's tag
	// (title_gate_tag), which the start page's [GT] takes; the owner's NWU session's. Without a
	// title the flow requests nothing.
	const TitleIdentity *title = nullptr;
	std::string gate_tag;
	std::string startup_url;  // gate "UNPS" StartupUrl ("[domainname]/..." templated, or concrete for OpenNova)
	std::string post_ip;      // gate POST host (fallback http_base)
	std::string post_port;    // gate POST port
	std::string web_domain;   // NovaworldWebDomainNameAndPortNumber from the SessionInit ("[domainname]" fill)
	std::string server_nwuid; // ServerAuth nwuid (the empty-NWUID identity cookie substitute)
	std::string locale;       // OS locale ("en_US"); the [CC] region is derived from it (injected, not read here)
	std::vector<std::pair<std::string, std::string>> identity_vars; // the NW-S5 "Cookie" identity set (seeded as cookies)
	// The gate reply's junction word as the session keeps it (USEJUNCTION, the command line's
	// override applied; NwuLobbySession::use_junction): set, the jar leads with USEJUNCTION "1" and
	// an empty JUNCTIONCODE.
	int32_t use_junction = 0;
	// game.cfg's mpextractextendedmetricinformation (cfg +0x33C, default 1): clear, neither the jar
	// nor the Cookie var-lists carry CountryName / Language / TimeZoneBias.
	bool extended_metrics = true;
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
	// Settled: the .joi page's refresh came back (its cookies merged); nothing more to do.
	enum class Kind { NeedRequest, Resolved, Failed, Settled };
	Kind kind = Kind::Failed;
	HttpRequestSpec request;  // when NeedRequest; when Resolved, the .joi page's refresh GET
	                          // (invalid when the page names none), which the owner ships after
	                          // its play leg starts and feeds back to on_join_response
	std::string host_ip;      // when Resolved: the decoded-NK endpoint (NK is the only dial authority)
	uint16_t host_port = 0;   // when Resolved
	// The .joi lobby number. Nonzero means the retail transport dials the
	// LAN-discovered endpoint for this session instead of host_ip/host_port
	// and reports the number as the session var "Lan". 0 when absent.
	// [orig: CNapiGameSession_InitTransportConnection @0x4C9E10 - the LN test
	//  @0x4C9E65..0x4c9e6c; CNapiGameSession_ConnectOrHost @0x4D4F10 - g_LobbyNum @0x4d5418,
	//  "Lan" @0x4D541F]
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

// --- the NovaWorld menu's entry: its web lobby's open ---
// Every entry of the NovaWorld menu (a re-entry after a match included) opens the gate's startup
// URL, and the pages drive the rest: the start page refreshes to NWStart, which answers the lobby
// page for a login the service remembers or the sign-in page otherwise. In OpenNova's page model
// (D-NET-NEW-L9-18) the lobby page is the panel's lobby, whose list load follows, and the sign-in
// page its sign-in screen, whose Login then posts that page's form.
struct EnterResult {
	enum class Kind { NeedRequest, Lobby, SignIn, Failed };
	Kind kind = Kind::Failed;
	HttpRequestSpec request;  // when NeedRequest
	std::string nwhandle;     // when Lobby: the jar's NWHANDLE (may be empty)
	std::string reason;       // when Failed
};

// The proxy-assisted join's install from a resolved .joi: node = inet_addr(NI) /
// atol(NP), cookie = atol(BK), relay = inet_addr of the NK-decoded host / the NK port,
// each atol the CRT's (io::retail_atol; D-NET-384). A field that does not parse stays 0,
// which the all-six gate (ProxyRendezvousConfig::enabled) reads as no proxy, as a .joi
// without NI/NP/BK.
// [orig: CNapiGameSession_InitTransportConnection @0x4c9e10 - the install @0x4ca051..0x4ca0c7:
//  _atol of BK @0x4ca05b, of NP @0x4ca08a, the relay port's @0x4ca0b9]
ProxyRendezvousConfig proxy_rendezvous_from_join(const JoinResult &resolved);

// The menu's two web opens' _connectlog.txt lines, past their "<time> : " prefix: the web lobby's
// open once the session login is verified (the next menu frame, state 9, over the gate's resolved
// STARTUPURL: resolve_startup_url), and the Host's open of the main page's @HOST_URL@ global as the
// page set it.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 8's success sets state 9 @0x559242;
//  state 9: "%s : HTTP Getting \"%s\"" over g_NovaWorldWebURL @0x5592AC..0x5592D8, UIScene_OpenUrl
//  @0x5592ED; UI_HandleHostSessionStart @0x556D00 - "%s : HTTP hosting with \"%s\"" (the append
//  @0x556F88), UIScene_OpenUrl @0x556F98]
inline std::string http_getting_line(const std::string &url) {
	return "HTTP Getting \"" + url + "\"";
}
inline std::string http_hosting_line(const std::string &url) {
	return "HTTP hosting with \"" + url + "\"";
}

// --- host ---
// The NovaWorld menu's Host leg: the hosting page the main page names answers, through its relay,
// with a page whose <TITLE> carries [HOSTKEY=...&]; that key rides the host registration.
struct HostKeyResult {
	// Settled: the HOSTKEY page's refresh came back (its cookies merged); nothing more to do.
	enum class Kind { NeedRequest, Resolved, Failed, Settled };
	Kind kind = Kind::Failed;
	HttpRequestSpec request;  // when NeedRequest; when Resolved, the HOSTKEY page's refresh GET
	                          // (invalid when the page names none), which the owner ships once
	                          // the host request's wait ends and feeds back to on_host_response
	std::string host_key;     // when Resolved: the HOSTKEY= value, trimmed at '&' then ']'
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

	// --- the NovaWorld menu's entry (EnterResult) ---
	// Begin: the startup URL's GET (NeedRequest), or Failed with no title or startup URL. The
	// connect's identity set is seeded ahead of the session's first request (seed_identity_cookies,
	// once per session); the jar is kept: a re-entry carries the login the service may remember.
	// [orig: UI_EnterNovaWorldMenu @0x5588F0 - g_StartupURL cleared @0x5588FF, the connect state 1
	//  @0x558949; UI_ProcessLANSessionStateMachine @0x558DE0 - state 1 stops hosting and playing
	//  on a live session (CGameSession_StopHosting called @0x558E76, CGameSession_StopPlaying
	//  @0x558E80) and the chain returns at once for it; state 9 (@0x559262):
	//  CMissionInfo_ProcessDescription called @0x559291 over the startup URL, UIScene_OpenUrl
	//  called @0x5592ED over g_NovaWorldWebURL]
	EnterResult enter();
	// Feed a completed entry response: the start page's META refresh is followed to NWStart, whose
	// page is the lobby (it sets @GSB_SERVER@, the address the lobby page's list load reads) or the
	// sign-in page (its form posts to NWLogin.dll); any other page fails with its message.
	EnterResult on_enter_response(bool transport_ok, int code,
	                              const std::vector<std::string> &response_headers,
	                              const std::vector<uint8_t> &body);
	bool enter_active() const { return enter_step_ != EnterStep::Idle || enter_template_.next.has_value(); }

	// --- EPASK login chain ---
	// Begin: returns the prepare GET (NeedRequest) or Failed (no startup_url); with the sign-in page
	// already shown (an entry that landed on it), the page's form POST at once. [orig: login()]
	LoginResult login(const std::string &username, const std::string &password);
	// Feed a completed login response; advances PREPARE -> (NWSTART, the start page's refresh when
	// it names one) -> POST -> POLL. [orig:
	// on_login_request_completed()]. `transport_ok` is the owner's "the HTTP call itself succeeded"
	// (Godot RESULT_SUCCESS); `code` is the HTTP status (must be 200).
	LoginResult on_login_response(bool transport_ok, int code, const std::vector<std::string> &response_headers,
	                              const std::vector<uint8_t> &body);
	bool login_active() const { return login_step_ != LoginStep::Idle || login_template_.next.has_value(); }

	// --- GSB server browser ---
	// Build the GSB fetch (the title's server list, JO's jop_2.gsb?a=1) with the current cookies;
	// invalid when no base URL or no title yet.
	// [orig: gsb_url() + request_server_list()]
	HttpRequestSpec gsb_request() const;
	// Parse a completed GSB response into `out` (reuses gsb_parse_response). Returns false on a failed
	// fetch / parse. Does NOT merge Set-Cookie (the retail GSB leg doesn't). [orig: on_gsb_request_completed()]
	bool on_gsb_response(bool transport_ok, int code, const std::vector<uint8_t> &body, GsbResponse &out);

	// --- NWJoin handshake ---
	// Begin: returns the first NWJoin GET (NeedRequest) or Failed. [orig: join()]
	JoinResult join(uint32_t rid);
	// Feed a completed join response; FIRST is the relay page, whose META refresh is the second GET
	// (the bare NWJoin.dll on the service's relay, its tag the jar's NWJOINSESSIONTAG cookie), SECOND
	// parses the .joi -> {host, port}. [orig: on_join_request_completed()]
	JoinResult on_join_response(bool transport_ok, int code, const std::vector<std::string> &response_headers,
	                            const std::vector<uint8_t> &body);
	bool join_active() const { return join_step_ != JoinStep::Idle || join_template_.next.has_value(); }

	// --- the Host leg ---
	// Begin: the jar takes PLoad "" and EXP `expansion` (the mounted expansion's name, the Host
	// lists' Exp) as the Host's open sets them, each updating its entry in place or appended at
	// the jar's end, so the host leg's requests and the host request's Cookie var-list carry them
	// after the login's cookies (D-NET-NEW-L9-48); then the GET of the main page's @HOST_URL@
	// global (NeedRequest), or Failed when no page set one (log in first: the login lands on the
	// main page).
	// [orig: UI_HandleHostSessionStart @0x556D00 - the NovaWorld connect type's branch
	//  @0x556E3B..0x556E47, the menu's test @0x556F13, CUIBrowser_AddFormField "PLoad" g_EmptyStr
	//  called @0x556F21 and "EXP" g_ExpansionName called @0x556F36, CUIScene_FindNamedValue
	//  ("@HOST_URL@") called @0x556F46, UIScene_OpenUrl called @0x556F98; CUIBrowser_AddFormField
	//  @0x63A5D0 - CookieJar_UpdateFromURL called @0x63A689 with one entry;
	//  CookieJar_UpdateFromURL @0x64E630 - a same-named entry's value replaced @0x64E7FF, else
	//  the entry appended @0x64E8F7..0x64E91C]
	HostKeyResult host(const std::string &expansion);
	// Feed a completed host-leg response. A page whose <TITLE> starts [HOSTKEY= resolves the key; a
	// relay page's META refresh is followed (at most kMaxHostHops pages); any other page fails with
	// its message. [orig: UI_ProcessWebResponseContent @0x63d450 — the title test @0x63d5bb..0x63d604
	//  posts screen event 7 with the title; UI_DispatchScreenEvent @0x54f2fa ->
	//  URL_ParseConnectionQueryString @0x54dfb0 (HOSTKEY= @0x54e0be)]
	HostKeyResult on_host_response(bool transport_ok, int code,
	                               const std::vector<std::string> &response_headers,
	                               const std::vector<uint8_t> &body);
	bool host_active() const { return host_step_ != HostStep::Idle || host_template_.next.has_value(); }
	static constexpr int kMaxHostHops = 8;

	// The menu browser's page globals (<IB3_GLOBAL name="@NAME@">value</IB3_GLOBAL>) the pages this
	// flow fetched set, by name ("@HOST_URL@"); null when no page set it.
	// [orig: UI_ProcessWebResponseContent @0x63d50a..0x63d558 -> CUIScene_SetProperty @0x63b480]
	const std::string *browser_global(const std::string &name) const;

	// Exposed for the owner's cookie diagnostics / tests.
	const CookieJar &cookies() const { return jar_; }
	// Current browser cookies plus the locale/initial identity for UDP statements.
	std::vector<std::pair<std::string, std::string>> session_cookie_vars() const;

private:
	enum class LoginStep { Idle, Prepare, NwStart, Post, Poll };
	enum class JoinStep { Idle, First, Second, Page };
	enum class HostStep { Idle, Fetching, Page };
	enum class EnterStep { Idle, Prepare, Start };

	// A page's IB3_TEMPLATE GET in flight on one leg, and the leg's result it holds back until the
	// template's answer: the browser runs its page requests one at a time in their order, the
	// template's (queued while the page is processed) ahead of the page's next (a refresh queued at
	// a later frame, a list load the template's own action starts).
	// [orig: CGopherWnd_PumpRequests @0x659740 (the gopher window's request pump) - a request whose
	//  +0x70 word (the fetch's count argument) is set marks the pass @0x6599A3..0x6599A8 and every
	//  later such request waits @0x65976C..0x659775; the template's fetch passes that count as 1
	//  (UI_ProcessWebResponseContent @0x63D450, `push 1` @0x63D6C7), as UIScene_OpenUrl's page fetch
	//  does (`push 1` @0x63E39E)]
	template <typename Result>
	struct TemplateHold {
		std::string url;            // the template GET's URL
		std::optional<Result> next; // the leg's result, held until the template's answer
	};
	// The leg's result behind the page's template GET when the process's browser cache does not
	// serve the template (browser_cache.h); `next` itself otherwise.
	template <typename Result>
	Result behind_template(TemplateHold<Result> &hold, const std::string &page_url,
	                       const std::vector<uint8_t> &page, Result next);
	// The held result once the answer fed in is the template's (its cookies merged, its end filed in
	// the cache); none when no template is in flight on the leg.
	template <typename Result>
	std::optional<Result> template_answered(TemplateHold<Result> &hold, bool transport_ok, int code,
	                                        const std::vector<std::string> &response_headers,
	                                        const std::vector<uint8_t> &body);

	// The menu browser's GET headers and its form post's (each past the request line and Host).
	std::vector<std::string> request_headers() const;
	std::vector<std::string> post_headers(std::size_t content_length) const;
	void merge_response_cookies(const std::vector<std::string> &response_headers);
	void seed_identity_cookies();
	// The jar's identity seed in retail's order (the gate reply's fields, then the connection-up's),
	// the live nwuid for the empty NWUID entry.
	std::vector<std::pair<std::string, std::string>> identity_seed() const;
	// The /NWLogin.dll POST of the sign-in page's form, its IB3_FORM 1 inputs `form`
	// (ib3_form_inputs), or Failed.
	LoginResult login_post(const std::vector<std::pair<std::string, std::string>> &form);
	// The jar's EPASK bundle read into epask_, as the sign-in page's submit encrypts under it; the
	// failure's reason when the jar holds none or the modexp gate rejects it.
	std::string take_epask(const std::vector<uint8_t> &page);
	LoginResult login_request(HttpRequestSpec req); // a login leg's request, its URL the page URL
	EnterResult enter_request(HttpRequestSpec req); // an entry leg's request, likewise
	std::string gsb_url() const;
	// A relay page's META refresh as an absolute URL ("" when it names none).
	std::string relay_refresh_url(const std::vector<uint8_t> &body) const;
	void capture_globals(const std::vector<uint8_t> &body);
	std::string absolute_url(const std::string &url) const;
	HostKeyResult host_get(const std::string &url);

	LobbyHttpContext ctx_;
	CookieJar jar_;
	EpaskParams epask_;
	LoginStep login_step_ = LoginStep::Idle;
	int login_poll_count_ = 0;
	std::string login_user_, login_pass_;
	JoinStep join_step_ = JoinStep::Idle;
	HostStep host_step_ = HostStep::Idle;
	int host_hops_ = 0;
	EnterStep enter_step_ = EnterStep::Idle;
	// The sign-in page is the one shown (an entry landed on it): Login posts its form, the page's
	// IB3_FORM 1 inputs kept with it.
	bool sign_in_page_shown_ = false;
	std::vector<std::pair<std::string, std::string>> sign_in_form_;
	// The connect's identity set is in the jar (seed_identity_cookies: once per session).
	bool identity_seeded_ = false;
	// Each leg's last request's URL: the page its answer is, whose template it names.
	std::string login_page_url_, join_page_url_, host_page_url_, enter_page_url_;
	TemplateHold<LoginResult> login_template_;
	TemplateHold<JoinResult> join_template_;
	TemplateHold<HostKeyResult> host_template_;
	TemplateHold<EnterResult> enter_template_;
	std::map<std::string, std::string> globals_; // upper-cased name -> value
};

} // namespace opennova
