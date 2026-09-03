// The joiner role: the non-authority client of a remote host. It owns the
// joiner's ClientRuntime and the world bridge that folds the decoded replica
// into the kernel's world every fixed tick. ADR 0043 d3 (slice E8a): the role
// replaces the shell's joiner pump; the bridge's shell-side hooks stay the
// shell's until slice E8b splits them into engine legs and typed events.
#pragma once

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_world_bridge.h>
#include <runtime/inmatch/session.h>

#include <cstdint>
#include <memory>
#include <string>

namespace opennova::inmatch {

class JoinerRole final : public Role {
public:
	RoleKind kind() const override { return RoleKind::Joiner; }

	// The joiner's replica runtime and the world bridge; the shell reads both.
	std::unique_ptr<ClientRuntime> runtime;
	JoinerWorldBridge bridge;
	// The bridge's shell-side legs (E8a: unchanged from the shell's pump).
	JoinerWorldBridge::PumpHooks hooks;

	// (Re)build the runtime for a join or a load that rebuilds an as-yet
	// unstarted joiner; the shell installs its tables on the new runtime after.
	ClientRuntime &create_runtime(const std::string &player_name, JoinRole join_role,
			const std::string &spectator_password);
	void bind(mission::MissionKernel &kernel) override;

	bool spectator() const override { return runtime && runtime->is_spectator(); }
	bool send_medic_request() override { return runtime && runtime->queue_medic_request(); }
	void run_tick(const TickInput &input) override;
	bool session_lost(SessionError &error) const override;
	bool reset_to_baseline(SessionError &error) override;
	void close() override;
	ClientRuntime *client_runtime() override { return runtime.get(); }
	int64_t last_net_us() const override { return last_net_us_; }

private:
	int64_t last_net_us_ = 0;
};

} // namespace opennova::inmatch
