#include "ec2_machine_driver.h"

#include "agent_protocol.h"
#include "hosted_settings.h"
#include "loggable.h"
#include "net_cidr.h"
#include "owner_file.h"
#include "text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool all_of(std::string_view text, bool (*ok)(char)) {
	for (const char c : text)
		if (!ok(c))
			return false;
	return true;
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}
bool is_lower_hex(char c) {
	return is_digit(c) || (c >= 'a' && c <= 'f');
}
bool is_region_char(char c) {
	return (c >= 'a' && c <= 'z') || is_digit(c) || c == '-';
}
bool is_name_char(char c) {
	return is_alnum(c) || c == '.' || c == '_' || c == '-';
}
bool is_label_char(char c) {
	return (c >= 'a' && c <= 'z') || is_digit(c) || c == '-';
}

// An id EC2 spells `<prefix>` and 8 to 17 lower-case hex digits.
bool is_aws_id(std::string_view text, std::string_view prefix) {
	if (text.substr(0, prefix.size()) != prefix)
		return false;
	const std::string_view digits = text.substr(prefix.size());
	return digits.size() >= 8 && digits.size() <= 17 && all_of(digits, is_lower_hex);
}

bool is_instance_id(std::string_view text) {
	return is_aws_id(text, "i-");
}

// A launch template's spec: "lt-<id>" (its default version) or
// "lt-<id>:<version>" (a version number, or $Default / $Latest).
bool split_template(const std::string &spec, std::string &id, std::string &version) {
	const size_t colon = spec.find(':');
	id = spec.substr(0, colon);
	version = colon == std::string::npos ? "$Default" : spec.substr(colon + 1);
	if (!is_aws_id(id, "lt-"))
		return false;
	if (version == "$Default" || version == "$Latest")
		return true;
	return !version.empty() && version.size() <= 9 && version[0] != '0' &&
			all_of(version, is_digit);
}

