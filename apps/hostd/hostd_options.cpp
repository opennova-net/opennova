#include "hostd_options.h"

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

const char *hostd_usage() {
	return "usage: opennova-hostd --service <http://address:port> --machine <id> [options]\n"
		   "\n"
		   "The machine agent: runs the hosted servers the NovaWorld service places on this\n"
		   "machine, each an opennova-serve container on a slot of its own, and reports them.\n"
		   "Runs as root (systemd). The containers keep running when the agent or the service\n"
		   "stops (with the process runtime they end with it).\n"
		   "\n"
		   "  --service <url>        the fleet's internal listener, http://<private "
		   "name or address>:<port>\n"
		   "  --machine <id>         this machine's id (the service's fleet_machines row)\n"
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
		   "  --iptables <path>      the per-slot egress counters' iptables, or none\n"
		   "                         (default /usr/sbin/iptables)\n"
		   "  --ip6tables <path>     their ip6tables where the kernel has IPv6, or none\n"
		   "                         (default /usr/sbin/ip6tables)\n"
		   "  --log-group <group>    with --log-region <region>: the containers' output to that\n"
		   "                         CloudWatch log group, a stream <machine>/slot-<k> each\n"
		   "                         (docker's awslogs driver; unset, the journal)\n"
		   "  --once                 one pass, then exit (0 when the sync was answered)\n"
		   "  --verbose              debug lines too\n"
		   "  --version, --help\n";
}

ParseOutcome parse_hostd_options(
		const std::vector<std::string> &args, HostdOptions &out, std::string &error) {
	bool machine_given = false;
	std::string runtime = "docker";
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &arg = args[i];
		if (arg == "--help" || arg == "-h")
			return ParseOutcome::Help;
		if (arg == "--version")
			return ParseOutcome::Version;
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
		else if (arg == "--iptables")
			target = &out.iptables;
		else if (arg == "--ip6tables")
			target = &out.ip6tables;
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
		const auto id = strutil::all_digits(value) && value.size() <= 15
				? strutil::parse_llong(value)
				: std::nullopt;
		if (!id || *id <= 0) {
			error = "--machine takes a machine id, a whole number above 0";
			return ParseOutcome::Error;
		}
		out.machine_id = *id;
		machine_given = true;
	}
	if (out.service_url.size() <= 7 || !strutil::starts_with_icase(out.service_url, "http://")) {
		error = "--service takes the service's http:// address";
		return ParseOutcome::Error;
	}
	if (!machine_given) {
		error = "--machine is required";
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
	for (const std::string *path : { &out.token_file, &out.state_dir, &out.docker, &out.serve, &out.aws }) {
		if (!absolute(*path)) {
			error = "every path is absolute: " + *path;
			return ParseOutcome::Error;
		}
	}
	for (const std::string *tool : { &out.iptables, &out.ip6tables }) {
		if (*tool != "none" && !absolute(*tool)) {
			error = "--iptables and --ip6tables take an absolute path or none";
			return ParseOutcome::Error;
		}
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
