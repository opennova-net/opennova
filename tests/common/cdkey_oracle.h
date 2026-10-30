// The client's reading of an NWACT on its own machine, the oracle the service's activation is
// checked against (the activation's make, cdkey::make_activation, and the activation routes):
// the mix under its fingerprint, then the registry chain decoded (a room of 0x800), giving the
// wire key it keeps as a C string. False where the client's decode fails (its status -3). No
// shipped program reads an NWACT; the tests that check one share this.
// [orig: CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe): the fingerprint @0x411ebb,
//  the copy @0x411eca, NapiNP_EncryptStringWithPRNG @0x411ee9, CDKey_Decode under
//  off_812EE8 @0x411f0a, the decoded value stored at +16 @0x411f37; CDKey_Decode @0x411af0
//  (dfx2.exe): NapiNP_DecodeEncryptedString under the descriptor's +20 chain @0x411b38]
#pragma once

#include <net/novacrypto/cdkey.h>
#include <net/novacrypto/pubcrypto.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cdkey_oracle {

inline bool read_activation(const opennova::cdkey::KeyChains &chains, std::string_view nwact,
		std::string_view fingerprint, std::string &wire_key) {
	wire_key.clear();
	std::string registry(nwact);
	opennova::cdkey::mix_down(registry, fingerprint);
	std::vector<uint8_t> plaintext;
	if (!opennova::decode_key_chain(registry, chains.registry, plaintext, 0x800))
		return false;
	for (const uint8_t b : plaintext) {
		if (b == 0)
			break;
		wire_key.push_back(static_cast<char>(b));
	}
	return true;
}

} // namespace cdkey_oracle
