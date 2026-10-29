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

// Encrypt + A-P encode `plaintext` under `pcid_key` (ASCII; key-fold treats it
// as latin-1 bytes) into `out`, the encoded ASCII string of length
// `2 * (plaintext.size() + 4)`. False (`out` emptied) for an empty key: the
// service encodes only under the host's PCIDKey, and a key it cannot encode
// under is a result, never a throw (ADR 0049 d5).
bool encode_pub_value(const std::vector<uint8_t> &plaintext, const std::string &pcid_key,
                      std::string &out);

// Convenience overload for std::string plaintext.
bool encode_pub_value(const std::string &plaintext, const std::string &pcid_key,
                      std::string &out);

// Reverse: decode + verify CRC into `out`, the recovered plaintext (without
// the trailing 4-byte CRC). False (`out` emptied, `error` naming why when
// given) on an empty key, an odd length or a character outside A-P, a payload
// shorter than its CRC, or a CRC mismatch: a value off the wire is input, so
// a bad one is a result, never a throw (ADR 0049 d5).
bool decode_pub_value(const std::string &encoded, const std::string &pcid_key,
                      std::vector<uint8_t> &out, std::string *error = nullptr);

// The non-throwing per-key probe over already A-P-decoded bytes: false for an
// empty key, fewer than four bytes or a CRC mismatch; `payload` is the bytes
// before the CRC.
bool try_decrypt_pub_bytes(std::vector<uint8_t> data, const std::string &pcid_key,
                           std::vector<uint8_t> &payload);

// The same codec over a key chain, the keys separated by ':' ("jop:2:oyez",
// gt.ssc's): encoding takes the keys left to right, each a CRC appended and
// the four-phase permutation under it, then the A-P pack
// [orig: NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0, the multi-key loop
// @ 0x619074..0x6190c4]; decoding reads the A-P characters among the
// printable ones (any other byte skipped: a line end), then takes the keys
// right to left, each a permutation undone and its CRC checked
// [orig: NapiNP_DecodeEncryptedString @ 0x619130, the isprint scan
// @ 0x6191c1, the key loop @ 0x619294..0x619302]. The decode never throws:
// false where the original returns -1 (a printable character outside A-P,
// an odd count of them, more bytes than the caller's `room`, too few bytes
// for the keys' CRCs, a CRC that does not match). `room` is the caller's
// output buffer, which the A-P text's byte count may not exceed
// [orig: @ 0x61920f, `bufSize < decodedLen`]; a caller with no buffer of
// its own passes none.
std::string encode_key_chain(const std::vector<uint8_t> &plaintext, const std::string &key_chain);
bool decode_key_chain(const std::string &encoded, const std::string &key_chain,
                      std::vector<uint8_t> &plaintext, std::size_t room = SIZE_MAX);

// A key / value pair under a key chain (the remembered login's cookie holds the account name so):
// decode_key_chain with a room of 0x8000 bytes, then the payload's text to its first NUL is the
// key and the text past that NUL to the next the value. False where the original returns -1
// (`key` / `value` emptied): the chain's decode fails; the key's NUL is the payload's last byte
// or past it (no value); or a string, its NUL included, exceeds its room.
// [orig: NapiNP_DecodeEncryptedKeyValue @0x619470 - the outputs cleared @0x6194B9 / @0x6194CA,
//  NapiNP_DecodeEncryptedString called @0x619517 (the room 0x8000 pushed @0x61950C), the key's
//  room test @0x619552..0x619556, the value's start test @0x61956E..0x619570, its room test
//  @0x619598..0x61959C]
bool decode_key_value(const std::string &encoded, const std::string &key_chain, std::string &key,
                      std::string &value, std::size_t key_room, std::size_t value_room);

// gt.ssc, the gate tag the game reads encoded by its name alone, under the protocol's key chain
// (net/protocol/protocol.h Protocol::gate_tag_keys, net/protocol/gate_tag.h decode_gate_tag_file)
// [orig: Mission_LoadEncryptedConfig @ 0x4cdcd0: the name @ 0x7cbb50].
inline constexpr const char *kGateTagFile = "gt.ssc";

} // namespace opennova
