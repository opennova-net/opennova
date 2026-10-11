#pragma once

// opennova-parity's command line, in the core library so the tests drive it in
// process:
//
//   opennova-parity --items <items.def> --rr <runDir> --rr-aa <runDir>
//                   [--ro <runDir>] [--or <runDir>] [--oo <runDir>]
//                   [--out <verdict.json>] [--evidence summary|external]
//
// Prints PARITY_FAIL lines for the first failures, then PARITY_STATUS=PASS|FAIL,
// PARITY_FAILURES=<n> and PARITY_REPORT=<path>. Returns 0 on PASS, 1 on FAIL,
// 2 on a usage or input error (nothing judged).

#include <cstdio>
#include <string>
#include <vector>

namespace opennova::parity {

inline constexpr int kExitPass = 0;
inline constexpr int kExitFail = 1;
inline constexpr int kExitUsage = 2;

// `args` excludes the program name.
int run_cli(const std::vector<std::string> &args, std::FILE *out, std::FILE *err);

} // namespace opennova::parity
