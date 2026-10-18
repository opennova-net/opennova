#include "parts.h"

#include "accounts/account_jobs.h"
#include "billing/billing_jobs.h"
#include "core/local_object_store.h"
#include "core/loggable.h"
#include "core/owner_file.h"
#include "core/s3_object_store.h"
#include "data_sets/data_set_jobs.h"
#include "hosted/cloud/address_zone.h"
#include "hosted/cloud/docker_address_driver.h"
#include "hosted/cloud/docker_machine_driver.h"
#include "hosted/cloud/ec2_machine_driver.h"
#include "hosted/model/fleet_jobs.h"
#include "hosted/model/hosted_settings.h"
#include "hosted/model/stats_jobs.h"
#include "hosted/protocol/agent_protocol.h"
#include "jobs/maintenance_jobs.h"

#include <cstdio>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// "the s3 store s3://<bucket>/<root>" or "the local store <dir>", for the boot line.
std::string store_text(const ObjectStoreConfig &store) {
	if (store.kind == ObjectStoreConfig::Kind::S3)
		return "the s3 store s3://" + store.bucket + "/" + store.root + " (" + store.region + ")";
	return "the local store " + loggable(store.root, 260);
}

// Whether `host` is no address a machine reaches the gate at: unset, or this
// system's own loopback.
bool loopback_host(const std::string &host) {
	return host.empty() || host == "localhost" || host.rfind("127.", 0) == 0;
}

// The ec2 driver's settings checked, into `setup`: false with `error`.
bool setup_ec2(hosted::DriverConfig &driver, std::string &error) {
	hosted::Ec2MachineConfig &ec2 = driver.ec2;
	std::string why;
	if (!hosted::load_machine_script(driver.machine_script_path, ec2.machine_script, why)) {
		error = "the ec2 driver: " + loggable(why, 300) + " (ONNET_HOSTED_MACHINE_SCRIPT)";
		return false;
	}
	if (const std::string bad = hosted::Ec2MachineDriver::check_config(ec2); !bad.empty()) {
		error = "the ec2 driver is not configured: " + bad;
		return false;
	}
	std::string subnets;
	for (const std::string &subnet : ec2.subnets)
		subnets += (subnets.empty() ? "" : ",") + subnet;
	std::printf("[boot] hosted servers: ec2 in %s, subnets %s, deployment %s, shared machines "
				"from %s, own from %s\n",
			ec2.region.c_str(), subnets.c_str(), ec2.deployment.c_str(),
			ec2.shared_template.c_str(), ec2.own_template.c_str());
	std::printf("[boot] hosted servers: the machines join ECS cluster %s\n", ec2.cluster.c_str());
	return true;
}

// The docker driver's settings checked (the local stack's machines): false
// with `error`.
bool setup_docker(const hosted::DriverConfig &driver, std::string &error) {
	const hosted::DockerMachineConfig &docker = driver.docker;
	if (const std::string why = hosted::DockerMachineDriver::check_config(docker); !why.empty()) {
		error = "the docker driver is not configured: " + why;
		return false;
	}
	std::printf("[boot] hosted servers: docker machines on network %s, the store's mounts "
				"from %s, deployment %s; the machines reach %s\n",
			docker.network.c_str(), docker.data_from.c_str(), docker.deployment.c_str(),
			loggable(docker.service_url, 255).c_str());
	return true;
}

// The static addresses' policy over a hosting policy that is on: on
// (addresses_on) with ONNET_FLEET_MAX_ADDRESSES above 0 and a zone name in
// ONNET_HOSTED_ADDRESS_ZONE, else none sold (no name: a boot warning).
void address_policy(HostedPolicy &policy) {
	hosted::PlacementPolicy &placement = policy.fleet.placement;
	if (placement.max_addresses <= 0)
		return;
	if (!hosted::is_zone_name(policy.fleet.address_zone)) {
		std::fprintf(stderr,
				"[boot] WARN static addresses: ONNET_HOSTED_ADDRESS_ZONE is no zone name "
				"(play.<domain>); none sold (ONNET_FLEET_MAX_ADDRESSES %d ignored)\n",
				placement.max_addresses);
		placement.max_addresses = 0;
		return;
	}
	policy.addresses_on = true;
	std::printf("[boot] static addresses: up to %d, named in %s\n", placement.max_addresses,
			policy.fleet.address_zone.c_str());
}

