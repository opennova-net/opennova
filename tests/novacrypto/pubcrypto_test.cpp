#include <novacrypto/pubcrypto.h>

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
	const auto decoded = opennova::decode_pub_value("OHLMEGCCCBHHEJHFEB", "shortkey");
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
	const auto endpoint_decoded = opennova::decode_pub_value(
		"OMIJONGKJMCOJOCCGJKCIPMOHBAIKAAMLKKMMBDJEDDCNCIG",
		"NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN");
	if (!expect_eq_bytes(endpoint_decoded, endpoint_plain, "endpoint round-trip")) return false;

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
	if (!ok) {
		std::fprintf(stderr, "pubcrypto_test failed\n");
		return 1;
	}
	std::printf("pubcrypto_test ok\n");
	return 0;
}
