#pragma once

#include <net/novacrypto/epask.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

// NovaWorld HTTP account-login wire helpers (ADR 0010 Phase 3).
//
// These are the Godot-free, socket-free pieces of the in-game-browser login
// flow: assembling the credential POST body the engine sends to NWLogin.dll,
// and the cookie jar the engine carries across every subsequent request. The
// transport (the actual HTTP GET/POST) lives in the host: the Godot binding
// drives it with HTTPRequest, the server answers it with Crow. This keeps the
// protocol/crypto in one place.
//
// Witnessed against retail Jointops.exe (grill NW-S5/B, 2026-06-12):
//   * POST builder GopherWebWidget_SendHttpPost @ 0x658b30 sends
//     `Content-type: application/x-www-form-urlencoded` with the field body
//     assembled by the form widgets (CWnd_BuildFormFieldQueryString @ 0x657760).
//   * The echoed EPASK public key travels plaintext; EVERY other field is
//     EPASK-encrypted — not just NAME/PASSWORD but the hidden fields too (pfid,
//     needtoagree, nodb, relay, msgbase, enterkey, failure, success, ...).
//     Witnessed directly in the genuine .204 capture (POST /NWLogin.dll body,
//     all values A-P-encoded). The binding builds this via build_login_post_body
//     with per-field encrypt=true; build_credentials_post_body below keeps the
//     hidden fields plaintext for the permissive OpenNova-server path only.
//   * The EPASK bundle arrives as a Set-Cookie at /nwprepare.dll, is stored in
//     the cookie jar (CookieJar_UpdateFromURL @ 0x64e630), and is read back by
//     name (sub_64EA90 @ 0x64ea90).
//   * BOTH the GET and POST request builders attach EVERY cookie in the jar to
//     the request, the .gsb server-browser fetch included. They key the host
//     first (Network_TruncateIPToSubnet @ 0x62dfe0, subnet_key below), but the
//     key selects nothing: retail's jar is one list for every host. This
//     resolves the ADR 0010 open question: on real NovaWorld the GSB GET
//     carries NWHANDLE/PCID.

