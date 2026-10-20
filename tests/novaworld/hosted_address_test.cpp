// The static addresses (apps/novaworld_common/hosted/address_*.h; ADR 0066). The pure
// parts first: the name rules and the hostname; the file zone's text, its records and its
// serial; the Route 53 zone's change batches, its argv and the record a delete finds gone;
// the EC2 address driver's argv for each call (the tags at allocation, no reassociation)
// and what each AWS error code comes to; the docker address driver's lowest free address,
// its association confirmed by inspect, and the addressed machine's create body. Then over
// a migrated database of the test's own (common/test_database.h): an address subscription
// named once, a name taken for good, a comp address within the dedicated machines, the
// entitlement's addresses, a start naming an address (refused while it is not ready, and
// for a second live server); and the jobs over a recording driver and the file zone, each
// handler run by hand: allocate (the tag looked up before any allocation, the record
// written, ready), associate (waiting for the previous holder's machine to end, a
// machine.describe after), release (the machine's end first, the record deleted and in
// sync, the TTL waited, the grant read again, an association ended, then released), a
// grant come back before the release, and fleet.sweep's orphans. A refusal: the row
// failed with its code, an exhausted quota's automatic tries (their backoff, due only
// at retry_at), an admin's retry, a released row's subscription free for a new name, and
// what the cloud holds by owner. The jobs with the add-on off: nothing allocated, a ready
// one's record still moved, a release still run. An admin's release racing the
// allocation: the record it writes is recorded on the lapsed row, and the release waits
// for the allocation's job, then deletes it. The comp addresses nothing covers (an
// admin's cuts of dedicated machines and servers, a paid address beside a comp one, a failed
// one, a banned and a deleted account's), and their release's grant read again.

#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "command_runner.h"
#include "hosted/cloud/address_job_handlers.h"
#include "hosted/cloud/address_zone.h"
#include "hosted/cloud/docker_address_driver.h"
#include "hosted/cloud/docker_machine_driver.h"
#include "hosted/cloud/ec2_address_driver.h"
#include "hosted/model/address_jobs.h"
#include "hosted/model/address_names.h"
#include "hosted/model/address_repository.h"
#include "hosted/model/fleet_jobs.h"
#include "hosted/model/fleet_repository.h"
#include "hosted/model/hosted_repository.h"
#include "jobs/job_queue.h"
#include "jobs/worker_pool.h"

#include <base/io/json.h>
#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "hosted_fleet_rig.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

using namespace hosted_rig;
namespace billing = opennova::novaworld_server::billing;
using hosted::AddressCall;
using hosted::AddressResult;
using hosted::AddressState;
using hosted::CloudAddress;
using opennova::proc::CommandResult;

namespace {

// --- the pure parts -------------------------------------------------------------------

int test_names() {
	for (const char *ok : { "smoke", "abc", "a-b", "my-server-01", "0ab", "abcdefghijklmnopqrstuvwxyz012345" })
		TEST_EXPECT(hosted::address_name_refusal(ok).empty());
	for (const char *bad : { "ab", "abcdefghijklmnopqrstuvwxyz0123456", "Abc", "-ab", "ab-", "a--b", "123", "a_b",
				 "a.b", "www", "nw", "admin", "staging", "ns1", "play", "srv-1", "srv-abc" })
		TEST_EXPECT(!hosted::address_name_refusal(bad).empty());
	TEST_EXPECT(hosted::address_hostname("smoke", "play.opennova.net") == "smoke.play.opennova.net");
	TEST_EXPECT(hosted::address_hostname("smoke", "play.opennova.net.") == "smoke.play.opennova.net");
	return 0;
}

std::string read_text(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream all;
	all << in.rdbuf();
	return all.str();
}

int test_file_zone() {
	test_temp::TempDir dir("opennova_file_zone");
	const std::string path = dir.file("zone/play.zone");
	hosted::FileZone zone("play.localhost", path);
	TEST_EXPECT(zone.zone() == "play.localhost");
	auto change = zone.upsert("smoke", "172.30.77.64");
	TEST_EXPECT(change.ok && change.change_id == "file-1" && zone.in_sync(change.change_id) == true);
	change = zone.upsert("other", "172.30.77.65");
	TEST_EXPECT(change.ok && change.change_id == "file-2");
	std::string text = read_text(path);
	TEST_EXPECT(text.find("$ORIGIN play.localhost.\n") != std::string::npos);
	TEST_EXPECT(text.find("$TTL 300\n") != std::string::npos);
	TEST_EXPECT(text.find("IN\tSOA\tns.play.localhost. hostmaster.play.localhost. ( 2 ") != std::string::npos);
	TEST_EXPECT(text.find("smoke\t300\tIN\tA\t172.30.77.64\n") != std::string::npos);
	auto records = hosted::FileZone::parse(text);
	TEST_EXPECT(records.size() == 2 && records["other"] == "172.30.77.65");
	// An upsert moves a name; a delete of another address leaves it; its own goes.
	TEST_EXPECT(zone.upsert("smoke", "172.30.77.66").ok);
	TEST_EXPECT(zone.remove("smoke", "172.30.77.64").ok);
	TEST_EXPECT(hosted::FileZone::parse(read_text(path))["smoke"] == "172.30.77.66");
	TEST_EXPECT(zone.remove("smoke", "172.30.77.66").ok);
	records = hosted::FileZone::parse(read_text(path));
	TEST_EXPECT(records.size() == 1 && records.count("smoke") == 0);
	TEST_EXPECT(!std::filesystem::exists(path + ".next"));
	return 0;
}

// A runner that records each argv and answers from a queue (an empty queue
// answers `{}`, exit 0).
class StubRunner : public opennova::proc::CommandRunner {
public:
	std::vector<std::vector<std::string>> calls;
	std::deque<CommandResult> replies;

	void answer(const std::string &out) {
		CommandResult r;
		r.outcome = CommandResult::Outcome::Exited;
		r.out = out;
		replies.push_back(r);
	}
	void fail(const std::string &code) {
		CommandResult r;
		r.outcome = CommandResult::Outcome::Exited;
		r.exit_code = 254;
		r.err = "\nAn error occurred (" + code + ") when calling the operation: refused\n";
		replies.push_back(r);
	}
	CommandResult run(const std::vector<std::string> &argv, int) override {
		calls.push_back(argv);
		if (replies.empty()) {
			CommandResult r;
			r.outcome = CommandResult::Outcome::Exited;
			r.out = "{}";
			return r;
		}
		CommandResult r = replies.front();
		replies.pop_front();
		return r;
	}
	// The value after `flag` in call `i`'s argv ("" for none).
	std::string arg(size_t i, const std::string &flag) const {
		const auto &argv = calls.at(i);
		for (size_t k = 0; k + 1 < argv.size(); ++k) {
			if (argv[k] == flag) return argv[k + 1];
		}
		return {};
	}
	bool has(size_t i, const std::string &word) const {
		const auto &argv = calls.at(i);
		return std::find(argv.begin(), argv.end(), word) != argv.end();
	}
};

int test_route53_zone() {
	hosted::Route53ZoneConfig config;
	config.region = "us-east-1";
	config.zone = "play.opennova.net";
	config.zone_id = "Z0123456789ABCDEFGHIJ";
	TEST_EXPECT(hosted::Route53Zone::check_config(config).empty());
	hosted::Route53ZoneConfig bad = config;
	bad.zone_id = "zone";
	TEST_EXPECT(!hosted::Route53Zone::check_config(bad).empty());
	bad = config;
	bad.zone = "play.opennova.net.";
	TEST_EXPECT(!hosted::Route53Zone::check_config(bad).empty());

	opennova::io::JsonValue batch;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(
			hosted::Route53Zone::change_batch("UPSERT", "smoke.play.opennova.net.", "203.0.113.7"), batch, error));
	const auto &one = batch.get("Changes")->array.at(0);
	TEST_EXPECT(one.get_string("Action", "") == "UPSERT");
	const auto *set = one.get("ResourceRecordSet");
	TEST_EXPECT(set->get_string("Name", "") == "smoke.play.opennova.net." && set->get_string("Type", "") == "A" &&
			set->get("TTL")->number == 300 &&
			set->get("ResourceRecords")->array.at(0).get_string("Value", "") == "203.0.113.7");

