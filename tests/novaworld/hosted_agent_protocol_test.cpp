// The machine agent's protocol (apps/novaworld_common/hosted/agent_protocol.h), the
// one opennova-hostd and the NovaWorld service both build: each message's round trip
// through its encoder and its strict decoder (the bootstrap's hello and reply, the
// sync's report, the reason a container never started and a listing that failed among
// them, and its reply, the files' bytes in base64, cp1252 and NUL included, the trees each
// container uses and their manifests), the exact member names on the wire, the protocol
// check every decoder makes first (protocol_of reads it alone), every field's shape (names,
// images, tree ids, URLs and file names, expansion codes, file names, environment names and
// values, the uid its slot's, the limits' ranges, one container per slot and per name, a
// container's trees among the reply's), the routes' paths, the container names and the
// secret's digest. Pure functions: no listener, no docker.

#include "hosted/agent_protocol.h"

#include <base/io/json.h>
#include <base/io/sha256.h>

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;
namespace io = opennova::io;
using hosted::ContainerState;

namespace {

const std::string kSecret = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789abcdefghijkl"; // 48
const std::string kImage =
		"ghcr.io/opennova-net/"
		"opennova-serve@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A literal's bytes, NULs included.
template <size_t N> std::string bytes(const char (&text)[N]) {
	return std::string(text, N - 1);
}

hosted::MachineBootstrap sample_bootstrap() {
	hosted::MachineBootstrap b;
	b.agent_secret = kSecret;
	b.serve_image = kImage;
	b.slots = 12;
	return b;
}

hosted::TreeSpec sample_tree(const char *url, const char *version) {
	hosted::TreeSpec t;
	t.url = url;
	t.version = version;
	t.files = { { "resource.pff", 1073741824ull }, { "version.txt", 38 } };
	return t;
}

// The trees sample_desired names: 7 the base, 8 the expansion.
std::map<int64_t, hosted::TreeSpec> sample_trees() {
	return { { 7, sample_tree("s3://opennova-backups/data/prod/trees/7/", "Joint Operations, V1.7.5.3") },
		{ 8, sample_tree("file:///var/lib/opennova/store/trees/8/", "Escalation, V1.0") } };
}

hosted::DesiredContainer sample_desired(int slot) {
	hosted::DesiredContainer d;
	d.name = hosted::hosted_container_name(40 + slot, 3);
	d.slot = slot;
	d.image = kImage;
	d.uid = hosted::slot_uid(slot);
	d.base_tree = 7;
	d.expansion_tree = 8;
	d.expansion_code = "jox01";
	d.args = { "/HOST", "server.host", "--credentials", "credentials.txt", "--master-host",
		"203.0.113.7", "--master-gate-port", "7597", "/game", "jo" };
	// The host file's cp1252 bytes (0xE9) and a NUL survive the trip.
	d.files = std::map<std::string, std::string>{ { "server.host",
														  bytes("GameName \"Caf\xE9\"\r\n") },
		{ "credentials.txt", bytes("NOVAWORLD_USER srv-41\nNUL\0here\n") } };
	d.env = { { "OPENNOVA_SLOT", std::to_string(slot) } };
	d.cpus = 1.0;
	d.memory_mb = 512;
	d.pids = 128;
	return d;
}

hosted::ContainerReport sample_report(int slot, ContainerState state) {
	hosted::ContainerReport r;
	r.name = hosted::hosted_container_name(40 + slot, 3);
	r.slot = slot;
	r.state = state;
	if (state == ContainerState::Exited) {
		r.exit_code = 137;
		r.oom = true;
	}
	if (state != ContainerState::Starting)
		r.started_at = 1760443200;
	r.cpu_percent = 12.5;
	r.memory_bytes = 123456789;
	r.egress_bytes = 9007199254740991ull; // 2^53 - 1, the widest a count carries
	return r;
}

// `json` with its member `key` replaced by `value` (a JSON text), at the top level or,
// with `index` set, in that element of the list member `list`.
std::string with_member(const std::string &json, const std::string &key, const std::string &value,
		const char *list = nullptr, size_t index = 0) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(json, v, error))
		return std::string();
	io::JsonValue replacement;
	if (!io::json_parse(value, replacement, error))
		return std::string();
	io::JsonValue *target = list ? &v.get(list)->array[index] : &v;
	target->set(key, replacement);
	return io::json_write(v);
}

