#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <net/novaworld/gsb.h>        // GsbResponse / gsb_parse_response (reused)
#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig (proxy_rendezvous_from_join)
#include <net/novaworld/http_login.h> // CookieJar / LoginFormField / build_login_post_body / parse_joi_connection_string
#include <net/novaworld/cookie_jar_file.h> // MenuBrowserState (the jar, REMEMBER_LOGIN, the LOGIN mark, nw_cdata.coo)

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
// -858672433). A URL with no path goes out as it is (D-NET-1251).
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

// The value of a menu-browser request's "Host:" line, which leads the lines past the request
// line: the URL's host as the browser's URL parse cuts it, the text past its "//" up to the first
// '/' or ':', so a port the URL names never rides it (the live login legs' URLs carry SessionInit's
// "<ip>:80", their Host lines "<ip>"). The GET and the form post write the same field
// (D-NET-1329).
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - the request's +0xC read @0x658952, "Host: %s"
//  @0x658959 (sprintf called @0x65895F); GopherWebWidget_SendHttpPost @0x658B30 - +0xC read
//  @0x658BA5, "Host: %s" @0x658BAE (sprintf called @0x658BB4); HTTP_RequestEnqueue @0x658460 -
//  the URL's parsed components copied to the request's +8 @0x6584BC..0x6584C8; URL_ParseComponents
//  @0x650140 - the fragment cut first (strchr '#' called @0x6501A7), the host's scan stopping at
//  '/' @0x650332 or ':' @0x650336, its copy stored @0x650358, the port its own field @0x6503B6]
std::string browser_request_host(const std::string &url);

// The URL a page's META refresh names, as the menu browser's parser reads it ("" when none):
// the last tag the element table takes as META whose HTTP-EQUIV is REFRESH, whole and
// case-insensitively, and which has a CONTENT; in that CONTENT, "URL=" sought from its first ';',
// one leading quote skipped, the copy to a quote or the end. A refresh with no ';' or no "URL="
// names none (retail copies an uninitialized buffer there). (D-NET-1263)
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
// in its order (D-NET-1313): each INPUT the element table takes inside an IB3_FORM whose VALUE
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
	// game.cfg's rememberlogin (cfg +0x4D4, which the sign-in page's REMEMBER_LOGIN checkbox
	// stores): set, the sign-in page's activation fills the remembered login from the jar
	// (LobbyHttpFlow::remembered_login_name, D-NET-1331).
	bool remember_login = false;
};

// The remembered login's cookie decodes under this chain into the account name and a second
// string the client never reads [orig: UI_LoadPersistentLoginCredentials @0x557330 - the chain
//  pushed @0x55736B for NapiNP_DecodeEncryptedKeyValue, called @0x55738F].
inline constexpr const char *kRememberedLoginKeyChain =
		"SLHALI289SZ79210987ZS:OCV789YHK2QJ3HSKDJHVS978THYG23";

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
// (D-NET-991) the lobby page is the panel's lobby, whose list load follows, and the sign-in
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
// GSB / join. The cookie jar is SHARED across all three (login's NWHANDLE/PCID ride GSB + join), and it
// is the menu browser state's: a flow's own unless the owner shares the process's (share_browser), as
// the game does, so the jar outlives every session and process's NovaWorld entry, as retail's does.
class LobbyHttpFlow {
public:
	void set_context(LobbyHttpContext ctx) { ctx_ = std::move(ctx); }
	const LobbyHttpContext &context() const { return ctx_; }
	// start()/stop() equivalent: both machines and the session's state cleared (keeps the context and
	// the browser state: the jar is the menu's, which no session start or end clears).
	void reset();
	// The flow works on `state` from now on (the game's: process_menu_browser()).
	void share_browser(MenuBrowserState &state) { browser_ = &state; }
	MenuBrowserState &browser() { return *browser_; }
	const MenuBrowserState &browser() const { return *browser_; }

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