	StubRunner runner;
	hosted::Route53Zone zone(config, runner);
	runner.answer("{\"ChangeInfo\":{\"Id\":\"/change/C1\",\"Status\":\"PENDING\"}}");
	auto change = zone.upsert("smoke", "203.0.113.7");
	TEST_EXPECT(change.ok && change.change_id == "/change/C1");
	TEST_EXPECT(runner.calls.at(0).at(1) == "route53" && runner.calls.at(0).at(2) == "change-resource-record-sets");
	TEST_EXPECT(runner.arg(0, "--hosted-zone-id") == config.zone_id && runner.arg(0, "--region") == "us-east-1");
	TEST_EXPECT(runner.arg(0, "--change-batch").find("\"smoke.play.opennova.net.\"") != std::string::npos);
	runner.answer("{\"ChangeInfo\":{\"Id\":\"/change/C1\",\"Status\":\"PENDING\"}}");
	TEST_EXPECT(zone.in_sync("/change/C1") == false && runner.arg(1, "--id") == "/change/C1");
	runner.answer("{\"ChangeInfo\":{\"Id\":\"/change/C1\",\"Status\":\"INSYNC\"}}");
	TEST_EXPECT(zone.in_sync("/change/C1") == true);
	// A delete of a record that is not there: done, nothing to wait for.
	CommandResult gone;
	gone.outcome = CommandResult::Outcome::Exited;
	gone.exit_code = 254;
	gone.err = "An error occurred (InvalidChangeBatch) when calling the ChangeResourceRecordSets operation: "
			   "[Tried to delete resource record set [name='smoke.play.opennova.net.', type='A'] but it was not found]";
	runner.replies.push_back(gone);
	change = zone.remove("smoke", "203.0.113.7");
	TEST_EXPECT(change.ok && change.change_id.empty() && zone.in_sync(change.change_id) == true);
	TEST_EXPECT(runner.arg(3, "--change-batch").find("\"DELETE\"") != std::string::npos);
	runner.fail("Throttling");
	TEST_EXPECT(!zone.upsert("smoke", "203.0.113.7").ok);
	return 0;
}

