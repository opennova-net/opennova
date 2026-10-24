#pragma once

#include <net/novacrypto/epask.h>

#include <cstdint>
#include <optional>
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
//     name (CookieJar_GetValueByName @ 0x64ea90).
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

// One entry of the menu browser's cookie jar as its store keeps it: five strings and a word (a
// 24-byte record). A string the store leaves null is std::nullopt here, since the store's path
// rule tells a null path from an empty one and the jar file reloads every string as text (a null
// one as ""). The browser's form-field write makes an entry of a name and a value, the rest null.
// [orig: HttpCookie_Parse @0x658F60 - the six slots cleared @0x658F67..0x658F7C, the name [0]
//  @0x658FB9, the value [1] @0x658FF8, the EXPIRES copy [2] @0x659090, PATH [3] @0x659111,
//  DOMAIN [4] @0x659175, SECURE's 1 [5] @0x65919D; CUIBrowser_AddFormField @0x63A5D0 - the
//  slots past the value cleared @0x63A5E7..0x63A5F3]
struct CookieEntry {
	std::string name;
	std::string value;
	std::optional<std::string> expires;
	std::optional<std::string> path;
	std::optional<std::string> domain;
	uint32_t secure = 0;
};

// A response's Set-Cookie headers as the menu browser stores them, in the order its scan meets
// them. The scan looks through the response's header text for "SET-COOKIE" (case-insensitively,
// anywhere), steps 12 bytes past each match and then past spaces and colons, and parses the cookie
// there: its name up to '=' (a CR or a NUL first refuses the cookie), its value up to ';', CR or
// NUL, then attributes until the walk stands on a CR LF. Each attribute step moves one byte and
// past spaces, takes the attribute's name up to '=', CR or NUL and compares it by prefix,
// case-insensitively: EXPIRES (7 letters), PATH (4), DOMAIN (6), SECURE (6). EXPIRES, PATH and
// DOMAIN refuse the cookie without their '='; PATH and DOMAIN take the text to ';', CR or NUL;
// EXPIRES keeps the 29 bytes from the attribute's own first letter and moves the walk 29 bytes
// past its value's end; SECURE sets the word and moves the walk 6 bytes past the name's end. So a
// cookie with no attribute, or one whose walk jumps, reads on into the next header line as
// attribute text (a following "Expires:" header refuses an attribute-less cookie). Our transport
// hands over the header lines, so the text is rebuilt as the wire carries it: each line and CR LF,
// then the blank line's CR LF. A walk that would step past that text ends there with its cookie
// kept (retail's runs on into whatever its buffer holds next).
// [orig: HTTP_ResponseHandler @0x659480 - the scan String_FindCaseInsensitiveSubstring(.., "SET-COOKIE")
//  called @0x65961B, the +12 @0x65962D and the space / ':' skip @0x659630..0x659645, the next
//  scan from the cookie's text @0x659654, the parse called @0x659658, a refused cookie skipped
//  (`jl` @0x65965F); HttpCookie_Parse @0x658F60 - the name scan @0x658F83..0x658FAB (a CR or NUL
//  first: the '=' test @0x6590AF refuses), the value scan @0x658FC9..0x658FDF, the attribute walk
//  @0x659010..0x659051, EXPIRES compared @0x65905B (its '=' @0x659067, the copy of 29 bytes from
//  the name @0x65909A, the jump `add ebp, 1Dh` @0x6590A7), PATH @0x6590CD (its '=' @0x6590D9),
//  DOMAIN @0x659130 (its '=' @0x65913C), SECURE @0x659191 (the word @0x65919D, the 6 @0x6591A4),
//  the CR LF test against word_7C1310 called @0x6591AF]
std::vector<CookieEntry> parse_set_cookie_headers(const std::vector<std::string> &response_headers);

