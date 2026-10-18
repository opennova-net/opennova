#include "agent_protocol.h"
#include "core/text_util.h"

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
constexpr double kMaxTreeId = 9007199254740991.0; // 2^53 - 1, what a JSON number carries whole
constexpr size_t kMaxArgBytes = 1024;
constexpr size_t kMaxEnvValueBytes = 4096;
constexpr size_t kSecretMinChars = 32;
constexpr size_t kSecretMaxChars = 128;
constexpr double kMaxCpus = 64.0;
constexpr int64_t kMinMemoryMb = 16;
constexpr int64_t kMaxMemoryMb = 65536;
constexpr int64_t kMinPids = 16;
constexpr int64_t kMaxPids = 32768;
// A frame time past this (ms) is no frame time; a tree fetched this often, no tree.
constexpr double kMaxTickMs = 1e6;
constexpr int64_t kMaxTreeTries = 1000;
constexpr size_t kMaxLogStreamBytes = 512;

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

JsonValue json_count(uint64_t value) {
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

// A number in [0, hi].
bool read_measure(
		const JsonValue &object, const char *key, double hi, double &out, std::string &error) {
	const JsonValue *v = object.get(key);
	if (!v || !v->is_number() || !(v->number >= 0.0 && v->number <= hi))
		return fail(error, std::string(key) + " is no measure in its range");
	out = v->number;
	return true;
}

// A failure's reason: 1..kMaxContainerReasonBytes printable ASCII.
bool is_reason_text(const JsonValue &v) {
	return v.is_string() && !v.string.empty() && v.string.size() <= kMaxContainerReasonBytes &&
			is_printable_ascii(v.string);
}

// A JSON object key that is a tree id (the replies' and the reports' `trees`).
bool read_tree_key(const std::string &key, int64_t &out) {
	if (key.empty() || key.size() > 16 ||
			!all_of(key, [](char c) { return c >= '0' && c <= '9'; }) || key.front() == '0')
		return false;
	int64_t id = 0;
	for (const char c : key)
		id = id * 10 + (c - '0');
	if (id < 1 || static_cast<double>(id) > kMaxTreeId)
		return false;
	out = id;
	return true;
}

bool read_tick(const JsonValue &v, TickHealth &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a container's tick is no object");
	if (!io::json_members_known(v, { "p50_ms", "p99_ms", "max_ms", "late" }, "tick", error))
		return false;
	return read_measure(v, "p50_ms", kMaxTickMs, out.p50_ms, error) &&
			read_measure(v, "p99_ms", kMaxTickMs, out.p99_ms, error) &&
			read_measure(v, "max_ms", kMaxTickMs, out.max_ms, error) &&
			read_count(v, "late", out.late, error);
}

bool read_machine_stats(const JsonValue &v, MachineStats &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "machine is no object");
	if (!io::json_members_known(v,
				{ "cpu_percent", "memory_used_bytes", "memory_total_bytes", "disk_free_bytes",
						"disk_total_bytes", "net_in_bytes", "net_out_bytes" },
				"machine", error))
		return false;
	return read_measure(v, "cpu_percent", 100.0, out.cpu_percent, error) &&
			read_count(v, "memory_used_bytes", out.memory_used_bytes, error) &&
			read_count(v, "memory_total_bytes", out.memory_total_bytes, error) &&
			read_count(v, "disk_free_bytes", out.disk_free_bytes, error) &&
			read_count(v, "disk_total_bytes", out.disk_total_bytes, error) &&
			read_count(v, "net_in_bytes", out.net_in_bytes, error) &&
			read_count(v, "net_out_bytes", out.net_out_bytes, error);
}

bool read_tree_report(const JsonValue &v, TreeReport &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a reported tree is no object");
	if (!io::json_members_known(v, { "state", "bytes", "tries", "reason" }, "reported tree", error))
		return false;
	std::string state;
	if (!read_string(v, "state", state, error))
		return false;
	const auto parsed = parse_tree_report_state(state);
	if (!parsed)
		return fail(error, "a reported tree's state is not fetching, ready or failed");
	out.state = *parsed;
	int64_t tries = 0;
	if (!read_count(v, "bytes", out.bytes, error) ||
			!read_whole(v, "tries", 0, static_cast<double>(kMaxTreeTries), tries, error))
		return false;
	out.tries = static_cast<int>(tries);
	out.reason.clear();
	if (const JsonValue *reason = v.get("reason")) {
		if (!is_reason_text(*reason))
			return fail(error, "a reported tree's reason is not a short line");
		out.reason = reason->string;
	}
	return true;
}