int test_ec2_address_driver() {
	hosted::Ec2AddressConfig config;
	config.region = "us-east-1";
	config.deployment = "prod";
	TEST_EXPECT(hosted::Ec2AddressDriver::check_config(config).empty());
	TEST_EXPECT(hosted::Ec2AddressDriver::error_code("x An error occurred (AddressLimitExceeded) when") ==
			"AddressLimitExceeded");
	TEST_EXPECT(hosted::Ec2AddressDriver::error_code("Could not connect").empty());
	StubRunner runner;
	hosted::Ec2AddressDriver driver(config, runner);

	// The tag first: none.
	runner.answer("{\"Addresses\":[]}");
	AddressResult r = driver.find_tagged(12);
	TEST_EXPECT(r.call == AddressCall::Done && !r.address);
	TEST_EXPECT(runner.calls.at(0).at(2) == "describe-addresses");
	const std::string filters = runner.arg(0, "--filters");
	TEST_EXPECT(filters.find("tag:opennova:hosted") != std::string::npos &&
			filters.find("tag:opennova:deployment") != std::string::npos && filters.find("\"prod\"") != std::string::npos &&
			filters.find("tag:opennova:address-id") != std::string::npos && filters.find("\"12\"") != std::string::npos);
	// Then allocated with its tags in the one call.
	runner.answer("{\"PublicIp\":\"203.0.113.7\",\"AllocationId\":\"eipalloc-01\",\"Domain\":\"vpc\"}");
	r = driver.allocate(12, "smoke", {});
	TEST_EXPECT(r.call == AddressCall::Done && r.address && r.address->allocation_id == "eipalloc-01" &&
			r.address->public_ip == "203.0.113.7");
	TEST_EXPECT(runner.calls.at(1).at(2) == "allocate-address" && runner.arg(1, "--domain") == "vpc");
	const std::string tags = runner.arg(1, "--tag-specifications");
	TEST_EXPECT(tags.find("\"ResourceType\":\"elastic-ip\"") != std::string::npos);
	for (const char *tag : { "{\"Key\":\"opennova:hosted\",\"Value\":\"1\"}", "{\"Key\":\"opennova:deployment\",\"Value\":\"prod\"}",
				 "{\"Key\":\"opennova:address-id\",\"Value\":\"12\"}", "{\"Key\":\"Name\",\"Value\":\"prod-address-smoke\"}" })
		TEST_EXPECT(tags.find(tag) != std::string::npos);
	// The account's quota: refused for good, its code kept.
	runner.fail("AddressLimitExceeded");
	r = driver.allocate(13, "other", {});
	TEST_EXPECT(r.call == AddressCall::Refused && r.code == "AddressLimitExceeded");
	runner.fail("RequestLimitExceeded");
	TEST_EXPECT(driver.allocate(13, "other", {}).call == AddressCall::Retry);

	CloudAddress held;
	held.allocation_id = "eipalloc-01";
	held.public_ip = "203.0.113.7";
	runner.answer("{\"AssociationId\":\"eipassoc-01\"}");
	TEST_EXPECT(driver.associate(held, "i-0aaa").call == AddressCall::Done);
	size_t at = runner.calls.size() - 1;
	TEST_EXPECT(runner.calls.at(at).at(2) == "associate-address" && runner.arg(at, "--allocation-id") == "eipalloc-01" &&
			runner.arg(at, "--instance-id") == "i-0aaa" && runner.has(at, "--no-allow-reassociation") &&
			!runner.has(at, "--allow-reassociation"));
	runner.fail("IncorrectInstanceState");
	TEST_EXPECT(driver.associate(held, "i-0aaa").call == AddressCall::Retry);
	// Already this machine's (an answer lost): done; another's: wait.
	runner.fail("Resource.AlreadyAssociated");
	runner.answer("{\"Addresses\":[{\"AllocationId\":\"eipalloc-01\",\"PublicIp\":\"203.0.113.7\","
				  "\"AssociationId\":\"eipassoc-01\",\"InstanceId\":\"i-0aaa\"}]}");
	TEST_EXPECT(driver.associate(held, "i-0aaa").call == AddressCall::Done);
	runner.fail("Resource.AlreadyAssociated");
	runner.answer("{\"Addresses\":[{\"AllocationId\":\"eipalloc-01\",\"PublicIp\":\"203.0.113.7\","
				  "\"AssociationId\":\"eipassoc-00\",\"InstanceId\":\"i-0old\"}]}");
	TEST_EXPECT(driver.associate(held, "i-0aaa").call == AddressCall::Retry);

	runner.fail("InvalidAllocationID.NotFound");
	r = driver.describe("eipalloc-gone");
	TEST_EXPECT(r.call == AddressCall::Done && !r.address);
	runner.fail("InvalidAssociationID.NotFound");
	TEST_EXPECT(driver.disassociate("eipassoc-gone").call == AddressCall::Done);
	runner.fail("InvalidIPAddress.InUse");
	TEST_EXPECT(driver.release("eipalloc-01").call == AddressCall::Retry);
	runner.fail("InvalidAllocationID.NotFound");
	TEST_EXPECT(driver.release("eipalloc-01").call == AddressCall::Done);
	runner.answer("{}");
	TEST_EXPECT(driver.release("eipalloc-01").call == AddressCall::Done);
	TEST_EXPECT(runner.calls.back().at(2) == "release-address" && runner.arg(runner.calls.size() - 1, "--allocation-id") ==
			"eipalloc-01");

	runner.answer("{\"Addresses\":[{\"AllocationId\":\"eipalloc-01\",\"PublicIp\":\"203.0.113.7\",\"Tags\":["
				  "{\"Key\":\"opennova:address-id\",\"Value\":\"12\"}]}]}");
	const auto listed = driver.list_tagged();
	TEST_EXPECT(listed.size() == 1 && listed.front().address_id == 12);
	runner.fail("Unavailable");
	bool threw = false;
	try {
		driver.list_tagged();
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TEST_EXPECT(threw);
	return 0;
}

int test_docker_address_driver() {
	std::string why;
	TEST_EXPECT(hosted::DockerAddressDriver::parse_range("172.30.77.64/28", why).has_value());
	TEST_EXPECT(!hosted::DockerAddressDriver::parse_range("8.8.8.0/28", why) && !why.empty());
	TEST_EXPECT(!hosted::DockerAddressDriver::parse_range("172.30.0.0/16", why));
	TEST_EXPECT(!hosted::DockerAddressDriver::parse_range("nonsense", why));
	hosted::DockerMachineConfig machines;
	machines.network = "opennova-dev";
	std::string inspected = "172.30.77.64";
	const auto engine = [&inspected](const std::string &method, const std::string &path, const std::string &,
								const std::string &) {
		hosted::EngineReply r;
		r.answered = method == "GET" && path == "/containers/dev-machine-5/json";
		r.code = r.answered ? 200 : 404;
		r.body = "{\"NetworkSettings\":{\"Networks\":{\"opennova-dev\":{\"IPAddress\":\"" + inspected + "\"}}}}";
		return r;
	};
	hosted::DockerAddressDriver driver(*hosted::DockerAddressDriver::parse_range("172.30.77.64/30", why), machines, engine);
	AddressResult r = driver.allocate(1, "smoke", { "172.30.77.64", "172.30.77.66" });
	TEST_EXPECT(r.call == AddressCall::Done && r.address->public_ip == "172.30.77.65" &&
			r.address->allocation_id == "docker-172.30.77.65");
	TEST_EXPECT(hosted::DockerAddressDriver::address_of(r.address->allocation_id) == "172.30.77.65");
	r = driver.allocate(2, "full", { "172.30.77.64", "172.30.77.65", "172.30.77.66", "172.30.77.67" });
	TEST_EXPECT(r.call == AddressCall::Refused && r.code == "AddressLimitExceeded");
	CloudAddress held;
	held.allocation_id = "docker-172.30.77.64";
	held.public_ip = "172.30.77.64";
	TEST_EXPECT(driver.associate(held, "dev-machine-5").call == AddressCall::Done);
	inspected = "";
	TEST_EXPECT(driver.associate(held, "dev-machine-5").call == AddressCall::Retry);
	inspected = "172.30.77.9";
	TEST_EXPECT(driver.associate(held, "dev-machine-5").call == AddressCall::Refused);
	TEST_EXPECT(driver.find_tagged(1).call == AddressCall::Done && !driver.find_tagged(1).address);
	r = driver.describe(held.allocation_id);
	TEST_EXPECT(r.call == AddressCall::Done && r.address && r.address->association_id.empty());
	TEST_EXPECT(driver.release(held.allocation_id).call == AddressCall::Done && driver.list_tagged().empty());

	// An addressed machine starts at its address on the stack's network.
	hosted::MachineLaunch launch;
	launch.machine_id = 5;
	launch.kind = hosted::MachineKind::Own;
	launch.address = "172.30.77.64";
	const auto endpoint = [&machines](const hosted::MachineLaunch &l) {
		opennova::io::JsonValue body;
		std::string error;
		opennova::io::json_parse(hosted::DockerMachineDriver::create_body(machines, l), body, error);
		return *body.get("NetworkingConfig")->get("EndpointsConfig")->get("opennova-dev");
	};
	const opennova::io::JsonValue addressed = endpoint(launch);
	const auto *ipam = addressed.get("IPAMConfig");
	TEST_EXPECT(ipam != nullptr && ipam->get_string("IPv4Address", "") == "172.30.77.64");
	launch.address.clear();
	TEST_EXPECT(endpoint(launch).get("IPAMConfig") == nullptr);
	return 0;
}

// --- the rows -----------------------------------------------------------------------

constexpr int64_t kMonth = 30 * 24 * 60 * 60;

// A granting subscription of `item` for `user`, its period a month from now.
void subscribe_item(Database &db, int64_t user, billing::ItemKind item, int tier, const std::string &id,
		const std::string &status = "active") {
	billing::SubscriptionRow row;
	row.stripe_subscription_id = id;
	row.stripe_customer_id = "cus_" + id;
	row.user_id = user;
	row.status = status;
	row.item = item;
	row.price_id = "price_" + id;
	row.tier = tier;
	row.current_period_end = billing::unix_now() + kMonth;
	billing::upsert_subscription(db, row);
}

// Allocating address `id` allocated at `ip` and its record written: ready.
bool make_ready(Database &db, int64_t id, const std::string &ip) {
	return hosted::record_allocation(db, id, "eipalloc-" + std::to_string(id), ip) &&
			hosted::record_dns_upsert(db, id, hosted::find_address(db, id)->name, "c");
}

int test_naming_and_starts() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const SeededData data = seed_data(db);
	const int64_t now = billing::unix_now();
	const int64_t ana = make_player(db, "addr_ana", 0);
	const int64_t ben = make_player(db, "addr_ben", 0);
	TEST_EXPECT(ana > 0 && ben > 0);
	// Ana: tier 2, a dedicated machine, a static address bought.
	subscribe_item(db, ana, billing::ItemKind::Tier, 2, "sub_ana_t2");
	subscribe_item(db, ana, billing::ItemKind::OwnMachine, 0, "sub_ana_om");
	subscribe_item(db, ana, billing::ItemKind::StaticAddress, 0, "sub_ana_sa");
	billing::Entitlement e = billing::entitlement_of(db, ana, now);
	TEST_EXPECT(e.own_machines == 1 && e.address_grants == 1 && e.static_addresses == 1);
	TEST_EXPECT(hosted::unnamed_address_subscriptions(db, ana, now) == std::vector<std::string>{ "sub_ana_sa" });

	auto r = hosted::name_subscription_address(db, ana, "Smoke!", now);
	TEST_EXPECT(r.outcome == hosted::NameOutcome::InvalidName && !r.why.empty());
	r = hosted::name_subscription_address(db, ana, "smoke", now);
	TEST_EXPECT(r.outcome == hosted::NameOutcome::Created && r.id > 0);
	const int64_t smoke = r.id;
	TEST_EXPECT(hosted::unnamed_address_subscriptions(db, ana, now).empty());
	TEST_EXPECT(hosted::name_subscription_address(db, ana, "second", now).outcome == hosted::NameOutcome::NoAddress);
	auto row = hosted::find_address(db, smoke);
	TEST_EXPECT(row && row->state == AddressState::Allocating && row->source == "subscription" &&
			row->stripe_subscription_id == "sub_ana_sa" && row->owner_user_id == ana);

	// Ben: a comp address needs a dedicated machine for it; the name is taken for good.
	TEST_EXPECT(hosted::add_comp_address(db, ben, "benaddr", now).outcome == hosted::NameOutcome::AddressLimit);
	TEST_EXPECT(billing::set_comp_grants(db, ben, { 1, 1 }));
	TEST_EXPECT(hosted::add_comp_address(db, ben, "smoke", now).outcome == hosted::NameOutcome::NameTaken);
	r = hosted::add_comp_address(db, ben, "benaddr", now);
	TEST_EXPECT(r.outcome == hosted::NameOutcome::Created);
	const int64_t ben_addr = r.id;
	e = billing::entitlement_of(db, ben, now);
	TEST_EXPECT(e.comp_addresses == 1 && e.address_grants == 1 && e.static_addresses == 1);
	TEST_EXPECT(hosted::add_comp_address(db, ben, "benmore", now).outcome == hosted::NameOutcome::AddressLimit);
	TEST_EXPECT(hosted::add_comp_address(db, 999999, "nobody", now).outcome == hosted::NameOutcome::NoSuchAccount);
	// A rename frees the old name; the new one must be free.
	TEST_EXPECT(hosted::rename_address(db, ben_addr, "smoke").outcome == hosted::NameOutcome::NameTaken);
	TEST_EXPECT(hosted::rename_address(db, ben_addr, "benaddr2").outcome == hosted::NameOutcome::Created);
	TEST_EXPECT(hosted::rename_address(db, ben_addr, "benaddr").outcome == hosted::NameOutcome::Created);

	// A start naming it: refused while it is not ready, and on a shared machine.
	hosted::PlacementPolicy policy;
	const auto offered = hosted::offered_data_sets(db);
	auto start = [&](int64_t owner, bool own, std::optional<int64_t> address) {
		return hosted::request_server(db, owner, valid_settings(data.jo), offered, 24, now, own, &policy, address);
	};
	TEST_EXPECT(start(ana, true, smoke).outcome == hosted::RequestOutcome::AddressUnavailable);
	TEST_EXPECT(make_ready(db, smoke, "203.0.113.7"));
	TEST_EXPECT(hosted::find_address(db, smoke)->state == AddressState::Ready);
	TEST_EXPECT(start(ana, false, smoke).outcome == hosted::RequestOutcome::AddressUnavailable);
	TEST_EXPECT(start(ben, true, smoke).outcome == hosted::RequestOutcome::AddressUnavailable);
	const auto made = start(ana, true, smoke);
	TEST_EXPECT(made.outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::find_server(db, made.id)->address_id == smoke && hosted::server_of_address(db, smoke) == made.id);
	// One live server per address: her second start on a dedicated machine may not take it.
	subscribe_item(db, ana, billing::ItemKind::OwnMachine, 0, "sub_ana_om2");
	TEST_EXPECT(start(ana, true, smoke).outcome == hosted::RequestOutcome::AddressUnavailable);
	TEST_EXPECT(start(ana, true, std::nullopt).outcome == hosted::RequestOutcome::Created);
	// Ended, it is free again; a lapse lets a server go of it.
	TEST_EXPECT(hosted::request_stop(db, made.id, "test", hosted::StartCause::OwnerStop) &&
			hosted::transition(db, made.id, hosted::State::Stopping, hosted::State::Stopped, "test"));
	TEST_EXPECT(!hosted::server_of_address(db, smoke));
	const auto again = start(ana, true, smoke);
	TEST_EXPECT(again.outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::clear_server_address(db, again.id) && !hosted::find_server(db, again.id)->address_id);
	TEST_EXPECT(hosted::count_held_addresses(db) == 2);
	TEST_EXPECT(hosted::addresses_of(db, ana).size() == 1 && hosted::list_addresses(db).size() == 2);
	return 0;
}

