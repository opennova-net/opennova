#pragma once

// Internal to libs/mission — not part of the public interface. Split out of
// mission.cpp (quality campaign W3-1); the bodies are unchanged.
//
// Display names for the BMS event-logic enums. The facade serves them to the editor
// (trigger_main_types(), action_types(), ...) and the record conversions stamp them
// into each typed record, so they need one home rather than two copies.

#include <string>

namespace opennova::mission::detail {

std::string trigger_main_type_name(int value);

std::string trigger_sub_type_name(int main_type, int sub_type);

std::string action_type_name(int value);

std::string action_sub_type_name(int action_type, int sub_type);

} // namespace opennova::mission::detail
