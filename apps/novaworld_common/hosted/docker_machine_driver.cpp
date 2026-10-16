#include "docker_machine_driver.h"

#include "agent_protocol.h"
#include "ec2_machine_driver.h"
#include "loggable.h"
#include "net_http.h"
#include "text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using io::JsonValue;

bool all_of(std::string_view text, bool (*ok)(char)) {
	for (const char c : text)
		if (!ok(c))
			return false;
	return true;
}

bool is_name_char(char c) {
	return is_alnum(c) || c == '.' || c == '_' || c == '-';
}

// A docker object's name: 1..64 of letters, digits, '.', '_' and '-', a letter
// or digit first (what the engine takes for a container or a network).
bool is_docker_name(std::string_view name) {
	return !name.empty() && name.size() <= 64 && is_alnum(name[0]) && all_of(name, is_name_char);
}

// `text` percent-encoded for a URL's query: every byte but an unreserved one.
std::string query_encode(std::string_view text) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	for (const char c : text) {
		if (is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += c;
			continue;
		}
		const auto byte = static_cast<unsigned char>(c);
		out += '%';
		out += kHex[byte >> 4];
		out += kHex[byte & 0xFu];
	}
	return out;
}

std::string reply_text(const EngineReply &reply) {
	if (!reply.answered)
		return "no answer (" + loggable(reply.error, 200) + ")";
	std::string out = "HTTP " + std::to_string(reply.code);
	// The engine's own words: {"message": "..."}.
	JsonValue v;
	std::string error;
	if (io::json_parse(reply.body, v, error) && v.is_object()) {
		const std::string message = v.get_string("message", "");
		if (!message.empty())
			out += ": " + loggable(message, 200);
	}
	return out;
}

void warn(const std::string &text) {
	std::fprintf(stderr, "[fleet] WARN docker: %s\n", text.c_str());
}

// A ustar header block for `name` (at most 99 bytes), `mode`, `size` and type.
std::string tar_header(
		const std::string &name, unsigned mode, size_t size, char type, uint32_t uid) {
	std::string h(512, '\0');
	const auto put = [&h](size_t at, size_t width, const std::string &value) {
		std::memcpy(&h[at], value.data(), std::min(value.size(), width));
	};
	const auto octal = [](uint64_t value, size_t digits) {
		char buffer[32];
		std::snprintf(buffer, sizeof buffer, "%0*llo", static_cast<int>(digits),
				static_cast<unsigned long long>(value));
		return std::string(buffer);
	};
	put(0, 100, name);
	put(100, 8, octal(mode, 7));
	put(108, 8, octal(uid, 7));
	put(116, 8, octal(uid, 7));
	put(124, 12, octal(size, 11));
	put(136, 12, octal(0, 11));
	put(148, 8, std::string(8, ' ')); // the checksum's own bytes count as spaces
	h[156] = type;
	put(257, 6, std::string("ustar\0", 6));
	put(263, 2, "00");
	unsigned sum = 0;
	for (const char c : h)
		sum += static_cast<unsigned char>(c);
	put(148, 8, octal(sum, 6) + std::string("\0 ", 2));
	return h;
}

} // namespace

DockerMachineDriver::DockerMachineDriver(DockerMachineConfig config, EngineCall call) :
		config_(std::move(config)), call_(std::move(call)) {
	if (call_)
		return;
	const std::string base = config_.engine_url;
	const int timeout_ms = config_.call_timeout_ms;
	call_ = [base, timeout_ms](const std::string &method, const std::string &path,
					const std::string &body, const std::string &content_type) {
		std::vector<std::string> headers;
		if (!content_type.empty())
			headers.push_back("Content-Type: " + content_type);
		const net::HttpReply r = net::http_exchange(method, base + path, headers, body, timeout_ms,
				[](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out);
				});
		EngineReply out;
		out.answered = r.transport_ok;
		out.code = r.code;
		out.body.assign(r.body.begin(), r.body.end());
		out.error = r.error;
		return out;
	};
}

