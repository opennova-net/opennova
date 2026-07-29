#pragma once

#include <cstdint>

#include "world/entity.h"

// The entity-command network seam: INetCommandSink + the in-process
// LocalSink default. Split from the world.h umbrella (W3-7) so wire-side
// sinks (netsim) depend on the seam, not the aggregate.

namespace opennova::world {

// ----------------------------------------------------------------------------
// Replication seam. [orig: entity-targeted commands serialize to a NAPI payload
// and NapiNPServer_SendFiltered(..., 0x23, ...) to the owner when the target is
// not local.] Single-player uses LocalSink (always authoritative, run locally).
// ----------------------------------------------------------------------------
struct INetCommandSink {
    virtual ~INetCommandSink() = default;
    virtual bool is_authority(EntityHandle target) = 0;
    virtual void send_command(EntityHandle owner, uint16_t command_id,
                              const int32_t *args, int argc) = 0;
};

struct LocalSink : INetCommandSink {
    bool is_authority(EntityHandle) override { return true; }
    void send_command(EntityHandle, uint16_t, const int32_t *, int) override {}
};

} // namespace opennova::world
