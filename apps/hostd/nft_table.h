#pragma once

// The machine's firewall for its slots, in an nftables table the agent owns
// (`table inet opennova`, IPv4 and IPv6 in one): the fences that keep a slot's
// user (UIDs 10001..10032, hosted/protocol/slots.h) off what only the agent may reach,
// and each slot's traffic counters. The slots' containers run on the
// machine's own network, so their packets leave through the output hook under
// their own UID (`meta skuid`) and reach them through the input hook at their
// slot's ports.
//
//   output   every slot user refused (reject): the instance metadata service
//            (169.254.169.254, Nitro's fd00:ec2::254: the machine role's
//            credentials, the user-data with its token), the ECS task
//            metadata and credentials address 169.254.170.2, and on any
//            destination a new TCP connection to the fleet's internal port
//            (the agents' routes), to the slots' remote-admin ports (a
//            sibling's, 32800..32831) and to the ECS agent's 51678
//            (introspection) and 51679 (the task credentials 169.254.170.2:80
//            is DNAT'ed to); then a packet to a private destination (RFC 1918,
//            link-local, loopback; unique-local, link-local and loopback for
//            IPv6) goes uncounted; then slot k's user's is counted in
//            egress-<k>
//   input    a packet to slot k's game port (UDP 32768 + k) or its admin port
//            (TCP 32800 + k) is counted in ingress-<k>
//
// The counters are named objects (`counter egress-<k>`, `counter
// ingress-<k>`), one pair for every slot a machine can have (kMaxSlots), so
// they live as long as the table: since the machine booted, across restarts
// of the agent. ensure() applies the whole ruleset in one `nft -f` transaction
// (the table, the counters and the chains added where missing, the chains'
// rules flushed and written again), so the rules are always exactly these and
// the counts carry on. Nothing else on the machine (docker's tables, the ECS
// agent's) shares the table. Platform plumbing, not a port.

#include "command_runner.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::hostd {

// A slot's traffic since the machine booted: what its user sent to the
// internet, and what reached its ports.
struct SlotCount {
	uint64_t egress_bytes = 0;
	uint64_t ingress_bytes = 0;
};

// Where the agent reads the slots' counts (the tests' fake stands in).
class SlotCounters {
public:
	virtual ~SlotCounters() = default;
	// The fences and the counters in place. False with `error`.
	virtual bool ensure(std::string &error) = 0;
	// Each slot's counts, by slot (every slot the table counts). False when the
	// table does not read (the caller ensures again).
	virtual bool read(std::map<int, SlotCount> &out, std::string &error) = 0;
};

class NftTable : public SlotCounters {
public:
	static constexpr const char *kTable = "opennova";
	static constexpr int kCommandTimeoutMs = 15 * 1000;
	// The ECS agent's local ports: its introspection API and the task
	// credentials endpoint.
	static constexpr int kEcsIntrospectionPort = 51678;
	static constexpr int kEcsCredentialsPort = 51679;

	// `runner` must outlive this; `nft` is the nft binary (absolute),
	// `fleet_port` the fleet's internal port (the agent's --service), and
	// `scratch_dir` (0700) where the ruleset file is written for `nft -f`.
	NftTable(proc::CommandRunner &runner, std::string nft, int fleet_port, std::string scratch_dir);

	bool ensure(std::string &error) override;
	bool read(std::map<int, SlotCount> &out, std::string &error) override;

	// The ruleset ensure() applies (tests).
	static std::string ruleset(int fleet_port);

private:
	proc::CommandRunner &runner_;
	std::string nft_;
	int fleet_port_;
	std::string scratch_dir_;
};

// `nft -j list counters table inet opennova`'s reply as each slot's counts:
// every counter named egress-<k> or ingress-<k> (0 <= k < kMaxSlots) of the
// table. False with `error` when the reply does not read as nft's JSON.
bool parse_nft_counters(std::string_view json, std::map<int, SlotCount> &out, std::string &error);

} // namespace opennova::hostd
