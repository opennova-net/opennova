// opennova-parity: the retail-LAN parity verdict over one case's topology runs
// (ADR 0050 R4). The command line is cli.h's.

#include "cli.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i)
		args.emplace_back(argv[i]);
	return opennova::parity::run_cli(args, stdout, stderr);
}
