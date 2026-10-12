#include <net/novacrypto/ap_alphabet.h>
#include <net/novacrypto/crc32.h>
#include <net/novacrypto/nwu.h>
#include <net/novacrypto/pubcrypto.h>

#include <cstdint>
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

// The encoded value, or "<refused>" where encode_pub_value returns false.
std::string encode(const std::vector<uint8_t> &plaintext, const std::string &key) {
	std::string out = "stale";
	return opennova::encode_pub_value(plaintext, key, out) ? out : std::string("<refused>");
}

std::string encode(const std::string &plaintext, const std::string &key) {
	std::string out = "stale";
	return opennova::encode_pub_value(plaintext, key, out) ? out : std::string("<refused>");
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
	if (!expect_eq(encode(pcid_plain, key),
	               "DMDIKKLGLMLNONCCOICCNPAPDC",
	               "PCID encoding")) return false;

	// Legacy empty NAMEINFO/SQUADINFO fixture: encode_pub_value(b'\x00' * 7, key).
	// Real /NWJoin identity payload shape is covered by join_identity_test.
	const std::vector<uint8_t> seven_zeros(7, 0x00);
	if (!expect_eq(encode(seven_zeros, key),
	               "EMEGMGHGGMKKGLKOFGIPLM",
	               "NAMEINFO encoding")) return false;

	// Short-key fixture (sanity).
	if (!expect_eq(encode(std::string("hello"), "shortkey"),
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
	if (!expect_eq(encode(endpoint_plain, "NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN"),
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
	const std::string good = encode(std::string("hello"), "shortkey");
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

// The key-chain form: one key is the single-key codec's encoding; each key of a chain is a layer of
// it, so the chain decodes layer by layer through the single-key codec (the outer key first) to the
// plaintext; the chain's own decode reads it back past a line end, and refuses a character outside
// A-P, an odd count, a damaged CRC and too few bytes for the keys.
bool key_chain_layers() {
	const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
	if (!expect_eq(opennova::encode_key_chain(hello, "shortkey"), "OHLMEGCCCBHHEJHFEB", "one key")) return false;
	// gt.ssc's name and key chain [orig: Mission_LoadEncryptedConfig @ 0x4cdcd0].
	if (!expect_eq(opennova::kGateTagFile, "gt.ssc", "the gate tag's file") ||
	    !expect_eq(opennova::kGateTagKeys, "jop:2:oyez", "the gate tag's key chain"))
		return false;
	const std::vector<uint8_t> tag = {'j', 'o', 'p', ':', 'c', 'u', 's', '2'};
	const std::string chained = opennova::encode_key_chain(tag, opennova::kGateTagKeys);
	if (chained.size() != 2 * (tag.size() + 12)) {
		std::fprintf(stderr, "FAIL: a chain of three keys adds three CRCs\n");
		return false;
	}
	std::vector<uint8_t> layer;
	bool layered = opennova::decode_pub_value(chained, "oyez", layer);
	layered = layered && opennova::decode_pub_value(opennova::encode_ap(layer), "2", layer);
	layered = layered && opennova::decode_pub_value(opennova::encode_ap(layer), "jop", layer);
	if (!layered) {
		std::fprintf(stderr, "FAIL: each layer of the chain decodes through the single-key codec\n");
		return false;
	}
	if (!expect_eq_bytes(layer, tag, "the chain's layers")) return false;
	std::vector<uint8_t> back;
	if (!opennova::decode_key_chain(chained + "\r\n", opennova::kGateTagKeys, back) || back != tag) {
		std::fprintf(stderr, "FAIL: the chain reads back past a line end\n");
		return false;
	}
	std::string damaged = chained;
	damaged[0] = damaged[0] == 'A' ? 'B' : 'A';
	if (opennova::decode_key_chain(chained + "Z", opennova::kGateTagKeys, back) ||
	    opennova::decode_key_chain(chained.substr(1), opennova::kGateTagKeys, back) ||
	    opennova::decode_key_chain(damaged, opennova::kGateTagKeys, back) ||
	    opennova::decode_key_chain(opennova::encode_key_chain(tag, "jop"), opennova::kGateTagKeys, back) || !back.empty()) {
		std::fprintf(stderr, "FAIL: the chain's decode refuses what the original returns -1 for\n");
		return false;
	}
	return true;
}

// A key chain's empty segment is a key like any other: the original crypts each segment's layer
// (the buffer's CRC appended, then the ADD chain, our nwu_decrypt) with no emptiness test, so an
// empty one runs the seed-50 chain. The chain "jop:" is two layers, "jop" then the empty key, and
// "" one empty layer; both read back.
// [orig: NapiNP_EncryptAndEncodeToHexAlpha @0x618FD0 - the key count @0x619029..0x619034, the
//  CRC @0x6190A5..0x6190AE, NapiNP_EncryptBuffer @0x6190B7; NapiNP_DecodeEncryptedString
//  @0x619130 - NapiNP_DecryptBuffer @0x6192E1]
bool key_chain_empty_segment_is_a_key() {
	const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
	const auto layer = [](std::vector<uint8_t> buffer, const char *key) {
		const uint32_t crc = opennova::crc32_napi(buffer.data(), buffer.size());
		for (int shift = 0; shift < 32; shift += 8)
			buffer.push_back(static_cast<uint8_t>((crc >> shift) & 0xFFu));
		opennova::nwu_decrypt(buffer.data(), buffer.size(), key);
		return buffer;
	};
	const std::string two = opennova::encode_key_chain(hello, "jop:");
	if (!expect_eq(two, opennova::encode_ap(layer(layer(hello, "jop"), "")),
	               "an empty segment layers the seed-50 chain"))
		return false;
	const std::string one = opennova::encode_key_chain(hello, "");
	if (!expect_eq(one, opennova::encode_ap(layer(hello, "")), "the empty chain is one empty key"))
		return false;
	// The empty layer is crypted, not only the CRC appended.
	std::vector<uint8_t> crc_only = hello;
	const uint32_t hello_crc = opennova::crc32_napi(hello.data(), hello.size());
	for (int shift = 0; shift < 32; shift += 8)
		crc_only.push_back(static_cast<uint8_t>((hello_crc >> shift) & 0xFFu));
	if (one == opennova::encode_ap(crc_only)) {
		std::fprintf(stderr, "FAIL: the empty layer is not the identity\n");
		return false;
	}
	std::vector<uint8_t> back;
	if (!opennova::decode_key_chain(two, "jop:", back) || back != hello ||
	    !opennova::decode_key_chain(one, "", back) || back != hello) {
		std::fprintf(stderr, "FAIL: a chain with an empty segment reads back\n");
		return false;
	}
	return true;
}

// An empty key is refused as a result, never a throw (ADR 0049 d5): false, the
// output emptied, for either plaintext form.
bool empty_key_is_refused() {
	std::string out = "stale";
	if (opennova::encode_pub_value(std::string("x"), "", out) || !out.empty()) {
		std::fprintf(stderr, "FAIL: encode with an empty key must be refused\n");
		return false;
	}
	out = "stale";
	if (opennova::encode_pub_value(std::vector<uint8_t>{'x'}, "", out) || !out.empty()) {
		std::fprintf(stderr, "FAIL: encode of bytes with an empty key must be refused\n");
		return false;
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = fixtures_match_python() && ok;
	ok = empty_key_is_refused() && ok;
	ok = bad_values_are_rejected() && ok;
	ok = key_chain_layers() && ok;
	ok = key_chain_empty_segment_is_a_key() && ok;
	if (!ok) {
		std::fprintf(stderr, "pubcrypto_test failed\n");
		return 1;
	}
	std::printf("pubcrypto_test ok\n");
	return 0;
}
