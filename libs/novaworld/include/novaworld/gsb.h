#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NovaWorld server-browser binary (GSB) response — the wire format the retail
// Joint Operations / DF:X2 IB3 browser consumes from `/jop_2.gsb?a=1`.
//
// Witnessed against the retail parser `NapiGameList_ProcessEncryptedResponse
// @ 0x63d740` (docs/net/novaworld-net-re.md §7 Waves 7+9, D-NET-32..36 +
// D-NET-190..193). The blob
// is a FLAT sequence of chunks — there is NO leading bare "GSB " file header;
// "GSB " is simply the first chunk's tag (the init/reset record).
//
// Each chunk (magic is the PREFIX, not a trailer):
//   [4-byte magic tag][u32 LE payload_length][payload], advance by length+8.
//
// Chunk tags:
//   "GSB " (0x20425347) — init/reset; payload begins with u32 0x00010000.
//   "FLDS" (0x53444C46) — field-name table: [u16 LE count][count × NUL-term name].
//   "SVRS" (0x53525653) — server rows: [u16 LE count][count × row].
//   "XXXX" (0x58585858) — terminator.
//
// Server row (positional, keyed by the FLDS field-name table):
//   [u32 LE rid][4-byte server IPv4, in_addr byte order (a.b.c.d on the wire)]
//   [field_count × (ASCII value + NUL)]   — one per FLDS name, in order
//   [u16 LE player_count][player_count × (player name + NUL)]
//
// The first row u32 is the host id spliced over `@RID@` in the markup join URL
// (`NWJoin.dll?...&rid=@RID@`) — the browser event handler prints it `%d`
// [orig: CLanServerBrowser_UpdateServerList_0 @ 0x660200, sprintf @ 0x660386].
// Retail's parser locals called it "serverIP", but the witnessed `.204` values
// (e.g. 0x0A002728) are host ids in the join-`rid` range, NOT IPv4 addresses.
// The SECOND dword is the host's real IPv4: on the XXXX finalize retail formats
// entry+4 as an in_addr and pings every row with it
// [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. The join address still
// arrives separately via the NK token from `/NWJoin.dll?rid=`.
//
// Each chunk payload is encrypted INDEPENDENTLY with the 22-digit GSB key.
// Retail decodes with `NapiNP_DecryptBuffer @ 0x618880` (the SUBTRACT chain ==
// our `nwu_encrypt`), so the builder applies the inverse ADD chain
// (`NapiNP_EncryptBuffer @ 0x6187b0` == our `nwu_decrypt`).
//
// Verified byte-for-byte against the genuine `.204` blob
// (fixtures/novaworld/nw204_jop_2.gsb, gsb_real204_decode_test): the 26 FLDS
// columns below and this row layout match retail. (Retail also sends an "IVAR"
// chunk between "GSB " and "FLDS" which its parser — and ours — ignores.)
inline constexpr const char *GSB_NWU_KEY = "3209452104342624532341";

// Per-server wire data. The named fields are emitted/parsed positionally against
// the FLDS field-name table (see GSB_FIELD_NAMES in gsb.cpp); field lookup is
// case-insensitive, matching the retail browser.
struct GsbServerEntry {
	// Row header: [u32 rid][4-byte IPv4]. `rid` is the host id the client
	// substitutes for `@RID@` in the join URL; `ip` is the host's IPv4 in
	// dotted-quad text — emitted/parsed in in_addr byte order (a.b.c.d bytes on
	// the wire), the address retail's browser pings on the XXXX finalize
	// [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. Unparseable/empty emits
	// 0.0.0.0.
	uint32_t rid = 0;
	std::string ip = "0.0.0.0";

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
	std::string pb_server    = "0";              // PBSERVER
	std::string ver1         = "3";              // VER1
	std::string exp          = "";               // Exp
	std::string exp_bits     = "3";              // Expbits
	std::string joicon2      = "4000";           // Joicon2

	// Trailing per-row player-name list (retail row tail). Empty for a server
	// that reports a count but no names. `players` (the count column) is
	// independent of this list.
	std::vector<std::string> player_names{};
};

// Build the complete GSB response body as it appears on the wire (the chunk
// sequence GSB/FLDS/SVRS/XXXX, no bare header). Caller writes this verbatim as
// the HTTP body (`application/octet-stream`).
std::vector<uint8_t> gsb_build_response(const std::vector<GsbServerEntry> &servers);

// Decoded GSB response — what the client recovers from the wire blob.
struct GsbResponse {
	int total_servers = 0;                   // = servers.size() (derived; retail sums declared SVRS counts at ctx+120)
	int total_players = 0;                   // = sum of per-row player_names.size() (retail sums row u16 tails at ctx+124)
	std::vector<std::string> field_names;    // FLDS — the column order (last FLDS record wins)
	std::vector<GsbServerEntry> servers;     // SVRS — the rows (all SVRS records, accumulated)
};

// Parse a GSB response blob into rows — the client-side inverse of
// gsb_build_response, matching retail `NapiGameList_ProcessEncryptedResponse
// @ 0x63d740`: walk [magic][u32 len][payload] chunks (payload decrypted with the
// SUBTRACT chain / our nwu_encrypt under GSB_NWU_KEY); FLDS gives the column
// names; each SVRS row is read positionally ([u32 rid][4-byte IPv4][N values]
// [u16 player_count][names]) and SVRS records ACCUMULATE across the stream; a
// valid "GSB " record resets the accumulated list; undersized record payloads
// are skipped exactly like retail. Returns true on a well-formed blob (through
// the XXXX terminator), false on a short/corrupt buffer. (Retail itself is an
// incremental HTTP callback with no error path — requiring the terminator and
// bounds-checking every in-chunk read is our documented host hardening.)
bool gsb_parse_response(const uint8_t *data, size_t len, GsbResponse &out);

} // namespace opennova
