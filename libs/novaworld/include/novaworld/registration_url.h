#pragma once

#include <string>
#include <string_view>

namespace opennova {

// Parsed registration URL emitted by the NovaLogic launcher and consumed
// by Auth_ParseRegistrationURL@0x514c40 in jodemo.exe. Two mutually
// exclusive shapes on the wire:
//
//   1. Host flow:    nw://.../?HOSTKEY=<val>
//      `has_host_key` is true; only `host_key` is populated.
//
//   2. Client flow:  nw://.../?NK=<cipher>&CK=<cipher>&NI=<plain>&NP=<plain>&BK=<plain>
//      `has_host_key` is false; all six NK/CK/NI/NP/BK fields populated.
//      Note: NK= after URL-cipher decrypt is actually an IP:port-like
//      string in jodemo's usage (witnessed in CNapiGameSession_ConnectOrHost
//      feeding NK into the "IpAddress" VarList entry and the post-':'
//      suffix into "PortNumber"). The RegistrationUrl splits NK at the
//      pszSet separator into `name_key` (head) and `name_key_suffix`
//      (tail) to preserve that semantic.
//
// Only the NK= and CK= values are cipher-obfuscated (novacrypto::
// url_cipher_decode with the respective key literals). NI/NP/BK are
// plaintext and terminated by '&'.

struct RegistrationUrl {
	bool has_host_key = false;

	// When has_host_key: the HOSTKEY= value, trimmed of any trailing ']'.
	std::string host_key;

	// When !has_host_key: the six client-flow fields.
	std::string name_key;           // NK= after URL-cipher decode + split
	std::string name_key_suffix;    // NK= tail after splitting at `pszSet`
	std::string cd_key;             // CK= after URL-cipher decode
	std::string name_info;          // NI= plaintext
	std::string player_name;        // NP= plaintext
	std::string bank_key;           // BK= plaintext
};

// NK-split separator. Witnessed only as a reference-by-address
// (`(const char *)&pszSet`) in Auth_ParseRegistrationURL; its literal
// content has not yet been pulled from the binary. We default to ":" —
// the natural guess given NK= is consumed as "IpAddress:PortNumber" by
// the session layer. Override at call-time if the eventual witness shows
// a different string.
inline constexpr const char *DEFAULT_NK_SEPARATOR = ":";

// Parse a NovaLogic launcher registration URL. Returns true on success.
// `nk_separator` is used to split the decoded NK value (see comment above).
bool registration_url_parse(std::string_view url,
                            RegistrationUrl &out,
                            std::string_view nk_separator = DEFAULT_NK_SEPARATOR);

} // namespace opennova
