#include "agent_protocol.h"

#include <base/io/base64.h>
#include <base/io/json.h>
#include <base/io/sha256.h>

#include <algorithm>
#include <set>
#include <utility>

namespace opennova::novaworld_server::hosted {

using io::JsonValue;

namespace {

constexpr size_t kMaxReportedContainers = 4 * kMaxSlots;
constexpr size_t kMaxDataSets = 32;
constexpr size_t kMaxArgBytes = 1024;
constexpr size_t kMaxEnvValueBytes = 4096;
constexpr size_t kSecretMinChars = 32;
constexpr size_t kSecretMaxChars = 128;
constexpr double kMaxCpus = 64.0;
constexpr int64_t kMinMemoryMb = 16;
constexpr int64_t kMaxMemoryMb = 65536;
constexpr int64_t kMinPids = 16;
constexpr int64_t kMaxPids = 32768;

bool is_alnum(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool all_of(std::string_view text, bool (*ok)(char)) {
	for (const char c : text)
		if (!ok(c))
			return false;
	return true;
}

bool is_printable_ascii(std::string_view text) {
	return all_of(text, [](char c) { return c >= 0x20 && c <= 0x7E; });
}

bool is_secret_text(std::string_view text) {
	return text.size() >= kSecretMinChars && text.size() <= kSecretMaxChars &&
			all_of(text, is_alnum);
}

// RFC 4648's standard alphabet with its padding, strictly: no whitespace, no
// missing padding, nothing after it, the unused bits 0 (one text per byte string).
bool base64_decode_strict(std::string_view text, std::string &out) {
	const auto digit = [](char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '+')
			return 62;
		if (c == '/')
			return 63;
		return -1;
	};
	out.clear();
	if (text.size() % 4 != 0)
		return false;
	out.reserve(text.size() / 4 * 3);
	for (size_t i = 0; i < text.size(); i += 4) {
		int v[4] = {};
		int pad = 0;
		for (int j = 0; j < 4; ++j) {
			const char c = text[i + static_cast<size_t>(j)];
			if (c == '=') {
				if (i + 4 != text.size() || j < 2)
					return false;
				++pad;
				continue;
			}
			if (pad)
				return false;
			v[j] = digit(c);
			if (v[j] < 0)
				return false;
		}
		if ((pad == 2 && (v[1] & 0xF)) || (pad == 1 && (v[2] & 0x3)))
			return false;
		const uint32_t chunk =
				uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
		out.push_back(static_cast<char>((chunk >> 16) & 0xFF));
		if (pad < 2)
			out.push_back(static_cast<char>((chunk >> 8) & 0xFF));
		if (pad < 1)
			out.push_back(static_cast<char>(chunk & 0xFF));
	}
	return true;
}

JsonValue json_int(int64_t value) {
	return io::json_number(static_cast<double>(value));
}

// --- the reads ------------------------------------------------------------------

bool fail(std::string &error, std::string why) {
	error = std::move(why);
	return false;
}

bool parse_object(std::string_view json, JsonValue &out, std::string &error) {
	std::string why;
	if (!io::json_parse(json, out, why))
		return fail(error, "not JSON: " + why);
	if (!out.is_object())
		return fail(error, "not a JSON object");
	return true;
}

bool read_protocol(const JsonValue &object, int &out, std::string &error) {
	const JsonValue *v = object.get("protocol");
	int64_t protocol = 0;
	if (!v || !io::json_exact_whole_in(*v, 0, 2147483647.0, protocol))
		return fail(error, "no protocol");
	if (protocol != kAgentProtocol)
		return fail(error,
				"protocol " + std::to_string(protocol) + ", not " + std::to_string(kAgentProtocol));
	out = static_cast<int>(protocol);
	return true;
}

bool read_string(const JsonValue &object, const char *key, std::string &out, std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v || !v->is_string())
		return fail(error, std::string("no string ") + key);
	out = v->string;
	return true;
}

bool read_whole(const JsonValue &object, const char *key, double lo, double hi, int64_t &out,
		std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v || !io::json_exact_whole_in(*v, lo, hi, out))
		return fail(error, std::string(key) + " is no whole number in its range");
	return true;
}