// --- the jobs -----------------------------------------------------------------------

// A driver that records its calls and answers what the test sets.
class RecordingDriver : public hosted::AddressDriver {
public:
	std::vector<std::string> calls;
	std::optional<CloudAddress> tagged;     // find_tagged's
	std::optional<CloudAddress> described;  // describe's
	AddressCall associate_call = AddressCall::Done;
	std::string refuse; // allocate's refusal code ("" for none)
	std::vector<CloudAddress> listing;

	const char *name() const override { return "recording"; }
	AddressResult find_tagged(int64_t id) override {
		calls.push_back("find_tagged " + std::to_string(id));
		AddressResult r;
		r.call = AddressCall::Done;
		r.address = tagged;
		return r;
	}
	AddressResult allocate(int64_t id, const std::string &name, const std::vector<std::string> &) override {
		calls.push_back("allocate " + std::to_string(id) + " " + name);
		AddressResult r;
		if (!refuse.empty()) {
			r.call = AddressCall::Refused;
			r.code = refuse;
			r.error = "An error occurred (" + refuse + ") when calling the AllocateAddress operation";
			return r;
		}
		r.call = AddressCall::Done;
		CloudAddress a;
		a.allocation_id = "eipalloc-" + std::to_string(id);
		a.public_ip = "203.0.113." + std::to_string(id);
		a.address_id = id;
		r.address = a;
		return r;
	}
	AddressResult associate(const CloudAddress &a, const std::string &cloud_id) override {
		calls.push_back("associate " + a.allocation_id + " " + cloud_id);
		AddressResult r;
		r.call = associate_call;
		return r;
	}
	AddressResult describe(const std::string &allocation_id) override {
		calls.push_back("describe " + allocation_id);
		AddressResult r;
		r.call = AddressCall::Done;
		r.address = described;
		return r;
	}
	AddressResult disassociate(const std::string &association_id) override {
		calls.push_back("disassociate " + association_id);
		described->association_id.clear();
		AddressResult r;
		r.call = AddressCall::Done;
		return r;
	}
	AddressResult release(const std::string &allocation_id) override {
		calls.push_back("release " + allocation_id);
		AddressResult r;
		r.call = AddressCall::Done;
		return r;
	}
	std::vector<CloudAddress> list_tagged() override { return listing; }
};

// One run of `kind`'s handler on `payload`, by hand.
nws::jobs::JobResult run_job(ConnectionPool &pool, const nws::jobs::JobRegistry &registry, const char *kind,
		const std::map<std::string, int64_t> &payload) {
	nws::jobs::ClaimedJob job;
	job.kind = kind;
	job.payload = opennova::io::JsonValue::make_object();
	for (const auto &[key, value] : payload) job.payload.set(key, opennova::io::json_number(static_cast<double>(value)));
	nws::jobs::JobContext ctx{ pool, job };
	return registry.find(kind)->handler(ctx);
}

