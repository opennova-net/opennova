#include <net/novaworld/gate_probe.h>

#include <base/io/strutil.h>
#include <net/novacrypto/nwu.h>

namespace opennova {

bool is_novaworld_domain_host(std::string_view host) {
	if (!host.empty() && host.back() == '.') host.remove_suffix(1); // the root label's dot
	const std::string_view domain = NOVAWORLD_DOMAIN;
	if (strutil::iequals(host, domain)) return true;
	return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
			strutil::ends_with_icase(host, domain);
}

std::vector<uint8_t> gate_probe_build(std::string_view tag, std::string_view nwu_key) {
	// Match the binary's ping-thread layout: the tag string PLUS its
	// trailing NUL is the exact byte range fed to the cipher
	// [orig: CNapiGateManager_ProbeThreadProc @0x6339e0 — strlen+1 @0x633aa1,
	//  NapiNP_EncryptBuffer(tag, len, "GATEAPI") @0x633aaf, sendto @0x633ade].
	std::vector<uint8_t> buf;
	buf.reserve(tag.size() + 1);
	buf.insert(buf.end(), tag.begin(), tag.end());
	buf.push_back(0x00);
	// The retail send side runs NapiNP_EncryptBuffer, whose ADD chain is our
	// nwu_decrypt (the nwu.h name-swap note); we mirror that.
	nwu_decrypt(buf.data(), buf.size(), nwu_key);
	return buf;
}

bool gate_response_decrypt_and_parse(const uint8_t *data, size_t len,
                                     GateResponse &out,
                                     std::string_view nwu_key) {
	if (!data || len == 0 || nwu_key.empty()) {
		out = GateResponse{};
		return false;
	}
	std::vector<uint8_t> plain(data, data + len);
	// [orig: NapiNP_DecryptBuffer(buf, len, "GATEAPI") @0x633bcc ->
	//  CNapiGateManager_SetResponseBuffer @0x633650]
	nwu_encrypt(plain.data(), plain.size(), nwu_key);
	std::string body(reinterpret_cast<const char *>(plain.data()), plain.size());
	return gate_response_parse(body, out);
}

} // namespace opennova
