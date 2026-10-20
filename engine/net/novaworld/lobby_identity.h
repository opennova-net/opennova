#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <net/novaworld/lobby_vars.h> // LobbyIdentityParams / RetailMachineInputs / LobbyMachineTokens

namespace opennova {

// The host-OS reads behind the verify Cookie's identity set, for any embedder (the game shell,
// opennova-serve's listing). Each returns false, leaving `out` alone, where the platform has no
// such API (everything but Windows); the embedder then supplies its own values.

// The locale trio the Cookie leads with: the English country and language names and the base
// time-zone bias (UTC = local + Bias, not the daylight-adjusted offset).
// [orig: CNapiSession_ReadLocaleInfo @0x4ce390 — GetLocaleInfoA LOCALE_SENGCOUNTRY /
//  LOCALE_SENGLANGUAGE, GetTimeZoneInformation's Bias]
bool read_locale_identity(LobbyIdentityParams &out);

// The volume and first-Ethernet-adapter inputs retail's NWPSSK / NWUSID derive from
// (make_retail_machine_tokens).
// [orig: CDKey_GenerateHardwareFingerprint @0x4a4a00 / _0 @0x4a4d00 — GetVolumeInformationA on the
//  current drive, GetAdaptersInfo's first MIB_IF_TYPE_ETHERNET address]
bool read_retail_machine_inputs(RetailMachineInputs &out);

// The executable's file name as retail reads it off its process command line: the first token
// of the quote-aware tokenizer, from past its last backslash on (a '/' is kept); "" for a line with
// no token. [orig: Napi_GetExeBasename @0x62E0B0 - GetCommandLineA @0x62E11B,
//  String_TokenizeQuotedToArray @0x62E13E, Path_BasenameInPlace @0x62E14D (@0x617530, the backslash
//  test @0x617550)]
std::string exe_basename_from_command_line(std::string_view command_line);

// The running process's executable file name, what the lobby connection's AP and its 0x42's
// Application CU carry: on Windows exe_basename_from_command_line over the process command line;
// elsewhere the first argument's part past its last '/', the platform's separator; "" when the
// platform gives none.
std::string read_exe_basename();

// The lobby connection's AP keeps 63 characters (Napi_CopyString(.., 0x40)) and its 0x42's
// Application CU 511 (the 0x200 read). [orig: CNapiGameSession_InitNPConnection @0x4d3cd2;
//  CNapiGameSession_ConnectToNovaWorld @0x4d4780]
inline constexpr std::size_t kLobbyApChars = 63;
inline constexpr std::size_t kLobbyApplicationChars = 511;

// The stand-in tokens where retail's inputs are unavailable: the embedder's platform-stable opaque
// id, hashed (FNV-1a) so no raw machine identifier reaches the wire, then the same fixed-length A-Z
// encoding at the wire-load-bearing kNwpsskLen / kNwusidLen. The xor seeds are OpenNova's own.
LobbyMachineTokens fallback_machine_tokens(std::string_view stable_identity);

} // namespace opennova
