// WAC scripting system: an ISystem that ticks the WAC VM against the shared
// world. One of the two scripting evaluators (alongside the BMS event runtime);
// both register with the World tick service and drive the same entities/vars.
#ifndef OPENNOVA_WAC_WAC_SYSTEM_H
#define OPENNOVA_WAC_WAC_SYSTEM_H

#include <utility>

#include "wac/program.h"
#include "wac/vm.h"
#include "world/system.h"

namespace opennova::wac {

class WacSystem : public opennova::world::ISystem {
public:
    // The VM executes the whole program once every 62nd tick — the 0x3E divider.
    // [orig: WacScript_AdvanceTick @0x4f81a0: ++dword_C6EAD4, cmp 0x3E @0x4f81b1, reset, execute]
    static constexpr int kTicksPerExecution = 0x3E; // 62

    const char *name() const override { return "wac"; }

    // Script-disable gate. [orig: dword_C6EB28 checked at @0x4f81a0 entry]
    bool paused = false;

    // Install a compiled program (e.g. from compile_program). Reloads the VM.
    void set_program(Program program) {
        prog_ = std::move(program);
        vm_.load(prog_);
    }

    void on_load(opennova::world::World &) override {
        if (!prog_.code.empty()) vm_.load(prog_);
        accum_ = 0;
        runs_ = 0;
    }

    void tick(opennova::world::World &world, const opennova::world::TickContext &ctx) override {
        if (!ctx.is_authority) return; // WAC runs only on the authoritative host
        if (ctx.pre_mission) return;   // WAC does not participate in the BMS pre-mission pass
        if (!vm_.loaded()) return;     // no program installed (e.g. a BMS-only mission)
        if (paused) return;            // [orig: dword_C6EB28 gate]
        if (++accum_ < kTicksPerExecution) return; // [orig: dword_C6EAD4 ++ / cmp 0x3E]
        accum_ = 0;
        vm_.execute(world);
        ++runs_; // completed-executions counter [orig: dword_C6EAD8 @0x4f81d3]
    }

    // Completed VM executions since load (the original's "script has run" flag is
    // this counter being nonzero). [orig: dword_C6EAD8]
    uint32_t runs() const { return runs_; }

    const Program &program() const { return prog_; }
    WacVm &vm() { return vm_; }

private:
    Program prog_;
    WacVm vm_;
    int accum_ = 0;     // tick accumulator toward the next execution [orig: dword_C6EAD4]
    uint32_t runs_ = 0; // [orig: dword_C6EAD8]
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_WAC_SYSTEM_H
