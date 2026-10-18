#include "hostd_options.h"
#include "machine_identity.h"

#include "hosted/agent_protocol.h"

#include <base/io/strutil.h>

#ifndef OPENNOVA_HOSTD_VERSION
#define OPENNOVA_HOSTD_VERSION "dev"
#endif

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

namespace {

bool absolute(const std::string &path) {
	return !path.empty() && path[0] == '/';
}

// A directory that reaches docker's --mount argument: absolute, of letters,
// digits and ./_- alone (no comma, which --mount splits on), no "." or ".."
// part, no trailing slash.
bool is_bind_path(const std::string &path) {
	if (!absolute(path) || path.size() > 255 || path.back() == '/')
		return false;
	size_t start = 1;
	while (start <= path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		if (part.empty() || part == "." || part == "..")
			return false;
		for (const char c : part)
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
						c == '.' || c == '_' || c == '-'))
				return false;
		start = end + 1;
	}
	return true;
}

} // namespace

const char *hostd_version() {
	return OPENNOVA_HOSTD_VERSION;
}

bool service_host_port(const std::string &url, std::string &host, uint16_t &port) {
	if (url.size() <= 7 || !strutil::starts_with_icase(url, "http://"))
		return false;
	std::string authority = url.substr(7);
	if (const size_t slash = authority.find('/'); slash != std::string::npos)
		authority.resize(slash);
	port = 80;
	if (const size_t colon = authority.rfind(':'); colon != std::string::npos) {
		const std::string digits = authority.substr(colon + 1);
		const auto value = strutil::all_digits(digits) && !digits.empty() && digits.size() <= 5
				? strutil::parse_int(digits)
				: std::nullopt;
		if (!value || *value < 1 || *value > 65535)
			return false;
		port = static_cast<uint16_t>(*value);
		authority.resize(colon);
	}
	host = authority;
	return !host.empty();
}

const char *hostd_usage() {
	return "usage: opennova-hostd --service <http://address:port> "
		   "(--machine <id> | --machine-file <path>) [options]\n"
		   "       opennova-hostd --check-alive [--state-dir <path>]\n"
		   "\n"
		   "The machine agent: runs the hosted servers the NovaWorld service places on this\n"
		   "machine, each an opennova-serve container on a slot of its own, and reports them.\n"
		   "Runs as root (the fleet cluster's ECS daemon). The containers keep running when\n"
		   "the agent or the service stops (with the process runtime they end with it).\n"
		   "\n"
		   "  --service <url>        the fleet's internal listener, http://<private "
		   "name or address>:<port>\n"
		   "  --machine <id>         this machine's id (the service's fleet_machines row)\n"
		   "  --machine-file <path>  or the file holding it (the user-data's\n"
		   "                         /etc/opennova/machine-id)\n"
		   "  --ecs-config <path>    the ECS agent's config: refuse to run unless its\n"
		   "                         ECS_INSTANCE_ATTRIBUTES machine-id is this machine's\n"
		   "  --token-file <path>    the one-time bootstrap token, deleted once redeemed\n"
		   "                         (default /etc/opennova/hostd.token)\n"
		   "  --state-dir <path>     the saved bootstrap and the scratch files\n"
		   "                         (default /var/lib/opennova-hostd)\n"
		   "  --slots-root <path>    the slots' working directories (default /srv/slots)\n"
		   "  --runtime <kind>       docker: each server a docker container (the default);\n"
		   "                         process: each an opennova-serve child of the agent, on a\n"
		   "                         machine that is itself a container (the local stack's)\n"
		   "  --trees-root <path>    the trees the containers' game data is fetched into\n"
		   "                         (default /opt/game)\n"
		   "  --docker <path>        the docker CLI (default /usr/bin/docker)\n"
		   "  --serve <path>         the process runtime's opennova-serve\n"
		   "                         (default /usr/local/bin/opennova-serve)\n"
		   "  --aws <path>           the AWS CLI a tree's `s3 sync` runs\n"
		   "                         (default /usr/local/bin/aws)\n"
		   "  --nft <path>           the slots' fences and counters (table inet opennova), or\n"
		   "                         none (default /usr/sbin/nft)\n"
		   "  --log-group <group>    with --log-region <region>: the containers' output to that\n"
		   "                         CloudWatch log group, a stream <machine>/slot-<k> each\n"
		   "                         (docker's awslogs driver; unset, the journal)\n"
		   "  --once                 one pass, then exit (0 when the sync was answered)\n"
		   "  --check-alive          exit 0 while a running agent's pass loop turns (the\n"
		   "                         ECS health check)\n"
		   "  --verbose              debug lines too\n"
		   "  --version, --help\n";
}

