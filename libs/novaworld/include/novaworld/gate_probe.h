#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <novaworld/gate_response.h>

namespace opennova {

// NovaWorld gate probe — the UDP request/response to gs.novaworld.net:7597
// that yields the POSTIPADDRESS/UDPNOVAWORLD/STARTUPURL/etc. VARs.
//
// Witnessed by tracing CNapiGateManager_Init@0x4aecf0 (stores host tag),
// sub_4AD130 (drives the ping socket with the tag and port as
// parameters), and sub_5F5420 (the ping-thread body) which:
//
//   1. copies the tag literal ("jopd:cus4") into a local buffer and
//      appends a NUL;
//   2. calls Crypto_DecryptBuffer(buf, len, "GATEAPI") — despite the
//      name, this transforms plaintext into ciphertext on the outbound
//      side because the NWU phase chain is symmetric when applied to
//      either direction;
//   3. sendto's the transformed buffer to (resolved host, port 7597);
//   4. on recv, calls PFF_EncryptBuffer(buf, len, "GATEAPI") to recover
//      the plaintext VAR-tagged response.
//
// Constants (all IDA-verified):

// Gate server hostname. Stored at CNapiGateManager+8 by Init, literal at
// .rdata:0x746ac8.
inline constexpr const char *GATE_DEFAULT_HOST = "gs.novaworld.net";

// Gate server UDP port. Stored at CNapiGateManager+72 by Init.
inline constexpr uint16_t GATE_DEFAULT_PORT = 7597;

// Protocol tag the probe ships as its payload. Stored at
// CNapiGateManager+76 by Init; the literal differs per-binary and is
// how the gate distinguishes which NovaLogic title is calling in.
// Witnessed variants so far:
//   `jopd:cus4` — Joint Operations Demo (jodemo.exe, .rdata:0x746abc).
//   `jop:cus2`  — retail Joint Operations (observed live 2026-04-23,
//                 first probe on :7597 from the shipping binary).
// Additional NovaLogic titles (Delta Force: Xtreme 2 etc.) would ship
// their own tags; add here as they get witnessed.
inline constexpr const char *GATE_PROBE_TAG_JODEMO = "jopd:cus4";
inline constexpr const char *GATE_PROBE_TAG_JOINTOPS = "jop:cus2";

// NWU cipher key used to obfuscate both request and response. Literal at
// .rdata:0x75bc14.
inline constexpr const char *GATE_NWU_KEY = "GATEAPI";

// Build the outbound gate-probe payload. Allocates a buffer containing
// `tag` + trailing NUL, then runs novacrypto's NWU decrypt over it using
// `nwu_key` — the same transformation the binary applies via
// Crypto_DecryptBuffer on the send path.
std::vector<uint8_t> gate_probe_build(std::string_view tag = GATE_PROBE_TAG_JODEMO,
                                      std::string_view nwu_key = GATE_NWU_KEY);

// Decode an inbound gate response. Decrypts with `nwu_key` (matching the
// binary's PFF_EncryptBuffer on recv) and then runs gate_response_parse
// on the recovered plaintext. Returns true iff at least one VAR line was
// absorbed. The input buffer is not mutated; the plaintext is copied
// into an internal std::string for parsing.
bool gate_response_decrypt_and_parse(const uint8_t *data, size_t len,
                                     GateResponse &out,
                                     std::string_view nwu_key = GATE_NWU_KEY);

} // namespace opennova