bool read_bool(const JsonValue &object, const char *key, bool &out, std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v || !v->is_bool())
		return fail(error, std::string("no boolean ") + key);
	out = v->boolean;
	return true;
}

// A whole number in [lo, hi], or null; a missing member is refused.
bool read_optional_whole(const JsonValue &object, const char *key, double lo, double hi,
		std::optional<int64_t> &out, std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v)
		return fail(error, std::string("no ") + key);
	if (v->is_null()) {
		out.reset();
		return true;
	}
	int64_t whole = 0;
	if (!io::json_exact_whole_in(*v, lo, hi, whole))
		return fail(error, std::string(key) + " is no whole number in its range");
	out = whole;
	return true;
}

bool read_count(const JsonValue &object, const char *key, uint64_t &out, std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v || !io::json_exact_whole(*v, out))
		return fail(error, std::string(key) + " is no count");
	return true;
}

bool read_agent_version(const JsonValue &object, std::string &out, std::string &error) {
	if (!read_string(object, "agent_version", out, error))
		return false;
	if (out.empty() || out.size() > 64 || !is_printable_ascii(out))
		return fail(error, "agent_version is not 1..64 printable characters");
	return true;
}

bool read_report(const JsonValue &v, ContainerReport &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a container is no object");
	if (!io::json_members_known(v,
				{ "name", "slot", "state", "exit_code", "oom", "started_at", "cpu_percent",
						"memory_bytes", "egress_bytes" },
				"container", error))
		return false;
	if (!read_string(v, "name", out.name, error))
		return false;
	if (!is_container_name(out.name))
		return fail(error, "a container's name is not a container name");
	int64_t slot = 0;
	if (!read_whole(v, "slot", 0, kMaxSlots - 1, slot, error))
		return false;
	out.slot = static_cast<int>(slot);
	std::string state;
	if (!read_string(v, "state", state, error))
		return false;
	const auto parsed = parse_container_state(state);
	if (!parsed)
		return fail(error, "a container's state is not starting, running or exited");
	out.state = *parsed;
	if (!read_optional_whole(v, "exit_code", -2147483648.0, 2147483647.0, out.exit_code, error))
		return false;
	if (!read_bool(v, "oom", out.oom, error))
		return false;
	if (!read_optional_whole(v, "started_at", 0, io::kJsonSafeWholeMax, out.started_at, error))
		return false;
	const JsonValue *cpu = v.get("cpu_percent");
	if (!cpu || !cpu->is_number() || !(cpu->number >= 0.0 && cpu->number <= 100.0 * kMaxCpus))
		return fail(error, "cpu_percent is no percentage");
	out.cpu_percent = cpu->number;
	return read_count(v, "memory_bytes", out.memory_bytes, error) &&
			read_count(v, "egress_bytes", out.egress_bytes, error);
}

bool read_args(const JsonValue &v, std::vector<std::string> &out, std::string &error) {
	if (!v.is_array() || v.array.size() > kMaxContainerArgs)
		return fail(error, "args is no list of arguments");
	out.clear();
	for (const JsonValue &arg : v.array) {
		if (!arg.is_string() || arg.string.empty() || arg.string.size() > kMaxArgBytes ||
				arg.string.find('\0') != std::string::npos)
			return fail(error, "an argument is not 1..1024 bytes without a NUL");
		out.push_back(arg.string);
	}
	return true;
}

bool read_files(const JsonValue &v, std::map<std::string, std::string> &out, std::string &error) {
	if (!v.is_object() || v.object.size() > kMaxContainerFiles)
		return fail(error, "files is no map of files");
	out.clear();
	size_t total = 0;
	for (const io::JsonMember &member : v.object) {
		if (!is_slot_file_name(member.key))
			return fail(error, "a file's name is no bare file name");
		std::string bytes;
		if (!member.value.is_string() || !base64_decode_strict(member.value.string, bytes))
			return fail(error, "a file's content is not base64");
		total += bytes.size();
		if (total > kMaxContainerFileBytes)
			return fail(error, "the files are too large");
		out[member.key] = std::move(bytes);
	}
	return true;
}

