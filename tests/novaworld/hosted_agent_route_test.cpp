// The machine agents' routes (apps/novaworld_server/hosted/agent_route.h) on the
// internal listener (hosted/internal_listener.h) on a loopback port over a
// migrated database, driven with apps/common's http_exchange: the bootstrap (the
// one-time token redeemed once for a secret kept as its digest, the reply's
// image, slots and data sets, no-store; a wrong, missing or spent token, a
// hello that does not read, a machine not yet launched or reached from another
// address, a reply that would not read: each the one bare 404, the token left
// unspent), the sync (the agent's secret, the report stored and the machine
// ready, a listing that failed heard from with nothing stored, the desired
// containers from a fake FleetDesiredState with the files
// of a reported container left out, each uid its slot's, a slot past the
// machine's dropped, drain while draining, another protocol draining the
// machine, a desired state that throws or does not read), every other path and
// method the same 404, and a listener whose networks leave loopback out. Then
// the local machine driver (hosted/local_machine_driver.h) end to end against
// the routes, this test binary as its opennova-serve: a machine bootstraps,
// starts a container in its slot's directory with the files owner-only, the
// argv per data set and an environment without the service's secret, reports
// it running, stops it (SIGTERM) and removes it with its files when the
// desired state drops it, reports one that exited with its code, and ends with
// the driver's terminate.

#include "hosted/agent_protocol.h"
#include "hosted/fleet_desired_state.h"
#include "hosted/fleet_repository.h"
#include "hosted/internal_listener.h"
#include "hosted/local_machine_driver.h"
#include "net_http.h"
#include "net_sockets.h"

#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace hosted = opennova::novaworld_server::hosted;
namespace net = opennova::net;
using hosted::ContainerState;
using hosted::FleetMachineState;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

std::string g_self;
constexpr const char *kImage = "opennova-serve:local";
// What an unknown path gets (Crow's own status text, nothing of the routes'),
// read by each rig as it starts.
std::string g_unknown_path_body;

// --- the fake desired state ---------------------------------------------------------------

class FakeDesiredState : public hosted::FleetDesiredState {
public:
	hosted::FleetDecision sync(Database &, const hosted::FleetMachineRow &machine,
			const std::vector<hosted::ContainerReport> &reported) override {
		std::lock_guard<std::mutex> lock(mu);
		++calls;
		last_machine = machine;
		last_reported = reported;
		if (throws)
			throw std::runtime_error("the fake desired state throws");
		const auto it = decisions.find(machine.id);
		return it == decisions.end() ? hosted::FleetDecision{} : it->second;
	}

	void set(int64_t machine, hosted::FleetDecision decision) {
		std::lock_guard<std::mutex> lock(mu);
		decisions[machine] = std::move(decision);
	}

	std::vector<hosted::ContainerReport> reported() {
		std::lock_guard<std::mutex> lock(mu);
		return last_reported;
	}

	std::mutex mu;
	std::map<int64_t, hosted::FleetDecision> decisions;
	hosted::FleetMachineRow last_machine;
	std::vector<hosted::ContainerReport> last_reported;
	int calls = 0;
	bool throws = false;
};

hosted::DesiredContainer desired(const std::string &name, int slot,
		std::vector<std::string> args = {}, bool with_files = true) {
	hosted::DesiredContainer d;
	d.name = name;
	d.slot = slot;
	d.image = kImage;
	d.uid = hosted::slot_uid(slot);
	d.data_set = hosted::kDataSetJointOperations;
	d.args = args.empty() ? std::vector<std::string>{ "/HOST", "server.host" } : std::move(args);
	if (with_files)
		d.files = std::map<std::string, std::string>{ { "server.host", "GameName \"Caf\xE9\"\r\n" },
			{ "credentials.txt", "NOVAWORLD_USER=srv-5\nNOVAWORLD_PASS=secretpassword\n" } };
	d.env = { { "OPENNOVA_SLOT_NOTE", "slot-" + std::to_string(slot) } };
	return d;
}