ParseOutcome parse_hostd_options(
		const std::vector<std::string> &args, HostdOptions &out, std::string &error) {
	bool machine_given = false;
	bool check_alive = false;
	std::string runtime = "docker";
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &arg = args[i];
		if (arg == "--help" || arg == "-h")
			return ParseOutcome::Help;
		if (arg == "--version")
			return ParseOutcome::Version;
		if (arg == "--check-alive") {
			check_alive = true;
			continue;
		}
		if (arg == "--once") {
			out.once = true;
			continue;
		}
		if (arg == "--verbose") {
			out.verbose = true;
			continue;
		}
		std::string *target = nullptr;
		if (arg == "--service")
			target = &out.service_url;
		else if (arg == "--token-file")
			target = &out.token_file;
		else if (arg == "--state-dir")
			target = &out.state_dir;
		else if (arg == "--slots-root")
			target = &out.slots_root;
		else if (arg == "--trees-root")
			target = &out.trees_root;
		else if (arg == "--docker")
			target = &out.docker;
		else if (arg == "--serve")
			target = &out.serve;
		else if (arg == "--runtime")
			target = &runtime;
		else if (arg == "--aws")
			target = &out.aws;
		else if (arg == "--nft")
			target = &out.nft;
		else if (arg == "--machine-file")
			target = &out.machine_file;
		else if (arg == "--ecs-config")
			target = &out.ecs_config;
		else if (arg == "--log-group")
			target = &out.log_group;
		else if (arg == "--log-region")
			target = &out.log_region;
		else if (arg != "--machine") {
			error = "unknown argument " + arg;
			return ParseOutcome::Error;
		}
		if (i + 1 >= args.size()) {
			error = arg + " needs a value";
			return ParseOutcome::Error;
		}
		const std::string &value = args[++i];
		if (target) {
			*target = value;
			continue;
		}
		if (!parse_machine_id(value, out.machine_id)) {
			error = "--machine takes a machine id, a whole number above 0";
			return ParseOutcome::Error;
		}
		machine_given = true;
	}
	if (!absolute(out.state_dir)) {
		error = "every path is absolute: " + out.state_dir;
		return ParseOutcome::Error;
	}
	// The health check reads the state directory alone.
	if (check_alive)
		return ParseOutcome::CheckAlive;
	std::string host;
	uint16_t port = 0;
	if (!service_host_port(out.service_url, host, port)) {
		error = "--service takes the service's http:// address";
		return ParseOutcome::Error;
	}
	if (machine_given == !out.machine_file.empty()) {
		error = "--machine or --machine-file is required, not both";
		return ParseOutcome::Error;
	}
	if (runtime == "docker") {
		out.runtime = RuntimeKind::Docker;
	} else if (runtime == "process") {
		out.runtime = RuntimeKind::Process;
	} else {
		error = "--runtime takes docker or process";
		return ParseOutcome::Error;
	}
	for (const std::string *path : { &out.token_file, &out.docker, &out.serve, &out.aws }) {
		if (!absolute(*path)) {
			error = "every path is absolute: " + *path;
			return ParseOutcome::Error;
		}
	}
	for (const std::string *path : { &out.machine_file, &out.ecs_config }) {
		if (!path->empty() && !absolute(*path)) {
			error = "every path is absolute: " + *path;
			return ParseOutcome::Error;
		}
	}
	if (out.nft != "none" && !absolute(out.nft)) {
		error = "--nft takes an absolute path or none";
		return ParseOutcome::Error;
	}
	if (out.log_group.empty() != out.log_region.empty()) {
		error = "--log-group and --log-region go together";
		return ParseOutcome::Error;
	}
	if (!out.log_group.empty()) {
		if (out.runtime != RuntimeKind::Docker) {
			error = "--log-group is the docker runtime's (the process runtime's servers log "
					"where the agent does)";
			return ParseOutcome::Error;
		}
		if (!hosted::is_log_group_name(out.log_group)) {
			error = "--log-group takes a log group name of letters, digits and ./_-";
			return ParseOutcome::Error;
		}
		if (!hosted::is_aws_region(out.log_region)) {
			error = "--log-region takes an AWS region (a-z, 0-9, -)";
			return ParseOutcome::Error;
		}
	}
	// Both reach docker's --mount argument, so they hold no comma or odd part.
	if (!is_bind_path(out.slots_root)) {
		error = "--slots-root takes an absolute path of letters, digits and ./_-";
		return ParseOutcome::Error;
	}
	if (!is_bind_path(out.trees_root)) {
		error = "--trees-root takes an absolute path of letters, digits and ./_-";
		return ParseOutcome::Error;
	}
	return ParseOutcome::Run;
}

} // namespace opennova::hostd