std::string DockerMachineDriver::check_config(const DockerMachineConfig &c) {
	std::string host;
	uint16_t port = 0;
	if (!Ec2MachineDriver::parse_service_url(c.engine_url, host, port))
		return "ONNET_HOSTED_DOCKER_URL is no http://<host>:<port> of the Docker Engine API";
	if (!is_docker_name(c.network))
		return "ONNET_HOSTED_DOCKER_NETWORK is no docker network name";
	if (!is_docker_name(c.data_from))
		return "ONNET_HOSTED_DOCKER_DATA_FROM is no container name";
	if (c.deployment.empty() || c.deployment.size() > kMaxDeploymentBytes ||
			!is_alnum(c.deployment[0]) || !all_of(c.deployment, is_name_char))
		return "ONNET_HOSTED_DEPLOYMENT is not 1.." + std::to_string(kMaxDeploymentBytes) +
				" letters, digits, '.', '_' and '-', a letter or digit first";
	if (!is_image_reference(c.image))
		return "ONNET_HOSTED_SERVE_IMAGE is no image reference";
	if (!Ec2MachineDriver::parse_service_url(c.service_url, host, port))
		return "ONNET_INTERNAL_URL is no http://<host>:<port> the machines can reach";
	if (c.call_timeout_ms <= 0)
		return "no engine call time";
	return {};
}

MachineProfile DockerMachineDriver::profile(const DockerMachineConfig &config) {
	MachineProfile out;
	out.provider = "docker";
	out.boot_timeout_ms = kBootTimeoutMs;
	out.slot_plan = [pattern = Ec2MachineDriver::admin_pattern(config.admin_address)](int slot) {
		SlotPlan plan;
		plan.game_port = slot_game_port(slot);
		plan.admin_port = slot_admin_port(slot);
		plan.admin_allowed_ip = pattern;
		return plan;
	};
	return out;
}

std::string DockerMachineDriver::machine_name(const std::string &deployment, int64_t machine_id) {
	return deployment + "-machine-" + std::to_string(machine_id);
}

std::vector<std::string> DockerMachineDriver::machine_command(
		const DockerMachineConfig &config, int64_t machine_id) {
	return { "--runtime", "process", "--serve", kServe, "--service", config.service_url,
		"--machine", std::to_string(machine_id), "--token-file", kTokenFile, "--state-dir",
		kStateDir, "--slots-root", kSlotsRoot, "--iptables", "none", "--ip6tables", "none" };
}

std::string DockerMachineDriver::create_body(
		const DockerMachineConfig &config, const MachineLaunch &launch) {
	JsonValue body = JsonValue::make_object();
	body.set("Image", io::json_string(config.image));
	JsonValue entrypoint = JsonValue::make_array();
	entrypoint.push(io::json_string(kHostd));
	body.set("Entrypoint", std::move(entrypoint));
	JsonValue cmd = JsonValue::make_array();
	for (const std::string &arg : machine_command(config, launch.machine_id))
		cmd.push(io::json_string(arg));
	body.set("Cmd", std::move(cmd));
	JsonValue labels = JsonValue::make_object();
	labels.set(kLabelHosted, io::json_string("1"));
	labels.set(kLabelDeployment, io::json_string(config.deployment));
	labels.set(kLabelMachineId, io::json_string(std::to_string(launch.machine_id)));
	labels.set(kLabelMachineKind, io::json_string(machine_kind_text(launch.kind)));
	body.set("Labels", std::move(labels));
	JsonValue host = JsonValue::make_object();
	host.set("NetworkMode", io::json_string(config.network));
	JsonValue volumes_from = JsonValue::make_array();
	volumes_from.push(io::json_string(config.data_from + ":ro"));
	host.set("VolumesFrom", std::move(volumes_from));
	host.set("Init", JsonValue::make_bool(true));
	JsonValue restart = JsonValue::make_object();
	restart.set("Name", io::json_string("no"));
	host.set("RestartPolicy", std::move(restart));
	body.set("HostConfig", std::move(host));
	JsonValue endpoints = JsonValue::make_object();
	endpoints.set(config.network, JsonValue::make_object());
	JsonValue networking = JsonValue::make_object();
	networking.set("EndpointsConfig", std::move(endpoints));
	body.set("NetworkingConfig", std::move(networking));
	return io::json_write(body);
}

