#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <net/novaworld/gate_response.h>

namespace opennova {

// NovaWorld gate probe — the UDP request/response to gs.novaworld.net:7597
// that yields the POSTIPADDRESS/UDPNOVAWORLD/STARTUPURL/etc. VARs.
//
// Witnessed in retail Jointops.exe. The gate manager's initialiser stores the
// host, port and title tag; the probe thread then:
//
//   1. copies the tag literal into a local buffer and appends a NUL
//      (`strlen + 1` is the exact byte range fed to the cipher @0x633aa1);
//   2. runs NapiNP_EncryptBuffer(buf, len, "GATEAPI") @0x633aaf — the ADD
//      chain, which is our nwu_decrypt (nwu.h name-swap);
//   3. hands the buffer to NapiSocket_SendTo(.., encrypt=1) @0x633ade, whose
//      encrypt path wraps it in the NAPI CRC envelope
//      (NapiSocket_EncodePacket @0x62c8f0) before sendto — the envelope is
//      applied at the SOCKET layer, not by the probe thread;
//   4. on recv, runs NapiNP_DecryptBuffer(buf, len, "GATEAPI") @0x633bcc
//      (the SUB chain, our nwu_encrypt) over the envelope-stripped bytes to
//      recover the plaintext VAR-tagged response.
//
// [orig: CNapiGateManager_Init @0x633f90 (host "novaworld.net", port 7597,
//  3000 ms retry, 30000 ms timeout); CNapiGateManager_ProbeThreadProc
//  @0x6339e0; NapiSocket_SendTo @0x62cfa0; NapiSocket_EncodePacket @0x62c8f0]
// (The demo-era anchors were jodemo.exe's CNapiGateManager_Init @0x4aecf0,
//  sub_4AD130 and sub_5F5420 — jodemo addresses only: in retail 0x4aecf0 is
//  Entity_ComputeBoneCollisionForce.)

// Gate server hostname. [orig: "gs.novaworld.net" @0x7cc368]
inline constexpr const char *GATE_DEFAULT_HOST = "gs.novaworld.net";

// Gate server UDP port. [orig: CNapiGateManager_Init @0x633f90 stores 7597]
inline constexpr uint16_t GATE_DEFAULT_PORT = 7597;

// Protocol tag the probe ships as its payload; the literal differs per
// NovaLogic title and is how the gate distinguishes which title is calling
// in. Retail Joint Operations installs "jop:cus2" as the session's gate tag
// [orig: CNapiGameSession ctor Napi_CopyString(this+76, "jop:cus2", 64)
//  @0x4d14b6; literal @0x7cc35c]. The demo binary (jodemo.exe) ships
// "jopd:cus4"; it stays here only as the witnessed demo value.
inline constexpr const char *GATE_PROBE_TAG_JOINTOPS = "jop:cus2";
inline constexpr const char *GATE_PROBE_TAG_JODEMO = "jopd:cus4";

// NWU cipher key used to obfuscate both request and response.
// [orig: "GATEAPI" @0x7e0524]
inline constexpr const char *GATE_NWU_KEY = "GATEAPI";

// Build the INNER gate-probe payload: `tag` + trailing NUL, run through the
// NWU ADD chain under `nwu_key` — exactly the bytes the probe thread hands to
// the socket layer. It is NOT a complete datagram: the caller that owns the
// socket must wrap the result in the NAPI CRC envelope
// (napi_envelope_encode) before sendto, as NapiSocket_SendTo's encrypt path
// does in retail. There is no default tag — the caller names its title.
std::vector<uint8_t> gate_probe_build(std::string_view tag,
                                      std::string_view nwu_key = GATE_NWU_KEY);

// Decode an inbound gate response body (the bytes AFTER the caller has
// stripped the NAPI CRC envelope). Runs the NWU SUB chain under `nwu_key`
// (matching NapiNP_DecryptBuffer on recv) and then gate_response_parse on
// the recovered plaintext. Returns true iff at least one VAR line was
// absorbed. The input buffer is not mutated; the plaintext is copied into an
// internal std::string for parsing.
bool gate_response_decrypt_and_parse(const uint8_t *data, size_t len,
                                     GateResponse &out,
                                     std::string_view nwu_key = GATE_NWU_KEY);

} // namespace opennova
