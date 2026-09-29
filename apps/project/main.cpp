// opennova-project: the OpenNova Editor's project core on the command line
// (ADR 0046 d4). See commands.h for the commands and exit codes.
#include "commands.h"

#include <cstdio>

int main(int argc, char **argv) {
	return opennova::project_cli::run_project_command(argc - 1, argv + 1, stdout, stderr);
}
