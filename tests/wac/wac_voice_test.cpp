#include <cstdio>
#include <cstring>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::wac;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static std::vector<uint8_t> wave_bytes() {
    std::vector<uint8_t> out(60, 0);
    const auto word = [&](int off, unsigned v) {
        out[off] = uint8_t(v); out[off + 1] = uint8_t(v >> 8);
    };
    const auto dword = [&](int off, unsigned v) {
        word(off, v); word(off + 2, v >> 16);
    };
    std::memcpy(out.data(), "RIFF", 4);
    dword(4, 52);
    std::memcpy(out.data() + 8, "WAVEfmt ", 8);
    dword(16, 16); word(20, 1); word(22, 1);
    dword(24, 8000); dword(28, 16000); word(32, 2); word(34, 16);
    std::memcpy(out.data() + 36, "data", 4); dword(40, 16);
    return out;
}

struct Fixture {
    World world;
    EntityHandle local, speaker;
    int reads = 0;
    Fixture() {
        world.registry.configure_pool(0, 8);
        Entity actor;
        actor.net_id = 1; actor.item_id = 11; actor.health = 100; actor.has_item_def = true;
        local = world.registry.spawn(0, actor);
        actor.net_id = 2;
        actor.position.x = 5.0f;
        speaker = world.registry.spawn(0, actor);
        world.cached.local_player = local;
        world.script.voice.set_file_reader([this](const std::string &name, std::vector<uint8_t> &out) {
            ++reads;
            if (name != "tone.wav") return false;
            out = wave_bytes();
            return true;
        });
    }
    void script(const char *source) {
        CompileEnv env;
        const Program program = compile_source(source, env);
        CHECK(program.ok());
        WacVm vm; vm.load(program); vm.execute(world);
    }
};