// The queued address.allocate of address `id`, whose handler the test ran by
// hand, done as the worker that ran it leaves it (a release waits while it is
// pending).
void finish_allocate(Database &db, int64_t id) {
	db.exec("UPDATE jobs SET state = 'done', finished_at = now() WHERE dedupe_key = $1 AND "
			"state IN ('queued', 'running');",
			{ BindValue(std::string(hosted::kJobAddressAllocate) + ":" + std::to_string(id)) });
}

// A live dedicated machine carrying `address`, launched as `cloud_id`.
int64_t carrying_machine(Database &db, int64_t address, const std::string &cloud_id) {
	hosted::NewMachine m;
	m.provider = "docker";
	m.kind = hosted::MachineKind::Own;
	m.slots = 1;
	m.address_id = address;
	const auto made = hosted::create_machine(db, m);
	hosted::LaunchedMachine launched;
	launched.cloud_id = cloud_id;
	launched.private_address = "10.0.0.5";
	hosted::mark_launched(db, made.id, launched);
	return made.id;
}

int test_jobs() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	test_temp::TempDir dir("opennova_address_jobs");
	hosted::FileZone zone("play.localhost", dir.file("play.zone"));
	RecordingDriver driver;
	nws::jobs::JobRegistry registry;
	hosted::register_address_jobs(registry, driver, zone, true);
	using Kind = nws::jobs::JobResult::Kind;
	const int64_t now = billing::unix_now();
	const int64_t ana = make_player(db, "jobs_ana", 0);
	subscribe_item(db, ana, billing::ItemKind::Tier, 1, "sub_j_t1");
	subscribe_item(db, ana, billing::ItemKind::OwnMachine, 0, "sub_j_om");
	subscribe_item(db, ana, billing::ItemKind::StaticAddress, 0, "sub_j_sa");
	const int64_t id = hosted::name_subscription_address(db, ana, "smoke", now).id;
	TEST_EXPECT(id > 0);

	// allocate: the tag first, then the allocation, the record, ready.
	auto result = run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Done);
	TEST_EXPECT(driver.calls == std::vector<std::string>({ "find_tagged " + std::to_string(id), "allocate " +
			std::to_string(id) + " smoke" }));
	auto a = hosted::find_address(db, id);
	const std::string ip = "203.0.113." + std::to_string(id);
	TEST_EXPECT(a->state == AddressState::Ready && a->public_ip == ip && a->dns_name == "smoke" && !a->ready_at.empty());
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone")))["smoke"] == ip);
	// Again (a rename's): no second allocation; the old name goes, the new is written.
	driver.calls.clear();
	TEST_EXPECT(hosted::rename_address(db, id, "renamed").outcome == hosted::NameOutcome::Created);
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	TEST_EXPECT(driver.calls.empty());
	auto records = hosted::FileZone::parse(read_text(dir.file("play.zone")));
	TEST_EXPECT(records.count("smoke") == 0 && records["renamed"] == ip && hosted::find_address(db, id)->dns_name == "renamed");
	// One a cut-short run allocated is found by its tag, never allocated twice.
	// Her one dedicated machine has its address: a comp one waits for a second.
	TEST_EXPECT(hosted::add_comp_address(db, ana, "tagged", now).outcome == hosted::NameOutcome::AddressLimit);
	TEST_EXPECT(billing::set_comp_grants(db, ana, { 0, 1 }));
	const int64_t tagged_id = hosted::add_comp_address(db, ana, "tagged", now).id;
	TEST_EXPECT(tagged_id > 0);
	CloudAddress found;
	found.allocation_id = "eipalloc-found";
	found.public_ip = "198.51.100.9";
	driver.tagged = found;
	driver.calls.clear();
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", tagged_id } }).kind == Kind::Done);
	TEST_EXPECT(driver.calls == std::vector<std::string>({ "find_tagged " + std::to_string(tagged_id) }));
	TEST_EXPECT(hosted::find_address(db, tagged_id)->allocation_id == "eipalloc-found");
	driver.tagged.reset();

	// associate: only once the machine that held it before has ended; then a describe.
	const int64_t old_machine = carrying_machine(db, id, "dev-machine-old");
	const int64_t new_machine = carrying_machine(db, id, "dev-machine-new");
	driver.calls.clear();
	result = run_job(pool, registry, hosted::kJobAddressAssociate, { { "address_id", id }, { "machine_id", new_machine } });
	TEST_EXPECT(result.kind == Kind::Retry && driver.calls.empty());
	TEST_EXPECT(hosted::transition_machine(db, old_machine, hosted::FleetMachineState::Booting,
			hosted::FleetMachineState::Ended, "test"));
	result = run_job(pool, registry, hosted::kJobAddressAssociate, { { "address_id", id }, { "machine_id", new_machine } });
	TEST_EXPECT(result.kind == Kind::Done);
	TEST_EXPECT(driver.calls == std::vector<std::string>({ "associate eipalloc-" + std::to_string(id) + " dev-machine-new" }));
	TEST_EXPECT(nws::jobs::job_pending(db, std::string(hosted::kJobMachineDescribe) + ":" + std::to_string(new_machine)));
	driver.associate_call = AddressCall::Retry;
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAssociate, { { "address_id", id }, { "machine_id", new_machine } })
					.kind == Kind::Retry);
	driver.associate_call = AddressCall::Done;
	TEST_EXPECT(hosted::address_associate_pending(db, 999) == false);

	// release: lapsed, then releasing (the fleet's pass); the machine's end first.
	TEST_EXPECT(hosted::lapse_address(db, id, hosted::kLapseGrant, "its subscription ended"));
	TEST_EXPECT(!hosted::lapse_address(db, id, hosted::kLapseGrant, "again"));
	TEST_EXPECT(hosted::begin_release(db, id));
	driver.calls.clear();
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && driver.calls.empty());
	TEST_EXPECT(hosted::transition_machine(db, new_machine, hosted::FleetMachineState::Booting,
			hosted::FleetMachineState::Ended, "test"));
	// The record deleted, then in sync, then its TTL to drain.
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && hosted::find_address(db, id)->dns_state == "pending");
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone"))).count("renamed") == 0);
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	a = hosted::find_address(db, id);
	TEST_EXPECT(result.kind == Kind::Retry && result.delay_ms >= (hosted::kAddressTtlS + hosted::kReleaseMarginS - 5) * 1000 &&
			a->dns_name.empty() && a->dns_state == "none" &&
			a->release_after >= now + hosted::kAddressTtlS + hosted::kReleaseMarginS);
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && driver.calls.empty());
	db.exec("UPDATE hosted_addresses SET release_after = now() - interval '1 second' WHERE id = $1;", { BindValue(id) });
	// The grant read again: still granting, it is kept (ready, its record written again).
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Done && hosted::find_address(db, id)->state == AddressState::Ready);
	TEST_EXPECT(nws::jobs::job_pending(db, std::string(hosted::kJobAddressAllocate) + ":" + std::to_string(id)));
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	finish_allocate(db, id);
	TEST_EXPECT(!hosted::address_allocate_pending(db, id));
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone")))["renamed"] == ip);
	// Canceled now: the release goes through, an association the cloud still shows ended first.
	subscribe_item(db, ana, billing::ItemKind::StaticAddress, 0, "sub_j_sa", "canceled");
	TEST_EXPECT(hosted::lapse_address(db, id, hosted::kLapseGrant, "its subscription ended") &&
			hosted::begin_release(db, id));
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Retry);
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Retry);
	db.exec("UPDATE hosted_addresses SET release_after = now() - interval '1 second' WHERE id = $1;", { BindValue(id) });
	CloudAddress shown;
	shown.allocation_id = "eipalloc-" + std::to_string(id);
	shown.public_ip = ip;
	shown.association_id = "eipassoc-stale";
	driver.described = shown;
	driver.calls.clear();
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Retry);
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Done);
	const std::string alloc = "eipalloc-" + std::to_string(id);
	TEST_EXPECT(driver.calls == std::vector<std::string>({ "describe " + alloc, "disassociate eipassoc-stale",
			"describe " + alloc, "release " + alloc }));
	a = hosted::find_address(db, id);
	TEST_EXPECT(a->state == AddressState::Released && !a->released_at.empty());
	// Released, its name stays taken.
	TEST_EXPECT(hosted::add_comp_address(db, ana, "renamed", now).outcome == hosted::NameOutcome::NameTaken);

	// fleet.sweep's orphans: an address no held row names (the released one's among them).
	CloudAddress mine;
	mine.allocation_id = "eipalloc-found";
	mine.public_ip = "198.51.100.9";
	mine.address_id = tagged_id;
	CloudAddress stray;
	stray.allocation_id = "eipalloc-stray";
	stray.public_ip = "198.51.100.10";
	CloudAddress released;
	released.allocation_id = alloc;
	released.public_ip = ip;
	released.address_id = id;
	driver.listing = { mine, stray, released };
	TEST_EXPECT(hosted::sweep_addresses(pool, driver));
	const auto orphans = hosted::address_orphans(db);
	TEST_EXPECT(orphans.size() == 2 && orphans[0].allocation_id == alloc && orphans[1].allocation_id == "eipalloc-stray");
	driver.listing = { mine };
	TEST_EXPECT(hosted::sweep_addresses(pool, driver) && hosted::address_orphans(db).empty());
	return 0;
}

