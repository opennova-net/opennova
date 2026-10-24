#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <net/novaworld/gate_metrics.h> // GateMetricsLocale
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

// The machine locale a METPROTOCOL reporter prints under the gate's METEXT, read at the report:
// the user locale's English country and language names, each "" when its call's result is not
// above 0 in its low byte, and the time zone's Bias.
// [orig: Server_SendPingMetricsToGate @0x511BF0 - GetLocaleInfoA(0x400, 0x1002, .., 0x200)
//  @0x511D88 (`test al, al` @0x511D8A), 0x1001 @0x511DE6, GetTimeZoneInformation @0x511E3D]
bool read_gate_metrics_locale(GateMetricsLocale &out);

// The volume and first-Ethernet-adapter inputs retail's NWPSSK / NWUSID derive from
// (make_retail_machine_tokens).
// [orig: CDKey_GenerateHardwareFingerprint @0x4a4a00 / _0 @0x4a4d00 — GetVolumeInformationA on the
//  current drive, GetAdaptersInfo's first MIB_IF_TYPE_ETHERNET address]
bool read_retail_machine_inputs(RetailMachineInputs &out);

// The executable's file name as retail reads it off its process command line: the first token
// of the quote-aware tokenizer, from past its last backslash on (a '/' is kept); "" for a line with
// no token. [orig: Napi_GetExeBasename @0x62E0B0 - GetCommandLineA @0x62E11B,
//  String_TokenizeQuotedToArray (the call @0x62E133, its count @0x62E13E), Path_BasenameInPlace
//  @0x62E14D (@0x617530, the backslash test @0x617550)]
std::string exe_basename_from_command_line(std::string_view command_line);

// The running process's executable file name, what the lobby connection's AP and its 0x42's
// Application CU carry, and the in-match connection's AP too (every 0x41 / 0x42 / 0x81 builder
// through make_title_client_hello, cut to 63; CNapiNetwork_Init @0x4CA4A0, the copy to
// proto+0x58 @0x4CA759): on Windows exe_basename_from_command_line over the process command line;
// elsewhere the first argument's part past its last '/', the platform's separator; "" when the
// platform gives none.
std::string read_exe_basename();

// The executable's file name as the NovaWorld menu's reads see it, the lobby connection's AP and
// its 0x42's Application CU: the main loop's entry uppercases the process command line in place
// (the CRT _strupr under the ".ACP" LC_CTYPE the boot set, cp1252_strupr) after the boot's
// command-line parse and network init, so every later read of it gives "JOINTOPS.EXE", where the
// boot's reads (the game protocol's AP the network init keeps, the connect log's EXE line) keep
// the file's own case (read_exe_basename).
// [orig: WinMain @0x763A80 - Game_ParseCommandLineAndInit called @0x763B5B (CNapiNetwork_Init
//  called @0x4A7346), GetCommandLineA called @0x763BA2 and its buffer handed to Game_Run called
//  @0x763BA9; Game_Run @0x4A7FB0 - _strupr over it called @0x4A7FCF, ahead of the main loop;
//  System_InitTimerAndLocale @0x762A00 - setlocale(LC_ALL, ".ACP") called @0x762A6E, from WinMain
//  ahead of Game_Run]
std::string read_menu_exe_basename();

// The lobby connection's AP keeps 63 characters (Napi_CopyString(.., 0x40)) and its 0x42's
// Application CU 511 (the 0x200 read). [orig: CNapiGameSession_InitNPConnection @0x4d3cd2;
//  CNapiGameSession_ConnectToNovaWorld @0x4d4780]
inline constexpr std::size_t kLobbyApChars = 63;
inline constexpr std::size_t kLobbyApplicationChars = 511;

// The stand-in inputs where retail's are unavailable (everything but Windows): the embedder's
// platform-stable opaque id, hashed (FNV-1a) so no raw machine identifier reaches the wire,
// supplies by position the words the retail reads would: the volume serial, the maximum component
// length, the filesystem flags and the first Ethernet adapter's six bytes (drawn in that order; no
// volume or filesystem name). The inputs are OpenNova's own (D-NET-NEW-L9-8, PERMANENT, the inputs
// facet).
RetailMachineInputs fallback_machine_inputs(std::string_view stable_identity);

// The stand-in tokens: retail's two encoders (make_retail_machine_tokens) over
// fallback_machine_inputs, so each token has retail's shape: NWPSSK's 23 position-banded letters
// (A..P, C..R, E..T in turn), NWUSID's 8 pairs, pair i in 'A'+i..'A'+i+15.
// [orig: CDKey_GenerateHardwareFingerprint @0x4A4A00 - the bands @0x4A4BBA / @0x4A4BE3 / @0x4A4BCD;
//  CDKey_GenerateHardwareFingerprint_0 @0x4A4D00 - the pairs @0x4A4E46..0x4A4E4B]
LobbyMachineTokens fallback_machine_tokens(std::string_view stable_identity);

} // namespace opennova
