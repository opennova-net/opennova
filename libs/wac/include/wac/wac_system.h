// WAC scripting system: an ISystem that ticks the WAC VM against the shared
// world. One of the two scripting evaluators (alongside the BMS event runtime);
// both register with the World tick service and drive the same entities/vars.
#ifndef OPENNOVA_WAC_WAC_SYSTEM_H
#define OPENNOVA_WAC_WAC_SYSTEM_H

#include <utility>

#include "wac/program.h"
#include "wac/vm.h"
#include "world/world.h"

namespace opennova::wac {

class WacSystem : public opennova::world::ISystem {
public:
    const char *name() const override { return "wac"; }

    // Install a compiled program (e.g. from compile_program). Reloads the VM.
    void set_program(Program program) {
        prog_ = std::move(program);
        vm_.load(prog_);
    }

    void on_load(opennova::world::World &) override {
        if (!prog_.code.empty()) vm_.load(prog_);
    }

    void tick(opennova::world::World &world, const opennova::world::TickContext &ctx) override {
        if (!ctx.is_authority) return; // WAC runs only on the authoritative host
        vm_.execute(world);
    }

    const Program &program() const { return prog_; }
    WacVm &vm() { return vm_; }

private:
    Program prog_;
    WacVm vm_;
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_WAC_SYSTEM_H
