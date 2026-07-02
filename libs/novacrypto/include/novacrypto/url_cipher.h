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

// Decode a NovaWorld registration-URL field byte-exactly per the recipe
// witnessed in jodemo.exe:
//
//     plain[i] = cipher[i] - key[i % keylen] + '0'
//
// Decoding stops at the first '&' terminator in `cipher` (ASCII 38) or when
// the input is exhausted. The original binary's inner loop loads the key
// byte from a 128-byte stack buffer `Buffer[i]`, which after sprintf holds
// 21 key bytes + null + garbage; realistic inputs never reach past the 21st
// byte, so we use modular indexing as the intended semantic.
std::string url_cipher_decode(std::string_view cipher, std::string_view key);

// Inverse (useful for tests and synthetic URLs):
//
//     cipher[i] = plain[i] + key[i % keylen] - '0'
std::string url_cipher_encode(std::string_view plain, std::string_view key);

} // namespace opennova
