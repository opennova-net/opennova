#pragma once

// The play zone a static address's name lives in (ADR 0066): an A record
// <name>.<zone> at the address, TTL kAddressTtlS, written by the worker's
// address jobs (hosted/model/address_jobs.h) when an address is ready and deleted before it
// is released. The seam has two zones:
//
//   Route53Zone  the deployment's Route 53 public zone (infra/aws/edge.tf
//                play.<domain>, staging play.staging.<domain>; its id
//                ONNET_HOSTED_ADDRESS_ZONE_ID), through the AWS CLI like the
//                machine drivers (an argv, no shell, a timeout): an UPSERT or
//                a DELETE of one A record (change-resource-record-sets, the
//                change's id read back) and get-change for whether it is
//                INSYNC. A DELETE of a record that is not there is done.
//                Route 53 is global: the calls name the region all the same.
//   FileZone     the local stack's: an RFC 1035 zone file of the zone
//                (ONNET_HOSTED_ADDRESS_ZONE_FILE), its SOA and NS and one A
//                line per name, rewritten whole at every change through a
//                file beside it renamed over it (a reader never sees half of
//                it); every change is in sync at once.
//
// Service policy, not a port.

#include "command_runner.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace opennova::novaworld_server::hosted {

// The A records' TTL: the wait a released address's record drains for.
inline constexpr int64_t kAddressTtlS = 300;

struct ZoneChange {
	bool ok = false;
	std::string change_id; // ok: the zone's id for the change
	std::string error;     // !ok: why, for the log
};

class ZoneDriver {
public:
	virtual ~ZoneDriver() = default;
	// The zone's name ("play.opennova.net"), no trailing dot.
	virtual const std::string &zone() const = 0;
	// The A record <name>.<zone> at `ip` written (made, or its address moved).
	virtual ZoneChange upsert(const std::string &name, const std::string &ip) = 0;
	// The A record <name>.<zone> at `ip` deleted (one that is not there: ok).
	virtual ZoneChange remove(const std::string &name, const std::string &ip) = 0;
	// Whether change `change_id` is in sync on the zone's servers; nullopt when
	// the zone could not tell.
	virtual std::optional<bool> in_sync(const std::string &change_id) = 0;
};

struct Route53ZoneConfig {
	std::string aws_cli = "/usr/local/bin/aws"; // ONNET_HOSTED_AWS_CLI
	std::string region;  // ONNET_HOSTED_EC2_REGION
	std::string zone;    // ONNET_HOSTED_ADDRESS_ZONE
	std::string zone_id; // ONNET_HOSTED_ADDRESS_ZONE_ID (Z...)
	int command_timeout_ms = 20 * 1000;
};

class Route53Zone : public ZoneDriver {
public:
	// `runner` must outlive the zone.
	Route53Zone(Route53ZoneConfig config, proc::CommandRunner &runner);

	// "" when `config` can run, else why not.
	static std::string check_config(const Route53ZoneConfig &config);
	// The change batch of one A record: {"Changes": [{"Action", "ResourceRecordSet":
	// {"Name": "<name>.<zone>.", "Type": "A", "TTL": kAddressTtlS, "ResourceRecords":
	// [{"Value": "<ip>"}]}}]}.
	static std::string change_batch(const std::string &action, const std::string &fqdn, const std::string &ip);

	const std::string &zone() const override { return config_.zone; }
	ZoneChange upsert(const std::string &name, const std::string &ip) override;
	ZoneChange remove(const std::string &name, const std::string &ip) override;
	std::optional<bool> in_sync(const std::string &change_id) override;

private:
	ZoneChange change(const std::string &action, const std::string &name, const std::string &ip);

	const Route53ZoneConfig config_;
	proc::CommandRunner &runner_;
};

class FileZone : public ZoneDriver {
public:
	// The zone `zone` in the file `path` (made at the first change).
	FileZone(std::string zone, std::string path);

	// The zone file's text for `records` (name -> address) at `serial`.
	static std::string render(const std::string &zone, const std::map<std::string, std::string> &records,
			uint32_t serial);
	// The A records of a zone file's text this class wrote (name -> address).
	static std::map<std::string, std::string> parse(const std::string &text);

	const std::string &zone() const override { return zone_; }
	ZoneChange upsert(const std::string &name, const std::string &ip) override;
	ZoneChange remove(const std::string &name, const std::string &ip) override;
	std::optional<bool> in_sync(const std::string &change_id) override;

private:
	// The file read, `edit` applied to its records, and written back whole.
	ZoneChange rewrite(const std::function<void(std::map<std::string, std::string> &)> &edit);

	const std::string zone_;
	const std::string path_;
	std::mutex mu_; // one change at a time within the process
};

} // namespace opennova::novaworld_server::hosted
