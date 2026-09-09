#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

namespace opennova::wac {

void WacSystem::on_load(opennova::world::World &world) {
    if (!prog_.code.empty()) vm_.load(prog_);
    accum_ = 0;
    runs_ = 0;
    initial_executed_ = false;
    world.script.weapon_input.reset();
    world.script.voice.reset();
    world.script.squad_events = {}; // [orig: WacScript_InitAndLoad @0x4F96F3..0x4F9733]
    world.script.forced_animation = 0; // [orig: WacScript_InitAndLoad @0x4F9659]
}

} // namespace opennova::wac