// --- the HTTP side ------------------------------------------------------------------------

struct Reply {
	int code = 0;
	std::string body;
	std::vector<std::string> headers;

	bool header(const std::string &line) const {
		for (const std::string &h : headers)
			if (h == line)
				return true;
		return false;
	}
};

Reply exchange(uint16_t port, const std::string &method, const std::string &path,
		const std::string &bearer, const std::string &body) {
	std::vector<std::string> headers = { "Content-Type: application/json" };
	if (!bearer.empty())
		headers.push_back("Authorization: Bearer " + bearer);
	const net::HttpReply r =
			net::http_exchange(method, "http://127.0.0.1:" + std::to_string(port) + path, headers,
					body, 10000, [](const std::string &host, net::Endpoint &out) {
						return net::resolve_ipv4(host, out, false);
					});
	Reply out;
	out.code = r.transport_ok ? r.code : -1;
	out.body.assign(r.body.begin(), r.body.end());
	out.headers = r.headers;
	return out;
}

std::string hello(int protocol = hosted::kAgentProtocol) {
	hosted::AgentHello h;
	h.protocol = protocol;
	h.agent_version = "test-agent 1";
	return hosted::encode_agent_hello(h);
}

std::string sync_body(const std::vector<hosted::ContainerReport> &containers,
		int protocol = hosted::kAgentProtocol) {
	hosted::SyncRequest r;
	r.protocol = protocol;
	r.agent_version = "test-agent 1";
	r.containers = containers;
	return hosted::encode_sync_request(r);
}

hosted::ContainerReport running(const std::string &name, int slot) {
	hosted::ContainerReport r;
	r.name = name;
	r.slot = slot;
	r.state = ContainerState::Running;
	r.started_at = 1760000000;
	r.egress_bytes = 4096;
	return r;
}

// A machine row, launched at `address` (empty: still launching).
hosted::CreatedMachine machine_row(Database &db, const std::string &address, int slots = 4,
		const std::string &image = kImage) {
	hosted::NewMachine m;
	m.provider = "local";
	m.slots = slots;
	m.serve_image = image;
	const hosted::CreatedMachine created = hosted::create_machine(db, m);
	if (!address.empty()) {
		hosted::LaunchedMachine l;
		l.cloud_id = "local-" + std::to_string(created.id) + "-1";
		l.private_address = address;
		hosted::mark_launched(db, created.id, l);
	}
	return created;
}

std::string token_hash(Database &db, int64_t id) {
	return db
			.query("SELECT bootstrap_token_hash FROM fleet_machines WHERE id = $1;",
					{ BindValue(id) })
			.front()
			.as_text(0)
			.value_or("");
}

hosted::AgentRouteConfig route_config(std::vector<hosted::Ipv4Cidr> admitted) {
	hosted::AgentRouteConfig c;
	c.admitted = std::move(admitted);
	c.data_sets = { { hosted::kDataSetJointOperations, "/opt/game/jop_2_consumer" } };
	return c;
}

const hosted::Ipv4Cidr kLoopback = { 0x7F000000u, 0xFF000000u };

// A listener on a loopback port over `scratch`'s database.
struct Rig {
	explicit Rig(
			const TestDatabase &scratch, std::vector<hosted::Ipv4Cidr> admitted = { kLoopback }) :
			pool(scratch.options(), 4),
			db(scratch.open()),
			listener(pool, fake, route_config(std::move(admitted))) {
		started = listener.start("127.0.0.1", 0);
		g_unknown_path_body = exchange(listener.bound_port(), "GET", "/no/such/path", "", "").body;
	}

	Reply post(const std::string &path, const std::string &bearer, const std::string &body) {
		return exchange(listener.bound_port(), "POST", path, bearer, body);
	}

