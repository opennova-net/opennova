#pragma once

// The cloud AddressDriver (ONNET_HOSTED_PROVIDER=ec2; address_driver.h): each
// static address is an Elastic IP of the deployment's region, tagged
// opennova:hosted=1, opennova:deployment, opennova:address-id (its row) and
// Name at its allocation (TagSpecifications: the tags and the address are one
// call, so no untagged address is ever left; infra/aws/fleet.tf's worker
// policy admits the tags at allocation alone, and every other call on an
// address carrying this deployment's tag alone). The AWS CLI v2 through a
// CommandRunner, as the machine driver (ec2_machine_driver.h): an argv, no
// shell, a timeout, the region named, JSON, no pager; the credentials the
// worker task's role. A call's AWS error code (the CLI's "An error occurred
// (<code>)") decides what it came to:
//
//   find_tagged   describe-addresses on the three tags
//   allocate      allocate-address --domain vpc with its tags; refused for
//                 good on AddressLimitExceeded (the account's quota, which the
//                 row's reason shows) and other client errors
//   associate     associate-address --no-allow-reassociation (an address is
//                 never taken from another machine); IncorrectInstanceState
//                 (the instance is pending), InvalidInstanceID (not listed yet)
//                 and Resource.AlreadyAssociated (its last machine still
//                 terminating) are Retry, but when the describe after says it
//                 is this machine's already
//   describe      describe-addresses --allocation-ids; InvalidAllocationID.NotFound
//                 is the address gone
//   disassociate  disassociate-address; InvalidAssociationID.NotFound is done
//   release       release-address; InvalidIPAddress.InUse (still associated)
//                 is Retry, InvalidAllocationID.NotFound done
//   list_tagged   describe-addresses on opennova:hosted and the deployment
//
// Service policy over EC2's API, not a port.

#include "address_driver.h"
#include "command_runner.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct Ec2AddressConfig {
	std::string aws_cli = "/usr/local/bin/aws"; // ONNET_HOSTED_AWS_CLI
	std::string region;     // ONNET_HOSTED_EC2_REGION
	std::string deployment; // ONNET_HOSTED_DEPLOYMENT
	int command_timeout_ms = 20 * 1000;
};

class Ec2AddressDriver : public AddressDriver {
public:
	static constexpr const char *kTagAddressId = "opennova:address-id";

	// `runner` must outlive the driver.
	Ec2AddressDriver(Ec2AddressConfig config, proc::CommandRunner &runner);

	// "" when `config` can run, else why not.
	static std::string check_config(const Ec2AddressConfig &config);
	// The AWS error code a failed call's standard error names ("" for none).
	static std::string error_code(const std::string &err);

	const char *name() const override { return "ec2"; }
	AddressResult find_tagged(int64_t address_id) override;
	AddressResult allocate(int64_t address_id, const std::string &name, const std::vector<std::string> &held) override;
	AddressResult associate(const CloudAddress &address, const std::string &cloud_id) override;
	AddressResult describe(const std::string &allocation_id) override;
	AddressResult disassociate(const std::string &association_id) override;
	AddressResult release(const std::string &allocation_id) override;
	std::vector<CloudAddress> list_tagged() override;

private:
	// argv: the CLI, `ec2`, the operation and its parameters, then the globals.
	std::vector<std::string> command(const std::string &operation, std::vector<std::string> params) const;
	// describe-addresses with `params`: Done with every address it lists, or Retry.
	AddressResult describe_with(std::vector<std::string> params, std::vector<CloudAddress> &listed);

	const Ec2AddressConfig config_;
	proc::CommandRunner &runner_;
};

} // namespace opennova::novaworld_server::hosted
