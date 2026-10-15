#include "egress_counters.h"

#include "hosted/slots.h"

#include <base/io/strutil.h>

#include <utility>

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

namespace {

constexpr const char *kLockWaitSeconds = "5";

std::vector<std::string> words_of(std::string_view line) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < line.size()) {
		while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
			++at;
		const size_t start = at;
		while (at < line.size() && line[at] != ' ' && line[at] != '\t')
			++at;
		if (at > start)
			out.emplace_back(line.substr(start, at - start));
	}
	return out;
}

} // namespace

EgressCounters::EgressCounters(
		proc::CommandRunner &runner, std::vector<std::string> tools, int slots) :
		runner_(runner), tools_(std::move(tools)), slots_(slots) {}

std::vector<std::string> EgressCounters::rule_argv(
		const std::string &tool, const char *op, int slot) const {
	return { tool, "-w", kLockWaitSeconds, op, kChain, "-m", "owner", "--uid-owner",
		std::to_string(hosted::slot_uid(slot)), "-j", "RETURN" };
}

// Runs `argv`. With `refused`, exit 1 (the chain or rule a check asked about is
// not there) is an answer, not an error.
bool EgressCounters::call(std::vector<std::string> argv, std::string &error, bool *refused) {
	const proc::CommandResult result = runner_.run(argv, kCommandTimeoutMs);
	if (refused)
		*refused = result.outcome == proc::CommandResult::Outcome::Exited && result.exit_code == 1;
	if (result.ok() || (refused && *refused))
		return true;
	std::string err = strutil::trim(result.err);
	if (const size_t eol = err.find('\n'); eol != std::string::npos)
		err.resize(eol);
	if (err.size() > 200)
		err.resize(200);
	std::string why;
	switch (result.outcome) {
		case proc::CommandResult::Outcome::Exited:
			why = " exited " + std::to_string(result.exit_code) + (err.empty() ? "" : ": " + err);
			break;
		case proc::CommandResult::Outcome::TimedOut:
			why = " timed out";
			break;
		case proc::CommandResult::Outcome::NotStarted:
			why = " did not start: " + result.error;
			break;
	}
	// The tool, the operation and its chain: "iptables -C OUTPUT exited 2: ...".
	error = argv[0] + " " + argv[3] + " " + argv[4] + why;
	return false;
}

bool EgressCounters::ensure(std::string &error) {
	for (const std::string &tool : tools_) {
		bool missing = false;
		if (!call({ tool, "-w", kLockWaitSeconds, "-L", kChain, "-n" }, error, &missing))
			return false;
		if (missing && !call({ tool, "-w", kLockWaitSeconds, "-N", kChain }, error))
			return false;
		if (!call({ tool, "-w", kLockWaitSeconds, "-C", "OUTPUT", "-j", kChain }, error, &missing))
			return false;
		if (missing &&
				!call({ tool, "-w", kLockWaitSeconds, "-I", "OUTPUT", "1", "-j", kChain }, error))
			return false;
		for (int slot = 0; slot < slots_; ++slot) {
			if (!call(rule_argv(tool, "-C", slot), error, &missing))
				return false;
			if (missing && !call(rule_argv(tool, "-A", slot), error))
				return false;
		}
	}
	return true;
}

bool EgressCounters::read(std::map<int, uint64_t> &out, std::string &error) {
	out.clear();
	bool whole = true;
	for (const std::string &tool : tools_) {
		const proc::CommandResult result =
				runner_.run({ tool, "-w", kLockWaitSeconds, "-L", kChain, "-n", "-v", "-x" },
						kCommandTimeoutMs);
		if (!result.ok()) {
			error = tool + " cannot list " + kChain;
			whole = false;
			continue;
		}
		std::map<uint32_t, uint64_t> by_uid;
		parse_chain_counters(result.out, by_uid);
		for (int slot = 0; slot < slots_; ++slot) {
			const auto it = by_uid.find(hosted::slot_uid(slot));
			if (it == by_uid.end()) {
				error = tool + "'s " + kChain + " lacks slot " + std::to_string(slot) + "'s rule";
				whole = false;
				continue;
			}
			out[slot] += it->second;
		}
	}
	return whole;
}

void parse_chain_counters(std::string_view listing, std::map<uint32_t, uint64_t> &out) {
	static constexpr std::string_view kMatch = "owner UID match ";
	size_t start = 0;
	while (start < listing.size()) {
		size_t end = listing.find('\n', start);
		if (end == std::string_view::npos)
			end = listing.size();
		const std::string_view line = listing.substr(start, end - start);
		start = end + 1;
		const size_t match = line.find(kMatch);
		if (match == std::string_view::npos)
			continue;
		const std::vector<std::string> words = words_of(line);
		const std::vector<std::string> uid_words = words_of(line.substr(match + kMatch.size()));
		if (words.size() < 2 || uid_words.empty() || !strutil::all_digits(words[0]) ||
				!strutil::all_digits(words[1]) || !strutil::all_digits(uid_words[0]) ||
				uid_words[0].size() > 10)
			continue;
		const auto bytes = strutil::parse_ullong(words[1]);
		const auto uid = strutil::parse_ullong(uid_words[0]);
		if (!bytes || !uid || *uid > 0xFFFFFFFFull)
			continue;
		out[static_cast<uint32_t>(*uid)] += *bytes;
	}
}

} // namespace opennova::hostd
