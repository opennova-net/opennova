#pragma once

// What several programs build from the same settings, each deciding alike, so
// no program needs another's memory to agree with it (the HTTP layer's;
// program.h):
//
// - The hosted servers' setup (hosted_setup): whether hosting is on, its
//   provider, its fleet settings, the object store its data sets' trees live
//   in and the supervisor's MachineProfile. Web's routes (a start's
//   at_capacity, the list's `full`), the fleet's supervisor and the
//   worker's fleet jobs read the same environment the same way; the worker
//   alone builds the machine driver over it (apps/novaworld_worker/main.cpp),
//   its scratch directory and its runner.
// - The object store (make_object_store): web's data set routes and the
//   worker's data set jobs, each over a store of its own (S3 through the AWS
//   CLI, or the local directory); the fleet holds none (its supervisor
//   gets the store's URL of a tree as a pure function).
// - Billing (make_billing): web's routes and webhook and the worker's sync,
//   each over a curl transport of its own. The fleet schedules billing's
//   jobs from the settings alone (check_billing_config), building none of it.
// - Outbound mail (make_mail): whether mail is on (web queues mail.send jobs
//   only then) and the worker's SES mailer that sends them.
// - The periodic jobs (periodic_jobs): what the fleet's scheduler
//   enqueues, and what web's jobs page shows and runs on request.
//
// Each holds its owner-only scratch directory (ScratchDir) and removes it as
// it goes, after what used it. Service policy, not a port.

#include "billing/billing.h"
#include "billing/billing_config.h"
#include "billing/stripe_api.h"
#include "command_runner.h"
#include "hosted/fleet_config.h"
#include "hosted/machine_driver.h"
#include "jobs/scheduler.h"
#include "mailer.h"
#include "object_store.h"
#include "server_config.h"

#include <net/novaworld/db/database.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

// An owner-only scratch directory under the system's temporary one, removed
// with its owner; a move hands it on (the source then owns none).
class ScratchDir {
public:
	ScratchDir() = default;
	ScratchDir(ScratchDir &&other) noexcept;
	ScratchDir &operator=(ScratchDir &&other) noexcept;
	ScratchDir(const ScratchDir &) = delete;
	ScratchDir &operator=(const ScratchDir &) = delete;
	~ScratchDir();

	// A new one named `prefix` and a random tail; empty with `error` when it
	// cannot be made.
	static ScratchDir make(const char *prefix, std::string &error);

	const std::filesystem::path &path() const { return path_; }
	bool empty() const { return path_.empty(); }

private:
	void remove();
	std::filesystem::path path_;
};

struct HostedSetup {
	bool on = false;
	// The driver's name ("ec2" or "docker") while on, else "none".
	std::string provider = "none";
	// The object store (object_store.h) ONNET_OBJECT_STORE_URL names, when it
	// parses (store_ok): the data sets' trees live in it. Read whether or not
	// hosting is on (web's data set routes and the worker's data set jobs run
	// over it either way); hosting is on only with it.
	ObjectStoreConfig store;
	bool store_ok = false;
	hosted::FleetConfig fleet;
	// The supervisor's view of the driver's machines (the fleet's).
	hosted::MachineProfile profile;
};

// The hosted servers' setup ONNET_HOSTED_PROVIDER names, over the object store
// ONNET_OBJECT_STORE_URL names: off for "none", an unknown name, fleet settings
// that do not read, no store or one that does not parse (the local driver's
// must be a file:// one), or a driver missing what it needs (each but "none"
// logged as a warning). The fleet's settings are the environment's
// (hosted/fleet_config.h), the internal listener's bind the fleet found
// in its ECS task standing in for an unset ONNET_INTERNAL_HTTP_BIND
// (config.internal_bind).
HostedSetup hosted_setup(const ServerConfig &config);

// The object store as a program runs it: S3ObjectStore over the AWS CLI (the
// runner with the environment the EC2 driver's calls run in, an owner-only
// scratch directory for what the calls read and write), or LocalObjectStore
// over the directory. The members go in reverse: the store first, then its
// runner, then the scratch directory.
struct ObjectStoreStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<ObjectStore> store;
};

// The store `setup` names (store_ok), or none with `error`: no scratch
// directory, or a setup with no store.
ObjectStoreStack make_object_store(
		const HostedSetup &setup, const ServerConfig &config, std::string &error);

// Billing as a program runs it: the curl transport (its runner and its owner-only
// scratch directory, removed last) and the billing over it.
struct BillingStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<billing::CurlStripeApi> api;
	std::unique_ptr<billing::Billing> billing;
};

// Billing over `config`, or none with a boot warning naming why: settings not
// complete and well-formed (check_billing_config), no scratch directory, a
// transport that would not run, no instance id. Both secrets are log secrets
// either way.
BillingStack make_billing(
		const billing::BillingConfig &config, db::ConnectionPool &pool, db::Database &db);

// Outbound mail (mailer.h): the mailer the mail.send jobs send with (SES
// through the AWS CLI, with its runner and its owner-only scratch directory,
// removed at the end; a NullMailer while mail is off) and whether mail is on.
struct MailStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<Mailer> mailer;
	bool on = false;
};

// SES over the config's ONNET_MAIL_FROM, ONNET_MAIL_REGION and ONNET_SITE_URL,
// or mail off, logged: unset (no sender), else a warning naming what is wrong.
MailStack make_mail(const ServerConfig &config);

// The periodic jobs (jobs/scheduler.h) of a deployment over `config` and
// `hosting`: the prunes (the service's and the website's, every
// HOST_SWEEP_INTERVAL_MS; the hosted stats' and the finished jobs', hourly),
// billing's while its settings are complete (check_billing_config), the
// fleet's sweep while hosting is on, and the data sets' tree.prune while the
// store is there.
std::vector<jobs::PeriodicJob> periodic_jobs(
		const ServerConfig &config, const HostedSetup &hosting);

} // namespace opennova::novaworld_server