// The jobs with the add-on off: nothing allocated (no cloud call), the
// allocation waiting for it; a ready address's record still moved by a rename,
// and a release still run.
int test_jobs_add_on_off() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	test_temp::TempDir dir("opennova_address_off");
	hosted::FileZone zone("play.localhost", dir.file("play.zone"));
	RecordingDriver driver;
	nws::jobs::JobRegistry on;
	hosted::register_address_jobs(on, driver, zone, true);
	nws::jobs::JobRegistry off;
	hosted::register_address_jobs(off, driver, zone, false);
	using Kind = nws::jobs::JobResult::Kind;
	const int64_t ben = make_player(db, "off_ben", 0);
	TEST_EXPECT(billing::set_comp_grants(db, ben, { 1, 1 }));
	const int64_t id = hosted::add_comp_address(db, ben, "kept", billing::unix_now()).id;
	TEST_EXPECT(id > 0);

	const auto result = run_job(pool, off, hosted::kJobAddressAllocate, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && result.delay_ms >= 60 * 1000 && driver.calls.empty() &&
			hosted::find_address(db, id)->state == AddressState::Allocating);
	TEST_EXPECT(run_job(pool, on, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	const std::string ip = hosted::find_address(db, id)->public_ip;
	TEST_EXPECT(hosted::find_address(db, id)->state == AddressState::Ready && !ip.empty());
	TEST_EXPECT(hosted::rename_address(db, id, "moved").outcome == hosted::NameOutcome::Created);
	driver.calls.clear();
	TEST_EXPECT(run_job(pool, off, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	auto records = hosted::FileZone::parse(read_text(dir.file("play.zone")));
	TEST_EXPECT(driver.calls.empty() && records.count("kept") == 0 && records["moved"] == ip &&
			hosted::find_address(db, id)->dns_name == "moved");
	TEST_EXPECT(hosted::lapse_address(db, id, hosted::kLapseAdmin, "released by an admin") &&
			hosted::begin_release(db, id));
	TEST_EXPECT(run_job(pool, off, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Retry);
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone"))).count("moved") == 0 &&
			hosted::find_address(db, id)->dns_state == "pending");
	return 0;
}

// The seconds from now to address `id`'s retry_at (-1 for none).
int64_t retry_in(Database &db, int64_t id) {
	const auto rows = db.query("SELECT COALESCE(CAST(extract(epoch FROM retry_at - now()) AS bigint), -1) "
							   "FROM hosted_addresses WHERE id = $1;",
			{ BindValue(id) });
	return rows.empty() ? -1 : rows.front().as_int(0).value_or(-1);
}

int test_refusals_and_retries() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	test_temp::TempDir dir("opennova_address_retries");
	hosted::FileZone zone("play.localhost", dir.file("play.zone"));
	RecordingDriver driver;
	nws::jobs::JobRegistry registry;
	hosted::register_address_jobs(registry, driver, zone, true);
	using Kind = nws::jobs::JobResult::Kind;
	const int64_t now = billing::unix_now();

	// The pure parts: what passes, and the backoff.
	TEST_EXPECT(hosted::address_refusal_passes("AddressLimitExceeded"));
	TEST_EXPECT(!hosted::address_refusal_passes("InvalidParameterValue") && !hosted::address_refusal_passes(""));
	TEST_EXPECT(hosted::address_retry_delay_s(0) == hosted::kAddressRetryBaseS &&
			hosted::address_retry_delay_s(1) == 2 * hosted::kAddressRetryBaseS &&
			hosted::address_retry_delay_s(20) == hosted::kAddressRetryMaxS);

	const int64_t ana = make_player(db, "retry_ana", 0);
	subscribe_item(db, ana, billing::ItemKind::Tier, 1, "sub_r_t1");
	subscribe_item(db, ana, billing::ItemKind::OwnMachine, 0, "sub_r_om");
	subscribe_item(db, ana, billing::ItemKind::StaticAddress, 0, "sub_r_sa");
	const int64_t id = hosted::name_subscription_address(db, ana, "quota", now).id;
	TEST_EXPECT(id > 0);

	// An exhausted quota: failed with its code, tried again automatically from
	// its retry_at on, never before.
	driver.refuse = "AddressLimitExceeded";
	auto result = run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Fail);
	auto a = hosted::find_address(db, id);
	TEST_EXPECT(a->state == AddressState::Failed && a->reason == "AddressLimitExceeded" && a->retries == 1 &&
			!a->retry_at.empty());
	TEST_EXPECT(retry_in(db, id) > hosted::kAddressRetryBaseS - 30 && retry_in(db, id) <= hosted::kAddressRetryBaseS);
	TEST_EXPECT(!hosted::retry_due_address(db, id));
	db.exec("UPDATE hosted_addresses SET retry_at = now() - interval '1 second' WHERE id = $1;", { BindValue(id) });
	TEST_EXPECT(hosted::retry_due_address(db, id) && !hosted::retry_due_address(db, id));
	a = hosted::find_address(db, id);
	TEST_EXPECT(a->state == AddressState::Allocating && a->reason.empty() && a->retry_at.empty() && a->retries == 1);
	// Refused again: the wait doubles.
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Fail);
	a = hosted::find_address(db, id);
	TEST_EXPECT(a->retries == 2 && retry_in(db, id) > 2 * hosted::kAddressRetryBaseS - 30);
	// An admin's retry counts from 0 again; only a failed one is retried.
	TEST_EXPECT(hosted::retry_address(db, id) && !hosted::retry_address(db, id));
	a = hosted::find_address(db, id);
	TEST_EXPECT(a->state == AddressState::Allocating && a->retries == 0 && a->retry_at.empty());
	// A refusal that does not pass waits for an admin.
	driver.refuse = "InvalidParameterValue";
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Fail);
	a = hosted::find_address(db, id);
	TEST_EXPECT(a->state == AddressState::Failed && a->reason == "InvalidParameterValue" && a->retry_at.empty() &&
			a->retries == 0 && retry_in(db, id) == -1);
	db.exec("UPDATE hosted_addresses SET updated_at = now() - interval '1 day' WHERE id = $1;", { BindValue(id) });
	TEST_EXPECT(!hosted::retry_due_address(db, id));

	// Released for good (an admin's), its subscription, granting still, names another.
	TEST_EXPECT(hosted::unnamed_address_subscriptions(db, ana, now).empty());
	TEST_EXPECT(hosted::lapse_address(db, id, hosted::kLapseAdmin, "released by an admin") &&
			hosted::begin_release(db, id));
	TEST_EXPECT(hosted::count_held_addresses(db) == 1);
	auto held = hosted::held_addresses_by_owner(db);
	TEST_EXPECT(held.size() == 1 && held[ana].granted == 0 && held[ana].leaving == 1);
	TEST_EXPECT(hosted::unnamed_address_subscriptions(db, ana, now).empty());
	driver.calls.clear();
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Done);
	TEST_EXPECT(hosted::find_address(db, id)->state == AddressState::Released &&
			driver.calls == std::vector<std::string>({ "find_tagged " + std::to_string(id) }));
	TEST_EXPECT(hosted::unnamed_address_subscriptions(db, ana, now) == std::vector<std::string>{ "sub_r_sa" });
	driver.refuse.clear();
	const int64_t second = hosted::name_subscription_address(db, ana, "quota-two", now).id;
	TEST_EXPECT(second > 0 && hosted::find_address(db, second)->stripe_subscription_id == "sub_r_sa");
	TEST_EXPECT(hosted::name_subscription_address(db, ana, "quota-three", now).outcome ==
			hosted::NameOutcome::NoAddress);
	held = hosted::held_addresses_by_owner(db);
	TEST_EXPECT(held.size() == 1 && held[ana].granted == 1 && held[ana].leaving == 0);
	return 0;
}

