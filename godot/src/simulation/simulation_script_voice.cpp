#include "simulation/simulation.h"
#include "util/axes.h"

#include <formats/gamecfg/game_cfg.h>  // gamecfg::defaults (DialogueLevel)
#include <runtime/inmatch/client_runtime.h> // ClientRuntime::track_voice_anchor
#include <runtime/inmatch/host_role.h> // HostRole::dialog_line_loaded
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/fire_sound.h> // the sound layer's source reads

using namespace godot;

opennova::world::ScriptVoiceChannel::Frame Simulation::script_voice_frame(
        const Vector3 &p_listener) {
    const auto listener = godot_to_mission(p_listener);
    // A joiner's decoded-row speaker rides its replica row (ClientRuntime::track_voice_anchor),
    // inside the same update.
    if (runtime_ != nullptr) runtime_->track_voice_anchor(kernel_->world);
    return kernel_->world.script.voice.frame(kernel_->world, {listener.x, listener.y, listener.z});
}

opennova::world::ScriptVoiceChannel::Frame Simulation::script_voice_state(
        const Vector3 &p_listener) const {
    const auto listener = godot_to_mission(p_listener);
    return kernel_->world.script.voice.sample(kernel_->world, {listener.x, listener.y, listener.z});
}

void Simulation::finish_script_voice(uint64_t p_serial, const opennova::lwf::WavPcm *p_clip) {
    kernel_->world.script.voice.finish(p_serial, p_clip);
}

void Simulation::set_audio_out_meter(const opennova::audio::AudioOutMeter &p_meter) {
    if (kernel_) kernel_->world.audio_out_meter = p_meter;
}

bool Simulation::sound_source_place(uint32_t p_sound_id, int32_t r_position[3]) const {
    return kernel_ && opennova::world::sound_source_place(kernel_->world, p_sound_id, r_position);
}

bool Simulation::sound_source_ridden(uint32_t p_sound_id) const {
    return kernel_ && opennova::world::sound_source_ridden(kernel_->world, p_sound_id);
}

bool Simulation::sound_spawn_ridden(uint64_t p_spawn_id) const {
    return kernel_ && opennova::world::sound_spawn_ridden(kernel_->world, p_spawn_id);
}

bool Simulation::play_script_wave(const String &p_filename) {
    return kernel_->world.script.voice.wave(kernel_->world, p_filename.utf8().get_data()) == 0;
}

void Simulation::set_script_voice_resolver(opennova::world::ScriptVoiceChannel::SetResolver p_resolver,
        opennova::world::ScriptVoiceChannel::DialogueVolume p_dialogue_volume) {
    voice_set_resolver_ = std::move(p_resolver);
    voice_dialogue_volume_ = std::move(p_dialogue_volume);
    kernel_->world.script.voice.set_set_resolver(voice_set_resolver_);
    kernel_->world.script.voice.set_dialogue_volume(voice_dialogue_volume_);
}

opennova::audio::DialogQueue *Simulation::dialog_queue() {
    return kernel_ ? &kernel_->world.script.dialog : nullptr;
}

void Simulation::tick_dialog_queue(bool p_frame_rendered,
        const opennova::audio::DialogQueue::LoadLine &p_load,
        const opennova::audio::DialogQueue::VoicePlaying &p_playing) {
    if (!kernel_) return;
    // g_DialogueVolume: the process block's DialogueLevel, the word the frame stamps on
    // the client runtime too (Simulation::begin_session_frame).
    static const int32_t kDefaultDialogueLevel = opennova::gamecfg::defaults().dialogue_level;
    const int32_t dialogue_volume = net_.game_config.is_valid()
            ? net_.game_config->native().cfg().dialogue_level
            : kDefaultDialogueLevel;
    kernel_->world.script.dialog.tick(p_frame_rendered, dialogue_volume,
            [this, &p_load](const opennova::audio::DialogLineRef &p_line, bool p_play) {
                const opennova::audio::DialogClip clip = p_load(p_line, p_play);
                if (host_role_ != nullptr) host_role_->dialog_line_loaded(p_line);
                return clip;
            },
            p_playing);
}

void Simulation::drain_dialog_lines(std::vector<opennova::world::DialogLineOrder> &r_lines) {
    r_lines.clear();
    if (!kernel_) return;
    r_lines.swap(kernel_->world.out.dialog_lines);
}

bool Simulation::play_dialog(int p_dialog_index) {
    // The BMS PlayWavList's play, ungated: the dialog of the number in the
    // mission's dialog bank registers in the world's table.
    if (!kernel_) return false;
    opennova::world::World &world = kernel_->world;
    return world.script.dialog.play(world.tables.dialog_bank, world.tables.dialog_sounds,
            p_dialog_index);
}