std::string without_member(const std::string &json, const std::string &key,
		const char *list = nullptr, size_t index = 0) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(json, v, error))
		return std::string();
	io::JsonValue *target = list ? &v.get(list)->array[index] : &v;
	for (auto it = target->object.begin(); it != target->object.end(); ++it) {
		if (it->key == key) {
			target->object.erase(it);
			break;
		}
	}
	return io::json_write(v);
}

int test_paths_and_names() {
	TEST_EXPECT(hosted::machine_bootstrap_path(7) == "/hosted/machines/7/bootstrap");
	TEST_EXPECT(hosted::machine_sync_path(1234567890123) == "/hosted/machines/1234567890123/sync");
	TEST_EXPECT(hosted::hosted_container_name(12, 3) == "srv-12-g3");
	TEST_EXPECT(hosted::is_container_name(
			hosted::hosted_container_name(9007199254740991, 9007199254740991)));
	TEST_EXPECT(
			hosted::agent_secret_digest(kSecret) == io::sha256_hex(kSecret.data(), kSecret.size()));
	TEST_EXPECT(hosted::agent_secret_digest(kSecret).size() == 64);

	TEST_EXPECT(hosted::is_container_name("srv-1-g1") && hosted::is_container_name("a") &&
			hosted::is_container_name("A_b.c-9"));
	TEST_EXPECT(!hosted::is_container_name("") && !hosted::is_container_name("-srv") &&
			!hosted::is_container_name("srv 1") && !hosted::is_container_name("srv/1") &&
			!hosted::is_container_name(std::string(65, 'a')) &&
			!hosted::is_container_name("srv;rm"));
	TEST_EXPECT(
			hosted::is_image_reference(kImage) && hosted::is_image_reference("opennova-serve:ci"));
	TEST_EXPECT(!hosted::is_image_reference("") && !hosted::is_image_reference("--privileged") &&
			!hosted::is_image_reference("a b") &&
			!hosted::is_image_reference(std::string(256, 'a')));
	TEST_EXPECT(hosted::is_expansion_code("jox01") && hosted::is_expansion_code("x") &&
			hosted::is_expansion_code("abcd1234"));
	for (const char *bad : { "", "JOX01", "jox-1", "jox_01", "abcd12345", "../x" })
		TEST_EXPECT(!hosted::is_expansion_code(bad));
	TEST_EXPECT(hosted::is_tree_url("s3://opennova-backups/data/prod/trees/7/") &&
			hosted::is_tree_url("file:///var/lib/opennova/store/trees/8/") &&
			hosted::is_tree_url("file:///C:/store/trees/8/"));
	for (const char *bad : { "", "s3://", "file://", "file://relative", "http://x/", "s3://a b",
				 "s3://a\tb" })
		TEST_EXPECT(!hosted::is_tree_url(bad));
	TEST_EXPECT(!hosted::is_tree_url("s3://" + std::string(600, 'a')));
	TEST_EXPECT(hosted::is_tree_file_name("resource.pff") && hosted::is_tree_file_name("jox01L.pff"));
	for (const char *bad : { "", ".", "..", "a/b", "expansion/jox01/x", "a b" })
		TEST_EXPECT(!hosted::is_tree_file_name(bad));
	TEST_EXPECT(hosted::is_slot_file_name("server.host") && hosted::is_slot_file_name("game.cfg"));
	for (const char *bad : { "", ".", "..", "a/b", "a\\b", "a b" })
		TEST_EXPECT(!hosted::is_slot_file_name(bad));
	TEST_EXPECT(hosted::is_env_name("OPENNOVA_SLOT") && hosted::is_env_name("_x1"));
	for (const char *bad : { "", "1X", "A-B", "A=B", "A B" })
		TEST_EXPECT(!hosted::is_env_name(bad));
	TEST_EXPECT(hosted::is_log_group_name("/opennova/prod/machines") &&
			hosted::is_log_group_name(std::string(512, 'a')));
	for (const std::string &bad : { std::string(), std::string(513, 'a'), std::string("/a b"),
				 std::string("/a'b"), std::string("/a:b"), std::string("/a%b") })
		TEST_EXPECT(!hosted::is_log_group_name(bad));
	TEST_EXPECT(hosted::is_aws_region("us-east-1") && hosted::is_aws_region("eu-central-2"));
	for (const char *bad : { "", "US-EAST-1", "us east 1", "-us-east-1", "1us", "us_east_1" })
		TEST_EXPECT(!hosted::is_aws_region(bad));

	for (const ContainerState s :
			{ ContainerState::Starting, ContainerState::Running, ContainerState::Exited })
		TEST_EXPECT(hosted::parse_container_state(hosted::container_state_text(s)) == s);
	TEST_EXPECT(
			!hosted::parse_container_state("created") && !hosted::parse_container_state("Running"));
	return 0;
}

