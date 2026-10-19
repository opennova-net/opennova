#include "nft_table.h"

#include "hosted/protocol/slots.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

namespace {

// The private destinations no slot's egress counts (the fences above them
// refuse what of those a slot may not reach).
constexpr const char *kPrivateIpv4 =
		"{ 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, 127.0.0.0/8 }";
constexpr const char *kPrivateIpv6 = "{ fc00::/7, fe80::/10, ::1 }";

std::string slot_uids() {
	return std::to_string(hosted::slot_uid(0)) + "-" +
			std::to_string(hosted::slot_uid(hosted::kMaxSlots - 1));
}

std::string admin_ports() {
	return std::to_string(hosted::slot_admin_port(0)) + "-" +
			std::to_string(hosted::slot_admin_port(hosted::kMaxSlots - 1));
}

// A command's failure in one line: "nft -f exited 1: <its first line>".
std::string failure(const std::string &what, const proc::CommandResult &result) {
	std::string err = strutil::trim(result.err);
	if (const size_t eol = err.find('\n'); eol != std::string::npos)
		err.resize(eol);
	if (err.size() > 200)
		err.resize(200);
	switch (result.outcome) {
		case proc::CommandResult::Outcome::Exited:
			return what + " exited " + std::to_string(result.exit_code) +
					(err.empty() ? "" : ": " + err);
		case proc::CommandResult::Outcome::TimedOut:
			return what + " timed out";
		case proc::CommandResult::Outcome::NotStarted:
			return what + " did not start: " + result.error;
	}
	return what + " failed";
}

// "egress-<k>" / "ingress-<k>": the slot, or -1.
int slot_of_counter(std::string_view name, std::string_view prefix) {
	if (name.substr(0, prefix.size()) != prefix)
		return -1;
	const std::string digits(name.substr(prefix.size()));
	if (digits.empty() || digits.size() > 2 || !strutil::all_digits(digits))
		return -1;
	const int slot = std::stoi(digits);
	return slot < hosted::kMaxSlots ? slot : -1;
}

} // namespace

NftTable::NftTable(
		proc::CommandRunner &runner, std::string nft, int fleet_port, std::string scratch_dir) :
		runner_(runner),
		nft_(std::move(nft)),
		fleet_port_(fleet_port),
		scratch_dir_(std::move(scratch_dir)) {}

std::string NftTable::ruleset(int fleet_port) {
	const std::string t = std::string("inet ") + kTable;
	std::string out = "add table " + t + "\n";
	for (int slot = 0; slot < hosted::kMaxSlots; ++slot)
		out += "add counter " + t + " egress-" + std::to_string(slot) + "\n";
	for (int slot = 0; slot < hosted::kMaxSlots; ++slot)
		out += "add counter " + t + " ingress-" + std::to_string(slot) + "\n";
	out += "add chain " + t + " output { type filter hook output priority 0; policy accept; }\n";
	out += "add chain " + t + " input { type filter hook input priority 0; policy accept; }\n";
	out += "flush chain " + t + " output\n";
	out += "flush chain " + t + " input\n";
	const std::string slots = "meta skuid " + slot_uids();
	out += "add rule " + t + " output " + slots +
			" ip daddr { 169.254.169.254, 169.254.170.2 } reject\n";
	out += "add rule " + t + " output " + slots + " ip6 daddr fd00:ec2::254 reject\n";
	// The fleet's port where no other entry of the set holds it already.
	std::string ports = admin_ports() + ", " + std::to_string(kEcsIntrospectionPort) + ", " +
			std::to_string(kEcsCredentialsPort);
	const bool in_admin = fleet_port >= hosted::slot_admin_port(0) &&
			fleet_port <= hosted::slot_admin_port(hosted::kMaxSlots - 1);
	if (fleet_port > 0 && !in_admin && fleet_port != kEcsIntrospectionPort &&
			fleet_port != kEcsCredentialsPort)
		ports = std::to_string(fleet_port) + ", " + ports;
	out += "add rule " + t + " output " + slots + " tcp dport { " + ports +
			" } ct state new reject with tcp reset\n";
	out += "add rule " + t + " output ip daddr " + kPrivateIpv4 + " return\n";
	out += "add rule " + t + " output ip6 daddr " + kPrivateIpv6 + " return\n";
	for (int slot = 0; slot < hosted::kMaxSlots; ++slot)
		out += "add rule " + t + " output meta skuid " + std::to_string(hosted::slot_uid(slot)) +
				" counter name \"egress-" + std::to_string(slot) + "\"\n";
	for (int slot = 0; slot < hosted::kMaxSlots; ++slot) {
		const std::string counter = " counter name \"ingress-" + std::to_string(slot) + "\"\n";
		out += "add rule " + t + " input udp dport " +
				std::to_string(hosted::slot_game_port(slot)) + counter;
		out += "add rule " + t + " input tcp dport " +
				std::to_string(hosted::slot_admin_port(slot)) + counter;
	}
	return out;
}

