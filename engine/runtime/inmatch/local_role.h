// The bare local role: the no-net editor/unit tick over a kernel with no
// session. ADR 0043 d3: the role replaces MissionKernel::tick_no_net and the
// embedder's medic-cooldown leg around it.
#pragma once

#include <runtime/inmatch/session.h>

namespace opennova::inmatch {

class LocalRole final : public Role {
public:
	RoleKind kind() const override { return RoleKind::SinglePlayer; }
	void run_tick(const TickInput &input) override;
	bool reset_to_baseline(SessionError &error) override;
	void close() override;
};

} // namespace opennova::inmatch