int test_hello() {
	hosted::AgentHello hello;
	hello.agent_version = "abc1234";
	const std::string json = hosted::encode_agent_hello(hello);
	TEST_EXPECT(json.find("\"agent_version\": \"abc1234\"") != std::string::npos);
	TEST_EXPECT(json.find("\"protocol\": 2") != std::string::npos);
	hosted::AgentHello back;
	std::string error;
	TEST_EXPECT(hosted::decode_agent_hello(json, back, error));
	TEST_EXPECT(back.protocol == 2 && back.agent_version == "abc1234");
	TEST_EXPECT(
			!hosted::decode_agent_hello(with_member(json, "agent_version", "\"\""), back, error));
	TEST_EXPECT(!hosted::decode_agent_hello(
			with_member(json, "agent_version", "\"tab\\there\""), back, error));
	TEST_EXPECT(!hosted::decode_agent_hello(with_member(json, "extra", "1"), back, error));
	TEST_EXPECT(!hosted::decode_agent_hello("[]", back, error) &&
			!hosted::decode_agent_hello("{", back, error));
	return 0;
}

int test_bootstrap() {
	const hosted::MachineBootstrap b = sample_bootstrap();
	const std::string json = hosted::encode_machine_bootstrap(b);
	for (const char *member : { "\"agent_secret\"", "\"serve_image\"", "\"slots\": 12" })
		TEST_EXPECT(json.find(member) != std::string::npos);
	// The machine mounts no data set of its own any more: none is named here.
	TEST_EXPECT(json.find("data_sets") == std::string::npos);
	hosted::MachineBootstrap back;
	std::string error;
	TEST_EXPECT(hosted::decode_machine_bootstrap(json, back, error));
	TEST_EXPECT(back.agent_secret == kSecret && back.serve_image == kImage && back.slots == 12);

	// Every field in its shape. The error never quotes the secret.
	TEST_EXPECT(!hosted::decode_machine_bootstrap(
			with_member(json, "agent_secret", "\"short\""), back, error));
	TEST_EXPECT(error.find("short") == std::string::npos);
	TEST_EXPECT(!hosted::decode_machine_bootstrap(
			with_member(json, "agent_secret", "\"" + kSecret.substr(0, 40) + "!!!!!!!!\""), back,
			error));
	TEST_EXPECT(error.find(kSecret.substr(0, 40)) == std::string::npos);
	TEST_EXPECT(!hosted::decode_machine_bootstrap(
			with_member(json, "serve_image", "\"--rm\""), back, error));
	for (const char *slots : { "0", "33", "1.5", "\"12\"" })
		TEST_EXPECT(
				!hosted::decode_machine_bootstrap(with_member(json, "slots", slots), back, error));
	TEST_EXPECT(hosted::decode_machine_bootstrap(with_member(json, "slots", "32"), back, error) &&
			back.slots == 32);
	// A reply of the protocol before this one carried the mounts: refused as an unknown member.
	TEST_EXPECT(!hosted::decode_machine_bootstrap(
			with_member(json, "data_sets", "{\"jop\":\"/opt/game\"}"), back, error));
	TEST_EXPECT(
			!hosted::decode_machine_bootstrap(without_member(json, "serve_image"), back, error));
	TEST_EXPECT(
			!hosted::decode_machine_bootstrap(with_member(json, "token", "\"x\""), back, error));
	return 0;
}

