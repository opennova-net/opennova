#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NovaWorld server-browser binary (GSB) response — the wire format the
// retail Joint Operations / DF:X2 IB3 browser consumes from
// `/jop_2.gsb?a=1`. Ported byte-for-byte from onnet `onnw/gsb.py`.
//
// Top-level layout:
//   "GSB " (4 bytes literal, no encryption)
//   <IVAR chunk>
//   <FLDS chunk> — summary KV pairs (TotalServers, TotalPlayers, ...)
//   <SVRS chunk> — field-name table
//   <SVRS chunk> — per-server rows
//   <XXXX chunk> — terminator (empty payload)
//
// Each chunk:
//   [u32 encrypted_payload_length]
//   [nwu_encrypt(payload, GSB_NWU_KEY)]
//   [4-byte magic trailer identifying the chunk kind]
//
// Notes:
// - The magic tag is the LAST 4 bytes of each chunk (not the first).
// - Each chunk's payload is encrypted INDEPENDENTLY with the 22-digit
//   GSB_NWU_KEY. This is the inverse operation of what the client's
//   parser does on decode (`PFF_EncryptBuffer`), so we call `nwu_encrypt`
//   here (our encrypt is the client's decode).
// - Contrast with the older demo jodemo.exe GLB format (header "GLB ",
//   per-chunk [tag][len][payload], whole-buffer NWU with key "NOVAWORLD",
//   different chunk tags). The demo parser at
//   `NapiGameList_ParseFromBuffer@0x6f5a50` reads GLB. Retail expects GSB.
//   Emitting GSB breaks the demo server-browser UI; we accept that because
//   demo lacks FORM_POST / GLB_JOIN and is a dead end for the Join flow.
//
// NWU key: literal 22-digit string used by onnet's builder. Retail's
// decoder uses the same key (onnet is the reference implementation that
// works against retail).
inline constexpr const char *GSB_NWU_KEY = "3209452104342624532341";

// Per-server wire data. Defaults mirror onnet's `_extract_server_fields`
// (`onnw/gsb.py:92-123`) — "Y"/"N" for booleans, ver1="3" for the
// Joint-Ops family, empty strings where onnet omits a value.
struct GsbServerEntry {
	uint32_t rid = 0;
	std::array<uint8_t, 4> ip{0, 0, 0, 0};
	uint16_t port = 0;

	// FIELDS emitted in the order `onnw/gsb.py::FIELD_NAMES` lists them.
	// Names match onnet's casing — client field lookup is case-insensitive
	// but matching retail casing keeps hex dumps readable against onnet's.
	std::string server_name = "Unnamed Server";  // ServerName
	std::string game_type   = "COOP";            // GameType
	std::string mission_name = "";               // MissionName
	std::string region       = "";               // Region
	int players              = 0;                // Players
	int max_players          = 0;                // MaxPlayers
	std::string dedicated    = "Y";              // Dedicated   (Y/N)
	std::string time_left    = "NA";             // TimeLeft
	std::string password     = "N";              // Password    (Y/N)
	std::string country      = "";               // Country
	std::string msg          = "";               // Msg
	std::string age          = "0 00:00:00";     // Age
	std::string time_of_day  = "";               // TimeOfDay
	std::string stat         = "N";              // Stat
	std::string level_range  = "";               // LevelRange
	std::string locked       = "N";              // Locked      (Y/N)
	std::string tracers      = "Y";              // Tracers     (Y/N)
	std::string skins        = "N";              // Skins       (Y/N)
	std::string bb_mode      = "0";              // BBMode
	std::string mod          = "";               // Mod
	std::string pix          = "1";              // PIX
	std::string pb_server    = "0";              // PBSERVER    (onnet uppercase)
	std::string ver1         = "3";              // VER1        (onnet uppercase)
	std::string exp          = "";               // Exp
	std::string exp_bits     = "3";              // Expbits
	std::string joicon2      = "4000";           // Joicon2
};

// Build the complete GSB response body as it appears on the wire.
// Caller writes this verbatim as the HTTP body (Content-Type
// `application/octet-stream`).
std::vector<uint8_t> gsb_build_response(const std::vector<GsbServerEntry> &servers);

} // namespace opennova