std::string DockerMachineDriver::token_archive(const std::string &token) {
	const std::string dir = std::string(kTokenDir).substr(std::string(kTokenDir).rfind('/') + 1);
	const std::string file = std::string(kTokenFile).substr(std::string(kTokenFile).rfind('/') + 1);
	std::string out = tar_header(dir + "/", 0700, 0, '5', kMachineUid);
	out += tar_header(dir + "/" + file, 0600, token.size(), '0', kMachineUid);
	out += token;
	out.append((512 - token.size() % 512) % 512, '\0');
	out.append(1024, '\0');
	return out;
}

EngineReply DockerMachineDriver::call(const std::string &method, const std::string &path,
		const std::string &body, const std::string &content_type) {
	return call_(method, path, body, content_type);
}

bool DockerMachineDriver::inspect(const std::string &name, Inspected &out, std::string &error) {
	out = {};
	const EngineReply r = call("GET", "/containers/" + name + "/json");
	if (r.answered && r.code == 404)
		return true;
	if (!r.answered || r.code != 200) {
		error = "inspect " + name + ": " + reply_text(r);
		return false;
	}
	JsonValue v;
	if (!io::json_parse(r.body, v, error) || !v.is_object()) {
		error = "inspect " + name + ": the reply does not read";
		return false;
	}
	out.found = true;
	if (const JsonValue *state = v.get("State"); state && state->is_object())
		out.status = state->get_string("Status", "");
	if (const JsonValue *config = v.get("Config"); config && config->is_object()) {
		if (const JsonValue *labels = config->get("Labels"); labels && labels->is_object()) {
			out.hosted = labels->get_string(kLabelHosted, "");
			out.deployment = labels->get_string(kLabelDeployment, "");
			const std::string id = labels->get_string(kLabelMachineId, "");
			if (!id.empty() && id.size() <= 15 && strutil::all_digits(id))
				out.machine_id = strutil::parse_llong(id).value_or(0);
		}
	}
	if (const JsonValue *settings = v.get("NetworkSettings"); settings && settings->is_object())
		if (const JsonValue *networks = settings->get("Networks");
				networks && networks->is_object())
			if (const JsonValue *mine = networks->get(config_.network); mine && mine->is_object())
				out.address = mine->get_string("IPAddress", "");
	return true;
}

std::optional<LaunchedMachine> DockerMachineDriver::launch(
		const MachineLaunch &launch, std::string &error) {
	if (launch.machine_id <= 0) {
		error = "no machine id";
		return std::nullopt;
	}
	const std::string name = machine_name(config_.deployment, launch.machine_id);
	const EngineReply created = call("POST", "/containers/create?name=" + query_encode(name),
			create_body(config_, launch), "application/json");
	bool fresh = created.answered && created.code == 201;
	if (!fresh) {
		if (!created.answered || created.code != 409) {
			error = "create " + name + ": " + reply_text(created);
			return std::nullopt;
		}
		// A launch run again: the container its first run made, if it is this row's.
		Inspected held;
		if (!inspect(name, held, error))
			return std::nullopt;
		if (!held.found || held.deployment != config_.deployment ||
				held.machine_id != launch.machine_id) {
			error = "a container named " + name + " that is not this machine's is in the way";
			return std::nullopt;
		}
		fresh = held.status == "created";
	}
	if (fresh) {
		// The machine's user-data: its one-time token, as a file only its agent reads.
		const EngineReply put = call("PUT",
				"/containers/" + name + "/archive?path=" + query_encode("/run") + "&copyUIDGID=1",
				token_archive(launch.bootstrap_token), "application/x-tar");
		if (!put.answered || put.code != 200) {
			error = "the token to " + name + ": " + reply_text(put);
			call("DELETE", "/containers/" + name + "?force=1&v=1");
			return std::nullopt;
		}
		const EngineReply started = call("POST", "/containers/" + name + "/start");
		if (!started.answered || (started.code != 204 && started.code != 304)) {
			error = "start " + name + ": " + reply_text(started);
			call("DELETE", "/containers/" + name + "?force=1&v=1");
			return std::nullopt;
		}
	}
	Inspected running;
	if (!inspect(name, running, error))
		return std::nullopt;
	if (!running.found || running.address.empty()) {
		error = name + " has no address on " + config_.network + " (" +
				(running.found ? running.status : std::string("gone")) + ")";
		call("DELETE", "/containers/" + name + "?force=1&v=1");
		return std::nullopt;
	}
	LaunchedMachine out;
	out.cloud_id = name;
	out.private_address = running.address;
	out.public_address = running.address;
	out.instance_type = "container";
	out.template_version = config_.image;
	std::printf("[fleet] docker: machine %lld (%s) runs as %s at %s\n",
			static_cast<long long>(launch.machine_id), machine_kind_text(launch.kind), name.c_str(),
			running.address.c_str());
	return out;
}

