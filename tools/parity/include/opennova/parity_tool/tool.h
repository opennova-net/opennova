#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace opennova::parity_tool {

inline constexpr int kExitClean = 0;
inline constexpr int kExitMismatch = 2;
inline constexpr int kExitInvalidEvidence = 3;
inline constexpr int kExitUsageOrIo = 64;

// Runs the parity tool command without process-global I/O. `args` excludes the
// executable name, matching argv[1..]. This is the public command seam used by
// the executable and its synthetic integration tests.
[[nodiscard]] int run(const std::vector<std::string>& args,
                      std::ostream& output,
                      std::ostream& error);

}  // namespace opennova::parity_tool