// The Set-Cookie batch the browser's response handler takes from an answer of HTTP status
// `status`, which it runs for every answer: none from a 304 (the answer is freed and the handler
// returns), none from a 301 or 302 whose header text holds "LOCATION" anywhere, case-insensitively
// (the handler follows the redirect instead: the text past the word, its spaces and ':', up to a
// CR, LF, tab or space, enqueued as the next request), else parse_set_cookie_headers' cookies. A
// 301 or 302 with no "LOCATION" is taken as any other answer. The header text is the one
// parse_set_cookie_headers rebuilds.
// [orig: HTTP_ResponseHandler @0x659480 - the status (atol called @0x6594DA past the first word),
//  301 tested @0x6594E2, 302 @0x6594EC, 304 @0x6594F3 (the answer freed @0x6594FF, 1 returned
//  @0x65950A), "LOCATION" searched (String_FindCaseInsensitiveSubstring called @0x65951C; none:
//  on to the cookies, the jz @0x659528), the redirect enqueued (strtok called @0x65957A,
//  HTTP_RequestEnqueue called @0x659593); every other answer to the SET-COOKIE scan @0x65961B]
std::vector<CookieEntry> response_cookie_batch(int status, const std::vector<std::string> &response_headers);

// The connection tokens NWJoin.dll hands back in the `.joi` response, used to
// reach the hosted game. NK is url_cipher-encoded (host "ip:port"); CK is
// url_cipher-encoded and, decoded, is a DECIMAL the retail client atol()s into
// an int field of its net config and serializes on the wire as the ClientAuth
// **APPID** conn-tag (NOT the BT tag: the host reads the APPID tag into that
// same int field and the BT tag into a different one); NI/NP are the plaintext
// game-node ip/port; BK is the plaintext relay tunnel cookie.
// [orig: URL_ParseConnectionQueryString @0x54dfb0 (CK key "cfhdcegjigecjehcgjdhe")
//  -> UI_JoinSelectedSession @0x5699d0 (`net_config.app_id = atol(&g_NkExtraBuf[64])`
//  = atol(decoded CK), config +0x40); the host side NapiNetConfig_LoadFromConnTags
//  @0x4c7260 ("APPID" -> app_id @0x4C7415, "BT" -> bt) and the compare in
//  Server_ValidatePlayerJoinRequest @0x512100 @0x5122C3 (reject code 9)].
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
	//  @0x4C9E10 - the LN test @0x4C9E65..0x4c9e6c; CNapiGameSession_ConnectOrHost
	//  @0x4D4F10 - g_LobbyNum @0x4d5418, "Lan" @0x4D541F]
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
//  CookieJar_GetEntriesForHost @0x64EA40; the by-name read CookieJar_GetValueByName @0x64EA90; the jar file's save
//  CookieJar_SaveToFile @0x64EE60 (node+0 written @0x64EF16) and load
//  CUIStringTable_LoadFromFile @0x64F290 (node+0 set @0x64F547)]
std::string subnet_key(const std::string &host);

