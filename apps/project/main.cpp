// opennova-project: the OpenNova Editor's project session on the command line
// (ADR 0046 d4, S13 A7). See cli_verbs.h for the verbs and exit codes.
#include "cli_verbs.h"

#include <cstdio>

int main(int argc, char **argv) {
	return opennova::project_cli::run_project_command(argc - 1, argv + 1, stdout, stderr);
}
