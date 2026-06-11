#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace opennova {

// ASCII key strings for the NovaWorld registration-URL cipher, referenced
// only from Auth_ParseRegistrationURL@0x514c40. Literals verified at:
//   NK cipher key @ 0x74d8a0 (used for NK= field)
//   CK cipher key @ 0x74d874 (used for CK= field)
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