	// Bootstraps machine `m`; its agent's secret, or "" on a refusal.
	std::string bootstrap(const hosted::CreatedMachine &m) {
		const Reply r = post(hosted::machine_bootstrap_path(m.id), m.bootstrap_token, hello());
		hosted::MachineBootstrap got;
		std::string error;
		if (r.code != 200 || !hosted::decode_machine_bootstrap(r.body, got, error))
			return {};
		return got.agent_secret;
	}

	FakeDesiredState fake;
	opennova::db::ConnectionPool pool;
	Database db;
	hosted::InternalListener listener;
	bool started = false;
};

// The bare 404 (g_unknown_path_body), uncacheable.
bool bare_404(const Reply &r) {
	if (r.code != 404 || r.body != g_unknown_path_body || r.body.find("404") != 0 ||
			!r.header("Cache-Control: no-store")) {
		std::fprintf(stderr, "  expected the bare 404, got %d '%s'\n", r.code, r.body.c_str());
		for (const std::string &h : r.headers)
			std::fprintf(stderr, "    %s\n", h.c_str());
		return false;
	}
	return true;
}

// --- the routes ---------------------------------------------------------------------------

int test_bootstrap() {
	TestDatabase scratch;
	Rig rig(scratch);
	TEST_EXPECT(rig.started && rig.listener.bound_port() != 0);
	const hosted::CreatedMachine m = machine_row(rig.db, "127.0.0.1");
	const std::string path = hosted::machine_bootstrap_path(m.id);

	// Refusals, each the bare 404 with the token left unspent.
	std::string wrong = m.bootstrap_token;
	wrong[0] = wrong[0] == 'A' ? 'B' : 'A';
	TEST_EXPECT(bare_404(rig.post(path, "", hello())));
	TEST_EXPECT(bare_404(rig.post(path, wrong, hello())));
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, hello(2))));
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, "{\"protocol\":1}")));
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, "not json")));
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, std::string(300 * 1024, ' '))));
	TEST_EXPECT(bare_404(
			rig.post(hosted::machine_bootstrap_path(m.id + 100), m.bootstrap_token, hello())));
	TEST_EXPECT(bare_404(rig.post(
			"/hosted/machines/99999999999999999999/bootstrap", m.bootstrap_token, hello())));
	TEST_EXPECT(bare_404(exchange(rig.listener.bound_port(), "GET", path, m.bootstrap_token, "")));
	TEST_EXPECT(token_hash(rig.db, m.id) == hosted::agent_secret_digest(m.bootstrap_token));

	// The redeem: a secret, the image the machine was launched with, its slots, the data sets.
	const Reply r = rig.post(path, m.bootstrap_token, hello());
	TEST_EXPECT(r.code == 200 && r.header("Cache-Control: no-store") &&
			r.header("Content-Type: application/json"));
	hosted::MachineBootstrap got;
	std::string error;
	TEST_EXPECT(hosted::decode_machine_bootstrap(r.body, got, error));
	TEST_EXPECT(got.agent_secret.size() == hosted::kAgentSecretChars);
	TEST_EXPECT(got.serve_image == kImage && got.slots == 4);
	TEST_EXPECT(got.data_sets.size() == 1 &&
			got.data_sets.at("jop_2_consumer") == "/opt/game/jop_2_consumer");
	auto row = hosted::find_machine(rig.db, m.id);
	TEST_EXPECT(row && row->bootstrapped && row->agent_version == "test-agent 1");
	TEST_EXPECT(token_hash(rig.db, m.id).empty());
	TEST_EXPECT(
			rig.db.query("SELECT 1 FROM fleet_machines WHERE id = $1 AND agent_secret_hash = $2;",
						  { BindValue(m.id),
								  BindValue(hosted::agent_secret_digest(got.agent_secret)) })
					.size() == 1);
	// Spent: a second redeem (a lost reply's retry) is refused.
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, hello())));

	// A machine the cloud has not answered for, or one reached from another address.
	const hosted::CreatedMachine launching = machine_row(rig.db, "");
	TEST_EXPECT(bare_404(rig.post(
			hosted::machine_bootstrap_path(launching.id), launching.bootstrap_token, hello())));
	const hosted::CreatedMachine elsewhere = machine_row(rig.db, "10.44.1.21");
	TEST_EXPECT(bare_404(rig.post(
			hosted::machine_bootstrap_path(elsewhere.id), elsewhere.bootstrap_token, hello())));
	TEST_EXPECT(token_hash(rig.db, elsewhere.id) ==
			hosted::agent_secret_digest(elsewhere.bootstrap_token));
	// A reply that would not read (no serve image): refused before the token is spent.
	const hosted::CreatedMachine imageless = machine_row(rig.db, "127.0.0.1", 4, "");
	TEST_EXPECT(bare_404(rig.post(
			hosted::machine_bootstrap_path(imageless.id), imageless.bootstrap_token, hello())));
	TEST_EXPECT(token_hash(rig.db, imageless.id) ==
			hosted::agent_secret_digest(imageless.bootstrap_token));

	// Every other path and method: the same 404.
	TEST_EXPECT(bare_404(rig.post("/", "", "")));
	TEST_EXPECT(bare_404(rig.post("/api/me", "", "")));
	TEST_EXPECT(bare_404(rig.post("/hosted/bootstrap/1", m.bootstrap_token, "")));
	TEST_EXPECT(bare_404(
			exchange(rig.listener.bound_port(), "GET", "/hosted/machines/1/sync", "", "")));
	return 0;
}

