#include "parts.h"

#include "billing/billing_jobs.h"
#include "hosted/docker_machine_driver.h"
#include "hosted/ec2_machine_driver.h"
#include "hosted/fleet_jobs.h"
#include "hosted/hosted_settings.h"
#include "hosted/owner_file.h"
#include "hosted/stats_jobs.h"
#include "hosted/supervisor.h"
#include "jobs/account_jobs.h"
#include "jobs/maintenance_jobs.h"
#include "loggable.h"

#include <cstdio>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server {

namespace {

std::vector<std::string> data_set_slugs(const std::vector<hosted::DataSet> &data_sets) {
	std::vector<std::string> out;
	for (const hosted::DataSet &d : data_sets)
		out.push_back(d.game);
	return out;
}

// Whether `host` is no address a machine reaches the gate at: unset, or this
// system's own loopback.
bool loopback_host(const std::string &host) {
	return host.empty() || host == "localhost" || host.rfind("127.", 0) == 0;
}

// The ec2 driver's settings checked, into `setup` (on), or a boot warning
// naming what it lacks (off).
void setup_ec2(const ServerConfig &config, HostedSetup &setup) {
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN hosted servers: the ec2 driver %s; hosting off\n",
				why.c_str());
	};
	// The servers list through the service's public address, so the listing
	// records each machine's own public address (ADR 0053).
	if (loopback_host(config.public_host)) {
		off("needs ONNET_PUBLIC_HOST, the public address its servers list on");
		return;
	}
	hosted::Ec2MachineConfig &ec2 = setup.fleet.ec2;
	ec2.data_sets = data_set_slugs(setup.data_sets);
	std::string error;
	if (!hosted::load_machine_script(setup.fleet.machine_script_path, ec2.machine_script, error)) {
		off(loggable(error, 300) + " (ONNET_HOSTED_MACHINE_SCRIPT)");
		return;
	}
	if (const std::string why = hosted::Ec2MachineDriver::check_config(ec2); !why.empty()) {
		off("is not configured: " + why);
		return;
	}
	std::string subnets;
	for (const std::string &subnet : ec2.subnets)
		subnets += (subnets.empty() ? "" : ",") + subnet;
	std::printf("[boot] hosted servers: ec2 in %s, subnets %s, deployment %s, shared machines "
				"from %s, own from %s\n",
			ec2.region.c_str(), subnets.c_str(), ec2.deployment.c_str(),
			ec2.shared_template.c_str(), ec2.own_template.c_str());
	std::printf("[boot] hosted servers: serve image %s, hostd image %s, the machines reach %s\n",
			loggable(ec2.serve_image, 255).c_str(),
			loggable(ec2.hostd_image.empty() ? ec2.serve_image : ec2.hostd_image, 255).c_str(),
			loggable(ec2.service_url, 255).c_str());
	setup.profile = hosted::Ec2MachineDriver::profile(ec2);
	setup.provider = "ec2";
	setup.on = true;
}

// The docker driver's settings checked (the local stack's machines), into
// `setup` (on), or a boot warning naming what it lacks (off).
void setup_docker(const ServerConfig &config, HostedSetup &setup) {
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN hosted servers: the docker driver %s; hosting off\n",
				why.c_str());
	};
	// Its machines are containers of their own: they reach the gate at the
	// service's public address, never at their own loopback.
	if (loopback_host(config.public_host)) {
		off("needs ONNET_PUBLIC_HOST, an address its machines reach the gate at (not loopback)");
		return;
	}
	hosted::DockerMachineConfig &docker = setup.fleet.docker;
	docker.data_sets = data_set_slugs(setup.data_sets);
	if (const std::string why = hosted::DockerMachineDriver::check_config(docker); !why.empty()) {
		off("is not configured: " + why);
		return;
	}
	std::string data_sets;
	for (const std::string &data_set : docker.data_sets)
		data_sets += (data_sets.empty() ? "" : ",") + data_set;
	std::printf("[boot] hosted servers: docker machines of %s on network %s, data sets %s from "
				"%s, deployment %s; the machines reach %s\n",
			loggable(docker.image, 255).c_str(), docker.network.c_str(), data_sets.c_str(),
			docker.data_from.c_str(), docker.deployment.c_str(),
			loggable(docker.service_url, 255).c_str());
	setup.profile = hosted::DockerMachineDriver::profile(docker);
	setup.provider = "docker";
	setup.on = true;
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

HostedSetup hosted_setup(const ServerConfig &config) {
	HostedSetup setup;
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
	std::vector<hosted::DataSetConfig> configs;
	std::string error;
	if (!hosted::parse_data_set_configs(config.hosted_data_sets, configs, error)) {
		std::fprintf(stderr, "[boot] WARN hosted servers: %s; hosting off\n",
				loggable(error, 300).c_str());
		return setup;
	}
	setup.data_sets = hosted::offered_data_sets(configs);
	if (setup.data_sets.empty()) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: no data set (ONNET_HOSTED_DATA_SETS names none); "
				"hosting off\n");
		return setup;
	}
	if (config.hosted_provider == "ec2")
		setup_ec2(config, setup);
	else
		setup_docker(config, setup);
	return setup;
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

std::vector<jobs::PeriodicJob> periodic_jobs(
		const ServerConfig &config, const HostedSetup &hosting) {
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
	// The sweep of the driver's machines, as often as the supervisor's config says.
	if (hosting.on)
		add(hosted::fleet_schedule(
				static_cast<int64_t>(hosted::SupervisorConfig{}.orphan_sweep_interval_ms)));
	return out;
}

} // namespace opennova::novaworld_server
