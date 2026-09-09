// Retail WAC quick-reference and event-description exports.
#pragma once

namespace opennova::wac {

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
