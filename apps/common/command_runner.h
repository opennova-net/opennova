#pragma once

// How an app runs a command-line tool and reads its answer: from an argv array
// with no shell between (nothing in an argument is ever interpreted), bounded
// by a timeout, its standard output (the reply) kept apart from its standard
// error (the tool's diagnostics). The NovaWorld service's EC2 machine driver runs
// the AWS CLI v2 through it (apps/novaworld_common/hosted/cloud/ec2_machine_driver.h),
// its billing curl (apps/novaworld_common/billing/stripe_api.h), and
// opennova-hostd the docker and iptables CLIs (apps/hostd). The seam their
// tests replace with a stub that checks each argv and answers it
// (tests/novaworld/hosted_ec2_machine_driver_test.cpp, billing_stripe_test.cpp,
// tests/hostd; the runner itself: command_runner_test.cpp). In base/proc's
// namespace, as apps/common's sockets are in net's: the app layer's runner over
// base/proc/child_process.h. Platform plumbing, not a port.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::proc {

struct CommandResult {
	enum class Outcome {
		Exited,     // the program ran to its end: exit_code says how
		TimedOut,   // killed at the timeout
		NotStarted, // never ran: `error` says why
	};
	Outcome outcome = Outcome::NotStarted;
	uint32_t exit_code = 0; // Exited: the program's code, unsigned as base/proc reads it
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

// The real runner: base/proc/child_process.h, its standard output and error
// each in a file of its own under `scratch_dir` (owner-only), read back and
// deleted once it ends. The child's environment is `environment` alone (the
// NAME=value entries its app hands it, never the app's secrets). Each stream
// keeps at most kMaxOutputBytes.
class ProcessCommandRunner : public CommandRunner {
public:
	static constexpr size_t kMaxOutputBytes = 16u << 20;

	ProcessCommandRunner(std::vector<std::string> environment, std::string scratch_dir);
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override;

	// The variables the NovaWorld service's AWS CLI runs keep from its
	// environment (a name ending in '*' is a prefix): the system's own and the
	// AWS CLI's (AWS_*, its region and retry settings; its credentials come
	// from the instance role).
	static const std::vector<std::string> kEnvironment;
	// kEnvironment's entries of this process's environment, with the CLI's
	// standard retry mode (AWS_RETRY_MODE=standard, AWS_MAX_ATTEMPTS=4) unless
	// the service names its own.
	static std::vector<std::string> service_environment();

private:
	std::vector<std::string> environment_;
	std::string scratch_dir_;
};

} // namespace opennova::proc