bool read_env(const JsonValue &v, std::map<std::string, std::string> &out, std::string &error) {
	if (!v.is_object() || v.object.size() > kMaxContainerEnv)
		return fail(error, "env is no map of variables");
	out.clear();
	for (const io::JsonMember &member : v.object) {
		if (!is_env_name(member.key))
			return fail(error, "an environment name is not one");
		const std::string &value = member.value.string;
		if (!member.value.is_string() || value.size() > kMaxEnvValueBytes ||
				value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos)
			return fail(error, "an environment value is not one line");
		out[member.key] = value;
	}
	return true;
}

bool read_desired(const JsonValue &v, DesiredContainer &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a desired container is no object");
	if (!io::json_members_known(v,
				{ "name", "slot", "image", "uid", "data_set", "args", "files", "env", "cpus",
						"memory_mb", "pids" },
				"desired container", error))
		return false;
	if (!read_string(v, "name", out.name, error))
		return false;
	if (!is_container_name(out.name))
		return fail(error, "a desired container's name is not a container name");
	int64_t whole = 0;
	if (!read_whole(v, "slot", 0, kMaxSlots - 1, whole, error))
		return false;
	out.slot = static_cast<int>(whole);
	if (!read_string(v, "image", out.image, error))
		return false;
	if (!is_image_reference(out.image))
		return fail(error, "a desired container's image is no image reference");
	if (!read_whole(v, "uid", 0, 4294967295.0, whole, error))
		return false;
	out.uid = static_cast<uint32_t>(whole);
	if (out.uid != slot_uid(out.slot))
		return fail(error, "a desired container's uid is not its slot's");
	if (!read_string(v, "data_set", out.data_set, error))
		return false;
	if (!is_data_set_name(out.data_set))
		return fail(error, "a desired container's data set is no data set name");
	const JsonValue *args = v.get("args");
	if (!args || !read_args(*args, out.args, error))
		return args ? false : fail(error, "no args");
	out.files.reset();
	if (const JsonValue *files = v.get("files")) {
		std::map<std::string, std::string> read;
		if (!read_files(*files, read, error))
			return false;
		out.files = std::move(read);
	}
	const JsonValue *env = v.get("env");
	if (!env || !read_env(*env, out.env, error))
		return env ? false : fail(error, "no env");
	const JsonValue *cpus = v.get("cpus");
	if (!cpus || !cpus->is_number() || !(cpus->number > 0.0 && cpus->number <= kMaxCpus))
		return fail(error, "cpus is not above 0 and at most 64");
	out.cpus = cpus->number;
	if (!read_whole(v, "memory_mb", kMinMemoryMb, kMaxMemoryMb, whole, error))
		return false;
	out.memory_mb = static_cast<int>(whole);
	if (!read_whole(v, "pids", kMinPids, kMaxPids, whole, error))
		return false;
	out.pids = static_cast<int>(whole);
	return true;
}

} // namespace

std::string machine_bootstrap_path(int64_t machine_id) {
	return "/hosted/machines/" + std::to_string(machine_id) + "/bootstrap";
}

std::string machine_sync_path(int64_t machine_id) {
	return "/hosted/machines/" + std::to_string(machine_id) + "/sync";
}

std::string hosted_container_name(int64_t server_id, int64_t generation) {
	return "srv-" + std::to_string(server_id) + "-g" + std::to_string(generation);
}

std::string agent_secret_digest(std::string_view secret) {
	return io::sha256_hex(secret.data(), secret.size());
}

bool is_container_name(std::string_view name) {
	if (name.empty() || name.size() > 64 || !is_alnum(name[0]))
		return false;
	return all_of(name, [](char c) { return is_alnum(c) || c == '_' || c == '.' || c == '-'; });
}

bool is_image_reference(std::string_view image) {
	if (image.empty() || image.size() > 255 || !is_alnum(image[0]))
		return false;
	return all_of(image, [](char c) {
		return is_alnum(c) || c == '.' || c == '/' || c == ':' || c == '@' || c == '_' || c == '-';
	});
}

