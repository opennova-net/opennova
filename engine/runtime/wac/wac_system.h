// WAC scripting system: an ISystem that ticks the WAC VM against the shared
// world. One of the two scripting evaluators (alongside the BMS event runtime);
// both register with the World tick service and drive the same entities/vars.
#ifndef OPENNOVA_WAC_WAC_SYSTEM_H
#define OPENNOVA_WAC_WAC_SYSTEM_H

#include <utility>

#include <formats/wac/program.h>
#include <runtime/wac/vm.h>
#include <runtime/world/system.h>

namespace opennova::wac {

// Publishes the VM execution counter into the world and answers the shared
// script-advance gate. Out of line because this header only forward-declares
// World. [orig: wac_var_ticks + the wrapper at World::script_may_advance]
bool wac_publish_ticks_and_gate(opennova::world::World &world, uint32_t runs);

class WacSystem : public opennova::world::ISystem {
public:
	struct RuntimeState {
		WacVm::RuntimeState vm;
		int accumulator = 0;
		uint32_t runs = 0;
		bool initial_executed = false;
	};

    // The VM executes the whole program once every 62nd tick — the 0x3E divider.
    // [orig: WacScript_AdvanceTick @0x4f81a0: ++dword_C6EAD4, cmp 0x3E @0x4f81b1, reset, execute]
    static constexpr int kTicksPerExecution = 0x3E; // 62

    const char *name() const override { return "wac"; }

    // Script-disable gate. [orig: dword_C6EB28 checked at @0x4f81a0 entry]
    bool paused = false;

    // Install a compiled program (e.g. from compile_program). Reloads the VM.
    void set_program(Program program) {
        prog_ = std::move(program);
		vm_ = WacVm{};
		if (!prog_.code.empty()) vm_.load(prog_);
		accum_ = 0;
		runs_ = 0;
		initial_executed_ = false;
    }

    void on_load(opennova::world::World &) override {
        if (!prog_.code.empty()) vm_.load(prog_);
        accum_ = 0;
        runs_ = 0;
		initial_executed_ = false;
    }

	// WacScript_InitAndLoad executes the freshly loaded bytecode once before
	// the environment's 255-tick startup settle. This does not consume a logic
	// tick or the normal 62-tick divider.
	// [orig: call WacScript_ExecuteBytecode @0x4F976B, ++wac_var_ticks @0x4F9770]
	bool execute_initial(opennova::world::World &world) {
		if (!vm_.loaded() || initial_executed_ || runs_ != 0) return false;
		vm_.execute(world);
		++runs_;
		initial_executed_ = true;
		return true;
	}

    void tick(opennova::world::World &world, const opennova::world::TickContext &ctx) override {
        if (!ctx.is_authority) return; // WAC runs only on the authoritative host
        if (ctx.phase != opennova::world::TickPhase::Gameplay) return;
        if (!vm_.loaded()) return;     // no program installed (e.g. a BMS-only mission)
        if (paused) return;            // [orig: dword_C6EB28 gate]
        // Republish the execution counter, then apply the shared script-advance
        // gate — retail reads both out of the same global bag, and the WAC tick
        // sits under the same `if` as the BMS event pump.
        // [orig: wac_var_ticks @0x4f81d3; the wrapper described at
        //  World::script_may_advance]
        // The tick republish is kept; the human-presence GATE is deferred to
        // master pending the same decision as the #564-class wire commits.
        // Re-enabling it is one line, but it changes wac_behavior_test's
        // expectations, so it is not ours to switch on unilaterally.
        (void)wac_publish_ticks_and_gate(world, runs_);
        if (++accum_ < kTicksPerExecution) return; // [orig: dword_C6EAD4 ++ / cmp 0x3E]
        accum_ = 0;
        vm_.execute(world);
        ++runs_; // completed-executions counter [orig: wac_var_ticks @0x4f81d3]
    }

    // Completed VM executions since load (the original's "script has run" flag is
    // this counter being nonzero). [orig: wac_var_ticks]
    uint32_t runs() const { return runs_; }

	RuntimeState capture_runtime_state() const {
		return RuntimeState{vm_.capture_runtime_state(), accum_, runs_, initial_executed_};
	}

	void restore_runtime_state(const RuntimeState &state) {
		if (!prog_.code.empty()) vm_.restore_runtime_state(prog_, state.vm);
		accum_ = state.accumulator;
		runs_ = state.runs;
		initial_executed_ = state.initial_executed;
	}

    const Program &program() const { return prog_; }
    WacVm &vm() { return vm_; }

private:
    Program prog_;
    WacVm vm_;
    int accum_ = 0;     // tick accumulator toward the next execution [orig: dword_C6EAD4]
    uint32_t runs_ = 0; // [orig: wac_var_ticks]
	bool initial_executed_ = false;
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_WAC_SYSTEM_H
