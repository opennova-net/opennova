#pragma once

// How the cloud provider (ec2_provider.h) runs a command: the AWS CLI v2, from
// an argv array with no shell between (nothing in an argument is ever
// interpreted), bounded by a timeout, its standard output (the reply's JSON)
// kept apart from its standard error (the CLI's diagnostics). The seam the
// provider's tests replace with a stub that checks each argv and answers it
// (tests/novaworld/hosted_ec2_provider_test.cpp). Platform plumbing, not a port.

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct CommandResult {
	enum class Outcome {
		Exited,     // the program ran to its end: exit_code says how
		TimedOut,   // killed at the timeout
		NotStarted, // never ran: `error` says why
	};
	Outcome outcome = Outcome::NotStarted;
	int exit_code = -1;
	std::string out; // standard output
	std::string err; // standard error
	std::string error;

	bool ok() const { return outcome == Outcome::Exited && exit_code == 0; }
};

class CommandRunner {
public:
	virtual ~CommandRunner() = default;
	// Runs argv[0] (an absolute path, run as given) with the rest as its
	// arguments, waiting at most `timeout_ms` for it to end.
	virtual CommandResult run(const std::vector<std::string> &argv, int timeout_ms) = 0;
};

// The real runner: apps/common/child_process.h, its standard output and error
// each in a file of its own under `scratch_dir` (owner-only), read back and
// deleted once it ends. The child's environment is `environment` alone (the
// NAME=value entries the service hands it: the AWS CLI's own variables, never
// the service's secrets). Each stream keeps at most kMaxOutputBytes.
class ProcessCommandRunner : public CommandRunner {
public:
	static constexpr size_t kMaxOutputBytes = 16u << 20;

	ProcessCommandRunner(std::vector<std::string> environment, std::string scratch_dir);
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override;

	// The variables a run keeps from the service's environment (a name ending
	// in '*' is a prefix): the system's own and the AWS CLI's (AWS_*, its
	// region and retry settings; its credentials come from the instance role).
	static const std::vector<std::string> kEnvironment;
	// kEnvironment's entries of this process's environment, with the CLI's
	// standard retry mode (AWS_RETRY_MODE=standard, AWS_MAX_ATTEMPTS=4) unless
	// the service names its own.
	static std::vector<std::string> service_environment();

private:
	std::vector<std::string> environment_;
	std::string scratch_dir_;
};

} // namespace opennova::novaworld_server::hosted
