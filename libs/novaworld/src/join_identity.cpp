#include <novaworld/join_identity.h>

#include <algorithm>
#include <cstddef>

namespace opennova {
namespace {

void append_cstr(std::vector<uint8_t> &out, std::string_view s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

} // namespace

std::vector<uint8_t> build_pub_pcid_plaintext(std::string_view pcid) {
	if (pcid.empty()) return {};
	std::vector<uint8_t> out;
	out.reserve(pcid.size() + 1);
	append_cstr(out, pcid);
	return out;
}

std::vector<uint8_t> build_pub_nameinfo_plaintext(std::string_view nwhandle) {
	if (nwhandle.empty()) return {};
	std::vector<uint8_t> out;
	out.reserve(nwhandle.size() + 1);
	append_cstr(out, nwhandle);
	return out;
}

std::vector<uint8_t> build_pub_squadinfo_plaintext(std::string_view nwhandle) {
	if (nwhandle.empty()) return {};
	const std::size_t short_len = std::min<std::size_t>(8, nwhandle.size());
	std::vector<uint8_t> out;
	out.reserve(4 + nwhandle.size() + 1 + short_len + 1);
	out.push_back(0);
	out.push_back(0);
	out.push_back(0);
	out.push_back(0);
	append_cstr(out, nwhandle);
	append_cstr(out, nwhandle.substr(0, short_len));
	return out;
}

PubJoinIdentityPlaintexts build_pub_join_identity_plaintexts(
		std::string_view pcid,
		std::string_view nwhandle) {
	PubJoinIdentityPlaintexts out;
	out.pcid = build_pub_pcid_plaintext(pcid);
	out.name_info = build_pub_nameinfo_plaintext(nwhandle);
	out.squad_info = build_pub_squadinfo_plaintext(nwhandle);
	return out;
}

} // namespace opennova
