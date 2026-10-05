#pragma once

#include <string_view>

#include <net/novaworld/lobby_vars.h> // LobbyIdentityParams / RetailMachineInputs / LobbyMachineTokens

namespace opennova {

// The host-OS reads behind the verify Cookie's identity set, for any embedder (the game shell, the
// opennova-nw-lister app). Each returns false, leaving `out` alone, where the platform has no such API
// (everything but Windows); the embedder then supplies its own values.

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

// The stand-in tokens where retail's inputs are unavailable: the embedder's platform-stable opaque
// id, hashed (FNV-1a) so no raw machine identifier reaches the wire, then the same fixed-length A-Z
// encoding at the wire-load-bearing kNwpsskLen / kNwusidLen. The xor seeds are OpenNova's own.
LobbyMachineTokens fallback_machine_tokens(std::string_view stable_identity);

} // namespace opennova
