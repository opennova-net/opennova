#include "ec2_provider.h"

#include "hosted_settings.h"
#include "loggable.h"
#include "owner_file.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool all_of(const std::string &text, bool (*ok)(char)) {
	for (const char c : text) {
		if (!ok(c)) return false;
	}
	return true;
}

bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool is_region_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }
bool is_name_char(char c) { return is_alnum(c) || c == '.' || c == '_' || c == '-'; }
bool is_image_char(char c) { return is_name_char(c) || c == '/' || c == ':' || c == '@'; }
bool is_url_char(char c) { return is_name_char(c) || c == '/' || c == ':'; }

// An id EC2 spells `<prefix>` and 8 to 17 lower-case hex digits.
bool is_aws_id(const std::string &text, const std::string &prefix) {
	if (text.compare(0, prefix.size(), prefix) != 0) return false;
	const std::string digits = text.substr(prefix.size());
	return digits.size() >= 8 && digits.size() <= 17 && all_of(digits, is_lower_hex);
}

bool is_instance_id(const std::string &text) { return is_aws_id(text, "i-"); }

// A JSON string literal of `text` (the values here are checked ASCII; the
// escape is for form's sake).
std::string json_text(const std::string &text) {
	std::string out = "\"";
	for (const char c : text) {
		if (c == '"' || c == '\\') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

// One tag object, {"Key":...,"Value":...}.
std::string json_tag(const std::string &key, const std::string &value) {
	return "{\"Key\":" + json_text(key) + ",\"Value\":" + json_text(value) + "}";
}

const io::JsonValue *member(const io::JsonValue &object, const char *key, io::JsonValue::Type type) {
	const io::JsonValue *v = object.get(key);
	return v != nullptr && v->type == type ? v : nullptr;
}

std::string text_member(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = member(object, key, io::JsonValue::Type::String);
	return v != nullptr ? v->string : std::string();
}

// The value of tag `key` in an instance's Tags list; "" when absent.
std::string tag_value(const io::JsonValue &instance, const char *key) {
	const io::JsonValue *tags = member(instance, "Tags", io::JsonValue::Type::Array);
	if (tags == nullptr) return {};
	for (const io::JsonValue &tag : tags->array) {
		if (tag.is_object() && text_member(tag, "Key") == key) return text_member(tag, "Value");
	}
	return {};
}

// The instance's State.Name; "" when absent.
std::string state_name_of(const io::JsonValue &instance) {
	const io::JsonValue *state = member(instance, "State", io::JsonValue::Type::Object);
	return state != nullptr ? text_member(*state, "Name") : std::string();
}

int64_t server_id_of(const std::string &tag) {
	if (tag.empty() || tag.size() > 18 || !all_of(tag, [](char c) { return c >= '0' && c <= '9'; })) return 0;
	return static_cast<int64_t>(strutil::parse_llong(tag).value_or(0));
}

bool is_live_state(const std::string &state) {
	return state == "pending" || state == "running" || state == "stopping" || state == "stopped";
}

} // namespace

std::string aws_failure(const char *what, const CommandResult &result) {
	std::string out = std::string("aws ec2 ") + what;
	switch (result.outcome) {
		case CommandResult::Outcome::NotStarted: return out + " did not start: " + loggable(result.error, 200);
		case CommandResult::Outcome::TimedOut: return out + " timed out";
		case CommandResult::Outcome::Exited: break;
	}
	out += " exited with code " + std::to_string(result.exit_code);
	const std::string err(strutil::trim_view(result.err));
	if (!err.empty()) out += ": " + loggable(err, 300);
	return out;
}

Ec2Provider::Ec2Provider(Ec2Config config, CommandRunner &runner, BootstrapStore &bootstrap, Clock now_ms) :
		config_(std::move(config)), runner_(runner), bootstrap_(bootstrap),
		now_(now_ms ? std::move(now_ms) : Clock(steady_now_ms)) {}

std::string Ec2Provider::check_config(const Ec2Config &c) {
	const bool absolute_cli = !c.aws_cli.empty() &&
			(c.aws_cli[0] == '/' || (c.aws_cli.size() > 2 && c.aws_cli[1] == ':'));
	if (!absolute_cli) return "ONNET_HOSTED_AWS_CLI is no absolute path";
	if (c.region.empty() || c.region.size() > 32 || !all_of(c.region, is_region_char))
		return "ONNET_HOSTED_EC2_REGION is no region name";
	if (!is_aws_id(c.launch_template, "lt-")) return "ONNET_HOSTED_EC2_LAUNCH_TEMPLATE is no launch template id";
	if (!is_aws_id(c.subnet, "subnet-")) return "ONNET_HOSTED_EC2_SUBNET is no subnet id";
	if (c.deployment.empty() || c.deployment.size() > 64 || !all_of(c.deployment, is_name_char))
		return "ONNET_HOSTED_DEPLOYMENT is not 1..64 letters, digits, '.', '_' and '-'";
	// An image reference starts with a letter or digit (never '-', which
	// `docker pull` and `docker run` would read as an option).
	if (c.serve_image.empty() || c.serve_image.size() > 255 || !is_alnum(c.serve_image[0]) ||
			!all_of(c.serve_image, is_image_char))
		return "ONNET_HOSTED_SERVE_IMAGE is no image reference";
	if (!parse_ipv4(c.service_address)) return "ONNET_HOSTED_SERVICE_ADDRESS is no IPv4 address";
	if (c.machine_script.find(kUrlBlank) == std::string::npos ||
			c.machine_script.find(kTokenBlank) == std::string::npos)
		return "the machine script (ONNET_HOSTED_MACHINE_SCRIPT) lacks its bootstrap blanks";
	if (c.machine_script.size() > kMaxUserDataBytes - 512) return "the machine script is too long for user-data";
	if (c.scratch_dir.empty()) return "no scratch directory";
	return {};
}

std::string Ec2Provider::bootstrap_url(const std::string &address, uint16_t port, int64_t server_id) {
	return "http://" + address + ":" + std::to_string(port) + "/hosted/bootstrap/" + std::to_string(server_id);
}

std::optional<std::string> Ec2Provider::render_user_data(const std::string &script, const std::string &url,
		const std::string &token) {
	if (url.empty() || !all_of(url, is_url_char) || token.empty() || !all_of(token, is_alnum)) return std::nullopt;
	std::string out = script;
	// Every occurrence of `blank` filled with `value`; false when there is none.
	const auto fill = [&out](const std::string &blank, const std::string &value) {
		bool filled = false;
		for (size_t at = out.find(blank); at != std::string::npos; at = out.find(blank, at + value.size())) {
			out.replace(at, blank.size(), value);
			filled = true;
		}
		return filled;
	};
	const bool filled_url = fill(kUrlBlank, url);
	const bool filled_token = fill(kTokenBlank, token);
	if (!filled_url || !filled_token || out.size() > kMaxUserDataBytes) return std::nullopt;
	return out;
}

void Ec2Provider::set_bootstrap_port(uint16_t port) {
	std::lock_guard<std::mutex> lock(mu_);
	config_.bootstrap_port = port;
}

std::vector<std::string> Ec2Provider::command(const std::string &operation, std::vector<std::string> params) const {
	std::vector<std::string> argv = { config_.aws_cli, "ec2", operation };
	argv.insert(argv.end(), std::make_move_iterator(params.begin()), std::make_move_iterator(params.end()));
	const std::vector<std::string> globals = { "--region", config_.region, "--output", "json", "--no-cli-pager",
		"--cli-connect-timeout", "10", "--cli-read-timeout", "30" };
	argv.insert(argv.end(), globals.begin(), globals.end());
	return argv;
}

MachinePlan Ec2Provider::plan(int64_t) {
	MachinePlan plan;
	plan.nw_port = kGamePort;
	plan.admin_port = kAdminPort;
	std::lock_guard<std::mutex> lock(mu_);
	plan.admin_allowed_ip = config_.service_address;
	return plan;
}

std::optional<std::string> Ec2Provider::launch(const LaunchSpec &spec, std::string &error) {
	std::lock_guard<std::mutex> lock(mu_);
	if (spec.server_id <= 0) {
		error = "no server id";
		return std::nullopt;
	}
	const std::string server = std::to_string(spec.server_id);
	const std::string url = bootstrap_url(config_.service_address, config_.bootstrap_port, spec.server_id);
	BootstrapBundle bundle;
	bundle.server_id = spec.server_id;
	bundle.image = config_.serve_image;
	bundle.spec = spec;
	const std::string token = bootstrap_.issue(std::move(bundle));
	const std::optional<std::string> user_data = render_user_data(config_.machine_script, url, token);
	if (!user_data) {
		bootstrap_.revoke(token);
		error = "the machine script did not render";
		return std::nullopt;
	}
	const fs::path user_data_path = fs::u8path(config_.scratch_dir) / ("user-data-" + random_secret(16) + ".sh");
	if (!write_owner_file(user_data_path, *user_data, true, error)) {
		bootstrap_.revoke(token);
		return std::nullopt;
	}

	const std::string tags = json_tag(kTagHosted, "1") + "," + json_tag(kTagDeployment, config_.deployment) + "," +
			json_tag(kTagServerId, server) + "," + json_tag("Name", config_.deployment + "-hosted-" + server);
	const std::string tag_specifications = "[{\"ResourceType\":\"instance\",\"Tags\":[" + tags +
			"]},{\"ResourceType\":\"volume\",\"Tags\":[" + tags + "]}]";
	const CommandResult result = runner_.run(
			command("run-instances",
					{ "--launch-template",
							"{\"LaunchTemplateId\":" + json_text(config_.launch_template) +
									",\"Version\":\"$Default\"}",
							"--subnet-id", config_.subnet, "--count", "1", "--client-token",
							"opennova-" + random_secret(32), "--tag-specifications", tag_specifications,
							"--user-data", "file://" + user_data_path.u8string() }),
			config_.command_timeout_ms);
	std::error_code ec;
	fs::remove(user_data_path, ec);
	if (!result.ok()) {
		bootstrap_.revoke(token);
		error = aws_failure("run-instances", result);
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
	const std::string id = instance != nullptr ? text_member(*instance, "InstanceId") : std::string();
	const std::string address = instance != nullptr ? text_member(*instance, "PrivateIpAddress") : std::string();
	if (!is_instance_id(id)) {
		bootstrap_.revoke(token);
		error = "run-instances answered no machine id (" +
				(parse_error.empty() ? std::string("no Instances[0].InstanceId") : loggable(parse_error, 120)) + ")";
		return std::nullopt;
	}
	if (!parse_ipv4(address) || !bootstrap_.bind(token, id, address)) {
		// A machine nothing may bootstrap: ended now rather than left to boot.
		bootstrap_.revoke(token);
		error = "run-instances answered machine " + id + " with no private address";
		const CommandResult ended =
				runner_.run(command("terminate-instances", { "--instance-ids", id }), config_.command_timeout_ms);
		if (ended.ok())
			terminated_ms_[id] = now_();
		else
			error += "; ending it failed too (" + aws_failure("terminate-instances", ended) + ")";
		return std::nullopt;
	}
	const uint64_t now = now_();
	Instance seen;
	seen.state = state_name_of(*instance);
	if (seen.state.empty()) seen.state = "pending";
	seen.address = address;
	seen.server_id = spec.server_id;
	instances_[id] = seen;
	launched_ms_[id] = now;
	std::printf("[hosted] ec2: server %lld launched as machine %s at %s\n", static_cast<long long>(spec.server_id),
			id.c_str(), address.c_str());
	return id;
}

bool Ec2Provider::refresh(uint64_t now_ms, std::string &error) {
	const CommandResult result = runner_.run(
			command("describe-instances",
					{ "--filters",
							"[{\"Name\":\"tag:" + std::string(kTagHosted) + "\",\"Values\":[\"1\"]},{\"Name\":\"tag:" +
									kTagDeployment + "\",\"Values\":[" + json_text(config_.deployment) + "]}]" }),
			config_.command_timeout_ms);
	if (!result.ok()) {
		error = aws_failure("describe-instances", result);
		return false;
	}
	io::JsonValue reply;
	std::string parse_error;
	if (!io::json_parse(result.out, reply, parse_error)) {
		error = "describe-instances answered no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *reservations = member(reply, "Reservations", io::JsonValue::Type::Array);
	if (reservations == nullptr) {
		error = "describe-instances answered no Reservations list";
		return false;
	}
	std::map<std::string, Instance> listed;
	for (const io::JsonValue &reservation : reservations->array) {
		const io::JsonValue *list =
				reservation.is_object() ? member(reservation, "Instances", io::JsonValue::Type::Array) : nullptr;
		if (list == nullptr) continue;
		for (const io::JsonValue &instance : list->array) {
			if (!instance.is_object()) continue;
			const std::string id = text_member(instance, "InstanceId");
			// The filter's own scope again: a machine without both tags is not this deployment's.
			if (!is_instance_id(id) || tag_value(instance, kTagHosted) != "1" ||
					tag_value(instance, kTagDeployment) != config_.deployment)
				continue;
			Instance seen;
			seen.state = state_name_of(instance);
			seen.address = text_member(instance, "PrivateIpAddress");
			seen.server_id = server_id_of(tag_value(instance, kTagServerId));
			listed[id] = std::move(seen);
		}
	}
	// A machine this provider launched a moment ago that EC2 does not list yet
	// keeps what its launch read (its address among it).
	for (const auto &[id, when] : launched_ms_) {
		const auto known = instances_.find(id);
		if (listed.count(id) == 0 && known != instances_.end() && now_ms - when <= config_.launch_grace_ms)
			listed[id] = known->second;
	}
	instances_ = std::move(listed);
	have_listing_ = true;
	listed_ms_ = now_ms;
	// The grace and the terminations only need keeping while EC2 might still
	// list a machine otherwise (terminated ones stay listed about an hour).
	for (auto it = launched_ms_.begin(); it != launched_ms_.end();)
		it = now_ms - it->second > config_.launch_grace_ms ? launched_ms_.erase(it) : std::next(it);
	for (auto it = terminated_ms_.begin(); it != terminated_ms_.end();)
		it = now_ms - it->second > 3 * 60 * 60 * 1000ull ? terminated_ms_.erase(it) : std::next(it);
	if (describe_failing_) {
		describe_failing_ = false;
		std::printf("[hosted] ec2: describe-instances answers again\n");
	}
	return true;
}

void Ec2Provider::note_describe_failure(uint64_t now_ms, const std::string &error) {
	if (!describe_failing_ || now_ms - describe_failure_logged_ms_ >= 60 * 1000) {
		std::fprintf(stderr, "[hosted] WARN ec2: %s; the machines read as pending until it answers\n",
				error.c_str());
		describe_failure_logged_ms_ = now_ms;
	}
	describe_failing_ = true;
	describe_failed_ms_ = now_ms;
}

MachineStatus Ec2Provider::describe(const std::string &machine_id) {
	std::lock_guard<std::mutex> lock(mu_);
	MachineStatus status;
	if (terminated_ms_.count(machine_id) != 0 || !is_instance_id(machine_id)) return status; // Gone
	const uint64_t now = now_();
	bool fresh = have_listing_ && now - listed_ms_ < config_.describe_ttl_ms;
	// A describe-instances that failed is tried again describe_ttl_ms later,
	// not at every describe: while the EC2 API is down, each server's describe
	// on a pass would otherwise wait out its own CLI run (up to its timeout),
	// and the pass (its stops, health checks and launches) behind them all.
	const bool backing_off = describe_failing_ && now - describe_failed_ms_ < config_.describe_ttl_ms;
	if (!fresh && !backing_off) {
		std::string error;
		fresh = refresh(now, error);
		if (!fresh) note_describe_failure(now, error);
	}
	const auto it = instances_.find(machine_id);
	if (!fresh) {
		// No word from EC2: never a crash for that.
		status.state = MachineState::Pending;
		if (it != instances_.end()) status.address = it->second.address;
		return status;
	}
	if (it == instances_.end()) {
		const auto launched = launched_ms_.find(machine_id);
		if (launched != launched_ms_.end() && now - launched->second <= config_.launch_grace_ms)
			status.state = MachineState::Pending;
		return status;
	}
	const Instance &instance = it->second;
	status.address = instance.address;
	if (instance.state == "pending") {
		status.state = MachineState::Pending;
	} else if (instance.state == "running") {
		status.state = bootstrap_.awaiting_fetch(machine_id) ? MachineState::Pending : MachineState::Running;
	} else {
		// shutting-down, terminated; and stopping, stopped, which no hosted
		// machine should ever be (it is terminated, never stopped): Gone,
		// which the supervisor ends.
		status.state = MachineState::Gone;
		status.address.clear();
	}
	return status;
}

void Ec2Provider::terminate(const std::string &machine_id) {
	std::lock_guard<std::mutex> lock(mu_);
	if (!is_instance_id(machine_id)) {
		std::printf("[hosted] ec2: machine %s is no EC2 machine; nothing to end\n", loggable(machine_id).c_str());
		return;
	}
	const CommandResult result =
			runner_.run(command("terminate-instances", { "--instance-ids", machine_id }), config_.command_timeout_ms);
	const bool unknown = result.outcome == CommandResult::Outcome::Exited &&
			result.err.find("InvalidInstanceID.NotFound") != std::string::npos;
	if (!result.ok() && !unknown) {
		// It may run on: the supervisor's sweep finds it untracked and ends it.
		// Its token goes all the same, so nothing fetches a bundle for a
		// machine the service is ending.
		bootstrap_.forget_machine(machine_id);
		std::fprintf(stderr, "[hosted] WARN ec2: ending machine %s failed: %s\n", machine_id.c_str(),
				aws_failure("terminate-instances", result).c_str());
		return;
	}
	terminated_ms_[machine_id] = now_();
	launched_ms_.erase(machine_id);
	const auto it = instances_.find(machine_id);
	if (it != instances_.end()) it->second.state = "shutting-down";
	bootstrap_.forget_machine(machine_id);
	std::printf("[hosted] ec2: machine %s %s\n", machine_id.c_str(), unknown ? "was gone already" : "terminated");
}

std::vector<TaggedMachine> Ec2Provider::list_tagged() {
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t now = now_();
	std::string error;
	if (!refresh(now, error)) {
		note_describe_failure(now, error);
		throw std::runtime_error("ec2: the machines could not be listed: " + error);
	}
	std::vector<TaggedMachine> out;
	for (const auto &[id, instance] : instances_) {
		if (is_live_state(instance.state) && terminated_ms_.count(id) == 0) out.push_back({ id, instance.server_id });
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
