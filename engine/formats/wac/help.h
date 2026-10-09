// Retail WAC quick-reference and event-description exports.
#pragma once

#include <string>

namespace opennova::wac {

struct CommandDef;

// A command as the help file lists it, "name (type, type)": its name, a space, then its parameter
// slots' type names in parentheses, an empty slot skipped, ", " before every slot but the first
// [orig: WacScript_DumpActionDefsToFile @0x4F0400; the same form behind the compiler's parameter
// error, WacScript_FormatActionParameters @0x4EFC20].
std::string command_signature(const CommandDef &command);

struct HelpExportResult {
    bool help_written = false;
    bool events_written = false;
};

// Writes help.wac, then events.xml, in the process working directory. An
// unsuccessful help open prevents the XML attempt. Each successful open
// produces its own saved-file notification at the VM boundary.
// [orig: WacCmd_Help @0x4F6DE0]
HelpExportResult export_help();

} // namespace opennova::wac