int test_sync() {
	TestDatabase scratch;
	Rig rig(scratch);
	TEST_EXPECT(rig.started);
	const hosted::CreatedMachine m = machine_row(rig.db, "127.0.0.1");
	const std::string secret = rig.bootstrap(m);
	TEST_EXPECT(!secret.empty());
	const std::string path = hosted::machine_sync_path(m.id);

	hosted::FleetDecision decision;
	decision.desired = { desired("srv-7-g1", 0), desired("srv-8-g1", 1), desired("srv-9-g1", 7) };
	decision.desired[1].uid = 0; // the route sets each uid to its slot's
	rig.fake.set(m.id, decision);

	// Refusals first: nothing is stored, the desired state is not asked.
	TEST_EXPECT(bare_404(rig.post(path, "", sync_body({}))));
	TEST_EXPECT(bare_404(rig.post(path, m.bootstrap_token, sync_body({}))));
	std::string wrong = secret;
	wrong[5] = wrong[5] == 'z' ? 'y' : 'z';
	TEST_EXPECT(bare_404(rig.post(path, wrong, sync_body({}))));
	TEST_EXPECT(bare_404(rig.post(path, secret, "{\"protocol\":1,\"agent_version\":\"x\"}")));
	TEST_EXPECT(bare_404(rig.post(hosted::machine_sync_path(m.id + 1), secret, sync_body({}))));
	TEST_EXPECT(rig.fake.calls == 0);
	TEST_EXPECT(hosted::find_machine(rig.db, m.id)->state == FleetMachineState::Booting);

	// An agent whose listing failed: heard from, nothing of its slots stored, and a
	// booting machine still booting.
	hosted::SyncRequest unlisted;
	unlisted.agent_version = "test-agent 2";
	unlisted.listing_failed = true;
	const std::string unlisted_body = hosted::encode_sync_request(unlisted);
	TEST_EXPECT(rig.post(path, secret, unlisted_body).code == 200);
	auto heard = hosted::find_machine(rig.db, m.id);
	TEST_EXPECT(heard->state == FleetMachineState::Booting && !heard->last_report_at.empty() &&
			heard->agent_version == "test-agent 2");
	TEST_EXPECT(hosted::slot_reports(rig.db, m.id).empty() && rig.fake.calls == 1);

	const Reply r = rig.post(path, secret, sync_body({ running("srv-7-g1", 0) }));
	TEST_EXPECT(r.code == 200 && r.header("Cache-Control: no-store"));
	hosted::SyncReply reply;
	std::string error;
	TEST_EXPECT(hosted::decode_sync_reply(r.body, reply, error));
	TEST_EXPECT(!reply.drain && reply.desired.size() == 2);
	// The reported container's files left out, the new one's sent; each uid its slot's.
	TEST_EXPECT(reply.desired[0].name == "srv-7-g1" && !reply.desired[0].files);
	TEST_EXPECT(reply.desired[1].name == "srv-8-g1" && reply.desired[1].files &&
			reply.desired[1].files->at("credentials.txt").find("secretpassword") !=
					std::string::npos);
	TEST_EXPECT(reply.desired[1].uid == hosted::slot_uid(1) &&
			reply.desired[0].uid == hosted::slot_uid(0));
	TEST_EXPECT(reply.desired[1].env.at("OPENNOVA_SLOT_NOTE") == "slot-1");
	// Stored: the machine ready, its report, and what the desired state was handed.
	auto row = hosted::find_machine(rig.db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Ready && !row->last_report_at.empty());
	const auto slots = hosted::slot_reports(rig.db, m.id);
	TEST_EXPECT(
			slots.size() == 1 && slots[0].container == "srv-7-g1" && slots[0].egress_bytes == 4096);
	TEST_EXPECT(rig.fake.calls == 2 && rig.fake.last_machine.state == FleetMachineState::Ready);
	TEST_EXPECT(rig.fake.reported().size() == 1 && rig.fake.reported()[0].name == "srv-7-g1");
	// A listing that fails now leaves the slots' reports as they were.
	TEST_EXPECT(rig.post(path, secret, unlisted_body).code == 200);
	TEST_EXPECT(hosted::slot_reports(rig.db, m.id).size() == 1 &&
			hosted::find_machine(rig.db, m.id)->state == FleetMachineState::Ready);

	// A draining machine is told so.
	TEST_EXPECT(hosted::transition_machine(
			rig.db, m.id, FleetMachineState::Ready, FleetMachineState::Draining, "roll"));
	TEST_EXPECT(
			hosted::decode_sync_reply(rig.post(path, secret, sync_body({})).body, reply, error) &&
			reply.drain);
	TEST_EXPECT(hosted::transition_machine(
			rig.db, m.id, FleetMachineState::Draining, FleetMachineState::Ready, ""));
	// The desired state asking for drain.
	decision.drain = true;
	rig.fake.set(m.id, decision);
	TEST_EXPECT(
			hosted::decode_sync_reply(rig.post(path, secret, sync_body({})).body, reply, error) &&
			reply.drain);

	// A desired state that does not read (two containers on one slot), or throws: nothing sent.
	hosted::FleetDecision clash;
	clash.desired = { desired("srv-7-g1", 0), desired("srv-8-g1", 0) };
	rig.fake.set(m.id, clash);
	TEST_EXPECT(bare_404(rig.post(path, secret, sync_body({}))));
	rig.fake.throws = true;
	TEST_EXPECT(bare_404(rig.post(path, secret, sync_body({}))));
	rig.fake.throws = false;

	// Another protocol: not read, and the machine drains.
	TEST_EXPECT(bare_404(rig.post(path, secret, sync_body({}, 2))));
	row = hosted::find_machine(rig.db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Draining &&
			row->end_reason.find("protocol 2") != std::string::npos);
	// An ending machine's agent is refused from then on.
	TEST_EXPECT(hosted::transition_machine(
			rig.db, m.id, FleetMachineState::Draining, FleetMachineState::Ending, ""));
	TEST_EXPECT(bare_404(rig.post(path, secret, sync_body({}))));
	return 0;
}

