#pragma once

// The static addresses' seam to the cloud (ADR 0066; hosted/model/address_repository.h): a
// driver finds, allocates, associates, describes, disassociates and releases
// the addresses of this deployment, and lists the ones it holds. It decides
// nothing: the worker's address jobs (hosted/model/address_jobs.h) call it and write what it
// answered to the rows, and the fleet's pass decides when an address lapses.
// Only the workers call a driver, as the machine drivers' (hosted/model/machine_driver.h).
//
// Drivers: Ec2AddressDriver (ec2_address_driver.h, an Elastic IP through the
// AWS CLI, tagged opennova:hosted, opennova:deployment and
// opennova:address-id) and DockerAddressDriver (docker_address_driver.h, the
// local stack's: a fixed address of ONNET_HOSTED_DOCKER_ADDRESSES, which the
// docker machine driver starts the addressed machine at). Service policy,
// not a port.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// What a call came to.
enum class AddressCall : uint8_t {
	Done,    // it did what was asked (or found it done already)
	Retry,   // not now (the cloud did not answer, the instance is pending, the
			 // address is still associated): ask again later
	Refused, // the cloud refused it for good (`code`: its error code)
};

// An address the cloud holds for this deployment.
struct CloudAddress {
	std::string allocation_id; // eipalloc-... (the docker driver's own)
	std::string public_ip;
	std::string association_id; // "" while associated with nothing
	std::string instance_id;    // the machine it is associated with, "" for none
	int64_t address_id = 0;     // its tag's row id (0: none)
};

struct AddressResult {
	AddressCall call = AddressCall::Retry;
	std::string code;  // Refused's error code (AddressLimitExceeded), for the row
	std::string error; // the log's text
	std::optional<CloudAddress> address; // what a find, allocate or describe read
};

class AddressDriver {
public:
	virtual ~AddressDriver() = default;

	virtual const char *name() const = 0;
	// The address this deployment tagged for row `address_id`: Done with
	// `address` (none when there is no such address), or Retry.
	virtual AddressResult find_tagged(int64_t address_id) = 0;
	// A new address for row `address_id` (named `name`), its tags written with
	// it; `held` is every address the deployment's rows hold now (the docker
	// driver picks a free one). Done with `address`, Retry or Refused.
	virtual AddressResult allocate(int64_t address_id, const std::string &name, const std::vector<std::string> &held) = 0;
	// `address` associated with machine `cloud_id`, never taken from another
	// (no reassociation): Done (associated with it now, or already), Retry (the
	// machine is pending, or the address is still another's), or Refused.
	virtual AddressResult associate(const CloudAddress &address, const std::string &cloud_id) = 0;
	// What the cloud says of `allocation_id`: Done with `address`, Done with
	// none when the cloud no longer knows it, or Retry.
	virtual AddressResult describe(const std::string &allocation_id) = 0;
	// The association `association_id` ended (one that is gone already is
	// Done).
	virtual AddressResult disassociate(const std::string &association_id) = 0;
	// The address released (one the cloud no longer knows is Done; one still
	// associated is Retry).
	virtual AddressResult release(const std::string &allocation_id) = 0;
	// Every address this deployment tagged (fleet.sweep's report). Throws
	// std::runtime_error when the listing fails.
	virtual std::vector<CloudAddress> list_tagged() = 0;
};

} // namespace opennova::novaworld_server::hosted