bool is_data_set_name(std::string_view name) {
	if (name.empty() || name.size() > 64)
		return false;
	return all_of(name,
			[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool is_mount_path(std::string_view path) {
	if (path.size() < 2 || path.size() > 255 || path[0] != '/')
		return false;
	if (!all_of(path,
				[](char c) { return is_alnum(c) || c == '.' || c == '/' || c == '_' || c == '-'; }))
		return false;
	// Every part a name: no empty part (no "//", no trailing '/'), no "." or "..".
	size_t start = 1;
	while (start <= path.size()) {
		const size_t end = std::min(path.find('/', start), path.size());
		const std::string_view part = path.substr(start, end - start);
		if (part.empty() || part == "." || part == "..")
			return false;
		start = end + 1;
	}
	return true;
}

bool is_slot_file_name(std::string_view name) {
	if (name.empty() || name.size() > 64 || name == "." || name == "..")
		return false;
	return all_of(name, [](char c) { return is_alnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool is_env_name(std::string_view name) {
	if (name.empty() || name.size() > 128)
		return false;
	if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') ||
				name[0] == '_'))
		return false;
	return all_of(name, [](char c) { return is_alnum(c) || c == '_'; });
}

std::optional<int> protocol_of(std::string_view json) {
	JsonValue v;
	std::string error;
	if (!io::json_parse(json, v, error) || !v.is_object())
		return std::nullopt;
	const JsonValue *protocol = v.get("protocol");
	int64_t whole = 0;
	if (!protocol || !io::json_exact_whole_in(*protocol, -2147483648.0, 2147483647.0, whole))
		return std::nullopt;
	return static_cast<int>(whole);
}

const char *container_state_text(ContainerState state) {
	switch (state) {
		case ContainerState::Starting:
			return "starting";
		case ContainerState::Running:
			return "running";
		case ContainerState::Exited:
			return "exited";
	}
	return "exited";
}

std::optional<ContainerState> parse_container_state(std::string_view text) {
	if (text == "starting")
		return ContainerState::Starting;
	if (text == "running")
		return ContainerState::Running;
	if (text == "exited")
		return ContainerState::Exited;
	return std::nullopt;
}

// --- the bootstrap ----------------------------------------------------------------

std::string encode_agent_hello(const AgentHello &hello) {
	JsonValue v = JsonValue::make_object();
	v.set("protocol", json_int(hello.protocol));
	v.set("agent_version", io::json_string(hello.agent_version));
	return io::json_write(v);
}

bool decode_agent_hello(std::string_view json, AgentHello &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v, { "protocol", "agent_version" }, "hello", error))
		return false;
	return read_agent_version(v, out.agent_version, error);
}

std::string encode_machine_bootstrap(const MachineBootstrap &bootstrap) {
	JsonValue v = JsonValue::make_object();
	v.set("protocol", json_int(bootstrap.protocol));
	v.set("agent_secret", io::json_string(bootstrap.agent_secret));
	v.set("serve_image", io::json_string(bootstrap.serve_image));
	v.set("slots", json_int(bootstrap.slots));
	JsonValue data_sets = JsonValue::make_object();
	for (const auto &entry : bootstrap.data_sets)
		data_sets.set(entry.first, io::json_string(entry.second));
	v.set("data_sets", std::move(data_sets));
	return io::json_write(v);
}

bool decode_machine_bootstrap(std::string_view json, MachineBootstrap &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v,
				{ "protocol", "agent_secret", "serve_image", "slots", "data_sets" }, "bootstrap",
				error))
		return false;
	if (!read_string(v, "agent_secret", out.agent_secret, error))
		return false;
	if (!is_secret_text(out.agent_secret))
		return fail(error, "agent_secret is not 32..128 letters and digits");
	if (!read_string(v, "serve_image", out.serve_image, error))
		return false;
	if (!is_image_reference(out.serve_image))
		return fail(error, "serve_image is no image reference");
	int64_t slots = 0;
	if (!read_whole(v, "slots", 1, kMaxSlots, slots, error))
		return false;
	out.slots = static_cast<int>(slots);
	const JsonValue *data_sets = v.get("data_sets");
	if (!data_sets || !data_sets->is_object() || data_sets->object.empty() ||
			data_sets->object.size() > kMaxDataSets)
		return fail(error, "data_sets is no map of 1..32 data sets");
	out.data_sets.clear();
	for (const io::JsonMember &member : data_sets->object) {
		if (!is_data_set_name(member.key))
			return fail(error, "a data set's name is not one");
		if (!member.value.is_string() || !is_mount_path(member.value.string))
			return fail(error, "a data set's mount path is no absolute path");
		out.data_sets[member.key] = member.value.string;
	}
	return true;
}