// The static addresses' drivers' settings checked over a policy with them on
// (the play zone's Route 53 id for ec2; the zone file and the addresses for
// docker): false with `error`.
bool setup_addresses(const HostedPolicy &policy, const hosted::DriverConfig &driver,
		std::string &error) {
	const hosted::DriverConfig::Addresses &a = driver.addresses;
	std::string why;
	if (policy.provider == "ec2") {
		hosted::Route53ZoneConfig zone;
		zone.aws_cli = driver.ec2.aws_cli;
		zone.region = driver.ec2.region;
		zone.zone = policy.fleet.address_zone;
		zone.zone_id = a.zone_id;
		why = hosted::Route53Zone::check_config(zone);
	} else if (a.zone_file.empty() || a.zone_file[0] != '/') {
		why = "ONNET_HOSTED_ADDRESS_ZONE_FILE is no absolute path";
	} else {
		hosted::DockerAddressDriver::parse_range(a.docker_range, why);
	}
	if (!why.empty()) {
		error = "the static addresses' driver is not configured: " + why +
				" (ONNET_FLEET_MAX_ADDRESSES " + std::to_string(policy.fleet.placement.max_addresses) +
				")";
		return false;
	}
	const std::string from = policy.provider == "ec2"
			? "Elastic IPs, their records in Route 53 zone " + a.zone_id
			: "from " + a.docker_range + ", their records in " + loggable(a.zone_file, 260);
	std::printf("[boot] static addresses: %s\n", from.c_str());
	return true;
}

} // namespace

ScratchDir::ScratchDir(ScratchDir &&other) noexcept : path_(std::exchange(other.path_, {})) {}

ScratchDir &ScratchDir::operator=(ScratchDir &&other) noexcept {
	if (this != &other) {
		remove();
		path_ = std::exchange(other.path_, {});
	}
	return *this;
}

ScratchDir::~ScratchDir() {
	remove();
}

void ScratchDir::remove() {
	if (path_.empty())
		return;
	std::error_code ec;
	std::filesystem::remove_all(path_, ec);
	path_.clear();
}

ScratchDir ScratchDir::make(const char *prefix, std::string &error) {
	ScratchDir dir;
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	if (ec) {
		error = "no temporary directory";
		return dir;
	}
	const std::filesystem::path path = temp / (std::string(prefix) + hosted::random_secret(12));
	if (hosted::make_owner_directory(path, error))
		dir.path_ = path;
	return dir;
}

namespace {

// The hosted servers' policy but the static addresses' (hosted_policy).
HostedPolicy servers_policy(const ServerConfig &config) {
	HostedPolicy setup;
	// The store first, driver or none: web's data set routes and the worker's
	// data set jobs run over it whether or not hosting is on.
	if (!config.object_store_url.empty()) {
		std::string error;
		if (parse_object_store_url(config.object_store_url, config.object_store_region, setup.store, error))
			setup.store_ok = true;
		else
			std::fprintf(stderr, "[boot] WARN the object store: ONNET_OBJECT_STORE_URL %s\n",
					loggable(error, 300).c_str());
	}
	if (config.hosted_provider == "none")
		return setup;
	if (config.hosted_provider != "docker" && config.hosted_provider != "ec2") {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: unknown ONNET_HOSTED_PROVIDER '%s'; hosting off\n",
				loggable(config.hosted_provider).c_str());
		return setup;
	}
	std::vector<std::string> errors;
	const hosted::EnvLookup env = hosted::process_env();
	hosted::parse_fleet_config(
			[&env, &config](const char *name) -> std::optional<std::string> {
				if (!config.internal_bind.empty() &&
						std::string_view(name) == "ONNET_INTERNAL_HTTP_BIND")
					return config.internal_bind;
				return env(name);
			},
			setup.fleet, errors);
	if (!errors.empty()) {
		for (const std::string &e : errors)
			std::fprintf(stderr, "[boot] WARN hosted servers: %s\n", loggable(e, 300).c_str());
		std::fprintf(stderr,
				"[boot] WARN hosted servers: the fleet's settings do not read; hosting off\n");
		return setup;
	}
	// The data sets' trees live in the store: no store, no hosting (ADR 0064).
	if (!setup.store_ok) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: no object store (ONNET_OBJECT_STORE_URL, s3://<bucket>/data "
				"with ONNET_OBJECT_STORE_REGION, or file:///<dir>), where the data sets' trees live; "
				"hosting off\n");
		return setup;
	}
	// The servers list through the service's public address, so the listing
	// records each machine's own (ADR 0053); a local stack's machines are
	// containers of their own, which reach the gate there, never at their own
	// loopback.
	if (loopback_host(config.public_host)) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: %s needs ONNET_PUBLIC_HOST, the public address "
				"the servers list on (not loopback); hosting off\n",
				config.hosted_provider.c_str());
		return setup;
	}
	if (!hosted::is_image_reference(setup.fleet.serve_image)) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: ONNET_HOSTED_SERVE_IMAGE is no image reference; "
				"hosting off\n");
		return setup;
	}
	// What the supervisor knows of the driver's machines needs no driver: the
	// address their admin.cfg admits is the fleet's bind.
	if (config.hosted_provider == "ec2") {
		hosted::Ec2MachineConfig ec2;
		ec2.admin_address = setup.fleet.internal_address;
		setup.profile = hosted::Ec2MachineDriver::profile(ec2);
	} else {
		hosted::DockerMachineConfig docker;
		docker.admin_address = setup.fleet.internal_address;
		setup.profile = hosted::DockerMachineDriver::profile(docker);
	}
	setup.provider = config.hosted_provider;
	setup.on = true;
	std::printf("[boot] hosted servers: %s, serve image %s, the data sets' trees in %s\n",
			setup.provider.c_str(), loggable(setup.fleet.serve_image, 255).c_str(),
			store_text(setup.store).c_str());
	return setup;
}

} // namespace

