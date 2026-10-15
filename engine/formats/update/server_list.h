#pragma once

// SERVER.LST, the gateway's list of update servers. docs/update/updater-re.md §5.
// [orig: Cupdate2Dlg_ReadServerList @ 0x408070 (UPDATE.EXE)]
//
// STAGED, NOT WIRED: consumed by apps/novaworld_server/update (the listener, ADR 0061 phase 2) and
// apps/update (phase 3); until those land only tests/update include it.

#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// Each line's first token (separators space, tab, CR, LF) is a host, at most 259 characters;
// lines with no token are skipped.
std::vector<std::string> read_server_list(std::string_view text);

// One host per CR LF line. Empty when a host is empty, holds a separator, or does not fit.
std::string write_server_list(const std::vector<std::string> &hosts);

} // namespace opennova::update