// A JSON string literal of `text` (the values here are checked ASCII; the
// escape is for form's sake).
std::string json_text(const std::string &text) {
	std::string out = "\"";
	for (const char c : text) {
		if (c == '"' || c == '\\')
			out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

std::string json_tag(const std::string &key, const std::string &value) {
	return "{\"Key\":" + json_text(key) + ",\"Value\":" + json_text(value) + "}";
}

const io::JsonValue *member(
		const io::JsonValue &object, const char *key, io::JsonValue::Type type) {
	const io::JsonValue *v = object.get(key);
	return v != nullptr && v->type == type ? v : nullptr;
}

std::string text_member(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = member(object, key, io::JsonValue::Type::String);
	return v != nullptr ? v->string : std::string();
}

std::string tag_value(const io::JsonValue &instance, const char *key) {
	const io::JsonValue *tags = member(instance, "Tags", io::JsonValue::Type::Array);
	if (tags == nullptr)
		return {};
	for (const io::JsonValue &tag : tags->array)
		if (tag.is_object() && text_member(tag, "Key") == key)
			return text_member(tag, "Value");
	return {};
}

std::string state_name_of(const io::JsonValue &instance) {
	const io::JsonValue *state = member(instance, "State", io::JsonValue::Type::Object);
	return state != nullptr ? text_member(*state, "Name") : std::string();
}

int64_t machine_id_of(const std::string &tag) {
	if (tag.empty() || tag.size() > 18 || !all_of(tag, is_digit))
		return 0;
	return static_cast<int64_t>(strutil::parse_llong(tag).value_or(0));
}

// A zone that cannot take a launch now: no capacity for the machine type
// there, or the type not offered in it. Another zone may.
bool says_zone_cannot(const proc::CommandResult &result) {
	return result.outcome == proc::CommandResult::Outcome::Exited && result.exit_code != 0 &&
			(result.err.find("(InsufficientInstanceCapacity)") != std::string::npos ||
					result.err.find("(Unsupported)") != std::string::npos);
}

bool is_listed_state(const std::string &state) {
	return state == "pending" || state == "running" || state == "stopping" || state == "stopped";
}

// Every occurrence of `blank` in `out` filled with `value`; false when there is none.
bool fill(std::string &out, const std::string &blank, const std::string &value) {
	bool filled = false;
	for (size_t at = out.find(blank); at != std::string::npos;
			at = out.find(blank, at + value.size())) {
		out.replace(at, blank.size(), value);
		filled = true;
	}
	return filled;
}

} // namespace

std::string Ec2MachineDriver::cli_failure(const char *what, const proc::CommandResult &result) {
	std::string out = std::string("aws ec2 ") + what;
	switch (result.outcome) {
		case proc::CommandResult::Outcome::NotStarted:
			return out + " did not start: " + loggable(result.error, 200);
		case proc::CommandResult::Outcome::TimedOut:
			return out + " timed out";
		case proc::CommandResult::Outcome::Exited:
			break;
	}
	out += " exited with code " + std::to_string(result.exit_code);
	const std::string err(strutil::trim_view(result.err));
	if (!err.empty())
		out += ": " + loggable(err, 300);
	return out;
}

Ec2MachineDriver::Ec2MachineDriver(
		Ec2MachineConfig config, proc::CommandRunner &runner, Clock now_ms) :
		config_(std::move(config)),
		runner_(runner),
		now_(now_ms ? std::move(now_ms) : Clock(steady_now_ms)) {}

std::string Ec2MachineDriver::volume_label(std::string_view data_set) {
	constexpr std::string_view kSuffix = "_consumer";
	if (data_set.size() > kSuffix.size() &&
			data_set.substr(data_set.size() - kSuffix.size()) == kSuffix)
		data_set.remove_suffix(kSuffix.size());
	std::string label = "od-";
	for (const char c : data_set)
		if (c != '_')
			label.push_back(c);
	if (label.size() == 3 || label.size() > kMaxVolumeLabelBytes || !all_of(label, is_label_char))
		return {};
	return label;
}

std::string Ec2MachineDriver::check_config(const Ec2MachineConfig &c) {
	if (c.aws_cli.empty() || c.aws_cli[0] != '/')
		return "ONNET_HOSTED_AWS_CLI is no absolute path";
	if (c.region.empty() || c.region.size() > 32 || !all_of(c.region, is_region_char))
		return "ONNET_HOSTED_EC2_REGION is no region name";
	if (c.subnets.empty())
		return "ONNET_HOSTED_EC2_SUBNETS names no subnet";
	for (auto it = c.subnets.begin(); it != c.subnets.end(); ++it) {
		if (!is_aws_id(*it, "subnet-"))
			return "ONNET_HOSTED_EC2_SUBNETS holds '" + loggable(*it, 32) + "', no subnet id";
		if (std::find(c.subnets.begin(), it, *it) != it)
			return "ONNET_HOSTED_EC2_SUBNETS names " + *it + " twice";
	}
	// At most kMaxDeploymentBytes, so every launch's client token fits EC2's 64.
	if (c.deployment.empty() || c.deployment.size() > kMaxDeploymentBytes ||
			!all_of(c.deployment, is_name_char))
		return "ONNET_HOSTED_DEPLOYMENT is not 1.." + std::to_string(kMaxDeploymentBytes) +
				" letters, digits, '.', '_' and '-'";
	if (c.launch_attempts < 1 || c.launch_timeout_ms <= 0)
		return "no launch attempt";
	std::string id;
	std::string version;
	if (!split_template(c.shared_template, id, version))
		return "ONNET_HOSTED_EC2_LAUNCH_TEMPLATES names no shared launch template "
			   "(lt-<id>[:<version>])";
	if (!split_template(c.own_template, id, version))
		return "ONNET_HOSTED_EC2_LAUNCH_TEMPLATES names no own launch template "
			   "(lt-<id>[:<version>])";
	// An image reference starts with a letter or digit (never '-', which
	// docker would read as an option).
	if (!is_image_reference(c.serve_image))
		return "ONNET_HOSTED_SERVE_IMAGE is no image reference";
	if (!c.hostd_image.empty() && !is_image_reference(c.hostd_image))
		return "ONNET_HOSTED_HOSTD_IMAGE is no image reference";
	std::string service_host;
	uint16_t service_port = 0;
	if (!parse_service_url(c.service_url, service_host, service_port))
		return "ONNET_INTERNAL_URL is no http://<host>:<port> the machines can reach";
	if (c.data_sets.empty())
		return "no data set for the machines to mount";
	for (const std::string &data_set : c.data_sets)
		if (!is_data_set_name(data_set) || volume_label(data_set).empty())
			return "data set '" + loggable(data_set) + "' has no volume label";
	for (const char *blank : { kBlankServiceUrl, kBlankServicePort, kBlankMachineId,
				 kBlankMachineToken, kBlankServeImage, kBlankHostdImage, kBlankDataSets })
		if (c.machine_script.find(blank) == std::string::npos)
			return std::string(
						   "the machine script (ONNET_HOSTED_MACHINE_SCRIPT) lacks its blank ") +
					blank;
	if (c.machine_script.size() > kMaxUserDataBytes - 1024)
		return "the machine script is too long for user-data";
	return {};
}

bool Ec2MachineDriver::parse_service_url(std::string_view url, std::string &host, uint16_t &port) {
	constexpr std::string_view kScheme = "http://";
	if (url.substr(0, kScheme.size()) != kScheme)
		return false;
	const std::string_view authority = url.substr(kScheme.size());
	const size_t colon = authority.rfind(':');
	if (colon == std::string_view::npos || colon == 0 || colon > 253)
		return false;
	const std::string_view name = authority.substr(0, colon);
	const std::string_view digits = authority.substr(colon + 1);
	const auto host_char = [](char c) { return is_alnum(c) || c == '.' || c == '-'; };
	for (const char c : name)
		if (!host_char(c))
			return false;
	if (name.front() == '.' || name.front() == '-' || digits.empty() || digits.size() > 5 ||
			!all_of(digits, is_digit))
		return false;
	const auto value = strutil::parse_int(std::string(digits));
	if (!value || *value < 1 || *value > 65535)
		return false;
	host.assign(name);
	port = static_cast<uint16_t>(*value);
	return true;
}

std::optional<std::string> Ec2MachineDriver::render_user_data(
		const std::string &script, const MachineUserData &values) {
	std::string service_host;
	uint16_t service_port = 0;
	if (!parse_service_url(values.service_url, service_host, service_port) ||
			values.service_port != service_port || values.machine_id <= 0 ||
			values.bootstrap_token.size() != kMachineTokenChars ||
			!all_of(values.bootstrap_token, is_alnum) || !is_image_reference(values.serve_image) ||
			!is_image_reference(values.hostd_image) || values.data_sets.empty())
		return std::nullopt;
	std::string data_sets;
	for (const auto &[data_set, label] : values.data_sets) {
		if (!is_data_set_name(data_set) || label.size() < 4 ||
				label.size() > kMaxVolumeLabelBytes || !all_of(label, is_label_char))
			return std::nullopt;
		if (!data_sets.empty())
			data_sets += ' ';
		data_sets += data_set + "=" + label;
	}
	std::string out = script;
	const bool filled = fill(out, kBlankServiceUrl, values.service_url) &
			fill(out, kBlankServicePort, std::to_string(values.service_port)) &
			fill(out, kBlankMachineId, std::to_string(values.machine_id)) &
			fill(out, kBlankMachineToken, values.bootstrap_token) &
			fill(out, kBlankServeImage, values.serve_image) &
			fill(out, kBlankHostdImage, values.hostd_image) & fill(out, kBlankDataSets, data_sets);
	if (!filled || out.size() > kMaxUserDataBytes)
		return std::nullopt;
	return out;
}

std::vector<std::string> Ec2MachineDriver::command(
		const std::string &operation, std::vector<std::string> params) const {
	std::vector<std::string> argv = { config_.aws_cli, "ec2", operation };
	argv.insert(argv.end(), std::make_move_iterator(params.begin()),
			std::make_move_iterator(params.end()));
	const std::vector<std::string> globals = { "--region", config_.region, "--output", "json",
		"--no-cli-pager", "--cli-connect-timeout", "10", "--cli-read-timeout", "30" };
	argv.insert(argv.end(), globals.begin(), globals.end());
	return argv;
}

MachineProfile Ec2MachineDriver::profile(const Ec2MachineConfig &config) {
	MachineProfile out;
	out.provider = "ec2";
	out.boot_timeout_ms = kBootTimeoutMs;
	out.slot_plan = [pattern = admin_pattern(config.admin_address)](int slot) {
		SlotPlan plan;
		plan.game_port = slot_game_port(slot);
		plan.admin_port = slot_admin_port(slot);
		plan.admin_allowed_ip = pattern;
		return plan;
	};
	return out;
}

std::string Ec2MachineDriver::admin_pattern(const std::string &controller_address) {
	const auto address = parse_ipv4(controller_address);
	if (!address)
		return controller_address;
	return std::to_string(*address >> 24) + "." + std::to_string((*address >> 16) & 0xFFu) + ".*";
}

std::string Ec2MachineDriver::client_token(
		const std::string &deployment, int64_t machine_id, const std::string &subnet) {
	return deployment + "-m" + std::to_string(machine_id) + "-" +
			subnet.substr(subnet.size() < 8 ? 0 : subnet.size() - 8);
}

std::string Ec2MachineDriver::tag_filters(int64_t machine_id) const {
	std::string filters = "[{\"Name\":\"tag:" + std::string(kTagHosted) +
			"\",\"Values\":[\"1\"]},{\"Name\":\"tag:" + kTagDeployment + "\",\"Values\":[" +
			json_text(config_.deployment) + "]}";
	if (machine_id > 0)
		filters += ",{\"Name\":\"tag:" + std::string(kTagMachineId) + "\",\"Values\":[\"" +
				std::to_string(machine_id) + "\"]}";
	return filters + "]";
}

bool Ec2MachineDriver::parse_listing(const std::string &out,
		std::map<std::string, Instance> &listed, std::string &error) const {
	io::JsonValue reply;
	std::string parse_error;
	if (!io::json_parse(out, reply, parse_error)) {
		error = "describe-instances answered no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *reservations = member(reply, "Reservations", io::JsonValue::Type::Array);
	if (reservations == nullptr) {
		error = "describe-instances answered no Reservations list";
		return false;
	}
	for (const io::JsonValue &reservation : reservations->array) {
		const io::JsonValue *list = reservation.is_object()
				? member(reservation, "Instances", io::JsonValue::Type::Array)
				: nullptr;
		if (list == nullptr)
			continue;
		for (const io::JsonValue &instance : list->array) {
			if (!instance.is_object())
				continue;
			const std::string id = text_member(instance, "InstanceId");
			// The filter's own scope again: a machine without both tags is not
			// this deployment's.
			if (!is_instance_id(id) || tag_value(instance, kTagHosted) != "1" ||
					tag_value(instance, kTagDeployment) != config_.deployment)
				continue;
			Instance seen;
			seen.state = state_name_of(instance);
			seen.private_address = text_member(instance, "PrivateIpAddress");
			seen.public_address = text_member(instance, "PublicIpAddress");
			seen.subnet = text_member(instance, "SubnetId");
			if (!seen.private_address.empty() && !parse_ipv4(seen.private_address))
				seen.private_address.clear();
			if (!seen.public_address.empty() && !parse_ipv4(seen.public_address))
				seen.public_address.clear();
			seen.machine_id = machine_id_of(tag_value(instance, kTagMachineId));
			listed[id] = std::move(seen);
		}
	}
	return true;
}

std::optional<LaunchedMachine> Ec2MachineDriver::launch(
		const MachineLaunch &launch, std::string &error) {
	// config_ never changes: everything up to the bookkeeping runs unlocked, the
	// CLI call above all.
	if (launch.machine_id <= 0) {
		error = "no machine id";
		return std::nullopt;
	}
	const std::string &spec =
			launch.kind == MachineKind::Own ? config_.own_template : config_.shared_template;
	std::string template_id;
	std::string template_version;
	if (!split_template(spec, template_id, template_version)) {
		error = std::string("no launch template for ") + machine_kind_text(launch.kind) +
				" machines (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES)";
		return std::nullopt;
	}
	// The subnets it tries, in order: a run again first where its row recorded
	// an earlier run's call, else where a describe finds that run's machine,
	// and it launches nothing when it cannot tell.
	const std::optional<std::vector<std::string>> subnets = subnet_order(launch, error);
	if (!subnets)
		return std::nullopt;
	MachineUserData values;
	values.service_url = config_.service_url;
	std::string service_host;
	parse_service_url(config_.service_url, service_host, values.service_port);
	values.machine_id = launch.machine_id;
	values.bootstrap_token = launch.bootstrap_token;
	values.serve_image = config_.serve_image;
	values.hostd_image = config_.hostd_image.empty() ? config_.serve_image : config_.hostd_image;
	// The order the launch template attaches the volumes in.
	std::vector<std::string> data_sets = config_.data_sets;
	std::sort(data_sets.begin(), data_sets.end());
	for (const std::string &data_set : data_sets)
		values.data_sets.emplace_back(data_set, volume_label(data_set));
	const std::optional<std::string> user_data = render_user_data(config_.machine_script, values);
	if (!user_data) {
		error = "the machine script did not render";
		return std::nullopt;
	}
	const fs::path user_data_path =
			fs::u8path(config_.scratch_dir) / ("user-data-" + random_secret(16) + ".sh");
	if (!write_owner_file(user_data_path, *user_data, true, error))
		return std::nullopt;

	const std::string machine = std::to_string(launch.machine_id);
	const std::string tags = json_tag(kTagHosted, "1") + "," +
			json_tag(kTagDeployment, config_.deployment) + "," +
			json_tag(kTagMachineKind, machine_kind_text(launch.kind)) + "," +
			json_tag(kTagMachineId, machine) + "," +
			json_tag("Name",
					config_.deployment + "-" + machine_kind_text(launch.kind) + "-" + machine);
	const std::string tag_specifications = "[{\"ResourceType\":\"instance\",\"Tags\":[" + tags +
			"]},{\"ResourceType\":\"volume\",\"Tags\":[" + tags + "]}]";
	const std::string launch_template = "{\"LaunchTemplateId\":" + json_text(template_id) +
			",\"Version\":" + json_text(template_version) + "}";
	// A zone that cannot take the launch now hands it to the next subnet, under
	// that subnet's own client token.
	proc::CommandResult result;
	std::string subnet;
	std::string unrecorded;
	for (size_t next = 0; next < subnets->size(); ++next) {
		subnet = (*subnets)[next];
		// The subnet on the row before its call, so a run again calls here first
		// (the same token) whether or not EC2 lists this call's machine by then;
		// unrecorded, nothing is called.
		if (launch.record_subnet && !launch.record_subnet(subnet)) {
			unrecorded = subnet;
			break;
		}
		const std::vector<std::string> argv = command("run-instances",
				{ "--launch-template", launch_template, "--subnet-id", subnet, "--count", "1",
						"--client-token",
						client_token(config_.deployment, launch.machine_id, subnet),
						"--tag-specifications", tag_specifications, "--user-data",
						"file://" + user_data_path.u8string() });
		// The client token is the row's own in this subnet, so a call the timeout or
		// the network cut short (its machine may run) is made again, the same, and
		// EC2 answers it with the machine it launched then, never a second one.
		for (int attempt = 1; attempt <= config_.launch_attempts; ++attempt) {
			result = runner_.run(argv, config_.launch_timeout_ms);
			const bool cut_short = result.outcome == proc::CommandResult::Outcome::TimedOut ||
					(result.outcome == proc::CommandResult::Outcome::Exited &&
							result.exit_code == 255);
			if (result.ok() || !cut_short || attempt == config_.launch_attempts)
				break;
			std::fprintf(stderr,
					"[fleet] WARN ec2: machine %lld: %s; asking again with the same client token\n",
					static_cast<long long>(launch.machine_id),
					cli_failure("run-instances", result).c_str());
		}
		if (result.ok() || !says_zone_cannot(result) || next + 1 == subnets->size())
			break;
		std::fprintf(stderr, "[fleet] WARN ec2: machine %lld: %s in %s; launching in %s instead\n",
				static_cast<long long>(launch.machine_id),
				cli_failure("run-instances", result).c_str(), subnet.c_str(),
				(*subnets)[next + 1].c_str());
	}
	std::error_code ec;
	fs::remove(user_data_path, ec);
	if (!unrecorded.empty()) {
		error = "its call in " + unrecorded + " could not be recorded on its row first";
		if (unrecorded != subnets->front())
			error += " (after " + cli_failure("run-instances", result) + ")";
		return std::nullopt;
	}
	if (!result.ok()) {
		error = cli_failure("run-instances", result);
		return std::nullopt;
	}

	io::JsonValue reply;
	std::string parse_error;
	const io::JsonValue *instances = nullptr;
	if (io::json_parse(result.out, reply, parse_error))
		instances = member(reply, "Instances", io::JsonValue::Type::Array);
	const io::JsonValue *instance =
			instances != nullptr && instances->array.size() == 1 && instances->array[0].is_object()
			? &instances->array[0]
			: nullptr;
	LaunchedMachine out;
	out.cloud_id = instance != nullptr ? text_member(*instance, "InstanceId") : std::string();
	if (!is_instance_id(out.cloud_id)) {
		error = "run-instances answered no machine id (" +
				(parse_error.empty() ? std::string("no Instances[0].InstanceId")
									 : loggable(parse_error, 120)) +
				")";
		return std::nullopt;
	}
	out.private_address = text_member(*instance, "PrivateIpAddress");
	out.public_address = text_member(*instance, "PublicIpAddress");
	out.instance_type = text_member(*instance, "InstanceType");
	out.template_version = template_id + ":" + template_version;
	if (!parse_ipv4(out.private_address)) {
		// A machine whose agent nothing would admit: ended now rather than left to
		// boot (this very launch tagged it).
		error = "run-instances answered machine " + out.cloud_id + " with no private address";
		std::string why;
		if (!terminate_now(out.cloud_id, why))
			error += "; ending it failed too (" + why + ")";
		return std::nullopt;
	}
	if (!out.public_address.empty() && !parse_ipv4(out.public_address))
		out.public_address.clear();
	Instance seen;
	seen.state = state_name_of(*instance);
	if (seen.state.empty())
		seen.state = "pending";
	seen.private_address = out.private_address;
	seen.public_address = out.public_address;
	seen.subnet = subnet;
	seen.machine_id = launch.machine_id;
	{
		std::lock_guard<std::mutex> lock(mu_);
		instances_[out.cloud_id] = seen;
	}
	std::printf("[fleet] ec2: machine %lld (%s) launched as %s at %s in %s\n",
			static_cast<long long>(launch.machine_id), machine_kind_text(launch.kind),
			out.cloud_id.c_str(), out.private_address.c_str(), subnet.c_str());
	return out;
}

bool Ec2MachineDriver::read_listing(std::map<std::string, Instance> &listed, std::string &error) {
	const proc::CommandResult result =
			runner_.run(command("describe-instances", { "--filters", tag_filters() }),
					config_.command_timeout_ms);
	if (!result.ok()) {
		error = cli_failure("describe-instances", result);
		return false;
	}
	return parse_listing(result.out, listed, error);
}

void Ec2MachineDriver::take_listing(std::map<std::string, Instance> listed, uint64_t now_ms) {
	instances_ = std::move(listed);
	have_listing_ = true;
	listed_ms_ = now_ms;
	// EC2 lists a terminated machine for about an hour.
	for (auto it = terminated_ms_.begin(); it != terminated_ms_.end();)
		it = now_ms - it->second > 3 * 60 * 60 * 1000ull ? terminated_ms_.erase(it) : std::next(it);
	if (describe_failing_) {
		describe_failing_ = false;
		std::printf("[fleet] ec2: describe-instances answers again\n");
	}
}

void Ec2MachineDriver::note_describe_failure(uint64_t now_ms, const std::string &error) {
	if (!describe_failing_ || now_ms - describe_failure_logged_ms_ >= 60 * 1000) {
		std::fprintf(stderr,
				"[fleet] WARN ec2: %s; the machines read as unknown until it answers\n",
				error.c_str());
		describe_failure_logged_ms_ = now_ms;
	}
	describe_failing_ = true;
	describe_failed_ms_ = now_ms;
}

bool Ec2MachineDriver::refresh_listing(uint64_t now) {
	// `now` - `then`, 0 for a time ahead of it (another caller's, read later).
	const auto age = [now](uint64_t then) { return now > then ? now - then : 0; };
	std::unique_lock<std::mutex> lock(mu_);
	for (;;) {
		const bool fresh = have_listing_ && age(listed_ms_) < config_.describe_ttl_ms;
		// A describe-instances that failed is tried again describe_ttl_ms later,
		// not at every describe: while the EC2 API is down, a pass's describes
		// would otherwise each wait out a CLI run.
		const bool backing_off =
				describe_failing_ && age(describe_failed_ms_) < config_.describe_ttl_ms;
		if (fresh || backing_off)
			return fresh;
		if (!listing_in_flight_)
			break;
		// Another caller reads it now: its answer is this one's too, so the jobs
		// asking in one window run one describe-instances between them.
		listing_read_.wait(lock, [this] { return !listing_in_flight_; });
	}
	listing_in_flight_ = true;
	lock.unlock();
	std::map<std::string, Instance> listed;
	std::string error;
	// The CLI call outside the lock.
	const bool read = read_listing(listed, error);
	lock.lock();
	listing_in_flight_ = false;
	if (read)
		take_listing(std::move(listed), now);
	else
		note_describe_failure(now, error);
	listing_read_.notify_all();
	return read;
}

std::optional<std::vector<std::string>> Ec2MachineDriver::subnet_order(
		const MachineLaunch &launch, std::string &error) {
	const int64_t machine_id = launch.machine_id;
	const bool run_again = launch.attempt > 1;
	std::vector<std::string> order = config_.subnets;
	if (order.size() < 2)
		return order;
	// Where an earlier run last called, as its row recorded it before the call:
	// no listing needed, so none read too early (EC2 lists a launch late) sends
	// the run again to the other subnet's token and a second machine.
	std::string own = launch.subnet;
	if (run_again && own.empty()) {
		// Where an earlier run launched, if one did, from a describe-instances of
		// this machine id alone read now: the cached listing may predate that
		// launch (or be another worker's), and a run in the other subnet would
		// launch a second machine under that subnet's token.
		std::map<std::string, Instance> listed;
		const proc::CommandResult result =
				runner_.run(command("describe-instances", { "--filters", tag_filters(machine_id) }),
						config_.command_timeout_ms);
		std::string why;
		if (!result.ok())
			why = cli_failure("describe-instances", result);
		if (!result.ok() || !parse_listing(result.out, listed, why)) {
			error = "a run again could not read where its first run launched: " + why;
			return std::nullopt;
		}
		for (const auto &[id, instance] : listed) {
			if (instance.machine_id == machine_id &&
					(instance.state == "pending" || instance.state == "running"))
				own = instance.subnet;
		}
	}
	refresh_listing(now_());
	std::map<std::string, size_t> live;
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (const auto &[id, instance] : instances_) {
			if ((instance.state != "pending" && instance.state != "running") ||
					terminated_ms_.count(id) != 0)
				continue;
			++live[instance.subnet];
			if (!run_again && own.empty() && instance.machine_id == machine_id)
				own = instance.subnet;
		}
	}
	const auto live_in = [&live](const std::string &subnet) {
		const auto it = live.find(subnet);
		return it == live.end() ? size_t{ 0 } : it->second;
	};
	std::stable_sort(
			order.begin(), order.end(), [&live_in](const std::string &a, const std::string &b) {
				return live_in(a) < live_in(b);
			});
	const auto mine = std::find(order.begin(), order.end(), own);
	if (mine != order.end())
		std::rotate(order.begin(), mine, mine + 1);
	return order;
}

CloudMachine Ec2MachineDriver::describe(const std::string &cloud_id, int64_t launched_ago_ms) {
	CloudMachine status;
	if (!is_instance_id(cloud_id))
		return status; // Gone
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (terminated_ms_.count(cloud_id) != 0)
			return status; // Gone
	}
	const bool fresh = refresh_listing(now_());
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = instances_.find(cloud_id);
	if (it != instances_.end()) {
		status.private_address = it->second.private_address;
		status.public_address = it->second.public_address;
	}
	if (!fresh) {
		status.state = CloudState::Unknown;
		return status;
	}
	if (it == instances_.end()) {
		// EC2 lists a launch late: within the grace of its launch, as its row
		// dates it, the machine is Pending (its addresses the row's, from the
		// launch), past it Gone.
		if (launched_ago_ms >= 0 &&
				static_cast<uint64_t>(launched_ago_ms) <= config_.launch_grace_ms)
			status.state = CloudState::Pending;
		return status;
	}
	const std::string &state = it->second.state;
	if (state == "pending") {
		status.state = CloudState::Pending;
	} else if (state == "running") {
		status.state = CloudState::Running;
	} else {
		// shutting-down, terminated; and stopping, stopped, which no fleet
		// machine should ever be (it is terminated, never stopped): Gone.
		status.state = CloudState::Gone;
		status.private_address.clear();
		status.public_address.clear();
	}
	return status;
}

