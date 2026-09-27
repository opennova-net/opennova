#pragma once

#include <string>
#include <string_view>

namespace opennova {

// Parsed registration URL emitted by the NovaLogic launcher and consumed by
// URL_ParseConnectionQueryString @0x54dfb0 in retail Jointops.exe (jodemo:
// Auth_ParseRegistrationURL @0x514c40). Two mutually exclusive shapes on
// the wire:
//
//   1. Host flow:    nw://.../?HOSTKEY=<val>
//      `has_host_key` is true; only `host_key` is populated.
//
//   2. Client flow:  nw://.../?NK=<cipher>&CK=<cipher>&NI=<plain>&NP=<plain>&BK=<plain>&LN=<n>&GS=<plain>
//      `has_host_key` is false; the client-flow fields are populated.
//      NK= after URL-cipher decrypt is an "IpAddress:PortNumber" string
//      (CNapiGameSession_ConnectOrHost feeds the head into the "IpAddress"
//      VarList entry and the tail into "PortNumber"); the RegistrationUrl
//      splits it at the witnessed ':' into `name_key` / `name_key_suffix`.
//      LN is the lobby number: when nonzero the transport dials the
//      LAN-discovered endpoint instead of the NK relay pair and reports the
//      number as the session's "Lan" var. GS is copied by the parser but no
//      retail code ever reads the buffer (a single xref: the parse call).
//      [orig: LN `atol` @0x54e33e; GS `strcpy(g_GsBuf, ..)` @0x54e38a;
//       consumers CNapiGameSession_InitTransportConnection @0x4c9e6c
//       (`if (g_LobbyNum)` selects byte_C8FE7C/byte_C8FEBC over g_NkBuf /
//       g_NkExtraBuf) and CNapiGameSession_ConnectOrHost @0x4d5418 ("Lan")]
//
// Only the NK= and CK= values are cipher-obfuscated (novacrypto::
// url_cipher_decode with the respective key literals). NI/NP/BK/LN/GS are
// plaintext and terminated by '&'.

struct RegistrationUrl {
	bool has_host_key = false;

	// When has_host_key: the HOSTKEY= value, trimmed at '&' and then at ']'
	// [orig: delimiters "&" @0x7d3f20 then "]" @0x7c18e4].
	std::string host_key;

	// When !has_host_key: the client-flow fields.
	std::string name_key;           // NK= after URL-cipher decode + split
	std::string name_key_suffix;    // NK= tail after splitting at ':'
	std::string cd_key;             // CK= after URL-cipher decode
	std::string name_info;          // NI= plaintext
	std::string player_name;        // NP= plaintext
	std::string bank_key;           // BK= plaintext
	int ln = 0;                     // LN= lobby number (atol); 0 when absent
	std::string gs;                 // GS= plaintext; parsed, inert in retail
};

// NK-split separator, witnessed: `strstr(g_NkBuf, delimiters)` with
// asc_7C3B58 @0x7c3b58 == ":", and the tail copied with a single-byte skip
// (`sprintf(g_NkExtraBuf, "%s", ni_len + 1)`). Overridable at call-time
// for synthetic tests only.
inline constexpr const char *DEFAULT_NK_SEPARATOR = ":";

// Parse a NovaLogic launcher registration URL. Returns true on success.
// `nk_separator` is used to split the decoded NK value (see comment above).
bool registration_url_parse(std::string_view url,
                            RegistrationUrl &out,
                            std::string_view nk_separator = DEFAULT_NK_SEPARATOR);

} // namespace opennova
