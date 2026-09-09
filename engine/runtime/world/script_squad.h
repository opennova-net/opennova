#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {

// The four WAC squad events and the selected named-value words. The original
// publisher is retained as a native entry point; no caller exists in the JO
// executable, so this module does not invent AI/UI event producers.
class ScriptSquadEvents {
public:
    // [orig: WacScript_PublishSquadEvent @0x4F0390] First minimum signed TTL,
    // with the comparison ceiling initially 10000. Store raw argument words.
    int32_t publish(int32_t ssn, int32_t who, int32_t event, int32_t ttl) {
        int selected = 0;
        int32_t minimum = 10000;
        for (int i = 0; i < 4; ++i) {
            if (events_[i].ttl < minimum) {
                minimum = events_[i].ttl;
                selected = i;
            }
        }
        events_[selected] = {ssn, who, event, ttl};
        return event;
    }

    // [orig: WacCmd_SquadEvent @0x4ED070] A miss preserves both exports and
    // the selected slot. A match requires nonzero TTL, not a positive TTL.
    bool query(int32_t event) {
        for (int i = 0; i < 4; ++i) {
            if (events_[i].ttl != 0 && events_[i].event == event) {
                selected_ = i;
                selected_ssn = events_[i].ssn;
                selected_who = events_[i].who;
                return true;
            }
        }
        return false;
    }

    // [orig: WacCmd_SquadClear @0x4ED390] The selected index is retained.
    int32_t clear_selected() {
        events_[selected_] = {};
        selected_ssn = selected_who = 0;
        return 0;
    }

    // [orig: WacScript_DecrementTimersAndResetCounters @0x4EE6D0] Called
    // after each bytecode execution, including initial execution.
    void advance_execution() {
        for (auto &event : events_)
            if (event.ttl != 0) event.ttl = int32_t(uint32_t(event.ttl) - 1u);
    }

    // Mutable named values in the table at 0x82EEF0.
    int32_t selected_ssn = 0; // @0xC60DCC; packed entity handle when consumed as SSN
    int32_t selected_who = 0; // @0xC60DC4

private:
    struct Event { int32_t ssn = 0, who = 0, event = 0, ttl = 0; };
    std::array<Event, 4> events_{};
    int selected_ = 0;
};

} // namespace opennova::world