// A record writer that runs `before` ahead of each upsert (an admin's release
// landing mid-allocation).
class RacingZone : public hosted::ZoneDriver {
public:
	explicit RacingZone(hosted::FileZone &inner) : inner_(inner) {}
	std::function<void()> before;
	const std::string &zone() const override { return inner_.zone(); }
	hosted::ZoneChange upsert(const std::string &name, const std::string &ip) override {
		if (before) before();
		return inner_.upsert(name, ip);
	}
	hosted::ZoneChange remove(const std::string &name, const std::string &ip) override {
		return inner_.remove(name, ip);
	}
	std::optional<bool> in_sync(const std::string &change_id) override { return inner_.in_sync(change_id); }

private:
	hosted::FileZone &inner_;
};

int test_release_races_allocate() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	test_temp::TempDir dir("opennova_address_race");
	hosted::FileZone file("play.localhost", dir.file("play.zone"));
	RacingZone zone(file);
	RecordingDriver driver;
	nws::jobs::JobRegistry registry;
	hosted::register_address_jobs(registry, driver, zone, true);
	using Kind = nws::jobs::JobResult::Kind;
	const int64_t now = billing::unix_now();
	const int64_t ben = make_player(db, "race_ben", 0);
	TEST_EXPECT(billing::set_comp_grants(db, ben, { 1, 1 }));
	const int64_t id = hosted::add_comp_address(db, ben, "racer", now).id;
	TEST_EXPECT(id > 0);

	// The admin's release lands between the allocation and its record: the
	// record written is recorded on the lapsed row all the same.
	zone.before = [&db, id] { hosted::lapse_address(db, id, hosted::kLapseAdmin, "released by an admin"); };
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	zone.before = nullptr;
	auto a = hosted::find_address(db, id);
	const std::string ip = "203.0.113." + std::to_string(id);
	TEST_EXPECT(a->state == AddressState::Lapsed && a->public_ip == ip && a->dns_name == "racer" &&
			a->dns_state == "insync");
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone")))["racer"] == ip);

	// Its release waits while an allocation of it is queued, then deletes the record.
	TEST_EXPECT(hosted::begin_release(db, id));
	TEST_EXPECT(hosted::enqueue_address_allocate(db, id) > 0 && hosted::address_allocate_pending(db, id));
	auto result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && hosted::find_address(db, id)->dns_state == "insync");
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone"))).count("racer") == 1);
	// That allocation finds the row releasing and writes nothing.
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressAllocate, { { "address_id", id } }).kind == Kind::Done);
	finish_allocate(db, id);
	TEST_EXPECT(!hosted::address_allocate_pending(db, id));
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	TEST_EXPECT(result.kind == Kind::Retry && hosted::find_address(db, id)->dns_state == "pending");
	TEST_EXPECT(hosted::FileZone::parse(read_text(dir.file("play.zone"))).count("racer") == 0);
	result = run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } });
	a = hosted::find_address(db, id);
	TEST_EXPECT(result.kind == Kind::Retry && a->dns_name.empty() && a->release_after > 0);
	db.exec("UPDATE hosted_addresses SET release_after = now() - interval '1 second' WHERE id = $1;", { BindValue(id) });
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", id } }).kind == Kind::Done);
	TEST_EXPECT(hosted::find_address(db, id)->state == AddressState::Released);
	return 0;
}

// The comp addresses nothing covers (uncovered_comp_addresses), by id.
std::map<int64_t, hosted::UncoveredAddress> uncovered_at(Database &db, int64_t now) {
	std::map<int64_t, hosted::UncoveredAddress> out;
	for (hosted::UncoveredAddress &u : hosted::uncovered_comp_addresses(db, now)) out.emplace(u.id, std::move(u));
	return out;
}

