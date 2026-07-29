#include <novacrypto/epask.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

bool expect_eq(const std::string &got, const std::string &want, const char *what) {
	if (got == want) return true;
	std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n",
	             what, got.c_str(), want.c_str());
	return false;
}

// Fixtures generated against onnw/protocol/crypto.py::epask_encrypt with
// hand-picked params (so the test doesn't depend on Python's sympy):
//   p=449, q=463 → n=207887, e=10001 (coprime with phi(n)=206976), key as below.
// If the C++ port diverges, retail's encrypted form fields won't decode and
// real-auth login (Phase H.3) silently breaks.
bool fixtures_match_python() {
	opennova::EpaskParams params;
	params.exponent = 10001;
	params.modulus  = 207887;
	params.key      = "1700000000000000001";

	struct Case { const char *plaintext; const char *ciphertext; };
	const Case cases[] = {
		{"DevUser",
		 "BHJKLKADCGBMHOMCLJPIBCICMIKGNFECOILLLJBCDEHHHNMBLKIEDBIB"},
		{"hello",
		 "EDEJLKADCGBMHOMCKEHFCCICJBOMPFECIEMDKJBC"},
		{"jop_2_relay.htm",
		 "OMEELKADAFJLGOMCDEDIBCICKGJAOFECLLLHKJBCPMICGNMBLKIEDBIB"
		 "BNKKOEEBFDOCLIABACLOIMDBIOFCDAIAMGAKNDEAJDADJHAACCBBGLMP"
		 "HHOFIPIP"},
	};

	for (const auto &c : cases) {
		const std::string decrypted = opennova::epask_decrypt(c.ciphertext, params);
		if (!expect_eq(decrypted, c.plaintext, "decrypt fixture")) return false;

		// Encrypt direction is ALSO byte-comparable to Python: the
		// nwu_encrypt/nwu_decrypt label-swap is a pure naming inversion of
		// identical byte transforms, so our epask_encrypt reproduces Python's
		// ciphertext exactly. Confirmed in grill wave 3 (NW-C2) against retail
		// (EPASK_Encrypt@0x6669a0) and by compiling this source against the
		// production-proven onnw Python — the two agree byte-for-byte.
		const std::string ct = opennova::epask_encrypt(c.plaintext, params);
		if (!expect_eq(ct, c.ciphertext, "encrypt fixture")) return false;
		const std::string back = opennova::epask_decrypt(ct, params);
		if (!expect_eq(back, c.plaintext, "round-trip")) return false;
	}
	return true;
}

bool serialize_round_trip() {
	opennova::EpaskParams p;
	p.exponent = 12345;
	p.modulus  = 234567;
	p.key      = "1700000000123456789012";
	const auto str = opennova::epask_to_string(p);
	if (!expect_eq(str, "12345:234567:1700000000123456789012",
	               "epask_to_string format")) return false;
	const auto parsed = opennova::epask_from_string(str);
	if (parsed.exponent != p.exponent ||
	    parsed.modulus  != p.modulus ||
	    parsed.key      != p.key) {
		std::fprintf(stderr, "FAIL: epask_from_string round-trip\n");
		return false;
	}
	return true;
}

bool malformed_epask_string_is_atoi_tolerant() {
	const auto missing = opennova::epask_from_string("not-a-number");
	if (missing.exponent != 0 || missing.modulus != 0 || !missing.key.empty()) {
		std::fprintf(stderr, "FAIL: malformed EPASK without colons should parse as zeros\n");
		return false;
	}
	const auto partial = opennova::epask_from_string("123abc: 456xyz:key-tail");
	if (partial.exponent != 123 || partial.modulus != 456 || partial.key != "key-tail") {
		std::fprintf(stderr, "FAIL: EPASK fields should use atoi-style prefixes\n");
		return false;
	}
	const auto empty_numeric = opennova::epask_from_string(":bad:key");
	if (empty_numeric.exponent != 0 || empty_numeric.modulus != 0 || empty_numeric.key != "key") {
		std::fprintf(stderr, "FAIL: empty/non-numeric EPASK fields should parse as zero\n");
		return false;
	}
	return true;
}

bool encrypt_truncates_at_first_nul() {
	opennova::EpaskParams params;
	params.exponent = 10001;
	params.modulus  = 207887;
	params.key      = "1700000000000000001";

	const std::string with_tail("abc\0def", 7);
	const std::string ct = opennova::epask_encrypt(with_tail, params);
	if (!expect_eq(ct, opennova::epask_encrypt("abc", params),
	               "encrypt truncates at first NUL")) return false;
	if (!expect_eq(opennova::epask_decrypt(ct, params), "abc",
	               "NUL-truncated ciphertext decrypts to prefix")) return false;
	return true;
}

bool generate_yields_valid_params() {
	auto params = opennova::generate_epask();
	if (params.modulus <= 200000u || params.modulus >= 300000u) {
		std::fprintf(stderr, "FAIL: generate_epask modulus %u out of range\n",
		             params.modulus);
		return false;
	}
	if (params.exponent < 10000u || params.exponent >= 50000u) {
		std::fprintf(stderr, "FAIL: generate_epask exponent %u out of range\n",
		             params.exponent);
		return false;
	}
	if (params.key.size() != 19) {  // 13 ms + 6 suffix
		std::fprintf(stderr, "FAIL: generate_epask key len %zu (want 19)\n",
		             params.key.size());
		return false;
	}
	// Sanity: encrypt + decrypt round-trips with the generated params.
	const std::string back = opennova::epask_decrypt(
		opennova::epask_encrypt("test", params), params);
	if (!expect_eq(back, "test", "generated params round-trip")) return false;
	return true;
}

bool malformed_inputs_throw() {
	opennova::EpaskParams p{12345, 234567, std::string("k")};
	bool got_throw = false;
	try { opennova::epask_decrypt("ABC", p); }   // odd length
	catch (const std::exception &) { got_throw = true; }
	if (!got_throw) {
		std::fprintf(stderr, "FAIL: odd-length should throw\n");
		return false;
	}
	got_throw = false;
	try { opennova::epask_decrypt("AZ", p); }    // 'Z' out of A-P
	catch (const std::exception &) { got_throw = true; }
	if (!got_throw) {
		std::fprintf(stderr, "FAIL: out-of-range char should throw\n");
		return false;
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = fixtures_match_python()      && ok;
	ok = serialize_round_trip()       && ok;
	ok = malformed_epask_string_is_atoi_tolerant() && ok;
	ok = encrypt_truncates_at_first_nul() && ok;
	ok = generate_yields_valid_params() && ok;
	ok = malformed_inputs_throw()     && ok;
	if (!ok) {
		std::fprintf(stderr, "epask_test failed\n");
		return 1;
	}
	std::printf("epask_test ok\n");
	return 0;
}