int test_protocol() {
	const std::string json = hosted::encode_machine_bootstrap(sample_bootstrap());
	TEST_EXPECT(hosted::protocol_of(json) == 2);
	const std::string other = with_member(json, "protocol", "1");
	TEST_EXPECT(hosted::protocol_of(other) == 1);
	TEST_EXPECT(!hosted::protocol_of("{}") && !hosted::protocol_of("[1]") &&
			!hosted::protocol_of("nope") && !hosted::protocol_of("{\"protocol\":\"1\"}") &&
			!hosted::protocol_of("{\"protocol\":1.5}"));
	// Every decoder reads no further than another protocol.
	hosted::MachineBootstrap b;
	std::string error;
	TEST_EXPECT(!hosted::decode_machine_bootstrap(other, b, error) && error == "protocol 1, not 2");
	hosted::SyncRequest request;
	hosted::SyncReply reply;
	hosted::AgentHello hello;
	TEST_EXPECT(!hosted::decode_sync_request("{\"protocol\":1}", request, error) &&
			error == "protocol 1, not 2");
	TEST_EXPECT(!hosted::decode_sync_reply(
			"{\"protocol\":0,\"desired\":[],\"trees\":{},\"drain\":false}", reply, error));
	TEST_EXPECT(!hosted::decode_agent_hello("{\"agent_version\":\"x\"}", hello, error) &&
			error == "no protocol");
	return 0;
}