namespace opennova {

// One field of the NWLogin.dll credential POST body.
struct LoginFormField {
	std::string name;
	std::string value;
	// When true the value is EPASK-encrypted before transmission (retail does
	// this for EDIT widgets — NAME and PASSWORD). When false it is sent as-is
	// (hidden IB3_FORM fields, and the echoed EPASK public key).
	bool encrypt = false;
};

// Assemble the application/x-www-form-urlencoded body for POST /NWLogin.dll
// into `body`. `pub` is the EPASK bundle the server issued via the EPASK
// cookie; encrypted fields are transformed under it with epask_encrypt().
// Values are emitted raw (no percent-encoding), matching retail's faithful
// concatenation: every value in this flow is already URL-safe (A-P ciphertext,
// digits/colons in the EPASK bundle, template filenames in the hidden fields),
// and the server url-decodes regardless. False (`body` emptied) when an
// encrypted field's encryption fails (params the modexp gate rejects).
bool build_login_post_body(const EpaskParams &pub, const std::vector<LoginFormField> &fields,
                           std::string &body);

// Convenience over build_login_post_body for the standard credential submit:
// the echoed EPASK public key (plaintext), then EPASK-encrypted NAME and
// PASSWORD, then the IB3_FORM passthrough fields (success / failure / relay /
// msgbase / enterkey / pfid / rememberlogin ...) plaintext, in that order.
bool build_credentials_post_body(
    const EpaskParams &pub, const std::string &name, const std::string &password,
    std::string &body, const std::vector<std::pair<std::string, std::string>> &hidden = {});

// Parse a batch of `Set-Cookie` header VALUES (the text after the colon, one
// per header line) into name/value pairs. Only the leading `name=value` pair of
// each line is kept; attributes (Path, Expires, HttpOnly, ...) after the first
// ';' are dropped. Mirrors the cookie-jar store at CookieJar_UpdateFromURL.
std::vector<std::pair<std::string, std::string>>
parse_set_cookie_values(const std::vector<std::string> &set_cookie_values);

// The connection tokens NWJoin.dll hands back in the `.joi` response, used to
// reach the hosted game. NK is url_cipher-encoded (host "ip:port"); CK is
// url_cipher-encoded and, decoded, is a DECIMAL the retail client atol()s into
// an int field of its net config and serializes on the wire as the ClientAuth
// **APPID** conn-tag (NOT the BT tag: the host reads the APPID tag into that
// same int field and the BT tag into a different one); NI/NP are the plaintext
// game-node ip/port; BK is the plaintext relay tunnel cookie.
// [orig: URL_ParseConnectionQueryString @0x54dfb0 (CK key "cfhdcegjigecjehcgjdhe")
//  -> UI_JoinSelectedSession @0x5699d0 (`net_config.bt = atol(&g_NkExtraBuf[64])`
//  = atol(decoded CK)); the host side NapiNetConfig_LoadFromConnTags @0x4c7260
//  ("APPID" -> the same field IDA labels net_cfg.bt, "BT" -> char_name) and the
//  compare in Server_ValidatePlayerJoinRequest @0x512100 @0x5122c5 (reject
//  code 9)].
struct JoiConnection {
	std::string nk;
	std::string ck;
	std::string ni;  // host ip (plaintext)
	std::string np;  // host port (plaintext)
	std::string bk;
	// LN: the lobby number (atol). When nonzero the retail transport dials the
	// LAN-discovered endpoint instead of the NK relay pair and reports it as
	// the session var "Lan"; 0 when absent. GS is copied but never read by
	// any retail code. [orig: URL_ParseConnectionQueryString @0x54dfb0 LN
	//  @0x54e33e / GS @0x54e38a; CNapiGameSession_InitTransportConnection
	//  @0x4c9e6c; CNapiGameSession_ConnectOrHost @0x4d5418]
	int ln = 0;
	std::string gs;
	std::string host_ip;   // decoded NK head (empty without NK: no join)
	std::string host_port; // decoded NK tail
	// The game-session APPID join token: atol(decoded CK), re-serialized as retail
	// does (an int field). Sent as the ClientAuth APPID conn-tag, which the
	// NovaWorld host validates (code 9). "0" when no CK is present (the LAN
	// default; LAN sends no APPID). Witnessed live: stock `CU APPID="3225"`.
	std::string app_id = "0";
	bool ok = false; // true when a dial endpoint was recovered
};

// Extract the bracketed connection string the join page carries in its <TITLE>:
//   [NK=<enc>&CK=<enc>&NI=<ip>&NP=<port>&BK=986119&LN=<n>&GS=<s>&]
// This is exactly what retail's browser scrapes (URL_ParseConnectionQueryString
// @ 0x54dfb0). Splits the first `[...]` run on '&' into KEY=VALUE pairs. The
// url_cipher-encoded NK/CK never contain '&', so the split is unambiguous (and
// matches retail, whose NK/CK decode also terminates at '&'). The host address
// is taken from decoded NK; NI/NP are preserved but are not the dial authority.
JoiConnection parse_joi_connection_string(const std::string &body);

// The NovaWorld join's endpoint gate, run when the in-match join starts (after
// the play request): an NK port that reads 0 or an NK host string shorter than
// eight characters is refused with gameerr "MP Errors" CVSTATCLIENTERR before
// any transport opens (so a short dotted address such as "1.2.3.4" is refused
// too). [orig: UI_EnumerateAndJoinSession @0x569f50 — atol(g_NkExtraBuf)
//  @0x569fdd, strlen(g_NkBuf) @0x569ff9, the `!port || len < 8` test @0x56a006,
//  CVSTATCLIENTERR @0x56a145]
inline constexpr char kJoinEndpointRejectTag[] = "CVSTATCLIENTERR";
bool joi_endpoint_usable(const std::string &host_ip, long port);

// The cookie-jar host key (the IDB's name; no subnet is kept): a host
// Network_ParseIPv4AddressOctets accepts (parse_ipv4_octets, gate_response.h, with no end
// pointer, so "256.1.1.1" and "1.2.3.4x" are accepted) is returned whole, and any other
// keeps its LAST two dot-labels, reversed, cut at its second '.' and reversed back
// ("nw.novalogic.com" -> "novalogic.com", "192.168.1" -> "168.1", " 1.2.3.4" -> "3.4",
// "nw.novalogic.com." -> "com."); a host with fewer than two dots, or empty, is unchanged.
// The host is read as a C string (to its first NUL). Retail computes the key and decides
// nothing by it: the four jar accesses that take a URL (the store, the PUB* gather, the two
// request builders' read) each measure it and then take the first node of the jar's list
// whose +0 dword is 0, and the by-name read (EPASK's) computes none. The store makes every
// node with that dword 0 and the jar file restores the dword it saved, so a jar retail
// wrote reloads with 0 too: one jar serves every host, as CookieJar models.
// [orig: Network_TruncateIPToSubnet @0x62DFE0, the `jnz` @0x62DFFF; called from
//  CookieJar_UpdateFromURL @0x64E698 (the walk @0x64E6B0, a new node's 0 @0x64E98A),
//  Config_QueryMatchingEntries @0x64EC18, and the request builders
//  CUIBrowser_SendHTTPRequest @0x658A34 / GopherWebWidget_SendHttpPost @0x658C57 into
//  sub_64EA40 @0x64EA40; the by-name read sub_64EA90 @0x64EA90; the jar file's save
//  CookieJar_SaveToFile @0x64EE60 (node+0 written @0x64EF16) and load
//  CUIStringTable_LoadFromFile @0x64F290 (node+0 set @0x64F547)]
std::string subnet_key(const std::string &host);

// The cookie store the engine carries across the NovaWorld endpoint family.
// Retail computes a host key for each jar access that takes a URL (subnet_key)
// but keeps one jar for every host and attaches all of its cookies to every request, so the
// NovaWorld gate, login, host, join, and GSB endpoints share one jar here too. Insertion
// order is preserved for a stable Cookie sequence; re-setting a name updates
// the value in place.
class CookieJar {
public:
	void set(const std::string &name, const std::string &value);
	// Apply a batch of Set-Cookie header values (see parse_set_cookie_values).
	void merge_set_cookie_values(const std::vector<std::string> &set_cookie_values);
	// nullptr when absent.
	const std::string *find(const std::string &name) const;
	// One "name=value;" entry per cookie, in insertion order — the value part
	// of retail's per-cookie "Cookie: %s=%s;" header line
	// [orig: CUIBrowser_SendHTTPRequest @ 0x658840]. Empty when the jar is empty.
	std::vector<std::string> cookie_header_lines() const;
	bool empty() const { return order_.empty(); }
	const std::vector<std::string> &names() const { return order_; }
	// Concatenate every cookie whose name starts with `prefix` as [name\0][value\0]
	// pairs, in insertion order — the retail CD-cookie blob the game-session join
	// relays (the NovaWorld-issued PUB* identity: PUBPCID/PUBNAMEINFO/PUBSQUADINFO/
	// PUBJOINTICKET). Empty when nothing matches.
	// [orig: Config_QueryMatchingEntries @0x64eb70 gather-by-prefix ("PUB*") ->
	//  NetPacket_BuildAnnouncePayload @0x4c4bf0 -> the C2S 0x00 JOIN "CD" TLV
	//  (NapiNP_WriteClientAuthPayload @0x42a180)]
	std::vector<uint8_t> build_prefixed_blob(const std::string &prefix) const;

private:
	std::map<std::string, std::string> values_;
	std::vector<std::string> order_;
};

} // namespace opennova
