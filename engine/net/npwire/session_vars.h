#pragma once

// The session-variable list: the key/value stream the authority serializes at
// mission start and every client parses into its g_SessionVar* globals — the
// server name and mission title the loading screen and the Tab board's header
// read, the game type, the custom text, the mission file and the score-tone
// thresholds. It rides the S2C 0x60 server-info transfer to a joiner; a
// listen host parses its own copy (net-re §5.1, hud-re "The MP legs").
// [orig: Game_SerializeMissionInfoToDataStream @0x523620 (the stream + the
//  authority's direct copies); Client_ParseServerSessionVariables @0x5202f0;
//  the host's self-parse SaveFile_SendAndWaitForServerAck @0x5204c4..0x5204f5;
//  the readers HUD_GetLoadingScreenTextByGameType @0x51f300]

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

struct SessionVars {
	std::string server_name;  // g_SessionVarServerName @0x24C1400
	std::string mission_name; // g_SessionVarMissionName @0x24C13C0
	uint32_t game_type = 0;   // g_SessionVarGameType @0x24C13B8
	std::string custom_text;  // g_SessionVarCustomText @0x24C11B8
	std::string mission_file; // g_SessionVarMissionFileName @0x24C1178
	uint16_t exp_fanfare = 0; // g_SessionVarExpFanfare @0x24D5A10
};

// The stream, in the serializer's key order: SERVERNAME, MISSIONNAME,
// GAMETYPE (u32), CUSTOMTEXT, MISSIONFILENAME, EXP_FANFARE (u16). Each entry is
// the NUL-terminated key, a u32 LE length, then the value bytes; a string
// value carries its NUL.
// [orig: Game_SerializeMissionInfoToDataStream @0x523620 -- sub_4555C0 key,
//  sub_455560 length, CDataStream_Write value per field]
std::vector<uint8_t> encode_session_vars(const SessionVars &vars);

// The client parse. The five named globals clear first (EXP_FANFARE does not:
// it keeps its previous value when the stream omits the key); a null or empty
// stream leaves them cleared. Each entry's key matches case-insensitively and
// the strings copy through Napi_CopyString's caps -- the server name 31
// characters, the mission name and file 63, the custom text 511. A truncated
// entry ends the walk (retail reads unchecked; a stock stream never
// truncates).
// [orig: Client_ParseServerSessionVariables @0x5202f0 -- the clears
//  @0x520311..0x520329, Napi_StrCaseEqual per key, Napi_CopyString(.., 32)
//  @0x5203b5, (.., 64) @0x5203e0, (.., 512) @0x52042e, (.., 64) @0x520456,
//  the u32 @0x520405, the u16 @0x520478]
void decode_session_vars(const uint8_t *data, std::size_t size, SessionVars &vars);

} // namespace opennova
