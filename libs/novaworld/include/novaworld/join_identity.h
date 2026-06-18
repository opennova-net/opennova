#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace opennova {

struct PubJoinIdentityPlaintexts {
	std::vector<uint8_t> pcid;
	std::vector<uint8_t> name_info;
	std::vector<uint8_t> squad_info;
};

std::vector<uint8_t> build_pub_pcid_plaintext(std::string_view pcid);
std::vector<uint8_t> build_pub_nameinfo_plaintext(std::string_view nwhandle);
std::vector<uint8_t> build_pub_squadinfo_plaintext(std::string_view nwhandle);

PubJoinIdentityPlaintexts build_pub_join_identity_plaintexts(
	std::string_view pcid,
	std::string_view nwhandle);

} // namespace opennova
