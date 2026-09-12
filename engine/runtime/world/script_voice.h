// The WAC voice channel belongs to the mission. The host plays its decoded
// clip and acknowledges physical channel completion; WAC observes that state.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include <formats/lwf/wav_pcm.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/entity.h>

namespace opennova::world {
class World;

class ScriptVoiceChannel {
public:
    using FileReader = std::function<bool(const std::string &, std::vector<uint8_t> &)>;
    using SetSelection = audio::RadioVoice;
    using SetResolver = std::function<std::optional<SetSelection>(const std::string &, uint8_t)>;
    struct State {
        std::shared_ptr<const lwf::WavPcm> clip;
        std::string filename;
        EntityHandle anchor;
        EntityHandle portrait;
        int32_t max_distance = 0;
        bool channel_active = false;
        uint32_t pitch_q16 = 0x10000u;
        int32_t volume = 210;
    };
    struct Frame {
        uint64_t serial = 0;
        State state;
        Vec3 position;
        bool local = false;
        int32_t volume = 0;
    };

    void set_file_reader(FileReader reader) { read_file_ = std::move(reader); }
    void set_set_resolver(SetResolver resolver) { resolve_set_ = std::move(resolver); }
    // Radio and revive voice share channel zero; an active voice declines a
    // new set before member selection. The portrait is the speaker, while
    // the acoustic anchor is the local body.
    // [orig: Audio_StartAmbientSoundForPlayer @0x4ECE30]
    bool radio_set(World &world, const std::string &name, EntityHandle speaker);
    int wave(World &world, const std::string &filename);
    int ssn_wave(World &world, EntityHandle speaker, const std::string &filename,
            int32_t max_distance, bool radio);
    bool ready() const { return !state_.channel_active; }
    EntityHandle speaker() const { return state_.portrait; } // g_voicePlaybackEntity

    // Physical completion uses both the generation and clip identity: an old
    // callback cannot finish a replacement voice or a newly loaded mission.
    void finish(uint64_t serial, const lwf::WavPcm *clip);
    void refresh(World &world);
    Frame frame(World &world, Vec3 listener);
    State snapshot() const { return state_; }
    void restore(const State &state);
    void reset();

private:
    bool start(World &world, const std::string &filename, EntityHandle anchor,
            EntityHandle portrait, int32_t max_distance);
    void stop();
    FileReader read_file_;
    SetResolver resolve_set_;
    State state_;
    uint64_t serial_ = 0;
};

} // namespace opennova::world