CloudMachine DockerMachineDriver::describe(const std::string &cloud_id, int64_t) {
	CloudMachine out;
	Inspected seen;
	std::string error;
	if (!inspect(cloud_id, seen, error)) {
		warn(error);
		out.state = CloudState::Unknown;
		return out;
	}
	if (!seen.found)
		return out; // Gone
	if (seen.status == "created")
		out.state = CloudState::Pending;
	else if (seen.status == "running" || seen.status == "paused" || seen.status == "restarting")
		out.state = CloudState::Running;
	else
		out.state = CloudState::Gone;
	out.private_address = seen.address;
	out.public_address = seen.address;
	return out;
}

void DockerMachineDriver::terminate(const std::string &cloud_id) {
	Inspected seen;
	std::string error;
	if (!inspect(cloud_id, seen, error)) {
		warn(error);
		return;
	}
	if (!seen.found)
		return;
	if (seen.hosted != "1" || seen.deployment != config_.deployment) {
		warn("left " + loggable(cloud_id, 64) + " alone: it is no machine of deployment " +
				config_.deployment);
		return;
	}
	const EngineReply r = call("DELETE", "/containers/" + cloud_id + "?force=1&v=1");
	if (!r.answered || (r.code != 204 && r.code != 404)) {
		warn("remove " + loggable(cloud_id, 64) + ": " + reply_text(r));
		return;
	}
	std::printf("[fleet] docker: machine %s ended\n", cloud_id.c_str());
}

std::vector<TaggedCloudMachine> DockerMachineDriver::list_tagged() {
	// {"label":["opennova.hosted=1","opennova.deployment=<deployment>"]}: the
	// deployment is letters, digits and ._- alone (check_config), nothing to escape.
	const std::string filters = std::string("{\"label\":[\"") + kLabelHosted + "=1\",\"" +
			kLabelDeployment + "=" + config_.deployment + "\"]}";
	const EngineReply r = call("GET", "/containers/json?all=1&filters=" + query_encode(filters));
	if (!r.answered || r.code != 200)
		throw std::runtime_error("the docker machines' listing: " + reply_text(r));
	JsonValue v;
	std::string error;
	if (!io::json_parse(r.body, v, error) || !v.is_array())
		throw std::runtime_error("the docker machines' listing does not read");
	std::vector<TaggedCloudMachine> out;
	for (const JsonValue &c : v.array) {
		if (!c.is_object())
			continue;
		const JsonValue *names = c.get("Names");
		if (!names || !names->is_array() || names->array.empty() || !names->array[0].is_string())
			continue;
		std::string name = names->array[0].string;
		if (!name.empty() && name[0] == '/')
			name.erase(0, 1);
		TaggedCloudMachine m;
		m.cloud_id = name;
		if (const JsonValue *l = c.get("Labels"); l && l->is_object()) {
			// A listing filtered on both labels: anything else is not this deployment's.
			if (l->get_string(kLabelHosted, "") != "1" ||
					l->get_string(kLabelDeployment, "") != config_.deployment)
				continue;
			const std::string id = l->get_string(kLabelMachineId, "");
			if (!id.empty() && id.size() <= 15 && strutil::all_digits(id))
				m.machine_id = strutil::parse_llong(id).value_or(0);
		}
		out.push_back(std::move(m));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
