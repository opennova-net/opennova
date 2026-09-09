#pragma once

#include <cstdint>

namespace opennova::world {
// Read-only access to the BMS event owner's live active latch. Registered
// systems outlive their World use; no event state is copied into the WAC VM.
// [orig: WacCmd_Event @0x4ED1E0 reads event+20 directly]
struct IScriptEventQuery {
    virtual ~IScriptEventQuery() = default;
    virtual bool is_active(int32_t index) const = 0;
};
} // namespace opennova::world
