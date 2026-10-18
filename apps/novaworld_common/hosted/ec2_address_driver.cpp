#include "ec2_address_driver.h"

#include "core/loggable.h"
#include "core/text_util.h"
#include "ec2_machine_driver.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <stdexcept>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

// `text` as a JSON string (json_write ends its text with a line break).
std::string json_text(const std::string &text) {
	std::string out = io::json_write(io::json_string(text));
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
	return out;
}

std::string tag_text(const io::JsonValue &address, const char *key) {
	const io::JsonValue *tags = address.get("Tags");
	if (tags == nullptr || !tags->is_array()) return {};
	for (const io::JsonValue &tag : tags->array) {
		if (tag.is_object() && tag.get_string("Key", "") == key) return tag.get_string("Value", "");
	}
	return {};
}

CloudAddress address_of(const io::JsonValue &a) {
	CloudAddress out;
	out.allocation_id = a.get_string("AllocationId", "");
	out.public_ip = a.get_string("PublicIp", "");
	out.association_id = a.get_string("AssociationId", "");
	out.instance_id = a.get_string("InstanceId", "");
	const std::string id = tag_text(a, Ec2AddressDriver::kTagAddressId);
	if (!id.empty() && id.size() <= 18 && strutil::all_digits(id)) out.address_id = strutil::parse_llong(id).value_or(0);
	return out;
}

AddressResult retry(const char *what, const proc::CommandResult &result) {
	AddressResult out;
	out.call = AddressCall::Retry;
	out.error = Ec2MachineDriver::cli_failure(what, result);
	return out;
}

AddressResult done(std::optional<CloudAddress> address = std::nullopt) {
	AddressResult out;
	out.call = AddressCall::Done;
	out.address = std::move(address);
	return out;
}

// A client error the cloud will answer the same every time.
bool is_client_error(const std::string &code) {
	return !code.empty() && code != "RequestLimitExceeded" && code != "Unavailable" && code != "InternalError" &&
			code != "ServiceUnavailable" && code != "Throttling";
}

} // namespace

Ec2AddressDriver::Ec2AddressDriver(Ec2AddressConfig config, proc::CommandRunner &runner) :
		config_(std::move(config)), runner_(runner) {}

std::string Ec2AddressDriver::check_config(const Ec2AddressConfig &c) {
	if (c.aws_cli.empty() || c.aws_cli[0] != '/') return "ONNET_HOSTED_AWS_CLI is no absolute path";
	if (c.region.empty()) return "ONNET_HOSTED_EC2_REGION is not set";
	if (c.deployment.empty() || c.deployment.size() > Ec2MachineDriver::kMaxDeploymentBytes ||
			!all_of(c.deployment, is_object_name_char))
		return "ONNET_HOSTED_DEPLOYMENT is no deployment name";
	return {};
}

std::string Ec2AddressDriver::error_code(const std::string &err) {
	const size_t at = err.find("An error occurred (");
	if (at == std::string::npos) return {};
	const size_t start = at + std::string("An error occurred (").size();
	const size_t end = err.find(')', start);
	if (end == std::string::npos || end - start > 80) return {};
	return err.substr(start, end - start);
}

std::vector<std::string> Ec2AddressDriver::command(const std::string &operation, std::vector<std::string> params) const {
	std::vector<std::string> argv = { config_.aws_cli, "ec2", operation };
	argv.insert(argv.end(), std::make_move_iterator(params.begin()), std::make_move_iterator(params.end()));
	const std::vector<std::string> globals = { "--region", config_.region, "--output", "json", "--no-cli-pager",
		"--cli-connect-timeout", "10", "--cli-read-timeout", "30" };
	argv.insert(argv.end(), globals.begin(), globals.end());
	return argv;
}

AddressResult Ec2AddressDriver::describe_with(std::vector<std::string> params, std::vector<CloudAddress> &listed) {
	const proc::CommandResult result = runner_.run(command("describe-addresses", std::move(params)), config_.command_timeout_ms);
	if (!result.ok()) {
		AddressResult out = retry("describe-addresses", result);
		out.code = error_code(result.err);
		return out;
	}
	io::JsonValue reply;
	std::string error;
	const io::JsonValue *addresses = io::json_parse(result.out, reply, error) ? reply.get("Addresses") : nullptr;
	if (addresses == nullptr || !addresses->is_array()) {
		AddressResult out;
		out.error = "describe-addresses answered no address list";
		return out;
	}
	for (const io::JsonValue &a : addresses->array) {
		if (a.is_object()) listed.push_back(address_of(a));
	}
	return done();
}

