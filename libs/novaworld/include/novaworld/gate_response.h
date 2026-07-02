#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova {

// Parsed gate-server response, NWU-decrypted and then KV-tokenized.
// The full retail VAR set, witnessed in
// [orig: CNapiGateManager_ProcessResponse @ 0x4ced20] (grilled 2026-06-11;
// the earlier 0x4ad330 citation was wrong — that address is a save-file
// writer). Retail recognizes exactly these 19 keys; any other VAR line is
// counted-but-ignored. The two NW-POST fields are REQUIRED on the success
// path (see gate_response.cpp):
//
//   POSTIPADDRESS        -> post_ip (dotted-quad -> 4 bytes)        REQUIRED
//   POSTIPPORT           -> post_port (decimal uint32)             REQUIRED
//   LOBBYNAME            -> lobby_name (string)
//   METIPADDRESS         -> met_ip (stored as string; used as-is by client)
//   METIPPORT            -> met_port (decimal uint32)
//   METLABEL             -> met_label (string)
//   METPING              -> met_ping (decimal int)
//   METEXT               -> met_ext (decimal int)
//   STARTUPURL           -> startup_url (string)
//   UDPNOVAWORLD         -> udp_novaworld (string, IP:port form)
//   UDPCODE1             -> udp_code1 (string, int as text)
//   UDPCODE2             -> udp_code2 (string, int as text)
//   REFLECTEDIPADDRESS   -> reflected_ip (dotted-quad -> 4 bytes)
//   REFLECTEDPORTNUMBER  -> reflected_port (decimal uint32)
//   USEJUNCTION          -> use_junction (decimal int; relay/junction flag)
//   CLEARJUNCTION        -> clear_junction (decimal int)
//   GLSVSSREQUEST        -> glsvss_request (string; briefing-server request)
//   GLSVSSRIMS           -> glsvss_rims (decimal int)
//   GLSVSSAGRMS          -> glsvss_agrms (decimal int)
//
// The required-field gate (the NW-G1 finding): retail fails the gate
// (state -> -9) unless POSTIPADDRESS and POSTIPPORT are both present, OR
// the junction/direct-connect bypass `dword_B5FD2C` is set from command
// line flags. Our server therefore MUST emit both, which it does
// (apps/novaworld_server/gate_listener.cpp). onnet's omission of them was
// a bug; the reverted PR #37 stack adding them was the correct fix.
//
// CUS / PVT are NOT part of retail JO:CA's ProcessResponse key set (they
// came from a different/older witness). Retained as tolerant extras for
// compatibility but flagged: do not treat them as retail.
//
// Leading line tag (off_7CBEE4) is literal "VAR". Lines shorter than 3
// whitespace-separated tokens (or without a "VAR" tag) are ignored,
// matching the original's `readResult >= 3 && Napi_StrCaseEqual(.., "VAR")`
// gate.

struct GateResponse {
	// IPv4 addresses in NETWORK byte order (0xXXYYZZWW where X.Y.Z.W).
	std::array<uint8_t, 4> post_ip{0, 0, 0, 0};
	uint32_t post_port = 0;

	std::string lobby_name;

	std::string met_ip;
	uint32_t met_port = 0;
	std::string met_label;
	int met_ping = 0;
	int met_ext = 0;

	std::string startup_url;
	std::string udp_novaworld;
	std::string udp_code1;
	std::string udp_code2;

	std::array<uint8_t, 4> reflected_ip{0, 0, 0, 0};
	uint32_t reflected_port = 0;

	int use_junction = 0;
	int clear_junction = 0;
	std::string glsvss_request;
	int glsvss_rims = 0;
	int glsvss_agrms = 0;

	// Not in retail JO:CA's ProcessResponse. Retained only so old callers still
	// compile; the parser leaves them empty while counting the VAR lines.
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