HostedPolicy hosted_policy(const ServerConfig &config) {
	HostedPolicy policy = servers_policy(config);
	// Hosting off sells no static address, whatever ONNET_FLEET_MAX_ADDRESSES says.
	if (policy.on)
		address_policy(policy);
	else
		policy.fleet.placement.max_addresses = 0;
	return policy;
}

bool driver_setup(const HostedPolicy &policy, DriverSetup &out, std::string &error) {
	std::vector<std::string> errors;
	if (!hosted::parse_driver_config(hosted::process_env(), policy.fleet, out.driver, errors)) {
		error = "the driver's settings do not read: " + loggable(errors.front(), 300);
		return false;
	}
	const bool machines = policy.provider == "ec2" ? setup_ec2(out.driver, error)
												   : setup_docker(out.driver, error);
	if (!machines)
		return false;
	return !policy.addresses_on || setup_addresses(policy, out.driver, error);
}

ObjectStoreStack make_object_store(
		const HostedPolicy &setup, const ServerConfig &config, std::string &error) {
	ObjectStoreStack s;
	if (!setup.store_ok) {
		error = "no object store (ONNET_OBJECT_STORE_URL)";
		return s;
	}
	ScratchDir scratch = ScratchDir::make("opennova-store-", error);
	if (scratch.empty()) {
		error = "no scratch directory for the object store" + (error.empty() ? std::string() : ": " + error);
		return s;
	}
	s.scratch = std::move(scratch);
	if (setup.store.kind == ObjectStoreConfig::Kind::Local) {
		s.store = std::make_unique<LocalObjectStore>(setup.store.root);
		return s;
	}
	// S3 through the AWS CLI, as the EC2 driver and the mailer run it: the
	// program's role answers for it (the task role on ECS).
	const std::string scratch_path = s.scratch.path().u8string();
	s.runner = std::make_unique<proc::ProcessCommandRunner>(
			proc::ProcessCommandRunner::service_environment(), scratch_path);
	s.store = std::make_unique<S3ObjectStore>(setup.store, *s.runner, config.aws_cli, scratch_path);
	return s;
}

BillingStack make_billing(
		const billing::BillingConfig &config, db::ConnectionPool &pool, db::Database &db) {
	BillingStack s;
	add_log_secret(config.secret_key);
	add_log_secret(config.webhook_secret);
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN billing: %s; billing off\n", why.c_str());
	};
	if (const std::string why = billing::check_billing_config(config); !why.empty()) {
		off(why);
		return s;
	}
	std::string error;
	ScratchDir scratch = ScratchDir::make("opennova-billing-", error);
	if (scratch.empty()) {
		off("no scratch directory for the Stripe calls" +
				(error.empty() ? std::string() : ": " + error));
		return s;
	}
	billing::CurlStripeApi::Config api;
	api.curl = config.curl_path;
	api.secret_key = config.secret_key;
	api.scratch_dir = scratch.path().u8string();
	if (const std::string why = billing::CurlStripeApi::check_config(api); !why.empty()) {
		off("the Stripe transport: " + why);
		return s;
	}
	std::string instance;
	try {
		instance = billing::instance_id(db);
	} catch (const db::Error &e) {
		off(std::string("no instance id: ") + e.what());
		return s;
	}
	s.scratch = std::move(scratch);
	s.runner = std::make_unique<proc::ProcessCommandRunner>(
			billing::curl_environment(), api.scratch_dir);
	s.api = std::make_unique<billing::CurlStripeApi>(*s.runner, std::move(api));
	s.billing = std::make_unique<billing::Billing>(config, *s.api, pool, instance);
	std::printf("[boot] billing: on, %s mode, instance %s, checkout returns to %s%s\n",
			config.live() ? "live" : "test", instance.c_str(), config.site_url.c_str(),
			config.automatic_tax ? ", Stripe Tax on" : "");
	return s;
}

