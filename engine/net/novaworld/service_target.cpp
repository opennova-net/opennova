// The NovaWorld service the menu dials (service_target.h).
#include <net/novaworld/service_target.h>

#include <base/io/strutil.h>
#include <net/novaworld/gate_probe.h> // GATE_DEFAULT_HOST

#include <cctype>

namespace opennova {

namespace {

bool ends_with(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size() &&
			text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A dotted quad of four 0..255 decimal octets.
bool is_ipv4_literal(const std::string &text) {
	int octets = 0;
	size_t i = 0;
	while (i <= text.size()) {
		size_t digits = 0;
		int value = 0;
		while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
			value = value * 10 + (text[i] - '0');
			if (++digits > 3 || value > 255)
				return false;
			++i;
		}
		if (digits == 0)
			return false;
		++octets;
		if (i == text.size())
			break;
		if (text[i] != '.' || octets == 4)
			return false;
		++i;
	}
	return octets == 4;
}

// An IPv6 literal: colon-separated groups of at most four hex digits, at most one "::", eight
// groups without it (an IPv4 tail counting two).
bool is_ipv6_literal(const std::string &text) {
	if (text.find(':') == std::string::npos)
		return false;
	const size_t gap = text.find("::");
	if (gap != std::string::npos && text.find("::", gap + 1) != std::string::npos)
		return false;
	int groups = 0;
	size_t i = 0;
	while (i < text.size()) {
		if (text[i] == ':') {
			++i;
			continue;
		}
		size_t end = text.find(':', i);
		if (end == std::string::npos)
			end = text.size();
		const std::string group = text.substr(i, end - i);
		if (group.find('.') != std::string::npos) {
			if (end != text.size() || !is_ipv4_literal(group))
				return false;
			groups += 2;
		} else {
			if (group.size() > 4)
				return false;
			for (char c : group)
				if (!std::isxdigit(static_cast<unsigned char>(c)))
					return false;
			++groups;
		}
		i = end;
	}
	return gap != std::string::npos ? groups < 8 : groups == 8;
}

} // namespace

std::string opennova_gate_host(const std::string &flag_host, bool exported, bool debug) {
	if (!flag_host.empty())
		return flag_host;
	if (!exported)
		return kLocalGateHost;
	return debug ? kStagingGateHost : kOpenNovaGateHost;
}

std::string target_gate_host(NovaWorldTarget target, const std::string &opennova_host) {
	return target == NovaWorldTarget::Original ? std::string(GATE_DEFAULT_HOST) : opennova_host;
}

// The original gate's host is novaworld.net's [orig: "gs.novaworld.net" @0x7cc368;
// CNapiGateManager_Init @0x633f90 (host "novaworld.net")].
bool is_novalogic_host(const std::string &host) {
	const std::string name = strutil::to_lower(host);
	return name == "novaworld.net" || ends_with(name, ".novaworld.net");
}

std::string service_web_base(const std::string &gate_host) {
	if (is_novalogic_host(gate_host))
		return std::string();
	if (is_ipv4_literal(gate_host) || is_ipv6_literal(gate_host) || gate_host == "localhost") {
		const std::string host =
				gate_host.find(':') != std::string::npos ? "[" + gate_host + "]" : gate_host;
		return "http://" + host + ":" + std::to_string(kLocalWebPort);
	}
	return "https://" + (gate_host.rfind("nw.", 0) == 0 ? gate_host.substr(3) : gate_host);
}

NovaWorldTarget stored_novaworld_target(std::optional<int64_t> stored) {
	if (stored && *stored == static_cast<int64_t>(NovaWorldTarget::Original))
		return NovaWorldTarget::Original;
	return NovaWorldTarget::OpenNova;
}

NovaWorldTarget launch_novaworld_target(
		const std::string &flag_host, bool joining, NovaWorldTarget stored) {
	if (!flag_host.empty() && !joining)
		return NovaWorldTarget::OpenNova;
	return stored;
}

} // namespace opennova