// --- the sync -------------------------------------------------------------------

std::string encode_sync_request(const SyncRequest &request) {
	JsonValue v = JsonValue::make_object();
	v.set("protocol", json_int(request.protocol));
	v.set("agent_version", io::json_string(request.agent_version));
	JsonValue containers = JsonValue::make_array();
	for (const ContainerReport &c : request.containers) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", io::json_string(c.name));
		entry.set("slot", json_int(c.slot));
		entry.set("state", io::json_string(container_state_text(c.state)));
		entry.set("exit_code", c.exit_code ? json_int(*c.exit_code) : JsonValue::make_null());
		entry.set("oom", JsonValue::make_bool(c.oom));
		entry.set("started_at", c.started_at ? json_int(*c.started_at) : JsonValue::make_null());
		entry.set("cpu_percent", io::json_number(c.cpu_percent));
		entry.set("memory_bytes", io::json_number(static_cast<double>(c.memory_bytes)));
		entry.set("egress_bytes", io::json_number(static_cast<double>(c.egress_bytes)));
		containers.push(std::move(entry));
	}
	v.set("containers", std::move(containers));
	return io::json_write(v);
}

bool decode_sync_request(std::string_view json, SyncRequest &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v, { "protocol", "agent_version", "containers" }, "sync", error))
		return false;
	if (!read_agent_version(v, out.agent_version, error))
		return false;
	const JsonValue *containers = v.get("containers");
	if (!containers || !containers->is_array() || containers->array.size() > kMaxReportedContainers)
		return fail(error, "containers is no list of containers");
	out.containers.clear();
	std::set<std::string> names;
	for (const JsonValue &entry : containers->array) {
		ContainerReport report;
		if (!read_report(entry, report, error))
			return false;
		if (!names.insert(report.name).second)
			return fail(error, "a container is reported twice");
		out.containers.push_back(std::move(report));
	}
	return true;
}

std::string encode_sync_reply(const SyncReply &reply) {
	JsonValue v = JsonValue::make_object();
	v.set("protocol", json_int(reply.protocol));
	v.set("drain", JsonValue::make_bool(reply.drain));
	JsonValue desired = JsonValue::make_array();
	for (const DesiredContainer &d : reply.desired) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", io::json_string(d.name));
		entry.set("slot", json_int(d.slot));
		entry.set("image", io::json_string(d.image));
		entry.set("uid", json_int(d.uid));
		entry.set("data_set", io::json_string(d.data_set));
		entry.set("args", io::json_string_array(d.args));
		if (d.files) {
			JsonValue files = JsonValue::make_object();
			for (const auto &file : *d.files)
				files.set(file.first,
						io::json_string(io::base64_encode(
								reinterpret_cast<const uint8_t *>(file.second.data()),
								file.second.size())));
			entry.set("files", std::move(files));
		}
		JsonValue env = JsonValue::make_object();
		for (const auto &var : d.env)
			env.set(var.first, io::json_string(var.second));
		entry.set("env", std::move(env));
		entry.set("cpus", io::json_number(d.cpus));
		entry.set("memory_mb", json_int(d.memory_mb));
		entry.set("pids", json_int(d.pids));
		desired.push(std::move(entry));
	}
	v.set("desired", std::move(desired));
	return io::json_write(v);
}

bool decode_sync_reply(std::string_view json, SyncReply &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v, { "protocol", "desired", "drain" }, "sync reply", error))
		return false;
	if (!read_bool(v, "drain", out.drain, error))
		return false;
	const JsonValue *desired = v.get("desired");
	if (!desired || !desired->is_array() || desired->array.size() > static_cast<size_t>(kMaxSlots))
		return fail(error, "desired is no list of at most one container per slot");
	out.desired.clear();
	std::set<std::string> names;
	std::set<int> slots;
	for (const JsonValue &entry : desired->array) {
		DesiredContainer d;
		if (!read_desired(entry, d, error))
			return false;
		if (!names.insert(d.name).second)
			return fail(error, "a container is desired twice");
		if (!slots.insert(d.slot).second)
			return fail(error, "two desired containers share a slot");
		out.desired.push_back(std::move(d));
	}
	return true;
}

} // namespace opennova::novaworld_server::hosted
