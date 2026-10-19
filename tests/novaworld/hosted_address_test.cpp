// The static addresses (apps/novaworld_common/hosted/address_*.h; ADR 0066). The pure
// parts first: the name rules and the hostname; the file zone's text, its records and its
// serial; the Route 53 zone's change batches, its argv and the record a delete finds gone;
// the EC2 address driver's argv for each call (the tags at allocation, no reassociation)
// and what each AWS error code comes to; the docker address driver's lowest free address,
// its association confirmed by inspect, and the addressed machine's create body. Then over
// a migrated database of the test's own (common/test_database.h): an address subscription
// named once, a name taken for good, a comp address within the own machines, the
// entitlement's addresses, a start naming an address (refused while it is not ready, and
// for a second live server); and the jobs over a recording driver and the file zone, each
// handler run by hand: allocate (the tag looked up before any allocation, the record
// written, ready), associate (waiting for the previous holder's machine to end, a
// machine.describe after), release (the machine's end first, the record deleted and in
// sync, the TTL waited, the grant read again, an association ended, then released), a
// grant come back before the release, and fleet.sweep's orphans.

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
	// Ana: tier 2, an own machine, a static address bought.
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

	// Ben: a comp address needs an own machine for it; the name is taken for good.
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
	// One live server per address: her second own-machine start may not take it.
	subscribe_item(db, ana, billing::ItemKind::OwnMachine, 0, "sub_ana_om2");
	TEST_EXPECT(start(ana, true, smoke).outcome == hosted::RequestOutcome::AddressUnavailable);
	TEST_EXPECT(start(ana, true, std::nullopt).outcome == hosted::RequestOutcome::Created);
	// Ended, it is free again; a lapse lets a server go of it.
	TEST_EXPECT(hosted::request_stop(db, made.id, "test") &&
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

// A live own machine carrying `address`, launched as `cloud_id`.
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
	hosted::register_address_jobs(registry, driver, zone);
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
	// Her one own machine has its address: a comp one waits for a second.
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
