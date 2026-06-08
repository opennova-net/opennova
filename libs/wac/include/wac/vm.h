// WAC bytecode VM.
//
// Faithful port of WacScript_ExecuteBytecode @0x4f58b0: a central accumulator,
// 4-byte instruction stream, opcode-driven control flow + ALU folds, and command
// dispatch by table index. Re-runs the whole program each logic tick against the
// shared World. Holds the per-event temporal state (fired-tick / active flag) and
// the DORND LCG seed across ticks.
#ifndef OPENNOVA_WAC_VM_H
#define OPENNOVA_WAC_VM_H

#include <cstdint>
#include <vector>

#include "wac/program.h"

namespace opennova::world {
class World;
}

namespace opennova::wac {

// Per-event temporal record. [orig: dword_C6BE40 (fired tick) / byte_C6DE40
// (active flag) / event-fired bookkeeping.]
struct EventState {
    uint32_t last_fired_tick = 0;
    bool ever_fired = false;
    bool active = false;
    uint32_t fired_count = 0;
};

class WacVm {
public:
    // Bind a compiled program (sizes the per-event state). Resets temporal state.
    void load(const Program &program);

    // Execute one logic tick against the world (caller supplies authority).
    void execute(opennova::world::World &world);

    bool loaded() const { return prog_ != nullptr; }
    const std::vector<EventState> &events() const { return events_; }
    int32_t accumulator() const { return acc_; }

private:
    const Program *prog_ = nullptr;
    std::vector<EventState> events_;
    uint32_t rng_seed_ = 0x12345633u; // [orig: dword_C6EA40 init]
    int32_t acc_ = 0;
    int cur_event_ = 0;

    int32_t read(opennova::world::World &w, uint32_t ref) const;
    void write(opennova::world::World &w, uint32_t ref, int32_t v) const;
    int32_t arg_as_string_index(uint32_t ref) const; // for string-typed operands
    uint32_t next_rand();
    int32_t rand_range(int n);

    // Command dispatch (implemented subset; others recorded as effects).
    int32_t dispatch(opennova::world::World &w, int cmd_index, const uint32_t *args, int argc);
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_VM_H
