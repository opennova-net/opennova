#pragma once

// Every byte each slot's user sends, counted by the kernel: a chain of its own
// (kChain) that OUTPUT jumps to first, holding one rule per slot that matches
// the slot's UID (-m owner --uid-owner 10001 + k) and returns, so a packet is
// counted once and then meets the rest of OUTPUT (the metadata fence among it)
// as before. The containers run on the machine's own network, so their
// packets leave through OUTPUT under their own UID. The counters live as long
// as the rules: since the machine booted, across restarts of the agent (every
// rule is checked before it is added, never replaced). Read with `-n -v -x -L`
// (exact byte counts). Each tool (iptables, and ip6tables where the kernel has
// IPv6) keeps its own chain; a slot's count is their sum. Every call waits up to
// 5 s for the xtables lock docker also takes. Platform plumbing, not a port.

#include "command_runner.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::hostd {

// Where the agent reads each slot's egress (the tests' fake stands in).
class EgressSource {
public:
	virtual ~EgressSource() = default;
	// The counting in place, wherever any of it is missing. False with `error`.
	virtual bool ensure(std::string &error) = 0;
	// Each slot's bytes since the machine booted, by slot. False when a count
	// could not be read (the caller ensures again: a rule put back counts from 0).
	virtual bool read(std::map<int, uint64_t> &out, std::string &error) = 0;
};

class EgressCounters : public EgressSource {
public:
	static constexpr const char *kChain = "OPENNOVA-EGRESS";
	static constexpr int kCommandTimeoutMs = 15 * 1000;

	// `tools`: the absolute paths of iptables (and ip6tables). `runner` must
	// outlive this.
	EgressCounters(proc::CommandRunner &runner, std::vector<std::string> tools, int slots);

	// The chain, OUTPUT's jump to it and each slot's rule, wherever one is
	// missing. False with `error` when a tool refuses.
	bool ensure(std::string &error) override;
	// Each slot's bytes, summed over the tools. False when a chain does not
	// read or lacks a slot's rule.
	bool read(std::map<int, uint64_t> &out, std::string &error) override;

	// The argv each step runs (tests).
	std::vector<std::string> rule_argv(const std::string &tool, const char *op, int slot) const;

private:
	bool call(std::vector<std::string> argv, std::string &error, bool *refused = nullptr);

	proc::CommandRunner &runner_;
	std::vector<std::string> tools_;
	int slots_;
};

// `iptables -n -v -x -L <chain>`'s rule lines as UID -> bytes: each line whose
// match is "owner UID match <uid>", its second column the exact byte count.
void parse_chain_counters(std::string_view listing, std::map<uint32_t, uint64_t> &out);

} // namespace opennova::hostd
