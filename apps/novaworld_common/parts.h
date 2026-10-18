#pragma once

// What several programs build from the same settings, each deciding alike, so
// no program needs another's memory to agree with it (the HTTP layer's;
// program/program.h):
//
// - The hosted servers' policy (hosted_policy): whether hosting is on, its
//   provider, the fleet's policy settings (placement, the slots' limits, the
//   cap, the serve image), the object store its data sets' trees live in, the
//   supervisor's MachineProfile and whether the static addresses are on (with
//   their play zone's name). Web's routes (a start's at_capacity, the
//   list's `full`), the fleet's supervisor and the worker's fleet jobs read
//   the same environment the same way.
// - The machine driver's setup (driver_setup): the worker's alone, which
//   launches the machines: the driver's own settings and its machine script,
//   checked, with hosting on; the worker builds the driver over them
//   (apps/novaworld_worker/main.cpp), its scratch directory and its runner.
//   No other program reads the driver's settings or carries the script.
// - The object store (make_object_store): web's data set routes and the data
//   program's data set jobs, each over a store of its own (S3 through the AWS
//   CLI, or the local directory); the fleet holds none (its supervisor
//   gets the store's URL of a tree as a pure function).
// - Billing (make_billing): web's routes and webhook and the worker's sync,
//   each over a curl transport of its own.
// - Outbound mail (make_mail): whether mail is on (web queues mail.send jobs
//   only then) and the worker's SES mailer that sends them.
// - The periodic jobs: each program that runs jobs schedules its own kinds
//   alone (worker_schedule, data_schedule), and web's jobs page shows and
//   runs on request every one of them (periodic_jobs).
//
// Each holds its owner-only scratch directory (ScratchDir) and removes it as
// it goes, after what used it. Service policy, not a port.

#include "accounts/mailer.h"
#include "billing/billing.h"
#include "billing/billing_config.h"
#include "billing/stripe_api.h"
#include "command_runner.h"
#include "core/object_store.h"
#include "core/scratch_dir.h"
#include "hosted/model/fleet_config.h"
#include "hosted/model/machine_driver.h"
#include "jobs/scheduler.h"
#include "program/server_config.h"

#include <net/novaworld/db/database.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

struct HostedPolicy {
	bool on = false;
	// The driver's name ("ec2" or "docker") while on, else "none".
	std::string provider = "none";
	// The object store (core/object_store.h) ONNET_OBJECT_STORE_URL names, when it
	// parses (store_ok): the data sets' trees live in it. Read whether or not
	// hosting is on (web's data set routes and the data program's jobs run over
	// it either way); hosting is on only with it.
	ObjectStoreConfig store;
	bool store_ok = false;
	hosted::FleetConfig fleet;
	// The supervisor's view of the driver's machines (the fleet's).
	hosted::MachineProfile profile;
	// The static addresses (hosted/model/address_repository.h) are on: hosting on,
	// ONNET_FLEET_MAX_ADDRESSES above 0 and ONNET_HOSTED_ADDRESS_ZONE a zone
	// name (no name: a boot warning). Off, the fleet sells none
	// (fleet.placement.max_addresses is 0). Their drivers' settings are the
	// worker's (driver_setup).
	bool addresses_on = false;
};

// The hosted servers' policy ONNET_HOSTED_PROVIDER names, over the object store
// ONNET_OBJECT_STORE_URL names: off for "none", an unknown name, fleet settings
// that do not read, no store or one that does not parse, a loopback
// ONNET_PUBLIC_HOST (the servers list through the service's public address) or
// a serve image that is no image reference (each but "none" logged as a
// warning). The driver's own settings are no part of it (driver_setup). The
// fleet's settings are the environment's (hosted/model/fleet_config.h), the internal
// listener's bind the fleet found in its ECS task standing in for an unset
// ONNET_INTERNAL_HTTP_BIND (config.internal_bind).
HostedPolicy hosted_policy(const ServerConfig &config);

// The machine driver's settings over a policy that is on (the worker's): the
// drivers' own variables (hosted/model/fleet_config.h parse_driver_config) and, for
// the ec2 driver, its machine script's text, each driver's check_config passed;
// with the static addresses on, their drivers' settings too (the play zone's
// Route 53 id for ec2; the zone file and the addresses for docker).
struct DriverSetup {
	hosted::DriverConfig driver;
};

// The driver `policy` names, set up, into `out`; false with `error` when its
// settings do not read or are incomplete (the worker's FATAL: hosting on with
// no driver to launch its machines).
bool driver_setup(const HostedPolicy &policy, DriverSetup &out, std::string &error);

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

// The store `policy` names (store_ok), or none with `error`: no scratch
// directory, or a policy with no store.
ObjectStoreStack make_object_store(
		const HostedPolicy &policy, const ServerConfig &config, std::string &error);

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

// Outbound mail (accounts/mailer.h): the mailer the mail.send jobs send with (SES
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

// The worker's periodic jobs (jobs/scheduler.h) over `config` and `hosting`:
// the prunes (the service's and the website's, every HOST_SWEEP_INTERVAL_MS;
// the hosted stats' and the finished jobs', hourly), billing's while its
// settings are complete (check_billing_config), and the fleet's sweep while
// hosting is on. Every one of the worker's kinds that is periodic.
std::vector<jobs::PeriodicJob> worker_schedule(
		const ServerConfig &config, const HostedPolicy &hosting);
// The data program's: the data sets' tree.prune while the store is there.
std::vector<jobs::PeriodicJob> data_schedule(const HostedPolicy &hosting);
// Every program's together (the worker's, then the data program's): what
// web's jobs page shows and runs on request.
std::vector<jobs::PeriodicJob> periodic_jobs(
		const ServerConfig &config, const HostedPolicy &hosting);

} // namespace opennova::novaworld_server