namespace {

// EC2's answer for an instance id it does not know (any longer).
bool says_not_found(const proc::CommandResult &result) {
	return result.outcome == proc::CommandResult::Outcome::Exited &&
			result.err.find("InvalidInstanceID.NotFound") != std::string::npos;
}

} // namespace

void Ec2MachineDriver::forget(const std::string &cloud_id, const char *how) {
	{
		std::lock_guard<std::mutex> lock(mu_);
		terminated_ms_[cloud_id] = now_();
		const auto it = instances_.find(cloud_id);
		if (it != instances_.end())
			it->second.state = "shutting-down";
	}
	std::printf("[fleet] ec2: machine %s %s\n", cloud_id.c_str(), how);
}

bool Ec2MachineDriver::terminate_now(const std::string &cloud_id, std::string &why) {
	const proc::CommandResult result =
			runner_.run(command("terminate-instances", { "--instance-ids", cloud_id }),
					config_.command_timeout_ms);
	const bool unknown = says_not_found(result);
	if (!result.ok() && !unknown) {
		why = cli_failure("terminate-instances", result);
		return false;
	}
	forget(cloud_id, unknown ? "was gone already" : "terminated");
	return true;
}

void Ec2MachineDriver::terminate(const std::string &cloud_id) {
	if (!is_instance_id(cloud_id)) {
		std::printf("[fleet] ec2: machine %s is no EC2 machine; nothing to end\n",
				loggable(cloud_id).c_str());
		return;
	}
	// A row's cloud id could name any machine of the account: only one that
	// carries both of this deployment's tags is ended. Whatever stops the check
	// leaves the machine as it is (the supervisor's sweep tries again).
	const std::vector<std::string> check = command(
			"describe-instances", { "--instance-ids", cloud_id, "--filters", tag_filters() });
	const proc::CommandResult checked = runner_.run(check, config_.command_timeout_ms);
	if (says_not_found(checked)) {
		forget(cloud_id, "was gone already");
		return;
	}
	std::map<std::string, Instance> listed;
	std::string why;
	if (!checked.ok())
		why = cli_failure("describe-instances", checked);
	if (!checked.ok() || !parse_listing(checked.out, listed, why)) {
		std::fprintf(stderr,
				"[fleet] WARN ec2: machine %s was not ended: its tags did not read (%s)\n",
				cloud_id.c_str(), why.c_str());
		return;
	}
	const auto it = listed.find(cloud_id);
	if (it == listed.end()) {
		std::fprintf(stderr,
				"[fleet] WARN ec2: machine %s carries no %s=1 and %s=%s: no machine of this "
				"deployment's, it is not ended\n",
				cloud_id.c_str(), kTagHosted, kTagDeployment, config_.deployment.c_str());
		return;
	}
	if (it->second.state == "terminated") {
		forget(cloud_id, "was gone already");
		return;
	}
	if (!terminate_now(cloud_id, why)) {
		// It may run on: the supervisor's sweep finds it and ends it again.
		std::fprintf(stderr, "[fleet] WARN ec2: ending machine %s failed: %s\n", cloud_id.c_str(),
				why.c_str());
	}
}

std::vector<TaggedCloudMachine> Ec2MachineDriver::list_tagged() {
	const uint64_t now = now_();
	std::map<std::string, Instance> listed;
	std::string error;
	// The CLI call outside the lock.
	const bool read = read_listing(listed, error);
	std::lock_guard<std::mutex> lock(mu_);
	if (!read) {
		note_describe_failure(now, error);
		throw std::runtime_error("ec2: the machines could not be listed: " + error);
	}
	take_listing(std::move(listed), now);
	std::vector<TaggedCloudMachine> out;
	for (const auto &[id, instance] : instances_)
		if (is_listed_state(instance.state) && terminated_ms_.count(id) == 0)
			out.push_back({ id, instance.machine_id });
	return out;
}

} // namespace opennova::novaworld_server::hosted
