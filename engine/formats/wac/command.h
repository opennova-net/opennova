// The WAC command registry — the ISA.
//
// One C++ table is the source of truth for the 165 WAC keywords: name, kind
// (condition/action), replication flags, param types, and derived argc/call_conv.
// Extracted from the binary command table (Jointops.exe @0x82D290, 165 x 44-byte
// records); argc/call_conv derived per the WacScript_InitAndLoad classification.
// The generated rows live in command_table.gen.cpp.
#ifndef OPENNOVA_WAC_COMMAND_H
#define OPENNOVA_WAC_COMMAND_H

#include <cstdint>
#include <string_view>

#include "wac/param_type.h"

namespace opennova::wac {

struct CommandDef {
    const char *name;     // WAC keyword (<= 18 chars)
    uint8_t flags;        // raw flags byte (+0 in the original record)
    uint8_t argc;         // count of non-null param slots (derived)
    uint8_t call_conv;    // VM calling convention 0-14 (derived)
    ParamType params[4];
    uint32_t handler_ea;  // original handler address (reference/comment only)
};

// Flag bit semantics (faithful to the original record flags byte).
inline constexpr bool cmd_is_condition(const CommandDef &c) { return (c.flags & 0xFE) == 0; }
inline constexpr bool cmd_is_replicated(const CommandDef &c) { return (c.flags & 0x18) != 0; }

// The registry (generated). 165 entries; index == bytecode command id.
const CommandDef *wac_commands();
int wac_command_count();

// Case-insensitive keyword lookup. Returns nullptr / -1 if absent.
const CommandDef *wac_find_command(std::string_view name);
int wac_command_index(std::string_view name);

} // namespace opennova::wac

#endif // OPENNOVA_WAC_COMMAND_H
