#include "audio/mission_audio.h"
#include "simulation/simulation.h"
#include "lwf/wav_loader.h"
#include "util/axes.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

void MissionAudio::_stop_script_voice(bool p_report_finished) {
    if (p_report_finished) {
        const Ref<Simulation> sim = _simulation();
        if (sim.is_valid())
            sim->finish_script_voice(script_voice_frame_.serial, script_voice_frame_.state.clip.get());
    }
    Object *object = ObjectDB::get_instance(script_voice_id_);
    if (auto *voice = Object::cast_to<AudioStreamPlayer>(object)) {
        voice->stop();
        voice->queue_free();
    } else if (auto *voice = Object::cast_to<AudioStreamPlayer3D>(object)) {
        voice->stop();
        voice->queue_free();
    }
    script_voice_id_ = ObjectID();
    script_voice_frame_ = {};
}

void MissionAudio::_on_script_voice_finished(int64_t p_serial, int64_t p_player_id) {
    // Signals from interrupted/freed players cannot complete a replacement.
    if (uint64_t(p_serial) != script_voice_frame_.serial ||
            uint64_t(p_player_id) != uint64_t(script_voice_id_)) return;
    _stop_script_voice(true);
}

void MissionAudio::sync_script_voice() {
    const Ref<Simulation> sim = _simulation();
    if (sim.is_null() || !root_attached_) return;
    auto frame = sim->script_voice_frame(last_camera_pos_.is_finite() ? last_camera_pos_ : Vector3());
    if (frame.serial != script_voice_frame_.serial ||
            frame.state.clip.get() != script_voice_frame_.state.clip.get()) {
        _stop_script_voice(false);
        if (AudioStreamPlayer *manual = _wac_voice_node()) manual->stop();
        script_voice_frame_ = frame;
        if (!frame.state.clip) return;
        const Ref<AudioStreamWAV> stream = WavLoader::from_pcm(*frame.state.clip);
        Node *node = nullptr;
        if (frame.local) {
            auto *voice = memnew(AudioStreamPlayer);
            voice->set_stream(stream);
            node = voice;
        } else {
            auto *voice = memnew(AudioStreamPlayer3D);
            voice->set_stream(stream);
            voice->set_attenuation_model(AudioStreamPlayer3D::ATTENUATION_DISABLED);
            voice->set_max_distance(0.0);
            voice->set_doppler_tracking(AudioStreamPlayer3D::DOPPLER_TRACKING_DISABLED);
            node = voice;
        }
        add_child(node);
        script_voice_id_ = ObjectID(node->get_instance_id());
        node->connect("finished", Callable(this, "_on_script_voice_finished").bind(
                int64_t(frame.serial), int64_t(uint64_t(script_voice_id_))));
        const StringName bus = AudioServer::get_singleton()->get_bus_index("Voice") >= 0
                ? StringName("Voice") : StringName("Master");
        const float volume_db = static_cast<float>(SoundBank::volume_db_from_255(frame.volume));
        if (auto *voice = Object::cast_to<AudioStreamPlayer>(node)) {
            voice->set_bus(bus);
            voice->set_volume_db(volume_db);
            voice->play();
        } else if (auto *voice = Object::cast_to<AudioStreamPlayer3D>(node)) {
            voice->set_bus(bus);
            voice->set_global_position(mission_to_godot(frame.position));
            voice->set_volume_db(volume_db);
            voice->play();
        }
    }
    // The anchor follows the entity every presentation frame. The engine's
    // distance curve owns gain; the host only supplies spatial panning.
    Object *object = ObjectDB::get_instance(script_voice_id_);
    const float volume_db = static_cast<float>(SoundBank::volume_db_from_255(frame.volume));
    if (auto *voice = Object::cast_to<AudioStreamPlayer3D>(object)) {
        voice->set_global_position(mission_to_godot(frame.position));
        voice->set_volume_db(volume_db);
    } else if (auto *voice = Object::cast_to<AudioStreamPlayer>(object)) {
        voice->set_volume_db(volume_db);
    }
}
