#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

// NAPI LSB-scatter CRC envelope (4-byte header mode).
//
// Witnessed byte-exact in jodemo.exe:
//   NapiPacket_EncodeWithCRC@0x5f7540  (encoder)
//   NapiPacket_DecodeWithCRC@0x5f7670  (decoder — also handles a variable-
//     size header mode activated when the first dword is zero, with header
//     size stored at byte offset +9; that mode is not emitted by this port
//     and is therefore also not accepted by napi_envelope_decode below.
//     See notes/net_verification_log.md for the full wire analysis.)
//
// Wire format (4-byte header mode):
//   offset size   meaning
//   +0     4      header dword (LE). If payload length >= 32, this
//                 carries the ORIGINAL low bits of the first 32 payload
//                 bytes — bit i == low_bit(payload[i]) — and those same
//                 32 payload bytes have their low bits REPLACED with the
//                 corresponding bit of the CRC-32 of the (original)
//                 payload. If payload length < 32, this is the CRC-32
//                 of the payload directly (no scatter applied).
//   +4     N      payload (possibly with low bits of first 32 bytes
//                 replaced by CRC bits — see above).
//
// Overhead: fixed +4 bytes, same for both modes.

// Encode: wrap `src[0..src_len)` into an LSB-scatter envelope written to
// `out[0..out_size)`. Requires `out_cap >= src_len + 4`. Returns 0 on
// success and -1 on invalid input or insufficient output capacity.
int napi_envelope_encode(const uint8_t *src, size_t src_len,
                         uint8_t *out, size_t out_cap, size_t *out_size);

// Decode: unwrap a 4-byte-header envelope. Writes the restored payload to
// `out[0..out_size)` (low bits restored, CRC verified). Requires
// `out_cap >= packet_len - 4`. Returns 0 on success, -1 on invalid input
// or CRC mismatch.
int napi_envelope_decode(const uint8_t *packet, size_t packet_len,
                         uint8_t *out, size_t out_cap, size_t *out_size);

} // namespace opennova
