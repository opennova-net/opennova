#include "simulation/simulation.h"
#include "util/axes.h"

#include <runtime/mission/mission_kernel.h>

using namespace godot;

opennova::world::ScriptVoiceChannel::Frame Simulation::script_voice_frame(
        const Vector3 &p_listener) {
    const auto listener = godot_to_mission(p_listener);
    return kernel_->world.script.voice.frame(kernel_->world, {listener.x, listener.y, listener.z});
}

void Simulation::finish_script_voice(uint64_t p_serial, const opennova::lwf::WavPcm *p_clip) {
    kernel_->world.script.voice.finish(p_serial, p_clip);
}

bool Simulation::play_script_wave(const String &p_filename) {
    return kernel_->world.script.voice.wave(kernel_->world, p_filename.utf8().get_data()) == 0;
}