int test_networks() {
	TestDatabase scratch;
	// A listener admitting the VPC's network alone never answers loopback, whatever it presents.
	Rig rig(scratch, { hosted::Ipv4Cidr{ 0x0A000000u, 0xFF000000u } });
	TEST_EXPECT(rig.started);
	const hosted::CreatedMachine m = machine_row(rig.db, "127.0.0.1");
	TEST_EXPECT(
			bare_404(rig.post(hosted::machine_bootstrap_path(m.id), m.bootstrap_token, hello())));
	TEST_EXPECT(token_hash(rig.db, m.id) == hosted::agent_secret_digest(m.bootstrap_token));
	// A configured network that is neither private nor loopback is ignored.
	TestDatabase second;
	Rig open(second, { hosted::Ipv4Cidr{ 0, 0 } });
	TEST_EXPECT(open.started);
	const hosted::CreatedMachine n = machine_row(open.db, "127.0.0.1");
	TEST_EXPECT(
			bare_404(open.post(hosted::machine_bootstrap_path(n.id), n.bootstrap_token, hello())));
	return 0;
}

// --- the local machine driver end to end -----------------------------------------------------

std::string read_file(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool wait_for(const std::function<bool()> &done, int timeout_ms = 20000) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < until) {
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return done();
}

std::optional<hosted::LocalMachineView::Container> container_of(
		hosted::LocalMachineDriver &driver, const std::string &cloud_id, const std::string &name) {
	const auto view = driver.view(cloud_id);
	if (!view)
		return std::nullopt;
	for (const auto &c : view->containers)
		if (c.name == name)
			return c;
	return std::nullopt;
}

