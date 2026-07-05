#pragma once

#include <novacrypto/epask.h>

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
// protocol/crypto in one place (plan/README.md principle 3).
//
// Witnessed against retail Jointops.exe (grill NW-S5/B, 2026-06-12):
//   * POST builder GopherWebWidget_SendHttpPost @ 0x658b30 sends
//     `Content-type: application/x-www-form-urlencoded` with the field body
//     assembled by the form widgets (build_form_field_query_string @ 0x657760).
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
//   * BOTH the GET and POST request builders attach EVERY cookie for the
//     subnet-truncated host to the request (Network_TruncateIPToSubnet
//     @ 0x62dfe0) — including the .gsb server-browser fetch. This resolves the
//     ADR 0010 open question: on real NovaWorld the GSB GET carries
//     NWHANDLE/PCID.

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

// Assemble the application/x-www-form-urlencoded body for POST /NWLogin.dll.
// `pub` is the EPASK bundle the server issued via the EPASK cookie; encrypted
// fields are transformed under it with epask_encrypt(). Values are emitted raw
// (no percent-encoding), matching retail's faithful concatenation: every value
// in this flow is already URL-safe (A-P ciphertext, digits/colons in the EPASK
// bundle, template filenames in the hidden fields), and the server url-decodes
// regardless.
std::string build_login_post_body(const EpaskParams &pub,
                                  const std::vector<LoginFormField> &fields);

// Convenience over build_login_post_body for the standard credential submit:
// the echoed EPASK public key (plaintext), then EPASK-encrypted NAME and
// PASSWORD, then the IB3_FORM passthrough fields (success / failure / relay /
// msgbase / enterkey / pfid / rememberlogin ...) plaintext, in that order.
std::string build_credentials_post_body(
    const EpaskParams &pub, const std::string &name, const std::string &password,
    const std::vector<std::pair<std::string, std::string>> &hidden = {});

// Parse a batch of `Set-Cookie` header VALUES (the text after the colon, one
// per header line) into name/value pairs. Only the leading `name=value` pair of
// each line is kept; attributes (Path, Expires, HttpOnly, ...) after the first
// ';' are dropped. Mirrors the cookie-jar store at CookieJar_UpdateFromURL.
std::vector<std::pair<std::string, std::string>>
parse_set_cookie_values(const std::vector<std::string> &set_cookie_values);

// The connection tokens NWJoin.dll hands back in the `.joi` response, used to
// reach the hosted game. NK/CK are url_cipher-encoded (NK = host "ip:port",
// CK = the host app id); NI/NP are plaintext proxy/display slots; BK is the
// literal "986119".
struct JoiConnection {
	std::string nk;
	std::string ck;
	std::string ni;  // host ip (plaintext)
	std::string np;  // host port (plaintext)
	std::string bk;
	std::string host_ip;   // decoded NK head, fallback NI
	std::string host_port; // decoded NK tail, fallback NP
	bool ok = false; // true when a dial endpoint was recovered
};

// Extract the bracketed connection string the join page carries in its <TITLE>:
//   [NK=<enc>&CK=<enc>&NI=<ip>&NP=<port>&BK=986119&]
// This is exactly what retail's browser scrapes (parse_connection_query_string
// @ 0x54dfb0). Splits the first `[...]` run on '&' into KEY=VALUE pairs. The
// url_cipher-encoded NK/CK never contain '&', so the split is unambiguous (and
// matches retail, whose NK/CK decode also terminates at '&'). The host address
// is taken from decoded NK; NI/NP are preserved but are not the dial authority.
JoiConnection parse_joi_connection_string(const std::string &body);

// Truncate an HTTP host to the retail cookie-jar subnet key: a dotted-decimal
// IPv4 keeps its first two octets ("192.168.1.1" -> "192.168"); any other host
// (a DNS name, or a malformed address) is returned unchanged.
// [orig: Network_TruncateIPToSubnet @ 0x62dfe0 — reverse, strip past the 2nd
// dot from the end, reverse back; only when Network_ParseIPv4AddressOctets
// accepts the string as IPv4.]
std::string subnet_key(const std::string &host);

// The cookie store the engine carries across the NovaWorld endpoint family.
// Retail keys cookies by subnet-truncated host and attaches all of them to
// every request to that subnet; the NovaWorld gate, login, host, join, and GSB
// endpoints share a host, so we model the whole family as one jar. Insertion
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

private:
	std::map<std::string, std::string> values_;
	std::vector<std::string> order_;
};

} // namespace opennova
