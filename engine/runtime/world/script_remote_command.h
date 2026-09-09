// The WAC command the host VM replicates over S2C 0x23.
//
// One resolved operand per declared registry parameter, in the form the wire
// carries it: Text/Filename operands as the string, Ssn operands as the packed
// 16-bit handle, every other type as the resolved dword. The world cannot
// send, so the VM queues the record on World::out and the server tick drains
// it; a client-side World never fills it.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — flags gate @0x4f5ca5, wire index
//  @0x4f5cb5..0x4f5cce, payload @0x4f5cf9..0x4f5dc2, targeted arm
//  @0x4f5dd1..0x4f5e8b (send_mask 0x20), broadcast arm @0x4f5ec7..0x4f5ed1
//  (send_mask 0x90)]
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/entity.h>

namespace opennova::world {

struct ScriptRemoteArg {
    int32_t value = 0;  // Ssn (u16 handle) and every numeric operand
    std::string text;   // Text / Filename operands
};

struct ScriptRemoteCommand {
    // The wire index: the FIRST registry row sharing the handler, so ptext,
    // pwave and pconsol travel as text, wave and consol.
    uint16_t command_index = 0;
    std::vector<ScriptRemoteArg> args;
    // A flags-0x10 record reaches only the connection whose active player slot
    // owns `target`; a flags-0x08 record reaches every in-match remote.
    bool targeted = false;
    EntityHandle target;
};

} // namespace opennova::world