int test_local_machine_end_to_end(const std::string &scratch_dir) {
	TestDatabase scratch;
	Rig rig(scratch);
	TEST_EXPECT(rig.started);
	const fs::path root = fs::u8path(scratch_dir) / "e2e";
	const fs::path game = root / "game";
	const fs::path markers = root / "markers";
	fs::create_directories(game);
	fs::create_directories(markers);

	hosted::LocalMachineConfig config;
	config.serve_binary = g_self;
	hosted::DataSet jo = hosted::known_data_sets()[0];
	jo.resource_dir = game.u8string();
	jo.loose_root = true;
	config.data_sets = { jo };
	config.work_dir = (root / "work").u8string();
	config.service_url = "http://127.0.0.1:" + std::to_string(rig.listener.bound_port());
	config.agent_version = "local-test";
	config.sync_interval_ms = 100;
	config.busy_sync_interval_ms = 50;
	config.bootstrap_retry_ms = 50;
	config.stop_grace_ms = 3000;
	hosted::LocalMachineDriver driver(config);

	hosted::NewMachine spec;
	spec.provider = driver.name();
	spec.slots = 2;
	spec.serve_image = kImage;
	const hosted::CreatedMachine m = hosted::create_machine(rig.db, spec);
	hosted::MachineLaunch launch;
	launch.machine_id = m.id;
	launch.slots = 2;
	launch.bootstrap_token = m.bootstrap_token;
	std::string error;
	const auto launched = driver.launch(launch, error);
	TEST_EXPECT(launched);
	TEST_EXPECT(hosted::mark_launched(rig.db, m.id, *launched));
	const std::string cloud = launched->cloud_id;

	// The machine bootstraps and reports: ready, with nothing on it.
	TEST_EXPECT(wait_for([&] {
		const auto view = driver.view(cloud);
		return view && view->bootstrapped && view->syncs >= 1;
	}));
	TEST_EXPECT(wait_for(
			[&] { return hosted::find_machine(rig.db, m.id)->state == FleetMachineState::Ready; }));
	TEST_EXPECT(driver.view(cloud)->slots == 2);

	// A container: started in its slot's directory with its files.
	const fs::path started = markers / "srv-5-g1.started";
	const fs::path stopped = markers / "srv-5-g1.stopped";
	hosted::FleetDecision decision;
	decision.desired = { desired(
			"srv-5-g1", 1, { "--marker", (markers / "srv-5-g1").u8string(), "--run" }) };
	rig.fake.set(m.id, decision);
	TEST_EXPECT(wait_for([&] { return fs::exists(started); }));
	const auto c = container_of(driver, cloud, "srv-5-g1");
	TEST_EXPECT(c && c->slot == 1 && c->state == ContainerState::Running);
	TEST_EXPECT(c->argv.size() >= 4 && c->argv[0] == g_self && c->argv[1] == "--resource-dir");
	TEST_EXPECT(c->argv[2] == game.u8string() && c->argv[3] == "--loose-root");
	const fs::path slot_dir =
			fs::u8path(config.work_dir) / ("machine-" + std::to_string(m.id)) / "slot-1";
	// What the fake server saw: its working directory, the slot's files, its environment.
	const std::string seen = read_file(started);
	TEST_EXPECT(seen.find("cwd=" + fs::canonical(slot_dir).u8string() + "\n") != std::string::npos);
	TEST_EXPECT(seen.find("note=slot-1\n") != std::string::npos);
	TEST_EXPECT(seen.find("secret=\n") != std::string::npos);
	TEST_EXPECT(
			read_file(slot_dir / "credentials.txt").find("secretpassword") != std::string::npos);
#ifndef _WIN32
	struct stat st{};
	TEST_EXPECT(::stat((slot_dir / "credentials.txt").c_str(), &st) == 0 &&
			(st.st_mode & 0777) == 0600);
	TEST_EXPECT(::stat(slot_dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
#endif
	// Reported running, and from then on its files are left out of the replies.
	TEST_EXPECT(wait_for([&] {
		for (const auto &r : rig.fake.reported())
			if (r.name == "srv-5-g1" && r.state == ContainerState::Running && r.slot == 1)
				return true;
		return false;
	}));
	TEST_EXPECT(wait_for([&] {
		const auto slots = hosted::slot_reports(rig.db, m.id);
		return slots.size() == 1 && slots[0].container == "srv-5-g1" &&
				slots[0].state == ContainerState::Running;
	}));

	// Dropped: asked to stop (SIGTERM), removed with its slot's files.
	rig.fake.set(m.id, hosted::FleetDecision{});
	TEST_EXPECT(wait_for([&] { return !container_of(driver, cloud, "srv-5-g1"); }));
#ifndef _WIN32
	TEST_EXPECT(fs::exists(stopped));
#endif
	TEST_EXPECT(!fs::exists(slot_dir));
	TEST_EXPECT(wait_for([&] { return hosted::slot_reports(rig.db, m.id).empty(); }));

	// One that exits on its own is reported exited with its code until dropped, never restarted.
	hosted::FleetDecision exits;
	exits.desired = { desired(
			"srv-5-g2", 0, { "--marker", (markers / "srv-5-g2").u8string(), "--exit", "3" }) };
	rig.fake.set(m.id, exits);
	TEST_EXPECT(wait_for([&] {
		const auto slots = hosted::slot_reports(rig.db, m.id);
		return slots.size() == 1 && slots[0].container == "srv-5-g2" &&
				slots[0].state == ContainerState::Exited && slots[0].exit_code == int64_t{ 3 };
	}));
	const auto exited = container_of(driver, cloud, "srv-5-g2");
	TEST_EXPECT(
			exited && exited->state == ContainerState::Exited && exited->exit_code == int64_t{ 3 });
	// A container whose files were never sent cannot start (a lost agent state): it is not
	// reported.
	hosted::FleetDecision fileless;
	fileless.desired = { exits.desired[0], desired("srv-6-g1", 1, {}, false) };
	rig.fake.set(m.id, fileless);
	const uint64_t syncs = driver.view(cloud)->syncs;
	TEST_EXPECT(wait_for([&] { return driver.view(cloud)->syncs >= syncs + 3; }));
	TEST_EXPECT(!container_of(driver, cloud, "srv-6-g1"));
	rig.fake.set(m.id, hosted::FleetDecision{});
	TEST_EXPECT(wait_for([&] { return driver.view(cloud)->containers.empty(); }));

	// A running one at the machine's end is stopped with it.
	const fs::path last_started = markers / "srv-5-g3.started";
	hosted::FleetDecision last;
	last.desired = { desired(
			"srv-5-g3", 0, { "--marker", (markers / "srv-5-g3").u8string(), "--run" }) };
	rig.fake.set(m.id, last);
	TEST_EXPECT(wait_for([&] { return fs::exists(last_started); }));
	driver.terminate(cloud);
	TEST_EXPECT(!driver.view(cloud) && driver.describe(cloud).state == hosted::CloudState::Gone);
#ifndef _WIN32
	TEST_EXPECT(fs::exists(markers / "srv-5-g3.stopped"));
#endif
	TEST_EXPECT(driver.list_tagged().empty());
	return 0;
}

// --- the fake opennova-serve ---------------------------------------------------------------

// The variable the test plants in its own environment, which no container may see.
constexpr const char *kPlantedSecret = "ONNET_TEST_PLANTED_SECRET";

std::atomic<bool> g_stop{ false };

void on_term(int) {
	g_stop = true;
}

// `<self> --resource-dir <dir> [--loose-root] [/HOST <file>] --marker <path> --run | --exit
// <code>`: writes <path>.started (its working directory and environment), then runs until SIGTERM
// (writing <path>.stopped) or exits with <code>.
int fake_serve_main(int argc, char **argv) {
	std::signal(SIGTERM, on_term);
	std::string marker;
	int exit_code = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--marker") == 0 && i + 1 < argc)
			marker = argv[++i];
		else if (std::strcmp(argv[i], "--exit") == 0 && i + 1 < argc)
			exit_code = std::atoi(argv[++i]);
	}
	if (marker.empty())
		return 2;
	{
		std::error_code ec;
		std::ofstream out(marker + ".started", std::ios::binary);
		// What its environment holds of the reply's variable and of the planted
		// secret (read off the whole environment, as the supervisor test's fake
		// does: no variable is read by name).
		std::string note;
		std::string secret;
		for (const std::string &entry : opennova::proc::current_environment()) {
			if (entry.rfind("OPENNOVA_SLOT_NOTE=", 0) == 0)
				note = entry.substr(entry.find('=') + 1);
			if (entry.rfind(std::string(kPlantedSecret) + "=", 0) == 0)
				secret = entry.substr(entry.find('=') + 1);
		}
		out << "cwd=" << fs::current_path(ec).u8string() << "\n"
			<< "note=" << note << "\n"
			<< "secret=" << secret << "\n";
	}
	if (exit_code >= 0)
		return exit_code;
	for (int i = 0; i < 60 * 50 && !g_stop; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	std::ofstream(marker + ".stopped", std::ios::binary) << "stopped\n";
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--resource-dir") == 0)
		return fake_serve_main(argc, argv);
	test_db::init(argc, argv);
	if (argc < 2 || std::string(argv[1]) == "--pg") {
		std::fprintf(stderr, "usage: hosted_agent_route_test <this binary's path> --pg <uri>\n");
		return 2;
	}
	g_self = argv[1];
#ifdef _WIN32
	_putenv_s(kPlantedSecret, "hunter2");
#else
	setenv(kPlantedSecret, "hunter2", 1);
#endif
	test_temp::TempDir temp("hosted_agent_route");
	const std::string scratch = temp.root();
	struct Case {
		const char *name;
		int (*fn)(const std::string &);
	};
	const Case cases[] = {
		{ "bootstrap", [](const std::string &) { return test_bootstrap(); } },
		{ "sync", [](const std::string &) { return test_sync(); } },
		{ "networks", [](const std::string &) { return test_networks(); } },
		{ "local_machine_end_to_end", test_local_machine_end_to_end },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(scratch) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted agent routes\n");
	return 0;
}