bool NftTable::ensure(std::string &error) {
	const std::string path = scratch_dir_ + "/opennova.nft";
	const std::string text = ruleset(fleet_port_);
	::unlink(path.c_str());
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = "cannot create " + path + ": " + std::strerror(errno);
		return false;
	}
	const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
	::close(fd);
	if (!written) {
		::unlink(path.c_str());
		error = "cannot write " + path;
		return false;
	}
	const proc::CommandResult result = runner_.run({ nft_, "-f", path }, kCommandTimeoutMs);
	::unlink(path.c_str());
	if (!result.ok()) {
		error = failure(nft_ + " -f", result);
		return false;
	}
	return true;
}

bool NftTable::read(std::map<int, SlotCount> &out, std::string &error) {
	const proc::CommandResult result = runner_.run(
			{ nft_, "-j", "list", "counters", "table", "inet", kTable }, kCommandTimeoutMs);
	if (!result.ok()) {
		error = failure(nft_ + " list counters", result);
		return false;
	}
	std::map<int, SlotCount> read;
	if (!parse_nft_counters(result.out, read, error))
		return false;
	if (read.size() != static_cast<size_t>(hosted::kMaxSlots)) {
		error = std::string("the table ") + kTable + " lacks a slot's counters";
		return false;
	}
	out = std::move(read);
	return true;
}

bool parse_nft_counters(std::string_view json, std::map<int, SlotCount> &out, std::string &error) {
	io::JsonValue v;
	std::string why;
	if (!io::json_parse(json, v, why) || !v.is_object()) {
		error = "nft's counters are no JSON object";
		return false;
	}
	const io::JsonValue *items = v.get("nftables");
	if (!items || !items->is_array()) {
		error = "nft's counters list no nftables array";
		return false;
	}
	out.clear();
	std::map<int, int> seen; // slot -> 1 egress, 2 ingress
	for (const io::JsonValue &item : items->array) {
		const io::JsonValue *counter = item.is_object() ? item.get("counter") : nullptr;
		if (!counter || !counter->is_object() ||
				counter->get_string("table", "") != NftTable::kTable ||
				counter->get_string("family", "") != "inet")
			continue;
		const std::string name = counter->get_string("name", "");
		const io::JsonValue *bytes = counter->get("bytes");
		uint64_t count = 0;
		if (!bytes || !io::json_exact_whole(*bytes, count))
			continue;
		if (const int slot = slot_of_counter(name, "egress-"); slot >= 0) {
			out[slot].egress_bytes = count;
			seen[slot] |= 1;
		} else if (const int in = slot_of_counter(name, "ingress-"); in >= 0) {
			out[in].ingress_bytes = count;
			seen[in] |= 2;
		}
	}
	// A slot is read only with both its counters.
	for (const auto &[slot, which] : seen)
		if (which != 3)
			out.erase(slot);
	return true;
}

} // namespace opennova::hostd