int test_sync_request() {
	hosted::SyncRequest request;
	request.agent_version = "0123abcd";
	request.containers = { sample_report(0, ContainerState::Running),
		sample_report(1, ContainerState::Exited), sample_report(2, ContainerState::Starting) };
	const std::string json = hosted::encode_sync_request(request);
	// The wire's member names, and null for what is not yet known.
	for (const char *member : { "\"containers\"", "\"name\": \"srv-40-g3\"", "\"slot\": 0",
				 "\"state\": \"running\"", "\"state\": \"exited\"", "\"state\": \"starting\"",
				 "\"exit_code\": 137", "\"exit_code\": null", "\"oom\": true",
				 "\"started_at\": 1760443200", "\"started_at\": null", "\"cpu_percent\": 12.5",
				 "\"memory_bytes\": 123456789", "\"egress_bytes\": 9007199254740991" })
		TEST_EXPECT(json.find(member) != std::string::npos);
	// A container that started, runs or exited on its own carries no reason.
	TEST_EXPECT(json.find("reason") == std::string::npos);
	hosted::SyncRequest back;
	std::string error;
	TEST_EXPECT(hosted::decode_sync_request(json, back, error));
	TEST_EXPECT(back.agent_version == "0123abcd" && back.containers.size() == 3);
	for (size_t i = 0; i < 3; ++i) {
		const hosted::ContainerReport &a = request.containers[i];
		const hosted::ContainerReport &b = back.containers[i];
		TEST_EXPECT(a.name == b.name && a.slot == b.slot && a.state == b.state &&
				a.exit_code == b.exit_code);
		TEST_EXPECT(
				a.oom == b.oom && a.started_at == b.started_at && a.cpu_percent == b.cpu_percent);
		TEST_EXPECT(a.memory_bytes == b.memory_bytes && a.egress_bytes == b.egress_bytes);
		TEST_EXPECT(b.reason.empty());
	}
	// No containers at all is a report too.
	request.containers.clear();
	TEST_EXPECT(hosted::decode_sync_request(hosted::encode_sync_request(request), back, error) &&
			back.containers.empty());
	// A report of the trees the machine holds (an earlier agent's) is an unknown member.
	{
		std::string with_trees = hosted::encode_sync_request(request);
		with_trees.insert(with_trees.rfind('}'), ", \"trees\": []");
		TEST_EXPECT(!hosted::decode_sync_request(with_trees, back, error));
	}
	// A start that failed for a reason: exited with no start, the reason riding along, a
	// short printable line, and only there (never with a start, without an exit, or empty).
	hosted::ContainerReport unfetched = sample_report(3, ContainerState::Exited);
	unfetched.exit_code = 125;
	unfetched.oom = false;
	unfetched.started_at.reset();
	unfetched.reason = "its data could not be fetched: aws s3 sync exited 1";
	request.containers = { unfetched, sample_report(0, ContainerState::Running) };
	const std::string with_reason = hosted::encode_sync_request(request);
	TEST_EXPECT(with_reason.find("\"reason\": \"its data could not be fetched: aws s3 sync exited 1\"") !=
			std::string::npos);
	TEST_EXPECT(with_reason.find("\"reason\"") == with_reason.rfind("\"reason\""));
	TEST_EXPECT(hosted::decode_sync_request(with_reason, back, error) && back.containers.size() == 2 &&
			back.containers[0].reason == unfetched.reason && back.containers[1].reason.empty());
	const auto reason_refused = [&](const std::string &key, const std::string &value) {
		hosted::SyncRequest out;
		std::string why;
		return !hosted::decode_sync_request(
				with_member(with_reason, key, value, "containers", 0), out, why);
	};
	TEST_EXPECT(reason_refused("reason", "\"\"") && reason_refused("reason", "1") &&
			reason_refused("reason", std::string("\"") + std::string(201, 'x') + "\"") &&
			reason_refused("reason", "\"tab\\there\""));
	TEST_EXPECT(!reason_refused("reason", std::string("\"") + std::string(200, 'x') + "\""));
	TEST_EXPECT(reason_refused("started_at", "1760443200") && reason_refused("exit_code", "null") &&
			reason_refused("state", "\"running\""));

	request.containers = { sample_report(0, ContainerState::Running) };
	const std::string one = hosted::encode_sync_request(request);
	const auto refused = [&](const std::string &key, const std::string &value) {
		hosted::SyncRequest out;
		std::string why;
		return !hosted::decode_sync_request(
				with_member(one, key, value, "containers", 0), out, why);
	};
	TEST_EXPECT(refused("name", "\"srv 1\"") && refused("slot", "32") && refused("slot", "-1"));
	TEST_EXPECT(
			refused("state", "\"created\"") && refused("exit_code", "1.5") && refused("oom", "0"));
	TEST_EXPECT(refused("started_at", "-1") && refused("cpu_percent", "-1") &&
			refused("cpu_percent", "\"1%\""));
	TEST_EXPECT(refused("memory_bytes", "-1") && refused("egress_bytes", "18014398509481984") &&
			refused("egress_bytes", "1.5"));
	TEST_EXPECT(refused("extra", "1"));
	TEST_EXPECT(!hosted::decode_sync_request(
			without_member(one, "exit_code", "containers", 0), back, error));
	// One name, one report.
	request.containers.push_back(sample_report(0, ContainerState::Running));
	TEST_EXPECT(!hosted::decode_sync_request(hosted::encode_sync_request(request), back, error));
	TEST_EXPECT(!hosted::decode_sync_request(without_member(one, "agent_version"), back, error));

	// A listing that failed: the member only then, true, and no container with it.
	TEST_EXPECT(one.find("listing_failed") == std::string::npos);
	TEST_EXPECT(hosted::decode_sync_request(one, back, error) && !back.listing_failed);
	hosted::SyncRequest unlisted;
	unlisted.agent_version = "0123abcd";
	unlisted.listing_failed = true;
	const std::string failed = hosted::encode_sync_request(unlisted);
	TEST_EXPECT(failed.find("\"listing_failed\": true") != std::string::npos);
	TEST_EXPECT(hosted::decode_sync_request(failed, back, error) && back.listing_failed &&
			back.containers.empty() && back.agent_version == "0123abcd");
	unlisted.containers = { sample_report(0, ContainerState::Running) };
	TEST_EXPECT(!hosted::decode_sync_request(hosted::encode_sync_request(unlisted), back, error) &&
			error.find("listing that failed") != std::string::npos);
	const auto with_flag = [&one](const std::string &value) {
		std::string json = one;
		json.insert(json.rfind('}'), ", \"listing_failed\": " + value);
		return json;
	};
	TEST_EXPECT(!hosted::decode_sync_request(with_flag("false"), back, error));
	TEST_EXPECT(!hosted::decode_sync_request(with_flag("1"), back, error));
	return 0;
}

