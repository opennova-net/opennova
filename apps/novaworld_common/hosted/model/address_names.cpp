#include "address_names.h"

#include <algorithm>
#include <array>

namespace opennova::novaworld_server::hosted {

namespace {

// The names the service keeps: its hosts (www, nw, api, update, gs, lobby,
// gate, play, the name servers), its programs (web, fleet), its deployments
// (staging, prod, dev, test), the project's and the vendors' names, and the
// words a person would take for the service speaking (admin, billing,
// support, security, ...).
constexpr std::array<std::string_view, 33> kReserved = {
	"www", "nw", "api", "admin", "mail", "support", "help", "billing", "abuse", "security", "status",
	"update", "staging", "prod", "dev", "test", "opennova", "novaworld", "novalogic", "gs", "master",
	"lobby", "gate", "web", "fleet", "play", "ns", "ns1", "ns2", "ns3", "ns4", "root", "hostmaster",
};

bool is_lower_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

} // namespace

std::string address_name_refusal(std::string_view name) {
	if (name.size() < kAddressNameMin || name.size() > kAddressNameMax)
		return "a name is " + std::to_string(kAddressNameMin) + " to " + std::to_string(kAddressNameMax) +
				" characters";
	if (!std::all_of(name.begin(), name.end(), [](char c) { return is_lower_alnum(c) || c == '-'; }))
		return "a name holds lower-case letters, digits and '-' alone";
	if (!is_lower_alnum(name.front()) || !is_lower_alnum(name.back()))
		return "a name starts and ends with a letter or a digit";
	if (name.find("--") != std::string_view::npos) return "a name holds no \"--\"";
	if (std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
		return "a name is not all digits";
	if (name.rfind("srv-", 0) == 0 || std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end())
		return "that name is kept for the service";
	return {};
}

std::string address_hostname(std::string_view name, std::string_view zone) {
	std::string out(name);
	out += '.';
	out += zone;
	while (!out.empty() && out.back() == '.') out.pop_back();
	return out;
}

bool is_zone_name(const std::string &zone) {
	if (zone.empty() || zone.size() > 200 || zone.front() == '.' || zone.back() == '.') return false;
	bool dot_before = true;
	for (const char c : zone) {
		if (c == '.') {
			if (dot_before) return false;
			dot_before = true;
			continue;
		}
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
		dot_before = false;
	}
	return true;
}

} // namespace opennova::novaworld_server::hosted
