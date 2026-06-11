#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova {

// Parsed gate-server response, NWU-decrypted and then KV-tokenized.
// Witnessed VARs (from CNapiGateManager_ProcessResponse@0x4ad330):
//
//   POSTIPADDRESS        -> post_ip (dotted-quad -> 4 bytes)
//   POSTIPPORT           -> post_port (decimal uint16)
//   METIPADDRESS         -> met_ip (stored as string; used as-is by client)
//   METIPPORT            -> met_port (decimal uint16)
//   METLABEL             -> met_label (string)
//   METPING              -> met_ping (decimal int)
//   METEXT               -> met_ext (decimal int)
//   STARTUPURL           -> startup_url (string)
//   UDPNOVAWORLD         -> udp_novaworld (string, IP:port form)
//   UDPCODE1             -> udp_code1 (string, int as text)
//   UDPCODE2             -> udp_code2 (string, int as text)
//   REFLECTEDIPADDRESS   -> reflected_ip (dotted-quad -> 4 bytes)
//   REFLECTEDPORTNUMBER  -> reflected_port (decimal uint16)
//
// Two additional VARs are referenced indirectly in the decomp; the
// literals have been resolved:
//   CUS (@0x74694C)      -> cus (string — likely server type / custom tier)
//   PVT (@0x746948)      -> pvt (string — likely private-server flag)
// These get carried through because the original calls setter helpers on
// them, but their exact role isn't in the verification log yet.
//
// Leading line tag (@0x74696C) is literal "VAR". Lines shorter than 3
// whitespace-separated tokens (or without a "VAR" tag) are ignored,
// matching the original's `v7 >= 3 && String_CaseInsensitiveEqual(str1, "VAR")`
// gate.

struct GateResponse {
	// IPv4 addresses in NETWORK byte order (0xXXYYZZWW where X.Y.Z.W).
	std::array<uint8_t, 4> post_ip{0, 0, 0, 0};
	uint16_t post_port = 0;

	std::string met_ip;
	uint16_t met_port = 0;
	std::string met_label;
	int met_ping = 0;
	int met_ext = 0;

	std::string startup_url;
	std::string udp_novaworld;
	std::string udp_code1;
	std::string udp_code2;

	std::array<uint8_t, 4> reflected_ip{0, 0, 0, 0};
	uint16_t reflected_port = 0;

	std::string cus;
	std::string pvt;

	// Count of VAR lines we successfully absorbed. Matches the original's
	// `v6` counter; a zero count is treated as failure by the binary.
	int var_count = 0;
};

// Parse a gate-server response (the plaintext, post-NWU-decrypt body).
// Returns true if at least one VAR line was absorbed. Tolerant of blank
// lines and unknown keys (ignored, consistent with the binary).
bool gate_response_parse(std::string_view body, GateResponse &out);

} // namespace opennova