bool read_agent_version(const JsonValue &object, std::string &out, std::string &error) {
	if (!read_string(object, "agent_version", out, error))
		return false;
	if (out.empty() || out.size() > 64 || !is_printable_ascii(out))
		return fail(error, "agent_version is not 1..64 printable characters");
	return true;
}

// "" or an image reference: the agent's own image, which it knows only on ECS.
bool read_agent_image(const JsonValue &object, std::string &out, std::string &error) {
	if (!read_string(object, "agent_image", out, error))
		return false;
	if (!out.empty() && !is_image_reference(out))
		return fail(error, "agent_image is no image reference");
	return true;
}

bool read_report(const JsonValue &v, ContainerReport &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a container is no object");
	if (!io::json_members_known(v,
				{ "name", "slot", "state", "waiting", "exit_code", "oom", "started_at", "reason",
						"cpu_percent", "memory_bytes", "egress_bytes", "net_in_bytes", "tick" },
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
	std::string waiting;
	if (!read_string(v, "waiting", waiting, error))
		return false;
	const auto wait = parse_container_wait(waiting);
	if (!wait)
		return fail(error, "a container's waiting is not \"\", image or trees");
	if (*wait != ContainerWait::None && out.state != ContainerState::Starting)
		return fail(error, "a container waits while starting alone");
	out.waiting = *wait;
	if (!read_optional_whole(v, "exit_code", -2147483648.0, 2147483647.0, out.exit_code, error))
		return false;
	if (!read_bool(v, "oom", out.oom, error))
		return false;
	if (!read_optional_whole(v, "started_at", 0, io::kJsonSafeWholeMax, out.started_at, error))
		return false;
	// Why it ended, or never started: only with an exit code.
	out.reason.clear();
	if (const JsonValue *reason = v.get("reason")) {
		if (!is_reason_text(*reason))
			return fail(error, "a container's reason is not a short line");
		if (out.state != ContainerState::Exited || !out.exit_code)
			return fail(error, "a container's reason comes with an exit alone");
		out.reason = reason->string;
	}
	const JsonValue *cpu = v.get("cpu_percent");
	if (!cpu || !cpu->is_number() || !(cpu->number >= 0.0 && cpu->number <= 100.0 * kMaxCpus))
		return fail(error, "cpu_percent is no percentage");
	out.cpu_percent = cpu->number;
	if (!read_count(v, "memory_bytes", out.memory_bytes, error) ||
			!read_count(v, "egress_bytes", out.egress_bytes, error) ||
			!read_count(v, "net_in_bytes", out.net_in_bytes, error))
		return false;
	out.tick.reset();
	if (const JsonValue *tick = v.get("tick")) {
		if (out.state != ContainerState::Running)
			return fail(error, "a container's tick comes with a running one alone");
		TickHealth read;
		if (!read_tick(*tick, read, error))
			return false;
		out.tick = read;
	}
	return true;
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

bool read_tree_spec(const JsonValue &v, TreeSpec &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a tree is no object");
	if (!io::json_members_known(v, { "url", "version", "files" }, "tree", error))
		return false;
	if (!read_string(v, "url", out.url, error))
		return false;
	if (!is_tree_url(out.url))
		return fail(error, "a tree's url is no store URL");
	const JsonValue *version = v.get("version");
	if (!version || !version->is_string() || version->string.size() > 128 ||
			!is_printable_ascii(version->string))
		return fail(error, "a tree's version is not a short line");
	out.version = version->string;
	const JsonValue *files = v.get("files");
	if (!files || !files->is_array() || files->array.empty() || files->array.size() > kMaxTreeFiles)
		return fail(error, "a tree's files is no list of 1..64 files");
	out.files.clear();
	std::set<std::string> names;
	for (const JsonValue &entry : files->array) {
		if (!entry.is_object() ||
				!io::json_members_known(entry, { "name", "size" }, "tree file", error))
			return entry.is_object() ? false : fail(error, "a tree file is no object");
		TreeFileSpec file;
		if (!read_string(entry, "name", file.name, error))
			return false;
		if (!is_tree_file_name(file.name) || !names.insert(file.name).second)
			return fail(error, "a tree file's name is no file name, or is listed twice");
		if (!read_count(entry, "size", file.size, error))
			return false;
		out.files.push_back(std::move(file));
	}
	return true;
}

bool read_desired(const JsonValue &v, DesiredContainer &out, std::string &error) {
	if (!v.is_object())
		return fail(error, "a desired container is no object");
	if (!io::json_members_known(v,
				{ "name", "slot", "image", "uid", "base_tree", "expansion_tree", "expansion_code",
						"args", "files", "env", "cpus", "memory_mb", "pids", "log" },
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
	if (!read_whole(v, "base_tree", 1, kMaxTreeId, whole, error))
		return false;
	out.base_tree = whole;
	if (!read_optional_whole(v, "expansion_tree", 1, kMaxTreeId, out.expansion_tree, error))
		return false;
	if (!read_string(v, "expansion_code", out.expansion_code, error))
		return false;
	if (out.expansion_tree.has_value() != !out.expansion_code.empty())
		return fail(error, "a desired container's expansion tree and code come together");
	if (!out.expansion_code.empty() && !is_expansion_code(out.expansion_code))
		return fail(error, "a desired container's expansion code is not one");
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
	const JsonValue *log = v.get("log");
	if (!log || !log->is_object())
		return fail(error, "a desired container's log is no object");
	if (!io::json_members_known(*log, { "stream" }, "desired container's log", error) ||
			!read_string(*log, "stream", out.log_stream, error))
		return false;
	if (!is_log_stream_name(out.log_stream))
		return fail(error, "a desired container's log stream is no stream name");
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

std::string hosted_log_stream(int64_t server_id, int64_t generation) {
	return std::to_string(server_id) + "/g" + std::to_string(generation);
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

bool is_tree_file_name(std::string_view name) {
	return is_slot_file_name(name);
}

bool is_expansion_code(std::string_view code) {
	return !code.empty() && code.size() <= kMaxExpansionCodeChars &&
			all_of(code, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool is_tree_url(std::string_view url) {
	if (url.empty() || url.size() > kMaxTreeUrlBytes || !is_printable_ascii(url) ||
			url.find(' ') != std::string_view::npos)
		return false;
	return (url.rfind("s3://", 0) == 0 && url.size() > 5) ||
			(url.rfind("file:///", 0) == 0 && url.size() > 8);
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

bool is_log_stream_name(std::string_view name) {
	if (name.empty() || name.size() > kMaxLogStreamBytes)
		return false;
	const auto stream_char = [](char c) { return is_alnum(c) || c == '.' || c == '_' || c == '-'; };
	size_t start = 0;
	while (start <= name.size()) {
		size_t end = name.find('/', start);
		if (end == std::string_view::npos)
			end = name.size();
		const std::string_view part = name.substr(start, end - start);
		if (part.empty() || part == "." || part == ".." || !all_of(part, stream_char))
			return false;
		start = end + 1;
	}
	return true;
}

bool is_image_id(std::string_view id) {
	return !id.empty() && id.size() <= 64 &&
			all_of(id, [](char c) { return is_alnum(c) || c == '-'; });
}

bool is_log_group_name(std::string_view name) {
	if (name.empty() || name.size() > 512)
		return false;
	return all_of(name,
			[](char c) { return is_alnum(c) || c == '.' || c == '/' || c == '_' || c == '-'; });
}

bool is_aws_region(std::string_view region) {
	if (region.empty() || region.size() > 32 || region[0] < 'a' || region[0] > 'z')
		return false;
	return all_of(region,
			[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool decode_agent_envelope(std::string_view json, AgentEnvelope &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error))
		return false;
	int64_t protocol = 0;
	if (!read_whole(v, "protocol", 0, 2147483647.0, protocol, error))
		return false;
	out.protocol = static_cast<int>(protocol);
	if (!read_agent_version(v, out.agent_version, error))
		return false;
	out.agent_image.clear();
	return !v.get("agent_image") || read_agent_image(v, out.agent_image, error);
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

const char *container_wait_text(ContainerWait wait) {
	switch (wait) {
		case ContainerWait::None:
			return "";
		case ContainerWait::Image:
			return "image";
		case ContainerWait::Trees:
			return "trees";
	}
	return "";
}

std::optional<ContainerWait> parse_container_wait(std::string_view text) {
	for (const ContainerWait wait :
			{ ContainerWait::None, ContainerWait::Image, ContainerWait::Trees }) {
		if (text == container_wait_text(wait))
			return wait;
	}
	return std::nullopt;
}

const char *tree_report_state_text(TreeReportState state) {
	switch (state) {
		case TreeReportState::Fetching:
			return "fetching";
		case TreeReportState::Ready:
			return "ready";
		case TreeReportState::Failed:
			return "failed";
	}
	return "failed";
}

std::optional<TreeReportState> parse_tree_report_state(std::string_view text) {
	for (const TreeReportState state :
			{ TreeReportState::Fetching, TreeReportState::Ready, TreeReportState::Failed }) {
		if (text == tree_report_state_text(state))
			return state;
	}
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
	return io::json_write(v);
}

bool decode_machine_bootstrap(std::string_view json, MachineBootstrap &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v,
				{ "protocol", "agent_secret", "serve_image", "slots" }, "bootstrap", error))
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
	return true;
}

// --- the sync -------------------------------------------------------------------

std::string encode_sync_request(const SyncRequest &request) {
	JsonValue v = JsonValue::make_object();
	v.set("protocol", json_int(request.protocol));
	v.set("agent_version", io::json_string(request.agent_version));
	v.set("agent_image", io::json_string(request.agent_image));
	v.set("image_id", io::json_string(request.image_id));
	v.set("log_stream", io::json_string(request.log_stream));
	// Present once the agent has read its machine twice.
	if (request.machine) {
		const MachineStats &m = *request.machine;
		JsonValue machine = JsonValue::make_object();
		machine.set("cpu_percent", io::json_number(m.cpu_percent));
		machine.set("memory_used_bytes", json_count(m.memory_used_bytes));
		machine.set("memory_total_bytes", json_count(m.memory_total_bytes));
		machine.set("disk_free_bytes", json_count(m.disk_free_bytes));
		machine.set("disk_total_bytes", json_count(m.disk_total_bytes));
		machine.set("net_in_bytes", json_count(m.net_in_bytes));
		machine.set("net_out_bytes", json_count(m.net_out_bytes));
		v.set("machine", std::move(machine));
	}
	JsonValue trees = JsonValue::make_object();
	for (const auto &[id, tree] : request.trees) {
		JsonValue entry = JsonValue::make_object();
		entry.set("state", io::json_string(tree_report_state_text(tree.state)));
		entry.set("bytes", json_count(tree.bytes));
		entry.set("tries", json_int(tree.tries));
		if (!tree.reason.empty())
			entry.set("reason", io::json_string(tree.reason));
		trees.set(std::to_string(id), std::move(entry));
	}
	v.set("trees", std::move(trees));
	JsonValue containers = JsonValue::make_array();
	for (const ContainerReport &c : request.containers) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", io::json_string(c.name));
		entry.set("slot", json_int(c.slot));
		entry.set("state", io::json_string(container_state_text(c.state)));
		entry.set("waiting", io::json_string(container_wait_text(c.waiting)));
		entry.set("exit_code", c.exit_code ? json_int(*c.exit_code) : JsonValue::make_null());
		entry.set("oom", JsonValue::make_bool(c.oom));
		entry.set("started_at", c.started_at ? json_int(*c.started_at) : JsonValue::make_null());
		// Present only on an exited container, with why.
		if (!c.reason.empty())
			entry.set("reason", io::json_string(c.reason));
		entry.set("cpu_percent", io::json_number(c.cpu_percent));
		entry.set("memory_bytes", json_count(c.memory_bytes));
		entry.set("egress_bytes", json_count(c.egress_bytes));
		entry.set("net_in_bytes", json_count(c.net_in_bytes));
		// Present only on a running one whose server's ticks are fresh.
		if (c.tick) {
			JsonValue tick = JsonValue::make_object();
			tick.set("p50_ms", io::json_number(c.tick->p50_ms));
			tick.set("p99_ms", io::json_number(c.tick->p99_ms));
			tick.set("max_ms", io::json_number(c.tick->max_ms));
			tick.set("late", json_count(c.tick->late));
			entry.set("tick", std::move(tick));
		}
		containers.push(std::move(entry));
	}
	v.set("containers", std::move(containers));
	// Present only when it is so.
	if (request.listing_failed)
		v.set("listing_failed", JsonValue::make_bool(true));
	return io::json_write(v);
}

bool decode_sync_request(std::string_view json, SyncRequest &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v,
				{ "protocol", "agent_version", "agent_image", "image_id", "log_stream", "machine",
						"trees", "containers", "listing_failed" },
				"sync", error))
		return false;
	if (!read_agent_version(v, out.agent_version, error) ||
			!read_agent_image(v, out.agent_image, error))
		return false;
	if (!read_string(v, "image_id", out.image_id, error))
		return false;
	if (!out.image_id.empty() && !is_image_id(out.image_id))
		return fail(error, "image_id is no image id");
	if (!read_string(v, "log_stream", out.log_stream, error))
		return false;
	if (!out.log_stream.empty() && !is_log_stream_name(out.log_stream))
		return fail(error, "log_stream is no stream name");
	out.machine.reset();
	if (const JsonValue *machine = v.get("machine")) {
		MachineStats read;
		if (!read_machine_stats(*machine, read, error))
			return false;
		out.machine = read;
	}
	const JsonValue *trees = v.get("trees");
	if (!trees || !trees->is_object() || trees->object.size() > kMaxReportedTrees)
		return fail(error, "trees is no map of at most 64 trees");
	out.trees.clear();
	for (const io::JsonMember &member : trees->object) {
		int64_t id = 0;
		if (!read_tree_key(member.key, id))
			return fail(error, "a reported tree's key is no tree id");
		TreeReport tree;
		if (!read_tree_report(member.value, tree, error))
			return false;
		out.trees[id] = std::move(tree);
	}
	out.listing_failed = false;
	if (const JsonValue *failed = v.get("listing_failed")) {
		if (!failed->is_bool() || !failed->boolean)
			return fail(error, "listing_failed is no true");
		out.listing_failed = true;
	}
	const JsonValue *containers = v.get("containers");
	if (!containers || !containers->is_array() || containers->array.size() > kMaxReportedContainers)
		return fail(error, "containers is no list of containers");
	if (out.listing_failed && !containers->array.empty())
		return fail(error, "a listing that failed reports containers");
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
		entry.set("base_tree", json_int(d.base_tree));
		entry.set("expansion_tree", d.expansion_tree ? json_int(*d.expansion_tree) : JsonValue::make_null());
		entry.set("expansion_code", io::json_string(d.expansion_code));
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
		JsonValue log = JsonValue::make_object();
		log.set("stream", io::json_string(d.log_stream));
		entry.set("log", std::move(log));
		desired.push(std::move(entry));
	}
	v.set("desired", std::move(desired));
	JsonValue trees = JsonValue::make_object();
	for (const auto &[id, spec] : reply.trees) {
		JsonValue entry = JsonValue::make_object();
		entry.set("url", io::json_string(spec.url));
		entry.set("version", io::json_string(spec.version));
		JsonValue files = JsonValue::make_array();
		for (const TreeFileSpec &f : spec.files) {
			JsonValue file = JsonValue::make_object();
			file.set("name", io::json_string(f.name));
			file.set("size", io::json_number(static_cast<double>(f.size)));
			files.push(std::move(file));
		}
		entry.set("files", std::move(files));
		trees.set(std::to_string(id), std::move(entry));
	}
	v.set("trees", std::move(trees));
	return io::json_write(v);
}

bool decode_sync_reply(std::string_view json, SyncReply &out, std::string &error) {
	JsonValue v;
	if (!parse_object(json, v, error) || !read_protocol(v, out.protocol, error))
		return false;
	if (!io::json_members_known(v, { "protocol", "desired", "trees", "drain" }, "sync reply", error))
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
	const JsonValue *trees = v.get("trees");
	if (!trees || !trees->is_object() || trees->object.size() > kMaxReplyTrees)
		return fail(error, "trees is no map of at most 32 trees");
	out.trees.clear();
	for (const io::JsonMember &member : trees->object) {
		int64_t id = 0;
		if (!read_tree_key(member.key, id))
			return fail(error, "a tree's key is no tree id");
		TreeSpec spec;
		if (!read_tree_spec(member.value, spec, error))
			return false;
		out.trees[id] = std::move(spec);
	}
	for (const DesiredContainer &d : out.desired) {
		if (out.trees.count(d.base_tree) == 0 ||
				(d.expansion_tree && out.trees.count(*d.expansion_tree) == 0))
			return fail(error, "a desired container names a tree the reply does not carry");
	}
	return true;
}

} // namespace opennova::novaworld_server::hosted
