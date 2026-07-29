#pragma once

// Internal to libs/mission — not part of the public interface. Split out of
// mission.cpp (quality campaign W3-1); the bodies are unchanged.
//
// The .mis text format, one TU per direction: mission_mis_writer.cpp emits it and
// mission_mis_parser.cpp reads it. Only these two entry points cross a TU boundary;
// every section writer and token parser stays private to its own file.

#include "mission/mission.h"

#include <string>
#include <vector>

namespace opennova::mission::detail {

bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                    const std::vector<int32_t> *base_heights = nullptr);

bool parse_mis_text_to_bms(const std::string &text, bms::File &out, std::string &error);

} // namespace opennova::mission::detail
