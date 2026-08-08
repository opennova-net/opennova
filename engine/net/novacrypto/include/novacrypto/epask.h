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

// Random params: modulus is product of two distinct small primes p, q
// chosen so 200_000 < p*q < 300_000; exponent is a random 5-digit value
// coprime with phi(n). Throws std::runtime_error on the rare
// failure-to-converge path (suitable for `static const auto` at boot).
EpaskParams generate_epask();

// Serialize / parse the cookie + form-field representation.
std::string epask_to_string(const EpaskParams &p);
EpaskParams epask_from_string(const std::string &s);

// Decrypt one form field. Throws std::runtime_error on malformed input
// (odd length, out-of-range A-P chars, ciphertext word with no
// 0..255 inverse — the last typically means params are wrong or the
// client used a different EPASK).
std::string epask_decrypt(const std::string &ciphertext, const EpaskParams &params);

// Encrypt a plaintext field. Useful only for tests — retail is the one
// that does the encryption in the real flow.
std::string epask_encrypt(const std::string &plaintext, const EpaskParams &params);

} // namespace opennova