AddressResult Ec2AddressDriver::find_tagged(int64_t address_id) {
	const std::string filters = "[{\"Name\":\"tag:" + std::string(Ec2MachineDriver::kTagHosted) +
			"\",\"Values\":[\"1\"]},{\"Name\":\"tag:" + Ec2MachineDriver::kTagDeployment + "\",\"Values\":[" +
			json_text(config_.deployment) + "]},{\"Name\":\"tag:" + kTagAddressId + "\",\"Values\":[\"" +
			std::to_string(address_id) + "\"]}]";
	std::vector<CloudAddress> listed;
	AddressResult out = describe_with({ "--filters", filters }, listed);
	if (out.call == AddressCall::Done && !listed.empty()) out.address = listed.front();
	return out;
}

AddressResult Ec2AddressDriver::allocate(int64_t address_id, const std::string &name, const std::vector<std::string> &) {
	const auto tag = [](const std::string &key, const std::string &value) {
		return "{\"Key\":" + json_text(key) + ",\"Value\":" + json_text(value) + "}";
	};
	const std::string tags = tag(Ec2MachineDriver::kTagHosted, "1") + "," +
			tag(Ec2MachineDriver::kTagDeployment, config_.deployment) + "," +
			tag(kTagAddressId, std::to_string(address_id)) + "," + tag("Name", config_.deployment + "-address-" + name);
	const std::string specifications = "[{\"ResourceType\":\"elastic-ip\",\"Tags\":[" + tags + "]}]";
	const proc::CommandResult result = runner_.run(
			command("allocate-address", { "--domain", "vpc", "--tag-specifications", specifications }),
			config_.command_timeout_ms);
	if (!result.ok()) {
		AddressResult out = retry("allocate-address", result);
		out.code = error_code(result.err);
		if (is_client_error(out.code)) out.call = AddressCall::Refused;
		return out;
	}
	io::JsonValue reply;
	std::string error;
	if (!io::json_parse(result.out, reply, error) || !reply.is_object() ||
			reply.get_string("AllocationId", "").empty() || reply.get_string("PublicIp", "").empty()) {
		AddressResult out;
		out.error = "allocate-address answered no address";
		return out;
	}
	CloudAddress made;
	made.allocation_id = reply.get_string("AllocationId", "");
	made.public_ip = reply.get_string("PublicIp", "");
	made.address_id = address_id;
	return done(made);
}

AddressResult Ec2AddressDriver::associate(const CloudAddress &address, const std::string &cloud_id) {
	const proc::CommandResult result = runner_.run(command("associate-address",
														   { "--allocation-id", address.allocation_id, "--instance-id",
																   cloud_id, "--no-allow-reassociation" }),
			config_.command_timeout_ms);
	if (result.ok()) return done();
	AddressResult out = retry("associate-address", result);
	out.code = error_code(result.err);
	if (out.code == "Resource.AlreadyAssociated") {
		// Already this machine's (an earlier run's answer lost): done.
		const AddressResult now = describe(address.allocation_id);
		if (now.call == AddressCall::Done && now.address && now.address->instance_id == cloud_id) return done(now.address);
		return out;
	}
	if (out.code == "IncorrectInstanceState" || out.code == "InvalidInstanceID" ||
			out.code == "InvalidInstanceID.NotFound" || !is_client_error(out.code))
		return out;
	out.call = AddressCall::Refused;
	return out;
}

AddressResult Ec2AddressDriver::describe(const std::string &allocation_id) {
	std::vector<CloudAddress> listed;
	AddressResult out = describe_with({ "--allocation-ids", allocation_id }, listed);
	if (out.call != AddressCall::Done) {
		if (out.code == "InvalidAllocationID.NotFound") return done();
		return out;
	}
	if (!listed.empty()) out.address = listed.front();
	return out;
}

AddressResult Ec2AddressDriver::disassociate(const std::string &association_id) {
	const proc::CommandResult result =
			runner_.run(command("disassociate-address", { "--association-id", association_id }), config_.command_timeout_ms);
	if (result.ok()) return done();
	AddressResult out = retry("disassociate-address", result);
	out.code = error_code(result.err);
	if (out.code == "InvalidAssociationID.NotFound") return done();
	return out;
}

AddressResult Ec2AddressDriver::release(const std::string &allocation_id) {
	const proc::CommandResult result =
			runner_.run(command("release-address", { "--allocation-id", allocation_id }), config_.command_timeout_ms);
	if (result.ok()) return done();
	AddressResult out = retry("release-address", result);
	out.code = error_code(result.err);
	if (out.code == "InvalidAllocationID.NotFound") return done();
	return out;
}

std::vector<CloudAddress> Ec2AddressDriver::list_tagged() {
	const std::string filters = "[{\"Name\":\"tag:" + std::string(Ec2MachineDriver::kTagHosted) +
			"\",\"Values\":[\"1\"]},{\"Name\":\"tag:" + Ec2MachineDriver::kTagDeployment + "\",\"Values\":[" +
			json_text(config_.deployment) + "]}]";
	std::vector<CloudAddress> listed;
	const AddressResult out = describe_with({ "--filters", filters }, listed);
	if (out.call != AddressCall::Done) throw std::runtime_error("the addresses' listing: " + out.error);
	return listed;
}

} // namespace opennova::novaworld_server::hosted
