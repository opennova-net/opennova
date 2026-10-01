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
    // (set name, listener view flags, bank member): false selects the radio
    // line's member file (SoundBank_SelectTriggerEntry, no pitch), true the
    // loaded bank member with its pitch jitter (SoundBank_SelectTriggerEntryFromBank).
    using SetResolver =
            std::function<std::optional<SetSelection>(const std::string &, uint8_t, bool)>;
    struct State {
        std::shared_ptr<const lwf::WavPcm> clip;
        std::string filename;
        EntityHandle anchor;
        EntityHandle portrait;
        int32_t max_distance = 0;
        bool channel_active = false;
        uint32_t pitch_q16 = 0x10000u;
        int32_t volume = 210;
        // The anchor is a decoded remote row the world holds no entity for
        // (a joiner's speaker): its position rides track_row_anchor.
        bool row_anchor = false;
        Vec3 row_position;
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
    // The entity-anchored voice (the S2C 0x2D EMO_ line): nothing outside a
    // session peer or for an unknown set; while channel zero is busy the set
    // plays as a full-volume 3D one-shot at the speaker (world.out.slot_sounds);
    // else the set's bank member, with its pitch jitter, volume and radius,
    // plays on channel zero anchored at the speaker, also its portrait (the
    // local anchor takes the 100-unit radius). `row_anchor` marks a speaker
    // the world has no entity for; its owner then keeps its position live.
    // [orig: Audio_StartEntityPlayback @0x4ECD90 -- is_mp_session_peer
    //  @0x4ecd97, the set @0x4ecda3, the busy word dword_C6EC30 @0x4ecdac ->
    //  Entity_PlaySound3D_FullVolume @0x4ecdb8, else the channel-0 reset
    //  @0x4ecdc6, the anchor dword_C6EC34 and g_VoicePlaybackEntity
    //  @0x4ecdfe / @0x4ece03, SoundBank_SelectTriggerEntryFromBank @0x4ece0e,
    //  Audio_Play3DPositionalSound @0x4ece13]
    enum class EntityVoice { None, Oneshot, Channel };
    EntityVoice entity_set(World &world, const std::string &name, EntityHandle speaker,
            Vec3 position, bool row_anchor);
    // A row anchor's live state: its position while `present`, else the voice
    // stops as a dead or freed anchor does. No-op for any other anchor.
    // [orig: Audio_UpdateAmbientStream @0x4ED9E8..0x4EDA2A]
    void track_row_anchor(EntityHandle anchor, bool present, Vec3 position);
    int wave(World &world, const std::string &filename);
    int ssn_wave(World &world, EntityHandle speaker, const std::string &filename,
            int32_t max_distance, bool radio);
    bool ready() const { return !state_.channel_active; }
    EntityHandle speaker() const { return state_.portrait; } // g_VoicePlaybackEntity

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