MailStack make_mail(const ServerConfig &config) {
	MailStack s;
	const auto off = [&s](bool warn, const std::string &why) {
		std::fprintf(warn ? stderr : stdout, "[boot] %smail: %s; mail off\n", warn ? "WARN " : "",
				why.c_str());
		s.mailer = std::make_unique<NullMailer>();
	};
	if (config.mail_from.empty()) {
		off(false, "no ONNET_MAIL_FROM");
		return s;
	}
	if (config.site_url.empty()) {
		off(true, "ONNET_SITE_URL is not set, so no link could be sent");
		return s;
	}
	std::string error;
	ScratchDir scratch = ScratchDir::make("opennova-mail-", error);
	if (scratch.empty()) {
		off(true,
				"no scratch directory for the SES calls" +
						(error.empty() ? std::string() : ": " + error));
		return s;
	}
	SesMailerConfig ses;
	ses.region = config.mail_region;
	ses.from = config.mail_from;
	ses.scratch_dir = scratch.path().u8string();
	ses.aws_cli = config.aws_cli;
	if (const std::string why = SesMailer::check_config(ses); !why.empty()) {
		off(true, why);
		return s;
	}
	s.scratch = std::move(scratch);
	s.runner = std::make_unique<proc::ProcessCommandRunner>(
			proc::ProcessCommandRunner::service_environment(), ses.scratch_dir);
	s.mailer = std::make_unique<SesMailer>(ses, *s.runner);
	s.on = true;
	std::printf("[boot] mail: on, SES in %s, from %s, links to %s\n", ses.region.c_str(),
			ses.from.c_str(), config.site_url.c_str());
	return s;
}

std::vector<jobs::PeriodicJob> worker_schedule(
		const ServerConfig &config, const HostedPolicy &hosting) {
	jobs::MaintenanceConfig maintenance;
	maintenance.sweep_interval_ms = static_cast<int64_t>(config.host_sweep_interval_ms);
	maintenance.host_stale_window_ms = static_cast<int64_t>(config.host_stale_window_ms);
	std::vector<jobs::PeriodicJob> out = jobs::maintenance_schedule(maintenance);
	const auto add = [&out](std::vector<jobs::PeriodicJob> more) {
		for (jobs::PeriodicJob &p : more)
			out.push_back(std::move(p));
	};
	add(jobs::account_schedule(maintenance));
	// The hosted stats' prune whether or not hosting is on: a fleet turned off
	// leaves rows that still age out.
	add(hosted::stats_schedule());
	// Billing's jobs while its settings are complete, at their reconcile
	// period: the workers' billing runs them.
	if (const billing::BillingConfig billing_config = billing::billing_config_from_env();
			billing::check_billing_config(billing_config).empty())
		add(billing::billing_schedule(billing_config.reconcile_minutes));
	// The sweep of the driver's machines (fleet_jobs.h kFleetSweepIntervalMs).
	if (hosting.on)
		add(hosted::fleet_schedule());
	return out;
}

std::vector<jobs::PeriodicJob> data_schedule(const HostedPolicy &hosting) {
	// The data sets' prune of the trees nothing needs, while there is a store.
	if (!hosting.store_ok)
		return {};
	return hosted::data_set_schedule();
}

std::vector<jobs::PeriodicJob> periodic_jobs(
		const ServerConfig &config, const HostedPolicy &hosting) {
	std::vector<jobs::PeriodicJob> out = worker_schedule(config, hosting);
	for (jobs::PeriodicJob &p : data_schedule(hosting))
		out.push_back(std::move(p));
	return out;
}

} // namespace opennova::novaworld_server