static void test_wave_ready_and_physical_completion() {
    Fixture f;
    f.script(
        "if waveready() then inc(v1) endif\n"
        "if wave(\"tone.wav\") then inc(v2) endif\n"
        "if waveready() then inc(v3) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.script.vars.get_mission(2) == 0); // wave success returns zero
    CHECK(f.world.script.vars.get_mission(3) == 0);
    CHECK(f.reads == 1);
    const auto frame = f.world.script.voice.frame(f.world, {100.0f, 20.0f, 2.0f});
    CHECK(frame.state.clip && frame.state.clip->pcm16.size() == 16);
    CHECK(frame.local && frame.volume > 200);
    CHECK(frame.state.anchor == f.local);
    CHECK(!frame.state.portrait.valid());
    CHECK(frame.state.max_distance == 100 * 65536);
    f.world.script.voice.finish(frame.serial, frame.state.clip.get());
    f.script("if waveready() then inc(v4) endif\n");
    CHECK(f.world.script.vars.get_mission(4) == 1);
}

static void test_failed_replacement_invalidates_before_loading() {
    Fixture f;
    f.script("wave(\"tone.wav\")\n");
    const auto first = f.world.script.voice.frame(f.world, {});
    f.script(
        "if wave(\"missing.wav\") then inc(v1) endif\n"
        "if waveready() then inc(v2) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.script.vars.get_mission(2) == 0); // old handle remains until validation
    CHECK(!f.world.script.voice.snapshot().clip); // old buffer has been released
    f.world.script.voice.refresh(f.world);
    CHECK(f.world.script.voice.ready());
    f.script("wave(\"tone.wav\")\n");
    f.world.script.voice.finish(first.serial, first.state.clip.get());
    CHECK(!f.world.script.voice.ready()); // obsolete completion cannot stop this voice
    f.script("if wave(\"tone\") then inc(v3) endif\n");
    CHECK(f.world.script.vars.get_mission(3) == 1); // filename passed literally
}

static void test_spatial_speaker_and_radio_anchor() {
    Fixture f;
    f.script("if SSNwave(2,\"tone.wav\",10) then inc(v1) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 1); // SSN forms return one on success
    auto frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.state.anchor == f.speaker && frame.state.portrait == f.speaker);
    CHECK(!frame.local && frame.position.x == 5.0f);
    CHECK(frame.state.max_distance == 10 * 65536);
    CHECK(frame.volume > 40 && frame.volume < 60);
    f.world.registry.get(f.speaker)->position.x = 10.0f;
    frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.position.x == 10.0f && frame.volume == 0); // hard range edge
    CHECK(!f.world.script.voice.ready()); // inaudibility is not completion
    f.script("if SSNradio(2,\"tone.wav\") then inc(v2) endif\n");
    frame = f.world.script.voice.frame(f.world, {100.0f, 0.0f, 0.0f});
    CHECK(f.world.script.vars.get_mission(2) == 1);
    CHECK(frame.state.anchor == f.local && frame.state.portrait == f.speaker);
    CHECK(frame.local && frame.volume > 200);
    CHECK(frame.state.max_distance == 100 * 65536); // common start overrides prior radius
    f.world.registry.get(f.speaker)->flags |= 2u;
    f.world.script.voice.refresh(f.world);
    CHECK(!f.world.script.voice.ready()); // the portrait is not the radio's acoustic anchor
    f.world.registry.get(f.local)->flags |= 2u;
    f.world.script.voice.refresh(f.world);
    CHECK(f.world.script.voice.ready());
}

static void test_invalid_speaker_does_not_interrupt_and_dead_anchor_stops() {
    Fixture f;
    f.script("SSNwave(2,\"tone.wav\",10)\n");
    const auto first = f.world.script.voice.frame(f.world, {});
    f.script("if SSNwave(999,\"missing.wav\",1) then inc(v1) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.reads == 1);
    CHECK(f.world.script.voice.frame(f.world, {}).serial == first.serial);
    f.world.cached.local_player = {};
    f.script("if wave(\"missing.wav\") then inc(v2) endif\n");
    CHECK(f.world.script.vars.get_mission(2) == 1);
    CHECK(f.reads == 1 && !f.world.script.voice.ready());
    f.world.registry.get(f.speaker)->flags |= 2u;
    f.world.script.voice.refresh(f.world);
    CHECK(f.world.script.voice.ready());
    f.script("if SSNwave(2,\"tone.wav\",10) then inc(v3) endif\n");
    CHECK(f.world.script.vars.get_mission(3) == 1); // command has no health gate
    f.world.script.voice.refresh(f.world);
    CHECK(f.world.script.voice.ready());
}

static void test_item_type_gate_and_playback_definition_gate_are_distinct() {
    Fixture f;
    f.world.registry.get(f.speaker)->item_id = 0;
    f.script("if SSNwave(2,\"tone.wav\",10) then inc(v1) endif\n");
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.reads == 0);
    f.world.registry.get(f.speaker)->item_id = 11;
    f.world.registry.get(f.speaker)->has_item_def = false;
    f.script("if SSNwave(2,\"tone.wav\",10) then inc(v2) endif\n");
    CHECK(f.world.script.vars.get_mission(2) == 1); // command reads +28 type id
    CHECK(!f.world.script.voice.ready());
    f.world.script.voice.refresh(f.world); // updater reads +32 ItemDef pointer
    CHECK(f.world.script.voice.ready());
}

static void test_retry_restarts_channel_without_accepting_old_completion() {
    Fixture f;
    f.script("SSNwave(2,\"tone.wav\",10)\n");
    const auto first = f.world.script.voice.frame(f.world, {});
    const auto baseline = f.world.snapshot();
    f.script("wave(\"tone.wav\")\n");
    f.world.restore(baseline);
    auto restored = f.world.script.voice.frame(f.world, {});
    CHECK(restored.state.anchor == f.speaker);
    CHECK(restored.state.clip == first.state.clip);
    CHECK(restored.serial != first.serial);
    f.world.script.voice.finish(first.serial, first.state.clip.get());
    CHECK(!f.world.script.voice.ready());
    f.world.script.voice.finish(restored.serial, restored.state.clip.get());
    CHECK(f.world.script.voice.ready());
    f.world.script.voice.reset();
    CHECK(!f.world.script.voice.snapshot().anchor.valid());
}

static void test_radio_sets_share_channel_zero_and_keep_unity_pitch() {
    Fixture f;
    f.world.rules.mp_session = true;
    int selections = 0;
    f.world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool)
            -> std::optional<ScriptVoiceChannel::SetSelection> {
        ++selections;
        if (name != "MEDIC_VOICE") return std::nullopt;
        return ScriptVoiceChannel::SetSelection{"tone.wav", 180, 0};
    });
    CHECK(f.world.script.voice.radio_set(f.world, "MEDIC_VOICE", f.speaker));
    const auto frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.local && frame.state.portrait == f.speaker);
    CHECK(frame.state.pitch_q16 == 65536 && frame.state.volume == 180);
    CHECK(!f.world.script.voice.radio_set(f.world, "MEDIC_VOICE", f.local));
    CHECK(selections == 1); // busy gate precedes the shared random selection
    f.world.script.voice.finish(frame.serial, frame.state.clip.get());
    CHECK(f.world.script.voice.radio_set(f.world, "MEDIC_VOICE", f.local));
    CHECK(selections == 2);
    f.world.script.voice.reset();
    f.world.rules.mp_session = false;
    CHECK(!f.world.script.voice.radio_set(f.world, "MEDIC_VOICE", f.local));
    CHECK(selections == 2);
}

