// WAC scripting system: an ISystem that ticks the WAC VM against the shared
// world. One of the two scripting evaluators (alongside the BMS event runtime);
// both register with the World tick service and drive the same entities/vars.
#pragma once

#include <utility>

#include <formats/wac/program.h>
#include <runtime/wac/vm.h>
#include <runtime/world/system.h>

namespace opennova::wac {

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
    static constexpr int kTicksPerExecution = 0x3E; // 62: WAC's own divider, the same
                                                    // number as io::kTicksPerSecondInt by design, not by reference

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

    void on_load(opennova::world::World &world) override;
    // Publish the mutable VM clock before World's shared script admission.
    void prepare_tick(opennova::world::World &world) override;
    // Live program replacement also publishes the reset clock immediately.
    void set_program(Program program, opennova::world::World &world) {
        set_program(std::move(program));
        prepare_tick(world);
    }

	// WacScript_InitAndLoad executes the freshly loaded bytecode once before
	// the environment's 255-tick startup settle. This does not consume a logic
	// tick or the normal 62-tick divider.
	// [orig: call WacScript_ExecuteBytecode @0x4F976B, ++wac_var_ticks @0x4F9770]
    bool execute_initial(opennova::world::World &world);

    void tick(opennova::world::World &world,
              const opennova::world::TickContext &ctx) override;

    // Diagnostic completed-execution count. Script admission and the Ticks
    // builtin use vm().time(), which scripts may write, not this counter.
    uint32_t runs() const { return runs_; }

	RuntimeState capture_runtime_state() const {
		return RuntimeState{vm_.capture_runtime_state(), accum_, runs_, initial_executed_};
	}

    void restore_runtime_state(opennova::world::World &world, const RuntimeState &state);

    const Program &program() const { return prog_; }
    WacVm &vm() { return vm_; }

private:
    Program prog_;
    WacVm vm_;
    int accum_ = 0;     // tick accumulator toward the next execution [orig: dword_C6EAD4]
    uint32_t runs_ = 0; // diagnostics only; never used as the script clock
	bool initial_executed_ = false;
};

} // namespace opennova::wac