	// --- the remembered login (D-NET-1331) ---
	// The sign-in page's activation, with game.cfg's rememberlogin set, reads the jar's
	// PERSISTENTREMEMBERLOGINDATA (case-insensitively, its first 511 characters) and decodes it
	// under kRememberedLoginKeyChain into the account name and a second string; decoded, the NAME
	// edit takes the name, the masked password placeholder shows and the hidden rememberlogindata
	// edit takes the cookie's value as it stands. The first click on NAME, PASSWORD or the
	// placeholder clears that edit and hides the placeholder (clear_remembered_login): the click is
	// the class-3 event the callbacks' mask 8 takes, where a keystroke is class 2 and a TAB action
	// moves the focus with no event; no edit holds the focus until it is clicked, so every typed
	// login follows such a click. The form post then sends NAME, the PASSWORD edit (empty when
	// nothing was typed) and rememberlogindata, each EPASK-encrypted (login_remembered). LN-s6's S3
	// re-login: NAME, PASSWORD empty, rememberlogindata 1152 A-P characters decrypting to the
	// cookie's 144. A Login press on a page whose submit is the LOGIN control (DFX2's) marks the
	// LOGIN flag: every fill until the home page next shows (a login's success, an entry landing on
	// the lobby: show_home_page) fills the name only and blanks the jar's
	// PERSISTENTREMEMBERLOGINDATA (D-NET-1335).
	// [orig: CWnd_EmitEventToNamedHandlerAndCallbacks @0x646970 - a callback runs when its mask
	//  holds 1 << (type >> 24) @0x6469B8; CWnd_ProcessMouseEvent @0x647A00 - a press and its
	//  release over the window emit 0x3000001 (its vtable +0x1C called @0x647D15), the edits'
	//  own (CEditWnd vtable 0x7E2614: +0x14 CWnd_ProcessMouseEvent, +0x1C the emitter);
	//  CEditWnd_HandleInputEvent @0x661510 - the press 0x1000002 takes the focus @0x66161F, keys
	//  0x2000001 / 0x2000002; CUIWidget_HandleScriptedAction @0x6497F0 - a TAB action on key 9
	//  sets g_UIFocusWnd @0x649C50]
	// [orig: UI_LoadPersistentLoginCredentials @0x557330 (the ENABLE_BACK_BUTTON callback
	//  UI_RegisterNWMultiplayerCallbacks registers @0x559844; the sign-in template's ACTIVATE) -
	//  the rememberLogin_4D4 test @0x557344, the cookie's read (CUIBrowser_GetCookieValue called
	//  @0x557366, 0x1FF bytes), the decode called @0x55738F, the NAME edit's text (the call
	//  @0x5573C5), STATIC_NW_PASSWORD_HIDDEN shown (CWnd_SetShown called @0x5573EA), the
	//  REMEMBERLOGINDATA edit's text the cookie (the call @0x557413), the one-shot re-armed
	//  @0x55745D; UI_ClearRememberedLoginFill @0x557480 (the NAME / PASSWORD /
	//  STATIC_NW_PASSWORD_HIDDEN event-8 callback, registered @0x55989E / @0x5598BC / @0x5598DA)]
	const std::string &remembered_login_name() const { return remembered_name_; }
	bool has_remembered_login() const { return !remember_fill_.empty(); }
	// A sign-in field touched: the rememberlogindata edit cleared, the placeholder hidden.
	// [orig: UI_ClearRememberedLoginFill @0x557480 - the one-shot test @0x557480..0x557487,
	//  STATIC_NW_PASSWORD_HIDDEN hidden (CWnd_SetShown called @0x5574A3), the REMEMBERLOGINDATA
	//  edit's text "" (the call @0x5574CC), the flag set @0x5574D3]
	void clear_remembered_login() { remember_fill_.clear(); }
	// The sign-in page's Login with its remembered fill standing: the form posts the remembered
	// name, an empty PASSWORD and the cookie in rememberlogindata (through the start page when no
	// sign-in page is shown, as login does). Failed when nothing is filled.
	LoginResult login_remembered();
	// The sign-in page shown again (a failed login's way back to it): the template's activation
	// re-runs the remembered fill (D-NET-1335).
	// [orig: UI_ProcessScriptBuffer @0x63CBE0 - the ACTIVATE control's 0x3000001 (its vtable +0x1C
	//  called @0x63D3B9); UI_RegisterNWMultiplayerCallbacks @0x559800 -
	//  UI_LoadPersistentLoginCredentials on ENABLE_BACK_BUTTON for every event class (mask -1),
	//  registered @0x559855]
	void reshow_sign_in() { fill_remembered_login(); }
	// The sign-in page's REMEMBER_LOGIN checkbox ("Remember my login information.", the same
	// GLOBAL_VAR checkbox in JO's and DFX2's sign-in templates) clicked, `checked` its state after
	// the click: the click flips the state (checked to unchecked, any other to checked) and, the
	// checkbox being GLOBAL_VAR, writes the scene's property of its name, which the next jar save
	// reads; then the click's callback stores the state as game.cfg's rememberlogin, which the next
	// activation's fill reads and sets the checkbox from. Returns that word (1 or 0) for the
	// owner's cfg block, which retail's game.cfg save points write (the click saves nothing); the
	// flow's context takes it too.
	// [orig: CWnd_EmitEventToNamedHandlerAndCallbacks @0x646970 - the window's own handler called
	//  @0x64698E, then each callback whose mask holds 1 << (type >> 24) (@0x6469B8: 8 for the click
	//  0x3000001) called @0x6469D1; CCheckboxWnd_HandleNamedEvent @0x64AB80 - the click's state
	//  incremented @0x64ABE2, past 1 cleared @0x64ABF2, the property written under GLOBAL_VAR
	//  (+0xF8 tested @0x64ABF8, CUIScene_SetNamedValue called @0x64AC18);
	//  UI_RegisterNWMultiplayerCallbacks @0x559800 - UI_StoreRememberLoginCheckbox on
	//  REMEMBER_LOGIN, mask 8 (pushed @0x559934, CUIScene_RegisterControlCallback called
	//  @0x559945); UI_StoreRememberLoginCheckbox @0x5574F0 - the
	//  state read (CCheckWnd_GetCheckedState called @0x557508) into rememberLogin_4D4 @0x55750D]
	int32_t store_remember_login(bool checked);
	// The sign-in page's submit gate. A page whose submit is the LOGIN control (TitlePages::
	// sign_in_submit, DFX2's) has it enabled only with a NAME and a PASSWORD, or with a NAME and
	// the remembered fill's placeholder shown; the validation runs at the fill, at the clear and at
	// each keystroke in NAME or PASSWORD, so its state is this of the fields as they stand. A page
	// with no LOGIN control (JO's ACCEPT) is never gated (D-NET-1335).
	// [orig: UI_ValidateLoginFields @0x557230 - NAME, PASSWORD, LOGIN and STATIC_NW_PASSWORD_HIDDEN
	//  looked up (no LOGIN: nothing set), the texts read (31 characters), 1 pushed @0x55730D or 0
	//  @0x557311 for LOGIN's interactive state (UIWidget_SetInteractiveRecursive called @0x557315);
	//  called from UI_LoadPersistentLoginCredentials @0x55744F and UI_ClearRememberedLoginFill
	//  @0x5574CE, the NAME / PASSWORD event-4 callbacks registered
	//  @0x559909 / @0x559927]
	bool sign_in_submit_enabled(const std::string &name, const std::string &password) const;

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
	// after the login's cookies (D-NET-1328); then the GET of the main page's @HOST_URL@
	// global (NeedRequest), or Failed when no page set one (log in first: the login lands on the
	// main page).
	// [orig: UI_HandleHostSessionStart @0x556D00 - the NovaWorld connect type's branch
	//  @0x556E3B..0x556E47, the menu's test @0x556F13, CUIBrowser_SetCookie "PLoad" g_EmptyStr
	//  called @0x556F21 and "EXP" g_ExpansionName called @0x556F36, CUIScene_FindNamedValue
	//  ("@HOST_URL@") called @0x556F46, UIScene_OpenUrl called @0x556F98; CUIBrowser_SetCookie
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
	const CookieJar &cookies() const { return browser_->jar; }
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
	// An answer's cookies stored as the browser's response handler stores them, for every answer
	// whatever its step or status does next (response_cookie_batch: none from a 304 or from a 301 /
	// 302 that names a LOCATION).
	void merge_response_cookies(int code, const std::vector<std::string> &response_headers);
	void seed_identity_cookies();
	// The jar's identity seed in retail's order (the gate reply's fields, then the connection-up's),
	// the live nwuid for the empty NWUID entry.
	std::vector<std::pair<std::string, std::string>> identity_seed() const;
	// The /NWLogin.dll POST of the sign-in page's form, its IB3_FORM 1 inputs `form`
	// (ib3_form_inputs), or Failed.
	LoginResult login_post(const std::vector<std::pair<std::string, std::string>> &form);
	// The shown sign-in page's submit: its form posted under the start page's EPASK.
	LoginResult post_shown_sign_in();
	// A Login press: the shown page's submit, else the start page's open and its sign-in page's
	// submit; on a page whose submit is LOGIN the press marks the browser state's
	// login_button_pressed.
	LoginResult submit_sign_in();
	// The title's sign-in page submits with the LOGIN control (TitlePages::sign_in_submit).
	bool sign_in_submit_is_login() const;
	// The sign-in page's activation: its REMEMBER_LOGIN checkbox set to game.cfg's rememberlogin
	// (the scene's property with it), then the remembered login's fill (remembered_login_name).
	// [orig: UI_LoadPersistentLoginCredentials @0x557330 - the checkbox found @0x557433 and set
	//  (CCheckWnd_SetCheckedState called @0x55744A) whatever rememberlogin holds]
	void fill_remembered_login();
	// The home page shown (the main page, its template's ACTIVATE on ENABLE_HOME_TABS): a login's
	// success, an entry that lands on the lobby. The jar is saved to its file, then the LOGIN
	// press's mark is cleared.
	// [orig: UI_SaveCookieJarOnHomeTabs @0x557530 (registered on ENABLE_HOME_TABS for every event
	//  class, mask -1, by UI_RegisterNWMultiplayerCallbacks @0x559800, the call @0x559873) -
	//  CUIScene_SaveCookieJarFile("nw_cdata.coo") called @0x55753B, g_LoginButtonPressed = 0
	//  @0x557540]
	void show_home_page();
	// A page whose title raises the scene's event 7 (it starts "[NK=" or "[HOSTKEY=", past the
	// leading characters that are neither a letter nor '['): the jar is saved to its file ahead of
	// the connect string's parse.
	// [orig: UI_ProcessWebResponseContent @0x63D450 - the skip @0x63D5BB..0x63D5DE, "[NK=" compared
	//  (_strnicmp called @0x63D5E8), "[HOSTKEY=" (@0x63D604), event 7 raised (CUIScene_RaiseEvent
	//  called @0x63D634); UI_DispatchScreenEvent @0x54E6A0 - case 7's CUIScene_SaveCookieJarFile
	//  called @0x54F2C5, ahead of URL_ParseConnectionQueryString called @0x54F30A]
	void save_at_connect_page(const std::string &html);
	// The browser state's jar file written (MenuBrowserState::save), a failure only logged, as
	// retail drops the save's HRESULT.
	void save_jar_file();
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

	// The browser state: the jar, the REMEMBER_LOGIN property, the LOGIN mark and the jar file. A
	// flow's own until the owner shares another (share_browser).
	std::unique_ptr<MenuBrowserState> own_browser_ = std::make_unique<MenuBrowserState>();
	MenuBrowserState *browser_ = own_browser_.get();
	CookieJar &jar() { return browser_->jar; }
	const CookieJar &jar() const { return browser_->jar; }

	LobbyHttpContext ctx_;
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
	// The shown sign-in page's remembered fill: the NAME edit's decoded account name and the
	// rememberlogindata edit's value (the cookie), the latter empty once a field was clicked.
	std::string remembered_name_, remember_fill_;
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
