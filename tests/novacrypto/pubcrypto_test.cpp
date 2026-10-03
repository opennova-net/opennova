#include <net/novacrypto/ap_alphabet.h>
#include <net/novacrypto/pubcrypto.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect_eq(const std::string &got, const std::string &want, const char *what) {
	if (got == want) return true;
	std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n",
	             what, got.c_str(), want.c_str());
	return false;
}

bool expect_eq_bytes(const std::vector<uint8_t> &got,
                     const std::vector<uint8_t> &want, const char *what) {
	if (got == want) return true;
	std::fprintf(stderr, "FAIL: %s (lengths %zu vs %zu)\n",
	             what, got.size(), want.size());
	return false;
}

// Fixtures generated from onnw/protocol/pubcrypto.py (the Python reference).
// If a future change to the C++ port breaks these the wire format diverges
// from retail and the join flow will silently fail in the field.
bool fixtures_match_python() {
	const std::string key = "NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN";

	// PUBPCID equivalent: encode_pub_value(b'00000002\x00', key)
	const std::vector<uint8_t> pcid_plain = {
		'0', '0', '0', '0', '0', '0', '0', '2', 0x00,
	};
	if (!expect_eq(opennova::encode_pub_value(pcid_plain, key),
	               "DMDIKKLGLMLNONCCOICCNPAPDC",
	               "PCID encoding")) return false;

	// Legacy empty NAMEINFO/SQUADINFO fixture: encode_pub_value(b'\x00' * 7, key).
	// Real /NWJoin identity payload shape is covered by join_identity_test.
	const std::vector<uint8_t> seven_zeros(7, 0x00);
	if (!expect_eq(opennova::encode_pub_value(seven_zeros, key),
	               "EMEGMGHGGMKKGLKOFGIPLM",
	               "NAMEINFO encoding")) return false;

	// Short-key fixture (sanity).
	if (!expect_eq(opennova::encode_pub_value(std::string("hello"), "shortkey"),
	               "OHLMEGCCCBHHEJHFEB",
	               "hello/shortkey encoding")) return false;

	// Round-trip.
	std::vector<uint8_t> decoded;
	if (!opennova::decode_pub_value("OHLMEGCCCBHHEJHFEB", "shortkey", decoded)) {
		std::fprintf(stderr, "FAIL: hello/shortkey decodes\n");
		return false;
	}
	const std::vector<uint8_t> hello_bytes = {'h','e','l','l','o'};
	if (!expect_eq_bytes(decoded, hello_bytes, "hello round-trip")) return false;

	// Hardening vector (grill wave 3 NW-C3): a 20-byte IP:port payload — the only
	// fixture long enough to exercise multiple pseudorandom/sequential rounds with
	// ASCII byte diversity. Byte-identical to retail (NapiNP_EncryptAndEncodeToHexAlpha
	// @0x618fd0) and onnw Python.
	const std::vector<uint8_t> endpoint_plain = {
		'1','9','2','.','1','6','8','.','1','.','1','0','0',':','1','7','4','7','1', 0x00,
	};
	if (!expect_eq(opennova::encode_pub_value(endpoint_plain,
	               "NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN"),
	               "OMIJONGKJMCOJOCCGJKCIPMOHBAIKAAMLKKMMBDJEDDCNCIG",
	               "endpoint encoding")) return false;
	std::vector<uint8_t> endpoint_decoded;
	if (!opennova::decode_pub_value("OMIJONGKJMCOJOCCGJKCIPMOHBAIKAAMLKKMMBDJEDDCNCIG",
	                                "NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN", endpoint_decoded)) {
		std::fprintf(stderr, "FAIL: endpoint decodes\n");
		return false;
	}
	if (!expect_eq_bytes(endpoint_decoded, endpoint_plain, "endpoint round-trip")) return false;

	return true;
}

// A bad value off the wire is a result, never a throw (ADR 0049 d5): false, the
// output emptied, the reason named, for each way a value can be bad.
bool bad_values_are_rejected() {
	const std::string good = opennova::encode_pub_value(std::string("hello"), "shortkey");
	std::string tampered = good;
	tampered[0] = tampered[0] == 'A' ? 'B' : 'A'; // still A-P, the CRC no longer matches
	const struct { std::string value; const char *key; const char *what; } cases[] = {
		{good, "", "an empty key"},
		{good.substr(1), "shortkey", "an odd length"},
		{"AZ" + good.substr(2), "shortkey", "a character outside A-P"},
		{"ABCDEF", "shortkey", "a payload shorter than its CRC"},
		{tampered, "shortkey", "a CRC mismatch"},
	};
	for (const auto &c : cases) {
		std::vector<uint8_t> out = {1, 2, 3};
		std::string why;
		if (opennova::decode_pub_value(c.value, c.key, out, &why) || !out.empty() || why.empty()) {
			std::fprintf(stderr, "FAIL: %s must be rejected\n", c.what);
			return false;
		}
	}
	std::vector<uint8_t> bytes;
	if (opennova::decode_ap("ABC", bytes) || opennova::decode_ap("AQ", bytes) || !bytes.empty()) {
		std::fprintf(stderr, "FAIL: decode_ap rejects an odd length and 'Q'\n");
		return false;
	}
	if (!opennova::decode_ap("CE", bytes) || bytes != std::vector<uint8_t>{0x42}) {
		std::fprintf(stderr, "FAIL: decode_ap reads CE as 0x42\n");
		return false;
	}
	return true;
}

bool empty_key_throws() {
	try {
		opennova::encode_pub_value(std::string("x"), "");
	} catch (const std::exception &) {
		return true;
	}
	std::fprintf(stderr, "FAIL: encode with empty key should throw\n");
	return false;
}

} // namespace

int main() {
	bool ok = true;
	ok = fixtures_match_python() && ok;
	ok = empty_key_throws() && ok;
	ok = bad_values_are_rejected() && ok;
	if (!ok) {
		std::fprintf(stderr, "pubcrypto_test failed\n");
		return 1;
	}
	std::printf("pubcrypto_test ok\n");
	return 0;
}
