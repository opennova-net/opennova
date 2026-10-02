#include <net/novacrypto/epask.h>

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

// The decrypted field, or "<rejected>" where epask_decrypt returns false.
std::string decrypt(const std::string &ciphertext, const opennova::EpaskParams &params) {
	std::string out;
	return opennova::epask_decrypt(ciphertext, params, out) ? out : std::string("<rejected>");
}

// The parsed bundle; `ok` whether epask_from_string accepted it.
opennova::EpaskParams parse(const std::string &text, bool &ok) {
	opennova::EpaskParams out;
	ok = opennova::epask_from_string(text, out);
	return out;
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
		const std::string decrypted = decrypt(c.ciphertext, params);
		if (!expect_eq(decrypted, c.plaintext, "decrypt fixture")) return false;

		// Encrypt direction is ALSO byte-comparable to Python: the
		// nwu_encrypt/nwu_decrypt label-swap is a pure naming inversion of
		// identical byte transforms, so our epask_encrypt reproduces Python's
		// ciphertext exactly. Confirmed in grill wave 3 (NW-C2) against retail
		// (EPASK_Encrypt@0x6669a0) and by compiling this source against the
		// production-proven onnw Python — the two agree byte-for-byte.
		const std::string ct = opennova::epask_encrypt(c.plaintext, params);
		if (!expect_eq(ct, c.ciphertext, "encrypt fixture")) return false;
		const std::string back = decrypt(ct, params);
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
	bool ok = false;
	const auto parsed = parse(str, ok);
	if (!ok || parsed.exponent != p.exponent ||
	    parsed.modulus  != p.modulus ||
	    parsed.key      != p.key) {
		std::fprintf(stderr, "FAIL: epask_from_string round-trip\n");
		return false;
	}
	return true;
}

// A bundle missing either ':' is rejected outright (retail's parser returns -1
// @0x66679b / @0x6667e3); the numeric fields themselves stay atoi-tolerant.
bool malformed_epask_string_is_rejected_or_atoi_tolerant() {
	bool ok = true;
	for (const char *bad : {"7", "not-a-number", "12345:234567", ""}) {
		// A rejected bundle is a result: false, the reason named, the output untouched.
		opennova::EpaskParams kept{1, 2, "kept"};
		std::string why;
		if (opennova::epask_from_string(bad, kept, &why) || why.empty() || kept.key != "kept") {
			std::fprintf(stderr, "FAIL: EPASK '%s' without both separators must be rejected\n", bad);
			return false;
		}
	}
	const auto partial = parse("123abc: 456xyz:key-tail", ok);
	if (!ok || partial.exponent != 123 || partial.modulus != 456 || partial.key != "key-tail") {
		std::fprintf(stderr, "FAIL: EPASK fields should use atoi-style prefixes\n");
		return false;
	}
	const auto empty_numeric = parse(":bad:key", ok);
	if (!ok || empty_numeric.exponent != 0 || empty_numeric.modulus != 0 || empty_numeric.key != "key") {
		std::fprintf(stderr, "FAIL: empty/non-numeric EPASK fields should parse as zero\n");
		return false;
	}
	const std::string long_field(512, '9');
	(void)parse(long_field + ":1:k", ok);
	if (ok) {
		std::fprintf(stderr, "FAIL: a 512-byte numeric field must be rejected\n");
		return false;
	}
	return true;
}

// The modexp parameter gate: modulus <= 258 or a zero exponent is refused
// before any arithmetic (retail EPASK_ModexpEncrypt @0x66668a returns -1),
// so a "0:0:k" bundle can never reach a divide by zero.
bool rejected_params_never_reach_modexp() {
	bool ok = false;
	const auto zero = parse("0:0:k", ok);
	if (!ok || zero.exponent != 0 || zero.modulus != 0 || zero.key != "k") {
		std::fprintf(stderr, "FAIL: '0:0:k' parses (both separators present)\n");
		return false;
	}
	for (const opennova::EpaskParams &p : {
	             opennova::EpaskParams{0, 0, "k"},
	             opennova::EpaskParams{10001, 258, "k"},
	             opennova::EpaskParams{0, 207887, "k"},
	     }) {
		bool threw = false;
		try { (void)opennova::epask_encrypt("x", p); }
		catch (const std::exception &) { threw = true; }
		if (!threw) {
			std::fprintf(stderr, "FAIL: encrypt with exp=%u mod=%u must be rejected\n",
			             p.exponent, p.modulus);
			return false;
		}
		std::string out;
		std::string why;
		if (opennova::epask_decrypt("ABCDEFGH", p, out, &why) || why.empty()) {
			std::fprintf(stderr, "FAIL: decrypt with exp=%u mod=%u must be rejected\n",
			             p.exponent, p.modulus);
			return false;
		}
	}
	// 259 is the first modulus the gate admits. Exponent 1 keeps the per-byte
	// power a permutation (259 = 7 * 37 is not an RSA modulus, so x^3 mod 259
	// collides); the gate only checks the bounds, not the pair's validity.
	const opennova::EpaskParams edge{1, 259, "k"};
	const std::string back = decrypt(opennova::epask_encrypt("hi", edge), edge);
	if (!expect_eq(back, "hi", "modulus 259 passes the gate and round-trips")) return false;
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
	if (!expect_eq(decrypt(ct, params), "abc",
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
	const std::string back = decrypt(
		opennova::epask_encrypt("test", params), params);
	if (!expect_eq(back, "test", "generated params round-trip")) return false;
	return true;
}

// A malformed field is a result, never a throw (ADR 0049 d5): false, the reason named.
bool malformed_inputs_are_rejected() {
	opennova::EpaskParams p{12345, 234567, std::string("k")};
	for (const char *bad : {"ABC", "AZ", "ABCDEF"}) { // odd length; 'Z' out of A-P; 3 bytes, not words
		std::string out = "stale";
		std::string why;
		if (opennova::epask_decrypt(bad, p, out, &why) || why.empty() || !out.empty()) {
			std::fprintf(stderr, "FAIL: '%s' should be rejected\n", bad);
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = fixtures_match_python()      && ok;
	ok = serialize_round_trip()       && ok;
	ok = malformed_epask_string_is_rejected_or_atoi_tolerant() && ok;
	ok = rejected_params_never_reach_modexp() && ok;
	ok = encrypt_truncates_at_first_nul() && ok;
	ok = generate_yields_valid_params() && ok;
	ok = malformed_inputs_are_rejected() && ok;
	if (!ok) {
		std::fprintf(stderr, "epask_test failed\n");
		return 1;
	}
	std::printf("epask_test ok\n");
	return 0;
}
