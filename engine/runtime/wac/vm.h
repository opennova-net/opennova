// WAC bytecode VM.
//
// Faithful port of WacScript_ExecuteBytecode @0x4f58b0: a central accumulator,
// 4-byte instruction stream, opcode-driven control flow + ALU folds, and command
// dispatch by table index. Re-runs the whole program each logic tick against the
// shared World. Holds the per-event temporal state (fired-tick / active flag) and
// the DORND LCG seed across ticks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/wac/command.h>
#include <formats/wac/program.h>
#include <runtime/world/entity.h>
#include <runtime/world/script_remote_command.h>

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
	// Copyable execution state used by the mission-start restore baseline. The
	// compiled Program remains owned by WacSystem; restore_runtime_state binds
	// this state back to that owner's current program.
	struct RuntimeState {
		std::vector<EventState> events;
        std::vector<uint8_t> loop_counters;
        std::vector<uint8_t> loop_choices;
		uint32_t rng_seed = 0x12333333u;
		int32_t accumulator = 0;
		int current_event = 0;
		uint32_t time = 0;
        std::vector<uint16_t> entity_bindings;
        uint32_t auto_item = 0xFFFF;
	};

    // Bind a compiled program (sizes the per-event state). Resets temporal state.
    void load(const Program &program);

    // Execute the program once against the world (caller supplies authority and
    // the every-62nd-tick cadence; see WacSystem).
    void execute(opennova::world::World &world);

    bool loaded() const { return prog_ != nullptr; }
    const std::vector<EventState> &events() const { return events_; }
    int32_t accumulator() const { return acc_; }

    // The mutable WAC time word. The callers of an execution increment it
    // (about once per second), never the bytecode run itself, and scripts may
    // also write it through Ticks. Temporal commands and shared script
    // admission read this same clock, not engine ticks or diagnostic execution
    // counts. [orig: wac_var_ticks @0xC6EAD8; WacScript_AdvanceTick @0x4F81D3
    // and WacScript_InitAndLoad @0x4F9770 increment it after their calls]
    uint32_t time() const { return time_; }
    void advance_time() { ++time_; }
    // Executed CALL instructions since load(), the unsupported-command ones
    // included: the dispatch sweep's proof that every registry row ran.
    uint64_t dispatch_count() const { return dispatch_count_; }
	RuntimeState capture_runtime_state() const;
	void restore_runtime_state(const Program &program, const RuntimeState &state);
	// The dword behind a variable, event or engine operand as it stands now
	// (0 for any other kind): what a new compile's GLOOP operand reads before
	// the load resets it. [orig: Script_Compile @0x4F368A]
	int32_t current_value(opennova::world::World &world, uint32_t ref) const;

private:
    const Program *prog_ = nullptr;
    std::vector<EventState> events_;
    std::vector<uint8_t> loop_counters_;
    std::vector<uint8_t> loop_choices_;
    uint32_t rng_seed_ = 0x12333333u; // [orig: WacScript_InitAndLoad @ 0x4f966b — mov dword_C6EA40, 0x12333333]
    int32_t acc_ = 0;
    int cur_event_ = 0;
    uint32_t time_ = 0; // [orig: wac_var_ticks]
    uint64_t dispatch_count_ = 0;
    std::vector<uint16_t> entity_bindings_;
    uint32_t auto_item_ = 0xFFFF; // mutable DWORD; entity selection changes only LOWORD
    int32_t cached_mana_ = 0;
    int32_t cached_game_over_ = 0, cached_win_ = 0, cached_lose_ = 0;
    int32_t cached_tod_ = 0;
    int32_t scratch_ = 0; // the unresolved-parameter sink, zeroed at entry [orig: dword_C6EAEC]
    void cache_player_state(opennova::world::World &);
    void bind_auto_handle(uint16_t handle) {
        // [orig: cache @0x4F5814/@0x4F58A2; group @0x4F5B7E/@0x4F5BAF/@0x4F5BD2]
        auto_item_ = (auto_item_ & 0xFFFF0000u) | handle;
    }
    std::vector<std::vector<world::EntityHandle>> groups_;

    int32_t read(opennova::world::World &w, uint32_t ref) const;
    void write(opennova::world::World &w, uint32_t ref, int32_t v);
    std::string operand_string(opennova::world::World &w, uint32_t ref, ParamType type) const;
    uint32_t next_rand();

    void record_gap(opennova::world::World &w, int cmd, uint32_t instruction, const int32_t *arguments = nullptr);

    // Command dispatch (implemented subset; others recorded as effects).
    int32_t dispatch(opennova::world::World &w, int cmd_index, const uint32_t *args, int argc, uint32_t instruction);
    // The registry flags-0x18 arm of the call: serialize the operands for the
    // S2C 0x23 record the server tick sends, then decide the local call
    // (script_remote_command.h and remote_command.h carry the witnesses).
    std::vector<world::ScriptRemoteArg> resolve_remote_args(opennova::world::World &w, const CommandDef &def,
                                                            const uint32_t *args, int argc) const;
    int32_t replicate(opennova::world::World &w, int cmd_index, const CommandDef &def,
                      const uint32_t *args, int argc, uint32_t instruction);
};

} // namespace opennova::wac
