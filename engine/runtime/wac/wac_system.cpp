#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>
#include <base/io/strutil.h>

namespace opennova::wac {

void WacSystem::on_load(opennova::world::World &world) {
    if (!prog_.code.empty()) vm_.load(prog_);
    accum_ = 0;
    runs_ = 0;
    initial_executed_ = false;
    paused = false; // [orig: WacScript_InitAndLoad @0x4F965F]
    prepare_tick(world);
    world.script.weapon_input.reset();
    world.script.voice.reset();
    world.script.squad_events = {}; // [orig: WacScript_InitAndLoad @0x4F96F3..0x4F9733]
    world.script.forced_animation = 0; // [orig: WacScript_InitAndLoad @0x4F9659]
}

void WacSystem::set_program(Program program, opennova::world::World &world) {
    set_program(std::move(program));
    world.script.vars.reset_wac_load_state();
    prepare_tick(world);
}

void WacSystem::prepare_tick(opennova::world::World &world) {
    // This is a projection, not another clock: only vm_.time() is serialized.
    // [orig: mutable g_WacVarTicks @0xC6EAD8; shared gate @0x51D8BD]
    world.cached.wac_ticks = static_cast<int32_t>(vm_.time());
}

bool WacSystem::execute_initial(opennova::world::World &world) {
    if (!vm_.loaded() || initial_executed_ || runs_ != 0) return false;
    vm_.execute(world);
    vm_.advance_time(); // [orig: WacScript_InitAndLoad @0x4F9770]
    ++runs_;
    initial_executed_ = true;
    prepare_tick(world);
    return true;
}

void WacSystem::tick(opennova::world::World &world,
                     const opennova::world::TickContext &ctx) {
    if (!ctx.is_authority || ctx.phase != opennova::world::TickPhase::Gameplay) return;
    // Direct system callers have no World-owned admission; publish before
    // computing their gate too. A real world tick already froze that decision.
    if (!ctx.script_admitted.has_value()) prepare_tick(world);
    if (!vm_.loaded() || paused) return;
    const bool admitted = ctx.script_admitted.has_value()
            ? *ctx.script_admitted : world.script_may_advance();
    if (!admitted) return;
    // [orig: WacScript_AdvanceTick @0x4F81A0, divider @0x4F81B1]
    if (++accum_ < kTicksPerExecution) return;
    accum_ = 0;
    vm_.execute(world);
    vm_.advance_time(); // [orig: WacScript_AdvanceTick @0x4F81D3]
    ++runs_;
    prepare_tick(world);
}

void WacSystem::restore_runtime_state(opennova::world::World &world,
                                      const RuntimeState &state) {
    if (!prog_.code.empty()) vm_.restore_runtime_state(prog_, state.vm);
    accum_ = state.accumulator;
    runs_ = state.runs;
    initial_executed_ = state.initial_executed;
    prepare_tick(world);
}

bool WacSystem::toggle_cheat(std::string_view line, bool authority,
		opennova::world::World &world) const {
	if (!authority) return false;
	for (size_t i = 0; i < prog_.declarations.size(); ++i) {
		const Program::Declared &d = prog_.declarations[i];
		if (d.type != 0x19 || !strutil::iequals(d.name, line)) continue;
		const int var = 256 + static_cast<int>(i);
		world.script.vars.set_mission(var, world.script.vars.get_mission(var) == 0 ? 1 : 0);
		return true;
	}
	return false;
}

} // namespace opennova::wac
