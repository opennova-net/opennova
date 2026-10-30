// The DFX2 CD key (engine/net/novacrypto/cdkey.h): the vectors of
// docs/titles/dfx2-vs-jo-re.md section 9.7 (the wire key of a serial, a
// CDKIIDEXP1 under the empty chain, the nested NWACT for a sample NWPSSK and the
// boot-only one), the hardware mix's direction and inverse, the client's
// reading of an NWACT, and the LAN host's join codes 10 to 13.

#include <net/novacrypto/cdkey.h>
#include <net/protocol/protocol.h>
#include <net/novacrypto/pubcrypto.h>

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// DFX2's CD-key chains, its protocol's (Protocol::cd_key, games/dfx2/dfx2_protocol.cpp).
static const opennova::cdkey::KeyChains &dfx2_chains() {
	return *opennova::title_protocol("dfx2")->cd_key;
}

namespace cdkey = opennova::cdkey;

namespace {

constexpr const char *kSerial = "OPENNOVA-0001";
constexpr const char *kWire = "BMCCDKDKDLFFNGFDJHMFMMALJCLEFPADLPHHDHEEDHOGCIDLKC";
constexpr const char *kFingerprint = "ABCDEFGHIJKLMNOPQRSTUVW";
constexpr const char *kNestedNwact =
		"DQNWWKWDHZJHKFVXIHNJKEKYNSFFRIPKCOWFBPZIRXDVUSBLDCQDIQMRPDVQLBCDOPZRAYEREPP"
		"LAQYUYTMKDPRRYKCYXQGRVNIDZDVWMBTYOWSPQWUNOGWSNYIWCUOQJILP";
constexpr const char *kBootOnlyNwact =
		"CLCCYBAPFHAGHGAZHFWCHVGDJTDFFVUTBVBTPNEFMTIDNHEXTBVYYAOROHKMDZBVMMMVEX";

std::vector<uint8_t> bytes(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

std::string mixed_down(std::string text, const std::string &fingerprint) {
	cdkey::mix_down(text, fingerprint);
	return text;
}

std::string mixed_up(std::string text, const std::string &fingerprint) {
	cdkey::mix_up(text, fingerprint);
	return text;
}

// Section 9.7: the serial's wire key, 2 * (13 + 4 * 3) = 50 A-P characters, and back.
int wire_key_vector() {
	const std::string wire = cdkey::make_wire_key(dfx2_chains(), kSerial);
	TEST_EXPECT(wire == kWire);
	TEST_EXPECT(wire.size() == 50);
	std::string serial;
	TEST_EXPECT(cdkey::decode_wire_key(dfx2_chains(), wire, serial));
	TEST_EXPECT(serial == kSerial);
	// The same value under the chain constant spelled out (the descriptor's +16).
	TEST_EXPECT(opennova::encode_key_chain(bytes(kSerial),
						"5645236346346245:2345346541242546:4563451326353456") == kWire);
	return 0;
}

// Section 9.7: a CDKIIDEXP1 under the empty chain, 2 * (17 + 4) = 42 characters.
int exp1_vector() {
	const std::string exp1 =
			opennova::encode_key_chain(bytes("OPENNOVA-EXP-0001"), dfx2_chains().exp1);
	TEST_EXPECT(exp1 == "NCNCNGKECNHNNADHGAPONOAAODINDKCIHJNNKELPAN");
	std::vector<uint8_t> back;
	TEST_EXPECT(opennova::decode_key_chain(exp1, dfx2_chains().exp1, back));
	TEST_EXPECT(std::string(back.begin(), back.end()) == "OPENNOVA-EXP-0001");
	// The empty chain is one empty key, not none: the wire chain does not read it.
	TEST_EXPECT(!opennova::decode_key_chain(exp1, dfx2_chains().wire, back));
	return 0;
}

// Section 9.7: the nested (LAN-joinable) NWACT for the sample fingerprint, 2 * (50 + 4 * 4)
// = 132 letters, and the client's reading of it back to the wire key.
int nested_activation_vector() {
	const std::string nwact = cdkey::make_activation(dfx2_chains(), kWire, kFingerprint);
	TEST_EXPECT(nwact == kNestedNwact);
	TEST_EXPECT(nwact.size() == 132);
	for (const char c : nwact)
		TEST_EXPECT(c >= 'A' && c <= 'Z');
	std::string wire;
	TEST_EXPECT(cdkey::activation_wire_key(dfx2_chains(), nwact, kFingerprint, wire));
	TEST_EXPECT(wire == kWire);
	std::string serial;
	TEST_EXPECT(cdkey::decode_wire_key(dfx2_chains(), wire, serial));
	TEST_EXPECT(serial == kSerial);
	// The same inputs, the same NWACT: a machine activated again gets what it has.
	TEST_EXPECT(cdkey::make_activation(dfx2_chains(), kWire, kFingerprint) == nwact);
	// The registry chain's A-P intermediate is what the mix carries.
	TEST_EXPECT(mixed_down(nwact, kFingerprint) ==
			opennova::encode_key_chain(bytes(kWire), dfx2_chains().registry));
	return 0;
}

// Section 9.7: the boot-only NWACT, a plaintext straight under the registry chain
// (2 * (19 + 4 * 4) = 70 letters). The client reads it like any other.
int boot_only_activation_vector() {
	const std::string registry =
			opennova::encode_key_chain(bytes("OPENNOVA-LOCAL-0001"), dfx2_chains().registry);
	TEST_EXPECT(mixed_up(registry, kFingerprint) == kBootOnlyNwact);
	std::string value;
	TEST_EXPECT(cdkey::activation_wire_key(dfx2_chains(), kBootOnlyNwact, kFingerprint, value));
	TEST_EXPECT(value == "OPENNOVA-LOCAL-0001");
	// Its value is no wire key: a LAN host refuses it (code 11).
	std::string serial;
	TEST_EXPECT(!cdkey::decode_wire_key(dfx2_chains(), value, serial));
	return 0;
}

// The mix's direction, worked by hand from the original's arithmetic: the empty
// fingerprint seeds 50, the first step is (50 * 0x5731 + 1) mod 65536 = 1939, and a
// letter moves DOWN by 1939 mod 26 = 15 (A to L, wrapping), a digit by 1939 mod 10 = 9
// (0 to 1).
int mix_direction() {
	TEST_EXPECT(mixed_down("A", "") == "L");
	TEST_EXPECT(mixed_down("a", "") == "l");
	TEST_EXPECT(mixed_down("0", "") == "1");
	TEST_EXPECT(mixed_up("L", "") == "A");
	// One step per letter or digit; other bytes are left alone and take none.
	TEST_EXPECT(mixed_down("-A", "") == "-L");
	TEST_EXPECT(mixed_down("A-", "") == mixed_down("A", "") + "-");
	// The walk ends at a NUL.
	const std::string with_nul("A\0A", 3);
	const std::string down = mixed_down(with_nul, "");
	TEST_EXPECT(down.size() == 3 && down[0] == 'L' && down[1] == '\0' && down[2] == 'A');
	return 0;
}

// mix_up undoes mix_down over every class, for several fingerprints, and the
// fingerprint changes the stream.
int mix_round_trip() {
	std::string all;
	for (int c = 1; c < 256; ++c)
		all.push_back(static_cast<char>(c));
	for (const char *fingerprint :
			{ "", "ABCDEFGHIJKLMNOPQRSTUVW", "TTTTTTTTTTTTTTTTTTTTTTT", "x" }) {
		TEST_EXPECT(mixed_down(mixed_up(all, fingerprint), fingerprint) == all);
		TEST_EXPECT(mixed_up(mixed_down(all, fingerprint), fingerprint) == all);
		const std::string down = mixed_down(all, fingerprint);
		for (size_t i = 0; i < all.size(); ++i) {
			const char c = all[i];
			const bool shifts =
					(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
			if (!shifts)
				TEST_EXPECT(down[i] == c);
		}
	}
	TEST_EXPECT(mixed_down(kNestedNwact, kFingerprint) !=
			mixed_down(kNestedNwact, "ABCDEFGHIJKLMNOPQRSTUVX"));
	return 0;
}

// An NWACT is bound to its machine: under any other fingerprint, or damaged, the
// client's decode fails.
int activation_negatives() {
	std::string wire;
	TEST_EXPECT(!cdkey::activation_wire_key(
			dfx2_chains(), kNestedNwact, "ABCDEFGHIJKLMNOPQRSTUVX", wire));
	TEST_EXPECT(wire.empty());
	TEST_EXPECT(!cdkey::activation_wire_key(
			dfx2_chains(), kNestedNwact, "BBCDEFGHIJKLMNOPQRSTUVW", wire));
	std::string damaged = kNestedNwact;
	damaged[40] = damaged[40] == 'A' ? 'B' : 'A';
	TEST_EXPECT(!cdkey::activation_wire_key(dfx2_chains(), damaged, kFingerprint, wire));
	TEST_EXPECT(!cdkey::activation_wire_key(
			dfx2_chains(), std::string(kNestedNwact).substr(1), kFingerprint, wire));
	TEST_EXPECT(!cdkey::activation_wire_key(dfx2_chains(), "", kFingerprint, wire));
	// The wire key itself is no NWACT (it is A-P under the wire chain, not the registry's).
	TEST_EXPECT(!cdkey::activation_wire_key(dfx2_chains(), kWire, kFingerprint, wire));
	std::string serial;
	// A value that decodes past the client's 0x800-byte buffer is refused, one that fits is not.
	TEST_EXPECT(!cdkey::decode_wire_key(dfx2_chains(), cdkey::make_wire_key(dfx2_chains(), std::string(0x800, 'S')), serial));
	TEST_EXPECT(cdkey::decode_wire_key(dfx2_chains(), cdkey::make_wire_key(dfx2_chains(), std::string(0x800 - 12, 'S')), serial));
	TEST_EXPECT(!cdkey::activation_wire_key(dfx2_chains(),
			cdkey::make_activation(dfx2_chains(), std::string(0x800, 'S'), kFingerprint),
			kFingerprint, wire));
	TEST_EXPECT(!cdkey::decode_wire_key(dfx2_chains(), "", serial));
	TEST_EXPECT(!cdkey::decode_wire_key(dfx2_chains(), "QQQQ", serial));
	TEST_EXPECT(!cdkey::decode_wire_key(dfx2_chains(), std::string(kWire).substr(2), serial));
	return 0;
}

// The client keeps the decoded value as a C string (Napi_CopyString into g_CDKeyInfo +16): a
// value with a NUL in it is kept up to that NUL, and a failed read leaves nothing behind.
int activation_c_string() {
	std::string wire = "stale";
	const std::string with_nul("BMCC\0DKDK", 9);
	TEST_EXPECT(cdkey::activation_wire_key(dfx2_chains(),
			cdkey::make_activation(dfx2_chains(), with_nul, kFingerprint), kFingerprint, wire));
	TEST_EXPECT(wire == "BMCC");
	wire = "stale";
	TEST_EXPECT(!cdkey::activation_wire_key(dfx2_chains(), "NOTANWACT", kFingerprint, wire));
	TEST_EXPECT(wire.empty());
	return 0;
}

// A registry value in memory, as the read and the activation reach it: the reads and writes it
// answered, each refused while `refuse_reads` / `refuse_writes` count down.
struct MemoryStore {
	std::string value;
	bool present = false;
	int reads = 0;
	int writes = 0;
	int refuse_reads = 0;
	int refuse_writes = 0;
	cdkey::KeyStore store() {
		cdkey::KeyStore s;
		s.read = [this](std::string &out) {
			++reads;
			if (refuse_reads > 0) {
				--refuse_reads;
				return false;
			}
			if (!present)
				return false;
			out = value;
			return true;
		};
		s.write = [this](const std::string &text) {
			++writes;
			if (refuse_writes > 0) {
				--refuse_writes;
				return false;
			}
			value = text;
			present = true;
			return true;
		};
		return s;
	}
};

// The boot read as the shipped dfx2.exe runs it: the stored NWACT read back to its wire key, the
// status 1 whatever was read (the build's patch over the three refusals), a failed read tried
// once more, the "/MP" flag.
// [orig: CDKey_ReadRegistry @0x411b90 (dfx2.exe) - the reads @0x411bfe / @0x411c21, the patch
//  @0x411c2b / @0x411c3f / @0x411cb9, the "/MP" test @0x411c9b..0x411cb2, the status 1 @0x411cbb]
int registry_read() {
	MemoryStore memory;
	memory.value = kNestedNwact;
	memory.present = true;
	cdkey::KeyInfo info;
	TEST_EXPECT(cdkey::read_registry(info, dfx2_chains(), kFingerprint, memory.store()) == 1);
	TEST_EXPECT(info.status == 1 && info.keys.cdkiid == kWire && !info.multiplayer);
	TEST_EXPECT(info.keys.exp1.empty() && memory.reads == 1);
	// No value: still 1, with no key; the read tried twice.
	MemoryStore empty;
	cdkey::KeyInfo none;
	none.keys.cdkiid = "stale";
	TEST_EXPECT(cdkey::read_registry(none, dfx2_chains(), kFingerprint, empty.store()) == 1);
	TEST_EXPECT(none.status == 1 && none.keys.cdkiid.empty() && empty.reads == 2);
	// A first read refused, the second's value read.
	memory.reads = 0;
	memory.refuse_reads = 1;
	cdkey::KeyInfo second;
	TEST_EXPECT(cdkey::read_registry(second, dfx2_chains(), kFingerprint, memory.store()) == 1);
	TEST_EXPECT(second.keys.cdkiid == kWire && memory.reads == 2);
	// Another machine's value decodes to nothing here: 1, with no key.
	cdkey::KeyInfo other;
	TEST_EXPECT(cdkey::read_registry(
						other, dfx2_chains(), "ABCDEFGHIJKLMNOPQRSTUVX", memory.store()) == 1);
	TEST_EXPECT(other.keys.cdkiid.empty());
	// A decoded value ending "/MP" sets the multiplayer flag.
	MemoryStore mp;
	mp.value =
			mixed_up(opennova::encode_key_chain(bytes("OPENNOVA-0001/MP"), dfx2_chains().registry),
					kFingerprint);
	mp.present = true;
	cdkey::KeyInfo flagged;
	TEST_EXPECT(cdkey::read_registry(flagged, dfx2_chains(), kFingerprint, mp.store()) == 1);
	TEST_EXPECT(flagged.multiplayer && flagged.keys.cdkiid == "OPENNOVA-0001/MP");
	return 0;
}

// The activation from an NWACT: stored, then read back under this machine's fingerprint; a refused
// write tried once more and, refused again, -1 with nothing changed; a value that does not decode
// here -3 with the key kept.
// [orig: CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe) - the writes @0x411e88 / @0x411e9f,
//  the -1 @0x411eab, the -3 @0x411f13, the 1 @0x411f31 and the copy @0x411f37]
int registry_activation() {
	MemoryStore memory;
	cdkey::KeyInfo info;
	TEST_EXPECT(cdkey::write_activation(
						info, dfx2_chains(), kNestedNwact, kFingerprint, memory.store()) == 1);
	TEST_EXPECT(info.status == 1 && info.keys.cdkiid == kWire && memory.value == kNestedNwact);
	TEST_EXPECT(memory.writes == 1);
	// The value stored is the one the next boot reads.
	cdkey::KeyInfo booted;
	TEST_EXPECT(cdkey::read_registry(booted, dfx2_chains(), kFingerprint, memory.store()) == 1 &&
			booted.keys.cdkiid == kWire);
	// A first write refused, the second stored.
	MemoryStore once;
	once.refuse_writes = 1;
	cdkey::KeyInfo retried;
	TEST_EXPECT(cdkey::write_activation(
						retried, dfx2_chains(), kNestedNwact, kFingerprint, once.store()) == 1);
	TEST_EXPECT(once.writes == 2 && retried.keys.cdkiid == kWire);
	// Both refused (no administrator): -1, the record's key and flag as they were.
	MemoryStore refused;
	refused.refuse_writes = 2;
	cdkey::KeyInfo kept;
	kept.keys.cdkiid = "BEFORE";
	kept.multiplayer = true;
	TEST_EXPECT(cdkey::write_activation(
						kept, dfx2_chains(), kNestedNwact, kFingerprint, refused.store()) == -1);
	TEST_EXPECT(kept.status == -1 && kept.keys.cdkiid == "BEFORE" && kept.multiplayer &&
			!refused.present);
	// Stored but another machine's: -3, the key as it was.
	MemoryStore foreign;
	cdkey::KeyInfo before;
	before.keys.cdkiid = "BEFORE";
	TEST_EXPECT(cdkey::write_activation(before, dfx2_chains(), kNestedNwact,
						"ABCDEFGHIJKLMNOPQRSTUVX", foreign.store()) == -3);
	TEST_EXPECT(before.status == -3 && before.keys.cdkiid == "BEFORE" && foreign.present);
	return 0;
}

int fingerprint_shape() {
	TEST_EXPECT(cdkey::is_fingerprint(kFingerprint));
	TEST_EXPECT(cdkey::is_fingerprint("TTTTTTTTTTTTTTTTTTTTTTT"));
	TEST_EXPECT(!cdkey::is_fingerprint(""));
	TEST_EXPECT(!cdkey::is_fingerprint("ABCDEFGHIJKLMNOPQRSTUV"));
	TEST_EXPECT(!cdkey::is_fingerprint("ABCDEFGHIJKLMNOPQRSTUVWX"));
	TEST_EXPECT(!cdkey::is_fingerprint("abcdefghijklmnopqrstuvw"));
	TEST_EXPECT(!cdkey::is_fingerprint("ABCDEFGHIJKLMNOPQRSTUV1"));
	return 0;
}

// The LAN host's codes, in their order.
int lan_join_codes() {
	using V = cdkey::LanJoinVerdict;
	const std::string a = cdkey::make_wire_key(dfx2_chains(), "OPENNOVA-DFX2-A");
	const std::string b = cdkey::make_wire_key(dfx2_chains(), "OPENNOVA-DFX2-B");
	const std::string c = cdkey::make_wire_key(dfx2_chains(), "OPENNOVA-DFX2-C");
	const std::string exp_a = opennova::encode_key_chain(bytes("EXP-A"), dfx2_chains().exp1);
	const std::string exp_b = opennova::encode_key_chain(bytes("EXP-B"), dfx2_chains().exp1);
	std::string lower_a = a;
	for (char &ch : lower_a)
		ch = static_cast<char>(ch - 'A' + 'a');
	const cdkey::WireKeys host{ a, exp_a };
	const std::vector<cdkey::WireKeys> others{ { b, exp_b } };

	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, "" }, host, others) == V::Accepted);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { "", "" }, host, others) == V::MissingKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { "", exp_a }, host, others) == V::MissingKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { "NOTAKEY", "" }, host, others) == V::UndecodableKey);
	// A CDKIIDEXP1 value is not a wire key.
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { exp_a, "" }, host, others) == V::UndecodableKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { a, "" }, host, others) == V::HostsKey);
	// Compared case-insensitively: the lower-case spelling decodes nowhere, so it is
	// code 11 before the comparison, while the host's own value matches as sent.
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { lower_a, "" }, host, others) == V::UndecodableKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { a, "" }, { lower_a, "" }, {}) == V::HostsKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { b, "" }, host, others) == V::AnotherPlayersKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { b, "" }, host, { { lower_a, "" }, { c, "" }, { b, "" } }) ==
			V::AnotherPlayersKey);
	// A host with no key of its own refuses no joiner as its own.
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { a, "" }, { "", "" }, {}) == V::Accepted);
	// The CDKIIDEXP1 leg, run only for a joiner that sends one.
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, "NOTAKEY" }, host, others) == V::UndecodableKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, c }, host, others) == V::UndecodableKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, exp_a }, host, others) == V::HostsKey);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, exp_b }, host, others) == V::AnotherPlayersKey);
	const std::string exp_c = opennova::encode_key_chain(bytes("EXP-C"), dfx2_chains().exp1);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, exp_c }, host, others) == V::Accepted);
	TEST_EXPECT(cdkey::check_lan_join(dfx2_chains(), { c, exp_a }, { a, "" }, {}) == V::Accepted);
	TEST_EXPECT(static_cast<int>(V::MissingKey) == 10 &&
			static_cast<int>(V::UndecodableKey) == 11 && static_cast<int>(V::HostsKey) == 12 &&
			static_cast<int>(V::AnotherPlayersKey) == 13);
	return 0;
}

} // namespace

int main() {
	int rc = 0;
	rc |= wire_key_vector();
	rc |= exp1_vector();
	rc |= nested_activation_vector();
	rc |= boot_only_activation_vector();
	rc |= mix_direction();
	rc |= mix_round_trip();
	rc |= activation_negatives();
	rc |= activation_c_string();
	rc |= registry_read();
	rc |= registry_activation();
	rc |= fingerprint_shape();
	rc |= lan_join_codes();
	if (rc == 0)
		std::printf("OK: cdkey\n");
	return rc;
}
