#pragma once

// Internal to engine/formats/mission — not part of the public interface.
//
// Display names for the BMS event-logic enums. Record conversions stamp them into
// each typed record, so they live in one shared implementation.

#include <string>

namespace opennova::mission::detail {

std::string trigger_main_type_name(int value);

std::string trigger_sub_type_name(int main_type, int sub_type);

std::string action_type_name(int value);

std::string action_sub_type_name(int action_type, int sub_type);

} // namespace opennova::mission::detail
