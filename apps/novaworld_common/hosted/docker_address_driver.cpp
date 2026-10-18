#include "docker_address_driver.h"

#include "loggable.h"

#include <base/io/json.h>

#include <algorithm>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

constexpr const char *kAllocationPrefix = "docker-";

std::string dotted(uint32_t address) {
	return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 0xFFu) + "." +
			std::to_string((address >> 8) & 0xFFu) + "." + std::to_string(address & 0xFFu);
}

AddressResult done(std::optional<CloudAddress> address = std::nullopt) {
	AddressResult out;
	out.call = AddressCall::Done;
	out.address = std::move(address);
	return out;
}

} // namespace

DockerAddressDriver::DockerAddressDriver(Ipv4Cidr range, DockerMachineConfig machines,
		DockerMachineDriver::EngineCall call) :
		range_(range), machines_(std::move(machines)), call_(std::move(call)) {
	if (!call_) call_ = DockerMachineDriver::http_call(machines_);
}

std::optional<Ipv4Cidr> DockerAddressDriver::parse_range(const std::string &text, std::string &why) {
	const auto range = parse_ipv4_cidr(text);
	if (!range) {
		why = "ONNET_HOSTED_DOCKER_ADDRESSES is no IPv4 network (172.30.77.64/28)";
		return std::nullopt;
	}
	if (!ipv4_cidr_private(*range) || range->mask < 0xFFFFFF00u || range->mask > 0xFFFFFFFCu) {
		why = "ONNET_HOSTED_DOCKER_ADDRESSES must be a private network of /24 to /30";
		return std::nullopt;
	}
	return range;
}

std::string DockerAddressDriver::allocation_of(const std::string &address) { return kAllocationPrefix + address; }

std::string DockerAddressDriver::address_of(const std::string &allocation_id) {
	return allocation_id.rfind(kAllocationPrefix, 0) == 0 ? allocation_id.substr(std::string(kAllocationPrefix).size())
														  : std::string();
}

AddressResult DockerAddressDriver::find_tagged(int64_t) { return done(); }

AddressResult DockerAddressDriver::allocate(int64_t address_id, const std::string &, const std::vector<std::string> &held) {
	const uint32_t size = ~range_.mask + 1u;
	for (uint32_t i = 0; i < size; ++i) {
		const std::string candidate = dotted(range_.network + i);
		if (std::find(held.begin(), held.end(), candidate) != held.end()) continue;
		CloudAddress made;
		made.allocation_id = allocation_of(candidate);
		made.public_ip = candidate;
		made.address_id = address_id;
		return done(made);
	}
	AddressResult out;
	out.call = AddressCall::Refused;
	out.code = "AddressLimitExceeded";
	out.error = "every address of " + ipv4_cidr_text(range_) + " is held";
	return out;
}

AddressResult DockerAddressDriver::associate(const CloudAddress &address, const std::string &cloud_id) {
	AddressResult out;
	const EngineReply reply = call_("GET", "/containers/" + cloud_id + "/json", {}, {});
	if (!reply.answered || reply.code != 200) {
		out.error = "inspect " + loggable(cloud_id, 64) + ": " +
				(reply.answered ? "HTTP " + std::to_string(reply.code) : loggable(reply.error, 200));
		return out;
	}
	io::JsonValue v;
	std::string error;
	std::string at;
	if (io::json_parse(reply.body, v, error) && v.is_object()) {
		if (const io::JsonValue *settings = v.get("NetworkSettings"); settings && settings->is_object())
			if (const io::JsonValue *networks = settings->get("Networks"); networks && networks->is_object())
				if (const io::JsonValue *mine = networks->get(machines_.network); mine && mine->is_object())
					at = mine->get_string("IPAddress", "");
	}
	if (at == address.public_ip) return done();
	if (at.empty()) {
		out.error = loggable(cloud_id, 64) + " has no address on " + machines_.network + " yet";
		return out;
	}
	out.call = AddressCall::Refused;
	out.code = "WrongAddress";
	out.error = loggable(cloud_id, 64) + " runs at " + at + ", not at " + address.public_ip;
	return out;
}

AddressResult DockerAddressDriver::describe(const std::string &allocation_id) {
	const std::string address = address_of(allocation_id);
	if (address.empty()) return done();
	CloudAddress out;
	out.allocation_id = allocation_id;
	out.public_ip = address;
	return done(out);
}

AddressResult DockerAddressDriver::disassociate(const std::string &) { return done(); }

AddressResult DockerAddressDriver::release(const std::string &) { return done(); }

std::vector<CloudAddress> DockerAddressDriver::list_tagged() { return {}; }

} // namespace opennova::novaworld_server::hosted
