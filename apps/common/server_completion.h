#pragma once
#include <string_view>

namespace opennova {
// Reserved --exit-file reasons for an intentional session completion. A zero
// process status without one of these still follows the crash recovery policy.
inline constexpr std::string_view kRotationCompleted = "completed:rotation";
inline constexpr std::string_view kAdminSessionEnded = "completed:admin-session";
inline bool server_completed(int code, std::string_view reason) {
	return code == 0 && (reason == kRotationCompleted || reason == kAdminSessionEnded);
}
} //namespace opennova
