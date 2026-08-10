#pragma once

#include <cstdint>

#include <world/net_command_sink.h>

#include "netsim/session_transport.h"

namespace opennova::netsim {

// Replaces world::LocalSink for the SP in-process listen server. The host is
// authoritative for every entity (is_authority stays true), but send_command — the
// entity-targeted command boundary WAC/BMS funnel through — routes onto the loopback
// instead of the LocalSink no-op [orig: the entity-command serialize ->
// NapiNPServer_SendFiltered(..., 0x23, ...) path, the world/net_command_sink.h seam]. ADR 0009
// Decision 2: gameplay systems keep calling the same INetCommandSink and stay
// network-unaware.
//
// Phase 1 keeps send_command a no-op shim so the seam is wired (World::net points
// here); Phase 2+ serializes the command onto the channel.
struct SerializingSink : world::INetCommandSink {
	explicit SerializingSink(ISessionTransport &channel) : channel_(channel) {}

	bool is_authority(world::EntityHandle) override { return true; }
	void send_command(world::EntityHandle owner, uint16_t command_id,
	                  const int32_t *args, int argc) override;

private:
	ISessionTransport &channel_;
};

} // namespace opennova::netsim
