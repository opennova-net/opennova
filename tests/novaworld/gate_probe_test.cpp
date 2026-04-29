#include <novacrypto/nwu.h>
#include <novaworld/gate_probe.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Probe is the NWU-transformed (tag + NUL) bytes.
bool check_probe_length_and_determinism() {
	using opennova::gate_probe_build;
	using opennova::GATE_PROBE_TAG_JODEMO;
	const auto p1 = gate_probe_build();
	const auto p2 = gate_probe_build();
	if (!expect(p1.size() == std::strlen(GATE_PROBE_TAG_JODEMO) + 1,
			"probe length == strlen(tag) + 1 NUL")) return false;
	if (!expect(p1 == p2, "probe is deterministic")) return false;
	return true;
}

// Decrypt a probe back via nwu_encrypt -> recovers "jopd:cus4\0".
bool check_probe_roundtrip_to_tag() {
	using opennova::gate_probe_build;
	using opennova::GATE_NWU_KEY;
	using opennova::GATE_PROBE_TAG_JODEMO;
	auto probe = gate_probe_build();
	opennova::nwu_encrypt(probe.data(), probe.size(), GATE_NWU_KEY);
	if (!expect(probe.back() == 0, "last byte is NUL after inverse")) return false;
	const std::string recovered(reinterpret_cast<const char *>(probe.data()),
			probe.size() - 1);
	if (!expect(recovered == GATE_PROBE_TAG_JODEMO, "recovered plaintext == 'jopd:cus4'")) return false;
	return true;
}

// End-to-end: synthesize a fake NovaWorld gate response (encrypt with the
// GATEAPI key), then decrypt+parse it via gate_response_decrypt_and_parse.
bool check_gate_response_roundtrip() {
	using opennova::GATE_NWU_KEY;
	using opennova::gate_response_decrypt_and_parse;
	const std::string plain_response =
			"VAR POSTIPADDRESS 10.0.0.1\r\n"
			"VAR POSTIPPORT 8080\r\n"
			"VAR UDPNOVAWORLD 10.0.0.2:64206\r\n"
			"VAR REFLECTEDIPADDRESS 203.0.113.7\r\n"
			"VAR REFLECTEDPORTNUMBER 12345\r\n";
	// The binary sends NWU-encoded plaintext; mimic the server by running
	// nwu_decrypt over plaintext (the symmetric counterpart is nwu_encrypt
	// on the receive side — matches PFF_EncryptBuffer).
	std::vector<uint8_t> wire(plain_response.begin(), plain_response.end());
	opennova::nwu_decrypt(wire.data(), wire.size(), GATE_NWU_KEY);

	opennova::GateResponse r;
	if (!expect(gate_response_decrypt_and_parse(wire.data(), wire.size(), r),
			"decrypt+parse succeeds")) return false;
	if (!expect(r.var_count == 5, "5 VARs absorbed")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{10, 0, 0, 1}),
			"POSTIPADDRESS")) return false;
	if (!expect(r.post_port == 8080, "POSTIPPORT")) return false;
	if (!expect(r.udp_novaworld == "10.0.0.2:64206", "UDPNOVAWORLD")) return false;
	if (!expect((r.reflected_ip == std::array<uint8_t, 4>{203, 0, 113, 7}),
			"REFLECTEDIPADDRESS")) return false;
	if (!expect(r.reflected_port == 12345, "REFLECTEDPORTNUMBER")) return false;
	return true;
}

// Empty input / empty key are handled defensively.
bool check_bad_inputs() {
	opennova::GateResponse r;
	if (!expect(!opennova::gate_response_decrypt_and_parse(nullptr, 0, r),
			"null data rejected")) return false;
	const uint8_t dummy[1] = {0};
	if (!expect(!opennova::gate_response_decrypt_and_parse(dummy, 1, r, ""),
			"empty key rejected")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_probe_length_and_determinism()) return 1;
	if (!check_probe_roundtrip_to_tag()) return 1;
	if (!check_gate_response_roundtrip()) return 1;
	if (!check_bad_inputs()) return 1;
	std::printf("OK: gate probe + encrypted response roundtrip (key=\"GATEAPI\", tag=\"jopd:cus4\")\n");
	return 0;
}
