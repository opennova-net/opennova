#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace opennova {

// ASCII key strings for the NovaWorld registration-URL cipher. Witnessed at:
//   jodemo  Auth_ParseRegistrationURL@0x514c40 — NK key @ 0x74d8a0, CK key @ 0x74d874
//   retail  parse_connection_query_string@0x54dfb0 — NK key @ 0x7d3f30, CK key @ 0x7d3f04
// Re-grilled byte-exact in retail Jointops.exe (2026-06-11, grill wave 3 NW-C4): the NK=/CK=
// decode loops run plain[i] = cipher[i] - key[i] + '0' and stop at the first '&' (38).
inline constexpr const char *URL_CIPHER_KEY_NK = "diheijefhgcdjcgcjcfbd";
inline constexpr const char *URL_CIPHER_KEY_CK = "cfhdcegjigecjehcgjdhe";

// Decode a NovaWorld registration-URL field byte-exactly per the witnessed
// recipe:
//
//     plain[i] = cipher[i] - key[i] + '0'
//
// Decoding stops at the first '&' terminator in `cipher` (ASCII 38) or when
// the input is exhausted. The key index is LINEAR, never modular: the
// original loads the key byte from a 128-byte stack buffer the 21-char key
// was sprintf'd into, so position 21 reads the key's NUL (modeled: key byte
// 0) and positions past it read uninitialised stack (unreproducible, so the
// output is cut there). A token is therefore at most keylen + 1 chars long.
// [orig: parse_connection_query_string @0x54dfb0 NK loop @0x54e173 / CK loop @0x54e1fd]
std::string url_cipher_decode(std::string_view cipher, std::string_view key);

// Inverse (useful for tests and synthetic URLs), with the same linear key
// walk and keylen + 1 cut:
//
//     cipher[i] = plain[i] + key[i] - '0'
std::string url_cipher_encode(std::string_view plain, std::string_view key);

} // namespace opennova