// The menu browser's cookie jar: one list of entries (CookieEntry) for every host, every entry
// attached to every request. Retail computes a host key for each jar access that takes a URL
// (subnet_key) but keeps one node for all of them, so the NovaWorld gate, login, host, join and
// GSB endpoints share one jar here too.
class CookieJar {
public:
	// The browser's form-field write: an entry of `name` and `value`, the rest null, through the
	// store. [orig: CUIBrowser_AddFormField @0x63A5D0 - CookieJar_UpdateFromURL called @0x63A689
	//  with one entry]
	void set(const std::string &name, const std::string &value);
	// The store. A jar with no entry takes the batch whole, in order, duplicates and all (the
	// store's first node). Otherwise each entry in turn updates the first entry whose name
	// matches case-insensitively and whose path matches (the same path, case-insensitively and a
	// null path matching only a null one; or one of the two "/" and the other null or empty): the
	// old entry keeps its name, path, expires, domain and word and takes the new value. An entry
	// that updates none is appended whole, so a later entry of the batch can update it.
	// [orig: CookieJar_UpdateFromURL @0x64E630 - the node walk @0x64E6B0, no node: a new one
	//  @0x64E6BB holding the batch's copy (@0x64E97F); the new entry's name tested @0x64E710, the
	//  name compare called @0x64E723, the path compare called @0x64E73F, an old "/" against a new
	//  empty path (@0x64E758, String_SafeLen called @0x64E768), a new "/" against an old empty
	//  one (@0x64E77D), the value replaced @0x64E7FF and the new entry's name freed and nulled
	//  @0x64E80B..0x64E813 (so nothing past the first match matches), the append's copy of the
	//  whole entry @0x64E8FF..0x64E919; String_CompareNoCase @0x650000 - a null string equals only
	//  a null one]
	void update(const std::vector<CookieEntry> &batch);
	// The by-name read: the value of the first entry whose name matches case-insensitively;
	// nullptr when none does. [orig: CookieJar_GetValueByName @0x64EA90 - String_CompareNoCase
	//  called @0x64EAC8 over each entry's name in order (@0x64EAC0..0x64EADD)]
	const std::string *find(const std::string &name) const;
	// One "name=value;" per entry, in the jar's order: the text of retail's per-entry
	// "Cookie: %s=%s;" header line. Empty when the jar is empty.
	// [orig: CUIBrowser_SendHTTPRequest @0x658840 - the walk @0x658A60..0x658AC9, the format
	//  pushed @0x658A83]
	std::vector<std::string> cookie_header_lines() const;
	bool empty() const { return entries_.empty(); }
	const std::vector<CookieEntry> &entries() const { return entries_; }
	// Every entry dropped: the menu's teardown frees the list.
	// [orig: CUIScene_DestroyAllContent @0x63C490 - CookieJar_FreeHashTableEntries called
	//  @0x63C4A7]
	void clear() { entries_.clear(); }
	// A record of the jar file, appended as the loader appends it: no store, no match.
	// [orig: CUIStringTable_LoadFromFile @0x64F290 - the node's entries grown by one and the
	//  record copied in @0x64F5A7..0x64F5FF]
	void append_loaded(CookieEntry entry) { entries_.push_back(std::move(entry)); }
	// Concatenate every cookie whose name starts with `prefix` as [name\0][value\0]
	// pairs, in insertion order — the retail CD-cookie blob the game-session join
	// relays (the NovaWorld-issued PUB* identity: PUBPCID/PUBNAMEINFO/PUBSQUADINFO/
	// PUBJOINTICKET). Empty when nothing matches.
	// [orig: Config_QueryMatchingEntries @0x64eb70 gather-by-prefix ("PUB*") ->
	//  NetPacket_BuildAnnouncePayload @0x4c4bf0 -> the C2S 0x00 JOIN "CD" TLV
	//  (NapiNP_WriteClientAuthPayload @0x42a180)]
	std::vector<uint8_t> build_prefixed_blob(const std::string &prefix) const;

private:
	std::vector<CookieEntry> entries_;
};

// The menu browser's CHAR cookie as the session apply reads it, "" when the jar holds none or
// an empty one. The apply writes the profile's name as the local player's, then, on the
// NovaWorld transport with the menu up, writes this over it when it is not empty: the join
// sends that name as its ClientAuth NA and a host plays under it. The cookie is found by a
// case-insensitive walk of the jar and read with a 31-byte copy: a value of 31 characters or
// more keeps its first 31 here, where retail's 32-byte buffer runs on into its uninitialized
// last byte (an original bug no service name reaches).
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - the profile's name into
//  g_LocalProfileName (Napi_CopyString called @0x551695), the NovaWorld transport test
//  @0x551A53 / @0x551A6B and the menu test @0x551A79, the CHAR read through
//  CEffect_IncludeHandler_Open (the browser's cookie getter) called @0x551A96 with the size 31
//  pushed @0x551A7F, the copy over g_LocalProfileName @0x551AB0..0x551AC1;
//  CookieJar_GetValueByName @0x64EA90 - String_CompareNoCase over each entry's name, the copy
//  of min(length + 1, 31) @0x64EB2E..0x64EB59; UI_JoinSelectedSession @0x5699D0 - the apply
//  called @0x569B07, g_LocalProfileName into the net config's name (Napi_CopyString called
//  @0x569BE6); CNapiNetwork_StartClientConnection @0x4CA160 - CNapiNPConnection_SetName called
//  @0x4CA235; CNapiGameSession_BuildAndCreateSession - the apply called @0x5695B8;
//  Game_StartMission - the apply called @0x524662]
std::string novaworld_character_name(const CookieJar &jar);

} // namespace opennova
