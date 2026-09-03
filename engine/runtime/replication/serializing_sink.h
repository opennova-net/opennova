#pragma once

#include <cstdint>

#include <runtime/world/net_command_sink.h>

#include <runtime/session/session_transport.h>

namespace opennova::netsim {

// Replaces world::LocalSink for the SP in-process listen server. The host is
// authoritative for every entity (is_authority stays true), but send_command — the
// entity-targeted command boundary WAC/BMS funnel through — routes onto the loopback
// instead of the LocalSink no-op [orig: the entity-command serialize ->
// NapiNPServer_SendFiltered(..., 0x23, ...) path, the world/net_command_sink.h seam]. ADR 0009
// Decision 2: gameplay systems keep calling the same INetCommandSink and stay
// network-unaware.
//
// STAGED, NOT WIRED (2026-08-27 tidy): no production World points its `net` here
// yet — `World::local_sink` stays the SP default and send_command is a structural
// no-op, because the `0x23` entity-command body is unwitnessed and the sink has zero
// callers (engine/runtime/session/ROADMAP.md, P8: never invent bytes). The live owner is
// the in-match session's host role (engine/runtime/session/session.*), which installs this
// sink in place of LocalSink once `NapiNPServer_SendFiltered` 0x23 is grilled and the
// serialize leg ported. tests/netsim/loopback_identity_test.cpp pins the seam shape.
struct SerializingSink : world::INetCommandSink {
	explicit SerializingSink(ISessionTransport &channel) : channel_(channel) {}

	bool is_authority(world::EntityHandle) override { return true; }
	void send_command(world::EntityHandle owner, uint16_t command_id,
	                  const int32_t *args, int argc) override;

private:
	ISessionTransport &channel_;
};

} // namespace opennova::netsim
