#include <runtime/world/script_voice.h>

#include <cmath>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/world/world.h>

namespace opennova::world {

bool ScriptVoiceChannel::start(World &world, const std::string &filename,
        EntityHandle anchor, EntityHandle portrait, int32_t max_distance) {
    // Reset before loading, including a missing or malformed file.
    // [orig: wave @0x4ED625; SSNwave @0x4F7926; SSNradio @0x4F7A06]
    ++serial_;
    state_.clip.reset();
    state_.filename = filename;
    state_.anchor = anchor;
    state_.portrait = portrait;
    state_.max_distance = max_distance;
    state_.pitch_q16 = 0x10000u;
    state_.volume = 210;
    world.out.effects.push({"dialog_wav", 0, 0, 0, 0, filename});
    std::vector<uint8_t> bytes;
    auto decoded = std::make_shared<lwf::WavPcm>();
    std::string error;
    if (!read_file_ || !read_file_(filename, bytes) ||
            !lwf::wav_decode_pcm16(bytes.data(), bytes.size(), *decoded, error))
        return false;
    state_.clip = std::move(decoded);
    state_.channel_active = true;
    // Audio_Play3DPositionalSound overrides the radius for the local anchor,
    // including SSNwave(local,...). Radio and wave both take this path.
    // [orig: @0x4ECC90..0x4ECC96]
    if (anchor == world.cached.local_player) state_.max_distance = 100 * 65536;
    return true;
}

bool ScriptVoiceChannel::radio_set(World &world, const std::string &name, EntityHandle speaker) {
    if (!world.rules.mp_session || !ready() || !resolve_set_ ||
            !world.registry.get(world.cached.local_player)) return false;
    const auto selected = resolve_set_(name, world.cached.sound_listener_view_flags);
    if (!selected) return false;
    if (!start(world, selected->filename, world.cached.local_player, speaker,
            selected->max_distance)) return false;
    state_.volume = selected->volume;
    return true;
}

int ScriptVoiceChannel::wave(World &world, const std::string &filename) {
    // [orig: Wac_PlayScriptedVoiceWave @0x4ED610]
    if (world.registry.get(world.cached.local_player) == nullptr) return 1;
    return start(world, filename, world.cached.local_player, {}, state_.max_distance) ? 0 : 1;
}

int ScriptVoiceChannel::ssn_wave(World &world, EntityHandle speaker,
        const std::string &filename, int32_t max_distance, bool radio) {
    // The command validates allocation/definition, not health. The playback
    // update subsequently stops a dead anchor. Radio's speaker is only its
    // portrait identity; its acoustic anchor is the local player.
    // [orig: WacCmd_SsnWave @0x4F78D0; WacCmd_SsnRadio @0x4F79B0]
    const Entity *entity = world.registry.get(speaker);
    if (entity == nullptr || entity->item_id == 0) return 0;
    return start(world, filename, radio ? world.cached.local_player : speaker,
            speaker, radio ? state_.max_distance : max_distance) ? 1 : 0;
}

void ScriptVoiceChannel::stop() {
    ++serial_;
    state_.clip.reset();
    state_.filename.clear();
    state_.channel_active = false;
    state_.anchor = {};
    state_.portrait = {};
}

void ScriptVoiceChannel::finish(uint64_t serial, const lwf::WavPcm *clip) {
    if (serial == serial_ && state_.clip.get() == clip) stop();
}

void ScriptVoiceChannel::refresh(World &world) {
    if (ready()) return;
    const Entity *anchor = world.registry.get(state_.anchor);
    // Reset invalidates the old physical handle, but a failed load leaves
    // its nonzero word observable until this playback validation pass.
    // [orig: AudioChannel_ResetByHandle @0x767160; WacCmd_WaveReady @0x4ED380]
    if (!state_.clip) { stop(); return; }
    // [orig: Audio_UpdateAmbientStream @0x4ED9E8..0x4EDA2A]
    if (anchor == nullptr || !anchor->has_item_def || (anchor->flags & 2u) != 0) stop();
}

ScriptVoiceChannel::Frame ScriptVoiceChannel::frame(World &world, Vec3 listener) {
    refresh(world);
    Frame out;
    out.serial = serial_;
    out.state = state_;
    const Entity *anchor = world.registry.get(state_.anchor);
    if (ready() || anchor == nullptr) return out;
    out.position = anchor->position;
    out.local = state_.anchor == world.cached.local_player;
    int64_t distance = 0;
    if (!out.local) {
        // The listener/body delta is a wrapped Q16 dword on each axis.
        const int32_t dx = static_cast<int32_t>(uint32_t(to_fixed(anchor->position.x)) -
                uint32_t(to_fixed(listener.x)));
        const int32_t dy = static_cast<int32_t>(uint32_t(to_fixed(anchor->position.y)) -
                uint32_t(to_fixed(listener.y)));
        const int32_t dz = static_cast<int32_t>(uint32_t(to_fixed(anchor->position.z)) -
                uint32_t(to_fixed(listener.z)));
        const double length = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
        // [orig: Audio_UpdateAmbientStream — fsqrt @0x4EDAB2, the flt_7C19E0
        // saturation @0x4EDAB4..0x4EDAC9] Same Q16 distance saturation as the
        // common spatial-audio path.
        distance = length > 2147418112.0 ? 2147418112 : static_cast<int32_t>(length);
    }
    // User voice volume remains on the host's Voice bus. The channel's selected
    // gain and the existing retail distance curve are applied exactly once.
    // [orig: @0x4ED688; Audio_UpdateAmbientStream @0x4EDB22]
    out.volume = audio::calc_distance_volume(distance, state_.max_distance,
            (state_.volume * 255 + 128) >> 8, 255);
    return out;
}

void ScriptVoiceChannel::restore(const State &state) {
    ++serial_;
    state_ = state;
}

void ScriptVoiceChannel::reset() {
    stop();
    state_.max_distance = 0;
}

} // namespace opennova::world