// The comp addresses nothing covers, at an admin's cuts: its dedicated machines cut to 0, an
// account's one comp address; one of two cut, the newer of two; a paid address and a
// comp one past the dedicated machines, the comp one alone (judged with the cache); a failed
// one past the machines left; a banned account's and a deleted one's, every one. Then
// the release's grant read again: one covered once more kept, a deleted account's
// released.
int test_comp_uncovered() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const int64_t now = billing::unix_now();
	const std::string machines_why = "its account's dedicated machines no longer cover it";

	// Cal: one shared server, one dedicated machine, one comp address. Its
	// dedicated machines cut to 0, the address is uncovered, judged without the
	// cache (Cal has no subscription); its shared servers cut to 0, its
	// dedicated machine still covers it (it needs no shared server).
	const int64_t cal = make_player(db, "unc_cal", 0);
	TEST_EXPECT(billing::set_comp_grants(db, cal, { 1, 1 }));
	const int64_t cal_addr = hosted::add_comp_address(db, cal, "cal-place", now).id;
	TEST_EXPECT(cal_addr > 0 && uncovered_at(db, now).empty());
	TEST_EXPECT(billing::set_comp_grants(db, cal, { 1, 0 }));
	auto u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(cal_addr) == 1 && !u[cal_addr].cached && u[cal_addr].why == machines_why);
	TEST_EXPECT(billing::set_comp_grants(db, cal, { 0, 1 }) && uncovered_at(db, now).empty());
	TEST_EXPECT(billing::set_comp_grants(db, cal, { 1, 1 }) && uncovered_at(db, now).empty());

	// Dot: two comp addresses, one of her two dedicated machines cut: the newer goes.
	const int64_t dot = make_player(db, "unc_dot", 0);
	TEST_EXPECT(billing::set_comp_grants(db, dot, { 2, 2 }));
	const int64_t dot_old = hosted::add_comp_address(db, dot, "dot-old", now).id;
	const int64_t dot_new = hosted::add_comp_address(db, dot, "dot-new", now).id;
	TEST_EXPECT(dot_old > 0 && dot_new > dot_old && uncovered_at(db, now).empty());
	TEST_EXPECT(billing::set_comp_grants(db, dot, { 2, 1 }));
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(dot_new) == 1 && u[dot_new].why == machines_why);
	// A failed one counts for none, but past the dedicated machines left it goes too
	// (an allocation would only make it count): the older keeps the one.
	TEST_EXPECT(billing::set_comp_grants(db, dot, { 2, 2 }));
	TEST_EXPECT(hosted::fail_address(db, dot_new, "AddressLimitExceeded", hosted::kAddressRetryBaseS));
	TEST_EXPECT(uncovered_at(db, now).empty());
	TEST_EXPECT(billing::set_comp_grants(db, dot, { 2, 1 }));
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(dot_new) == 1);
	TEST_EXPECT(hosted::retry_address(db, dot_new));
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(dot_new) == 1);
	TEST_EXPECT(billing::set_comp_grants(db, dot, { 2, 2 }) && uncovered_at(db, now).empty());

	// Eve on tier 1 with a dedicated machine and a paid address, and an admin's dedicated
	// machine and comp address beside them. Cut to her paid one, the comp address
	// goes, judged with the cache (she has subscriptions); the paid one never does,
	// even with her tier gone.
	const int64_t eve = make_player(db, "unc_eve", 0);
	subscribe_item(db, eve, billing::ItemKind::Tier, 1, "sub_unc_eve_t1");
	subscribe_item(db, eve, billing::ItemKind::OwnMachine, 0, "sub_unc_eve_om");
	TEST_EXPECT(billing::set_comp_grants(db, eve, { 0, 1 }));
	const int64_t eve_comp = hosted::add_comp_address(db, eve, "eve-comp", now).id;
	subscribe_item(db, eve, billing::ItemKind::StaticAddress, 0, "sub_unc_eve_sa");
	const int64_t eve_paid = hosted::name_subscription_address(db, eve, "eve-paid", now).id;
	TEST_EXPECT(eve_comp > 0 && eve_paid > 0 && uncovered_at(db, now).empty());
	TEST_EXPECT(billing::set_comp_grants(db, eve, { 0, 0 }));
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(eve_comp) == 1 && u[eve_comp].cached);
	subscribe_item(db, eve, billing::ItemKind::Tier, 1, "sub_unc_eve_t1", "canceled");
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 1 && u.count(eve_comp) == 1);

	// Gil's only subscription ended for good: the cache can change nothing for
	// her, so her uncovered comp address lapses at once, as a comp-only one does.
	const int64_t gil = make_player(db, "unc_gil", 0);
	subscribe_item(db, gil, billing::ItemKind::Tier, 2, "sub_unc_gil_t2", "canceled");
	TEST_EXPECT(billing::set_comp_grants(db, gil, { 0, 1 }));
	const int64_t gil_comp = hosted::add_comp_address(db, gil, "gil-comp", now).id;
	TEST_EXPECT(gil_comp > 0 && billing::set_comp_grants(db, gil, { 0, 0 }));
	u = uncovered_at(db, now);
	TEST_EXPECT(u.count(gil_comp) == 1 && !u[gil_comp].cached && u[gil_comp].why == machines_why);
	TEST_EXPECT(billing::set_comp_grants(db, gil, { 0, 1 }) && uncovered_at(db, now).count(gil_comp) == 0);

	// A banned account's comp addresses go, every one; a deleted one's too.
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(nws::update_user(db, dot, ban).ok);
	TEST_EXPECT(nws::delete_user(db, cal).ok);
	TEST_EXPECT(!hosted::find_address(db, cal_addr)->owner_user_id);
	u = uncovered_at(db, now);
	TEST_EXPECT(u.size() == 4 && u.count(dot_old) == 1 && u.count(dot_new) == 1 && u.count(cal_addr) == 1);
	TEST_EXPECT(u[dot_old].why == "its account is banned" && !u[dot_old].cached);
	TEST_EXPECT(u[cal_addr].why == "its account was deleted" && !u[cal_addr].cached);

	// The release reads the grant again (comp_address_covered): a comp address
	// its account covers once more is kept (ready, allocated again); a deleted
	// account's is released.
	test_temp::TempDir dir("opennova_address_uncovered");
	hosted::FileZone zone("play.localhost", dir.file("play.zone"));
	RecordingDriver driver;
	nws::jobs::JobRegistry registry;
	hosted::register_address_jobs(registry, driver, zone, true);
	using Kind = nws::jobs::JobResult::Kind;
	TEST_EXPECT(hosted::lapse_address(db, eve_comp, hosted::kLapseGrant, machines_why) &&
			hosted::begin_release(db, eve_comp));
	TEST_EXPECT(!hosted::comp_address_covered(db, *hosted::find_address(db, eve_comp), now));
	TEST_EXPECT(!hosted::comp_address_covered(db, *hosted::find_address(db, eve_paid), now));
	TEST_EXPECT(billing::set_comp_grants(db, eve, { 1, 2 }));
	TEST_EXPECT(hosted::comp_address_covered(db, *hosted::find_address(db, eve_comp), now));
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", eve_comp } }).kind == Kind::Done);
	TEST_EXPECT(hosted::find_address(db, eve_comp)->state == AddressState::Ready &&
			nws::jobs::job_pending(db, std::string(hosted::kJobAddressAllocate) + ":" + std::to_string(eve_comp)));
	TEST_EXPECT(hosted::lapse_address(db, cal_addr, hosted::kLapseGrant, "its account was deleted") &&
			hosted::begin_release(db, cal_addr));
	TEST_EXPECT(!hosted::comp_address_covered(db, *hosted::find_address(db, cal_addr), now));
	TEST_EXPECT(run_job(pool, registry, hosted::kJobAddressRelease, { { "address_id", cal_addr } }).kind == Kind::Done);
	TEST_EXPECT(hosted::find_address(db, cal_addr)->state == AddressState::Released);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case pure[] = {
		{ "names", test_names },
		{ "file_zone", test_file_zone },
		{ "route53_zone", test_route53_zone },
		{ "ec2_address_driver", test_ec2_address_driver },
		{ "docker_address_driver", test_docker_address_driver },
	};
	for (const Case &c : pure) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	test_db::init(argc, argv);
	const Case database[] = {
		{ "naming_and_starts", test_naming_and_starts },
		{ "jobs", test_jobs },
		{ "jobs_add_on_off", test_jobs_add_on_off },
		{ "refusals_and_retries", test_refusals_and_retries },
		{ "release_races_allocate", test_release_races_allocate },
		{ "comp_uncovered", test_comp_uncovered },
	};
	for (const Case &c : database) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the static addresses\n");
	return 0;
}
