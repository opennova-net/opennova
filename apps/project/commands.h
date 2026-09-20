#pragma once
// The opennova-project commands (ADR 0046 d4): the same project core the editor uses,
// driven from a command line so modders and CI can create, inspect and validate a
// project without the editor, and so the ctest suite drives the real commands.
//
//   opennova-project new <dir> [--title <text>] [--game <code>]
//   opennova-project status <dir>
//   opennova-project validate <dir>
//   opennova-project create-missing <dir> [--role <token>]
//   opennova-project build <dir> [--out <dir>]
//
// Exit 0 on success (validate: no errors and no unmet required row; create-missing:
// every missing required file created; build: a directory the runtime boots), 1 when
// validate found errors or unmet requirements, create-missing left a required file
// uncreated, or the build was blocked or failed, 2 on a usage error or a project that
// could not be created or opened.
#include <cstdio>

namespace opennova::project_cli {

// `argv[0]` is the command word; `argc` counts it. Output goes to `out`, errors to `err`.
int run_project_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err);

} // namespace opennova::project_cli
