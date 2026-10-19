#pragma once

// The website's prunes as periodic jobs (maintenance_jobs.h; with the HTTP
// layer alone), every sweep interval, each its own job:
//
//   web_sessions.prune          website sessions past their (sliding) expiry
//   active_user_sessions.prune  retail logins' session rows unused for
//                               kRetailLoginIdleSeconds (accounts/auth.h), once a
//                               sweep every 64th POST /NWLogin.dll ran on
//                               the login's own thread
//   login_addresses.prune       login addresses not used for kLoginAddressDays
//   password_resets.prune       password reset links past their hour
//
// Service policy, not a port.

#include "jobs/maintenance_jobs.h"

#include <vector>

namespace opennova::novaworld_server::jobs {

inline constexpr const char *kJobWebSessionsPrune = "web_sessions.prune";
inline constexpr const char *kJobActiveSessionsPrune = "active_user_sessions.prune";
inline constexpr const char *kJobLoginAddressesPrune = "login_addresses.prune";
inline constexpr const char *kJobPasswordResetsPrune = "password_resets.prune";

void register_account_jobs(JobRegistry &registry);
std::vector<PeriodicJob> account_schedule(const MaintenanceConfig &config);

} // namespace opennova::novaworld_server::jobs
