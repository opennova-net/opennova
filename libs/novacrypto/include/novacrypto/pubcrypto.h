#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Joint Operations PUB* field encoder/decoder.
//
// Used by /NWJoin.dll to wrap the joiner's PCID + name/squad info under the
// host's per-connection `pcid_key` (delivered via ClientHostUpdate.PCIDKey)
// before retail's IB3 stuffs the result into PUBPCID/PUBNAMEINFO/PUBSQUADINFO
// cookies and forwards them to the game-server peer for verification.
//
// Wire layout:
//   1. CRC-32/MPEG-2 of plaintext (big-endian fold using the same table as
//      the NAPI envelope), appended LE to the plaintext.
//   2. Four-phase byte permutation keyed by `pcid_key`:
//        a) keycycle (add key bytes mod 256)
//        b) optional reverse (depends on a key-derived state bit)
//        c) sequential add (add a key-derived ramp)
//        d) pseudorandom add (16-bit LCG, seeded from key)
//   3. A-P alphabet pack: each byte → two chars, low nibble first ('A'+nibble),
//      e.g. 0x42 → "CE" ('A'+2, 'A'+4).
//
// 1:1 port of `onnw/protocol/pubcrypto.py::encode_pub_value` /
// `decode_pub_value`. Tests in
// tests/novacrypto/pubcrypto_test.cpp cross-check against Python via
// pre-computed fixtures.

// Encrypt + A-P encode `plaintext` under `pcid_key`. `pcid_key` must be
// non-empty (ASCII; key-fold treats it as latin-1 bytes). Returns the
// encoded ASCII string of length `2 * (plaintext.size() + 4)`.
std::string encode_pub_value(const std::vector<uint8_t> &plaintext,
                             const std::string &pcid_key);

// Convenience overload for std::string plaintext.
std::string encode_pub_value(const std::string &plaintext,
                             const std::string &pcid_key);

// Reverse: decode + verify CRC. Returns the recovered plaintext (without
// the trailing 4-byte CRC). Throws std::runtime_error on CRC mismatch,
// odd-length input, or out-of-range A-P character.
std::vector<uint8_t> decode_pub_value(const std::string &encoded,
                                      const std::string &pcid_key);

} // namespace opennova
