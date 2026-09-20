#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// The resource kind vocabulary the runtime catalog tags files with ("mission",
// "terrain", "object_model", "menu", "strings", ...): one classifier shared by
// ResourceIndex::scan and by the editor's asset registry (ADR 0046 d9: one
// implementation per fact). An empty kind means "not a browsable kind".

// Lower-cased extension of `name` including the dot ("" when there is none).
std::string resource_extension_for_name(const std::string &name);

// The two `.bin` content peeks: RTXT string tables carry "RTXT", SCR0-wrapped music
// scripts carry "SCR0"; a `.bin` with neither is raw.
bool resource_bin_has_rtxt_magic(const std::vector<uint8_t> &bytes);
bool resource_bin_has_scr_magic(const std::vector<uint8_t> &bytes);

// Kind for a logical name, given the `.bin` peeks (both false for a non-.bin name).
std::string resource_kind_for_name_and_magic(const std::string &name, bool is_rtxt_bin,
                                             bool is_scr_bin);

// Kind for a logical name with its decoded bytes at hand (`bytes` may be null when
// the caller has none; only `.bin` names are peeked).
std::string resource_kind_for_file(const std::string &name, const std::vector<uint8_t> *bytes);

} // namespace opennova
