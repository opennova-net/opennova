#pragma once

// The local stack's AddressDriver (ONNET_HOSTED_PROVIDER=docker;
// address_driver.h): a static address is one fixed address of
// ONNET_HOSTED_DOCKER_ADDRESSES (deploy/compose: 172.30.77.64/28, inside the
// half of the stack's network Docker never hands out), and the docker machine
// driver starts the machine of a server that names one at it
// (docker_machine_driver.h, MachineLaunch::address), as a cloud machine
// carries its Elastic IP. Nothing exists in the engine for an address, so:
//
//   find_tagged   none (the row's own allocation is all there is)
//   allocate      the lowest address of the range no row holds
//                 (`held`); AddressLimitExceeded when every one is held.
//                 Its allocation id is docker-<address>
//   associate     confirms by GET /containers/<machine>/json that the machine
//                 is at the address on the stack's network: Retry while it
//                 is not there yet, Refused when it is at another
//   describe      the address, associated with nothing the engine reports
//   disassociate, release  nothing to do
//   list_tagged   none
//
// Platform plumbing for the development stack, not a port.

#include "address_driver.h"
#include "docker_machine_driver.h"
#include "hosted/model/net_cidr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

class DockerAddressDriver : public AddressDriver {
public:
	// The addresses of `range` (ONNET_HOSTED_DOCKER_ADDRESSES), the machines'
	// network and engine `machines` (the docker machine driver's), over `call`
	// or, unset, HTTP to its engine URL.
	DockerAddressDriver(Ipv4Cidr range, DockerMachineConfig machines, DockerMachineDriver::EngineCall call = {});

	// The range ONNET_HOSTED_DOCKER_ADDRESSES names, private and /24 to /30;
	// nullopt with `why` otherwise.
	static std::optional<Ipv4Cidr> parse_range(const std::string &text, std::string &why);
	// The allocation id of `address` ("docker-172.30.77.64"), and back ("" for none).
	static std::string allocation_of(const std::string &address);
	static std::string address_of(const std::string &allocation_id);

	const char *name() const override { return "docker"; }
	AddressResult find_tagged(int64_t address_id) override;
	AddressResult allocate(int64_t address_id, const std::string &name, const std::vector<std::string> &held) override;
	AddressResult associate(const CloudAddress &address, const std::string &cloud_id) override;
	AddressResult describe(const std::string &allocation_id) override;
	AddressResult disassociate(const std::string &association_id) override;
	AddressResult release(const std::string &allocation_id) override;
	std::vector<CloudAddress> list_tagged() override;

private:
	const Ipv4Cidr range_;
	const DockerMachineConfig machines_;
	DockerMachineDriver::EngineCall call_;
};

} // namespace opennova::novaworld_server::hosted
