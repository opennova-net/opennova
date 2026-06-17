#include <novacrypto/url_cipher.h>

#include <cstdio>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// One explicit byte vector from the witnessed recipe in
// Auth_ParseRegistrationURL@0x514c40: cipher[0] = plain[0] + key[0] - '0'.
// plain='5' (0x35), key='d' (0x64) -> cipher = 0x35 + 0x64 - 0x30 = 0x69 = 'i'.
bool check_scalar_vector() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	const std::string encoded = url_cipher_encode("5", "d");
	if (!expect(encoded.size() == 1, "scalar encode preserves length")) return false;
	if (!expect(static_cast<uint8_t>(encoded[0]) == 0x69, "scalar encode: '5' + 'd' - '0' == 'i'")) return false;
	const std::string decoded = url_cipher_decode(encoded, "d");
	if (!expect(decoded == "5", "scalar decode reverses scalar encode")) return false;
	return true;
}

// End-to-end roundtrip with the real NK cipher key from jodemo .rdata.
bool check_nk_key_roundtrip() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain = "PLAYER123";
	const std::string encoded = url_cipher_encode(plain, URL_CIPHER_KEY_NK);
	if (!expect(encoded.size() == plain.size(), "NK encode preserves length")) return false;
	if (!expect(encoded != plain, "NK encode actually transforms the plaintext")) return false;
	const std::string decoded = url_cipher_decode(encoded, URL_CIPHER_KEY_NK);
	if (!expect(decoded == plain, "NK encode+decode is a byte-exact roundtrip")) return false;
	return true;
}

// CK key is a separate literal; confirm it doesn't collide with NK.
bool check_ck_key_roundtrip() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_CK;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain = "CDKEY-4512-ABCD";
	const std::string encoded_ck = url_cipher_encode(plain, URL_CIPHER_KEY_CK);
	const std::string encoded_nk = url_cipher_encode(plain, URL_CIPHER_KEY_NK);
	if (!expect(encoded_ck != encoded_nk, "NK and CK keys yield different ciphertexts")) return false;
	const std::string decoded = url_cipher_decode(encoded_ck, URL_CIPHER_KEY_CK);
	if (!expect(decoded == plain, "CK encode+decode is a byte-exact roundtrip")) return false;
	return true;
}

// The decoder must stop at '&' — URL field terminator witnessed in the
// Auth_ParseRegistrationURL loop (do { ... } while (*src != 38)).
bool check_ampersand_terminator() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain = "ABC";
	const std::string encoded = url_cipher_encode(plain, URL_CIPHER_KEY_NK);
	// Splice an '&' plus trailing garbage; the decoder must ignore everything
	// at and after the '&'.
	const std::string spliced = encoded + "&TRAILING_JUNK_IGNORED";
	const std::string decoded = url_cipher_decode(spliced, URL_CIPHER_KEY_NK);
	if (!expect(decoded == plain, "decode stops at '&' terminator")) return false;
	return true;
}

// Empty input -> empty output. Empty key -> empty output (defensive).
bool check_empty_cases() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	if (!expect(url_cipher_decode({}, URL_CIPHER_KEY_NK).empty(), "empty cipher decodes to empty plain")) return false;
	if (!expect(url_cipher_encode({}, URL_CIPHER_KEY_NK).empty(), "empty plain encodes to empty cipher")) return false;
	if (!expect(url_cipher_decode("abc", {}).empty(), "empty key returns empty")) return false;
	return true;
}

// Inputs longer than the key length must wrap modularly. The key length
// for both NK and CK is 21 bytes — verify past the 21st position.
bool check_key_wrap() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain(40, 'X'); // 40 chars, past the 21-byte key boundary
	const std::string encoded = url_cipher_encode(plain, URL_CIPHER_KEY_NK);
	const std::string decoded = url_cipher_decode(encoded, URL_CIPHER_KEY_NK);
	if (!expect(decoded == plain, "40-byte input roundtrips with 21-byte key via modular wrap")) return false;
	return true;
}

// Multi-byte golden vectors from the production-proven onnw _encode_token
// (grill wave 3 NW-C4): NK = host:port, CK = app_id. Byte-identical to retail
// parse_connection_query_string@0x54dfb0 and to the onnw Python reference.
// e.g. NK[0]: '1'(49) + 'd'(100) - '0'(48) = 101 = 'e'.
bool check_python_golden_vectors() {
	using opennova::url_cipher_decode;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_CK;
	using opennova::URL_CIPHER_KEY_NK;
	if (!expect(url_cipher_encode("127.0.0.1:7597", URL_CIPHER_KEY_NK) == "ekocihediqjisj",
	            "NK golden vector: 127.0.0.1:7597 -> ekocihediqjisj")) return false;
	if (!expect(url_cipher_encode("12345", URL_CIPHER_KEY_CK) == "dhkhh",
	            "CK golden vector: 12345 -> dhkhh")) return false;
	if (!expect(url_cipher_decode("ekocihediqjisj", URL_CIPHER_KEY_NK) == "127.0.0.1:7597",
	            "NK golden vector decodes back")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_scalar_vector()) return 1;
	if (!check_nk_key_roundtrip()) return 1;
	if (!check_ck_key_roundtrip()) return 1;
	if (!check_ampersand_terminator()) return 1;
	if (!check_empty_cases()) return 1;
	if (!check_key_wrap()) return 1;
	if (!check_python_golden_vectors()) return 1;
	std::printf("OK: url_cipher roundtrips + byte vector + '&' terminator\n");
	return 0;
}