// The entity voice (S2C 0x2D's EMO_ line): the bank member with its pitch on
// channel zero anchored at the speaker; a busy channel plays the set as a
// full-volume 3D one-shot at the speaker instead, without a member pick; an
// unknown set or a non-peer does nothing; a decoded-row anchor rides its
// tracked position and stops when the row goes.
// [orig: Audio_StartEntityPlayback @0x4ECD90]
static void test_entity_voice_anchors_at_the_speaker_or_falls_back_to_a_oneshot() {
    Fixture f;
    std::vector<std::pair<std::string, bool>> picks;
    f.world.script.voice.set_set_resolver([&](const std::string &name, uint8_t, bool bank)
            -> std::optional<ScriptVoiceChannel::SetSelection> {
        picks.emplace_back(name, bank);
        if (name != "BM1_EMO_3") return std::nullopt;
        ScriptVoiceChannel::SetSelection pick{"tone.wav", 190, 50 * 65536};
        pick.pitch_q16 = 0x14000u;
        return pick;
    });
    const Vec3 at{5.0f, 0.0f, 0.0f};
    CHECK(f.world.script.voice.entity_set(f.world, "BM1_EMO_3", f.speaker, at, false) ==
            ScriptVoiceChannel::EntityVoice::Channel);
    CHECK(picks.size() == 1 && picks[0].second);
    auto frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.state.anchor == f.speaker && frame.state.portrait == f.speaker);
    CHECK(!frame.local && frame.position.x == 5.0f);
    CHECK(frame.state.pitch_q16 == 0x14000u && frame.state.volume == 190);
    CHECK(frame.state.max_distance == 50 * 65536);
    // Busy: the one-shot at the speaker, before any member pick.
    CHECK(f.world.script.voice.entity_set(f.world, "BM1_EMO_3", f.speaker, at, false) ==
            ScriptVoiceChannel::EntityVoice::Oneshot);
    CHECK(picks.size() == 1);
    CHECK(f.world.out.slot_sounds.size() == 1);
    const SoundSlotEvent &oneshot = f.world.out.slot_sounds.front();
    CHECK(std::string(oneshot.set_name) == "BM1_EMO_3" && oneshot.pos[0] == 5 * 65536 &&
            oneshot.source_handle == f.speaker.packed);
    f.world.script.voice.finish(frame.serial, frame.state.clip.get());
    // The local speaker takes the 100-unit radius.
    CHECK(f.world.script.voice.entity_set(f.world, "BM1_EMO_3", f.local, {}, false) ==
            ScriptVoiceChannel::EntityVoice::Channel);
    frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.local && frame.state.max_distance == 100 * 65536);
    f.world.script.voice.finish(frame.serial, frame.state.clip.get());
    // An unknown set, and a non-peer, leave the channel alone.
    CHECK(f.world.script.voice.entity_set(f.world, "NOPE", f.speaker, at, false) ==
            ScriptVoiceChannel::EntityVoice::None);
    CHECK(f.world.script.voice.ready());
    f.world.rules.mp_session_peer = false;
    CHECK(f.world.script.voice.entity_set(f.world, "BM1_EMO_3", f.speaker, at, false) ==
            ScriptVoiceChannel::EntityVoice::None);
    f.world.rules.mp_session_peer = true;
    // A decoded row the world has no entity for.
    const EntityHandle row = EntityHandle::make(0, 7);
    CHECK(f.world.script.voice.entity_set(f.world, "BM1_EMO_3", row, {3.0f, 4.0f, 5.0f}, true) ==
            ScriptVoiceChannel::EntityVoice::Channel);
    frame = f.world.script.voice.frame(f.world, {});
    CHECK(!f.world.script.voice.ready() && frame.position.y == 4.0f);
    f.world.script.voice.track_row_anchor(row, true, {6.0f, 7.0f, 8.0f});
    frame = f.world.script.voice.frame(f.world, {});
    CHECK(frame.position.x == 6.0f && frame.position.z == 8.0f);
    f.world.script.voice.track_row_anchor(f.speaker, false, {});
    CHECK(!f.world.script.voice.ready()); // another anchor's state is ignored
    f.world.script.voice.track_row_anchor(row, false, {});
    CHECK(f.world.script.voice.ready());
}

int main() {
    test_wave_ready_and_physical_completion();
    test_entity_voice_anchors_at_the_speaker_or_falls_back_to_a_oneshot();
    test_radio_sets_share_channel_zero_and_keep_unity_pitch();
    test_failed_replacement_invalidates_before_loading();
    test_spatial_speaker_and_radio_anchor();
    test_invalid_speaker_does_not_interrupt_and_dead_anchor_stops();
    test_item_type_gate_and_playback_definition_gate_are_distinct();
    test_retry_restarts_channel_without_accepting_old_completion();
    std::printf(failures ? "WAC voice tests FAILED (%d)\n" : "WAC voice tests passed\n", failures);
    return failures ? 1 : 0;
}