int test_sync_reply() {
	hosted::SyncReply reply;
	reply.drain = true;
	reply.desired = { sample_desired(0), sample_desired(5) };
	reply.desired[1].files.reset(); // reported once: its files are no longer sent
	reply.desired[1].env.clear();
	reply.desired[1].expansion_tree.reset(); // the base game alone
	reply.desired[1].expansion_code.clear();
	reply.trees = sample_trees();
	const std::string json = hosted::encode_sync_reply(reply);
	for (const char *member : { "\"desired\"", "\"drain\": true", "\"uid\": 10001",
				 "\"uid\": 10006", "\"base_tree\": 7", "\"expansion_tree\": 8",
				 "\"expansion_tree\": null", "\"expansion_code\": \"jox01\"",
				 "\"expansion_code\": \"\"", "\"args\"", "\"files\"", "\"env\": {}", "\"cpus\": 1",
				 "\"memory_mb\": 512", "\"pids\": 128", "\"trees\"", "\"7\"", "\"8\"",
				 "\"url\": \"s3://opennova-backups/data/prod/trees/7/\"",
				 "\"version\": \"Escalation, V1.0\"", "\"name\": \"resource.pff\"",
				 "\"size\": 1073741824" })
		TEST_EXPECT(json.find(member) != std::string::npos);
	// The files' bytes travel as base64, never raw.
	TEST_EXPECT(json.find("Caf") == std::string::npos &&
			json.find("NOVAWORLD_USER") == std::string::npos);
	hosted::SyncReply back;
	std::string error;
	TEST_EXPECT(hosted::decode_sync_reply(json, back, error));
	TEST_EXPECT(back.drain && back.desired.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		const hosted::DesiredContainer &a = reply.desired[i];
		const hosted::DesiredContainer &b = back.desired[i];
		TEST_EXPECT(a.name == b.name && a.slot == b.slot && a.image == b.image && a.uid == b.uid);
		TEST_EXPECT(a.base_tree == b.base_tree && a.expansion_tree == b.expansion_tree &&
				a.expansion_code == b.expansion_code);
		TEST_EXPECT(a.args == b.args && a.files == b.files && a.env == b.env);
		TEST_EXPECT(a.cpus == b.cpus && a.memory_mb == b.memory_mb && a.pids == b.pids);
	}
	TEST_EXPECT(back.desired[0].files->at("credentials.txt").size() == 31);
	TEST_EXPECT(!back.desired[1].files);
	TEST_EXPECT(back.trees.size() == 2 && back.trees.at(7).url == reply.trees.at(7).url &&
			back.trees.at(7).version == reply.trees.at(7).version && back.trees.at(7).files.size() == 2 &&
			back.trees.at(7).files[0].name == "resource.pff" && back.trees.at(7).files[0].size == 1073741824ull &&
			back.trees.at(8).files[1].size == 38);

	reply.desired = { sample_desired(3) };
	const std::string one = hosted::encode_sync_reply(reply);
	const auto refused = [&](const std::string &key, const std::string &value) {
		hosted::SyncReply out;
		std::string why;
		return !hosted::decode_sync_reply(with_member(one, key, value, "desired", 0), out, why);
	};
	// The uid is its slot's, always: the metadata fence and the egress counters key on it.
	TEST_EXPECT(refused("uid", "10001") && refused("uid", "0") && refused("slot", "32"));
	TEST_EXPECT(refused("name", "\"--privileged\"") && refused("image", "\"a b\""));
	// The trees: the base and the expansion among the reply's, the expansion with its code.
	TEST_EXPECT(refused("base_tree", "0") && refused("base_tree", "9") && refused("base_tree", "\"7\""));
	TEST_EXPECT(refused("expansion_tree", "9") && refused("expansion_tree", "0"));
	TEST_EXPECT(refused("expansion_code", "\"\"") && refused("expansion_code", "\"JOX01\"") &&
			refused("expansion_code", "\"../x\""));
	TEST_EXPECT(!hosted::decode_sync_reply(with_member(one, "expansion_tree", "null", "desired", 0), back, error));
	{
		hosted::SyncReply base_only = reply;
		base_only.desired[0].expansion_tree.reset();
		base_only.desired[0].expansion_code.clear();
		TEST_EXPECT(hosted::decode_sync_reply(hosted::encode_sync_reply(base_only), back, error) &&
				!back.desired[0].expansion_tree && back.desired[0].expansion_code.empty());
		base_only.trees.erase(8); // a tree no container uses may be left out
		TEST_EXPECT(hosted::decode_sync_reply(hosted::encode_sync_reply(base_only), back, error));
		base_only.trees.erase(7); // the base's cannot
		TEST_EXPECT(!hosted::decode_sync_reply(hosted::encode_sync_reply(base_only), back, error));
	}
	// A tree's shapes: the key an id, the URL a store's, the version a line, the files 1..64
	// with their sizes, no two of one name; a digest (the manifest before this one carried
	// one) is an unknown member.
	const auto tree_refused = [&](const std::string &key, const std::string &value) {
		io::JsonValue v;
		std::string why;
		if (!io::json_parse(one, v, why))
			return false;
		io::JsonValue replacement;
		if (!io::json_parse(value, replacement, why))
			return false;
		v.get("trees")->get("7")->set(key, replacement);
		hosted::SyncReply out;
		return !hosted::decode_sync_reply(io::json_write(v), out, why);
	};
	TEST_EXPECT(tree_refused("url", "\"http://x/\"") && tree_refused("url", "\"\"") &&
			tree_refused("version", "\"a\\nb\"") && tree_refused("files", "[]") &&
			tree_refused("files", "{}") && tree_refused("extra", "1"));
	TEST_EXPECT(tree_refused("files", "[{\"name\":\"../x\",\"size\":1}]"));
	TEST_EXPECT(tree_refused("files", "[{\"name\":\"a.pff\",\"size\":-1}]"));
	TEST_EXPECT(tree_refused("files", "[{\"name\":\"a.pff\",\"size\":1,\"sha256\":\"" + std::string(64, 'a') + "\"}]"));
	TEST_EXPECT(tree_refused("files", "[{\"name\":\"a.pff\",\"size\":1},{\"name\":\"a.pff\",\"size\":2}]"));
	TEST_EXPECT(!tree_refused("files", "[{\"name\":\"a.pff\",\"size\":1},{\"name\":\"b.pff\",\"size\":0}]"));
	{
		io::JsonValue v;
		std::string why;
		TEST_EXPECT(io::json_parse(one, v, why));
		io::JsonValue *trees = v.get("trees");
		trees->set("07", trees->get("7") ? *trees->get("7") : io::JsonValue());
		TEST_EXPECT(!hosted::decode_sync_reply(io::json_write(v), back, error));
		io::JsonValue w;
		TEST_EXPECT(io::json_parse(one, w, why));
		w.get("trees")->set("x", *w.get("trees")->get("7"));
		TEST_EXPECT(!hosted::decode_sync_reply(io::json_write(w), back, error));
	}
	TEST_EXPECT(!hosted::decode_sync_reply(without_member(one, "trees"), back, error));
	TEST_EXPECT(refused("args", "[\"\"]") && refused("args", "[\"a\\u0000b\"]") &&
			refused("args", "\"/HOST\""));
	TEST_EXPECT(refused("files", "{\"../etc/passwd\":\"eA==\"}") &&
			refused("files", "{\"a\":\"not base64\"}"));
	TEST_EXPECT(refused("files", "{\"a\":\"eA=\"}") && refused("files", "{\"a\":\"eB==\"}") &&
			refused("files", "{\"a\":\"e A=\"}") && refused("files", "{\"a\":\"eA==eA==\"}"));
	TEST_EXPECT(refused("env", "{\"A-B\":\"x\"}") && refused("env", "{\"A\":\"x\\ny\"}") &&
			refused("env", "{\"A\":1}") && refused("env", "[]"));
	TEST_EXPECT(refused("cpus", "0") && refused("cpus", "65") && refused("memory_mb", "8") &&
			refused("memory_mb", "512.5") && refused("pids", "1") && refused("pids", "40000"));
	TEST_EXPECT(refused("privileged", "true"));
	// The files' total is capped; empty files are files.
	TEST_EXPECT(!refused("files", "{\"empty\":\"\"}"));
	{
		const size_t half = hosted::kMaxContainerFileBytes / 2 + 3;
		std::string big = "{";
		for (int i = 0; i < 2; ++i)
			big += std::string(i ? "," : "") + "\"f" + std::to_string(i) + "\":\"" +
					std::string((half + 2) / 3 * 4, 'A') + "\"";
		big += "}";
		TEST_EXPECT(refused("files", big));
	}
	TEST_EXPECT(!hosted::decode_sync_reply(without_member(one, "env", "desired", 0), back, error));
	TEST_EXPECT(!hosted::decode_sync_reply(without_member(one, "drain"), back, error));

	// One container per slot and per name.
	reply.desired = { sample_desired(3), sample_desired(3) };
	reply.desired[1].name = "srv-99-g1";
	TEST_EXPECT(!hosted::decode_sync_reply(hosted::encode_sync_reply(reply), back, error));
	reply.desired = { sample_desired(3), sample_desired(4) };
	reply.desired[1].name = reply.desired[0].name;
	TEST_EXPECT(!hosted::decode_sync_reply(hosted::encode_sync_reply(reply), back, error));
	// Nothing desired: the machine runs nothing, and needs no tree.
	reply.desired.clear();
	reply.trees.clear();
	reply.drain = false;
	TEST_EXPECT(hosted::decode_sync_reply(hosted::encode_sync_reply(reply), back, error) &&
			back.desired.empty() && back.trees.empty() && !back.drain);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "paths_and_names", test_paths_and_names },
		{ "hello", test_hello },
		{ "bootstrap", test_bootstrap },
		{ "protocol", test_protocol },
		{ "sync_request", test_sync_request },
		{ "sync_reply", test_sync_reply },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted agent protocol\n");
	return 0;
}
