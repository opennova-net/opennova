#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

// NAPI LSB-scatter CRC envelope.
//
// Witnessed byte-exact in jodemo.exe:
//   NapiPacket_EncodeWithCRC@0x5f7540  (encoder)
//   NapiPacket_DecodeWithCRC@0x5f7670  (decoder — also handles a variable-
//     size header mode activated when the first dword is zero, with header
//     size stored at byte offset +9. The port does not emit that form, but the
//     decoder accepts it and retains retail's four-byte fallback.)
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
// Encoder overhead is fixed at +4 bytes. An accepted extended packet's
// overhead is the header-size byte stored at +9.

// Encode: wrap `src[0..src_len)` into an LSB-scatter envelope written to
// `out[0..out_size)`. Requires `out_cap >= src_len + 4`. Returns 0 on
// success and -1 on invalid input or insufficient output capacity.
int napi_envelope_encode(const uint8_t *src, size_t src_len,
                         uint8_t *out, size_t out_cap, size_t *out_size);

// Decode: unwrap either witnessed header form. Writes the restored payload to
// `out[0..out_size)` (low bits restored, CRC verified). A zero first dword
// with header_size > 4 is first tried as the variable-header form and then as
// a valid four-byte form whose scatter carrier happens to be zero, matching
// retail's gated fallback. A zero carrier with header_size <= 4 is rejected.
// Returns 0 on success and -1 on invalid input, insufficient output capacity,
// or CRC mismatch.
int napi_envelope_decode(const uint8_t *packet, size_t packet_len,
                         uint8_t *out, size_t out_cap, size_t *out_size);

} // namespace opennova
