#pragma once

#include <cstdint>
#include <string>

namespace opennova {

// EPASK is retail Joint Operations' RSA-style public-key bundle for
// encrypting form-field credentials between IB3 and the server. Retail
// reads the cookie `EPASK="exponent:modulus:key"`, encrypts each form
// field byte as `pow(byte+2, exponent, modulus)` (4-byte LE uint), then
// runs NWU stream cipher + A-P alphabet packing. Server reverses.
//
// Our /nwprepare.dll generates the bundle, /NWStart.dll echoes it as a
// cookie, and POST /NWLogin.dll receives the same EPASK string back as
// a form field along with the encrypted NAME / PASSWORD / template
// names — we decode the latter using the bundle's exponent + modulus.
//
// Modulus is intentionally small (200_000 < n < 300_000) so that a
// brute-force decrypt without the private exponent is feasible: for
// each 4-byte ciphertext word, try byte_val 0..255 and compute
// pow(byte_val + 2, exponent, modulus) — at most 256 modexp ops per
// recovered plaintext byte.
//
// Direct port of onnw/protocol/{epask,crypto}.py. Tests cross-check
// against Python via fixtures.

struct EpaskParams {
	uint32_t exponent = 0;
	uint32_t modulus = 0;
	std::string key;       // ASCII timestamp+rand suffix (matches onnet's generate_epask_key)
};

// Random params into `out`: modulus is product of two distinct small primes
// p, q chosen so 200_000 < p*q < 300_000; exponent is a random 5-digit value
// coprime with phi(n). False (`out` untouched) on the failure-to-converge
// path (200 prime pairs, 200 exponents each): a result, never a throw
// (ADR 0049 d5).
bool generate_epask(EpaskParams &out);

// Serialize / parse the cookie + form-field representation. Parsing returns
// false (`out` untouched, `error` naming why when given) when either ':' is
// missing or a numeric field is 512+ bytes — retail's parser returns -1 there
// and the bundle is unusable [orig: EPASK_ParseColonDelimitedString @0x666710].
// Numeric fields use atoi prefixes ("123abc" -> 123, "" -> 0) like the retail
// _atoi64. The bundle is input (a server's cookie), so a bad one is a result,
// never a throw (ADR 0049 d5).
std::string epask_to_string(const EpaskParams &p);
bool epask_from_string(const std::string &s, EpaskParams &out, std::string *error = nullptr);

// Decrypt one form field into `out`. False (`error` naming why when given) on
// malformed input (odd length, out-of-range A-P chars, ciphertext word with no
// 0..255 inverse — the last typically means params are wrong or the client
// used a different EPASK) and on rejected params (modulus <= 258 or
// exponent == 0 — retail's EPASK_ModexpEncrypt @0x666600 gate @0x66668a).
bool epask_decrypt(const std::string &ciphertext, const EpaskParams &params, std::string &out,
                   std::string *error = nullptr);

// Encrypt a plaintext field into `out` (the client's login form fields). False
// (`out` emptied, `error` naming why when given) on rejected params (see
// epask_decrypt), retail's EPASK_Encrypt -1 [orig: @0x666a9b]: a result,
// never a throw (ADR 0049 d5).
bool epask_encrypt(const std::string &plaintext, const EpaskParams &params, std::string &out,
                   std::string *error = nullptr);

} // namespace opennova
