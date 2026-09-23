#include <runtime/wac/remote_command.h>

#include <formats/wac/command.h>
#include <runtime/world/world.h>

#include <base/io/strutil.h>

namespace opennova::wac {
namespace {

bool ieq(const char *a, const char *b) { return opennova::strutil::iequals(a, b); }

std::string table_name(const std::vector<std::string> *table, int32_t handle) {
    return table != nullptr && handle > 0 && size_t(handle) <= table->size()
            ? (*table)[size_t(handle) - 1] : std::string();
}

} // namespace

bool is_remote_command(int command_index) {
    return command_index >= 0 && command_index < wac_command_count() &&
            cmd_is_replicated(wac_commands()[command_index]);
}

int remote_command_wire_index(int command_index) {
    if (command_index < 0 || command_index >= wac_command_count()) return command_index;
    const uint32_t handler = wac_commands()[command_index].handler_ea;
    for (int i = 0; i < wac_command_count(); ++i)
        if (wac_commands()[i].handler_ea == handler) return i;
    return command_index;
}

RemoteCommandResult run_remote_command(world::World &w, int cmd,
                                       const std::vector<world::ScriptRemoteArg> &args,
                                       const RemoteCommandNames &names) {
    if (!is_remote_command(cmd)) return {};
    const CommandDef &def = wac_commands()[cmd];
    const char *n = def.name;
    auto A = [&](size_t i) -> int32_t { return i < args.size() ? args[i].value : 0; };
    auto H = [&](size_t i) -> world::EntityHandle {
        return i < args.size() ? world::EntityHandle{uint16_t(args[i].value)}
                               : world::EntityHandle{};
    };
    auto S = [&](size_t i) -> std::string { return i < args.size() ? args[i].text : std::string(); };
    auto FX = [&](size_t i) -> std::string { return table_name(names.effects, A(i)); };
    auto SOUND = [&](size_t i) -> std::string { return table_name(names.sounds, A(i)); };
    auto &cmds = w.commands;

    // ---- sound / effect ----
    if (ieq(n, "SS2SSN")) return {true, cmds.play_ssn_soundset(H(1), SOUND(0))};
    if (ieq(n, "sound2tgt")) return {true, cmds.sound_at_target(A(0), SOUND(0), A(1))};
    if (ieq(n, "sound")) return {true, cmds.direct_sound(SOUND(0), A(1), A(2))};
    if (ieq(n, "fx2ssn")) return {true, cmds.effect_at_ssn(A(0), FX(0), H(1))};
    if (ieq(n, "fx2tgt")) return {true, cmds.effect_at_target(A(0), FX(0), A(1))};
    if (ieq(n, "targetfx")) { cmds.spawn_marker_particle_effects(A(0)); return {true, 0}; }
    if (ieq(n, "teleSSN")) return {true, cmds.wac_teleport_ssn(H(0), A(1))};
    // [orig: Sbf_StartEntry @0x4ED910] The WAC stream handle is null: its
    // opener @0x4ED6C0 has no callers in retail JO. Preserve the success
    // return without touching the separate AudioVM/MUS music context.
    if (ieq(n, "music")) return {true, 1};
    // [orig: WacCmd_Face @0x4ED5D0]
    if (ieq(n, "face")) {
        const world::Entity *entity = w.registry.get(w.cached.local_player);
        if (entity == nullptr) return {true, 1};
        w.facials.override_expression(*entity, A(0));
        return {true, 0};
    }

    // ---- SSN state ----
    if (ieq(n, "hideSSN")) return {true, cmds.set_ssn_hidden(H(0), true) ? 1 : 0};
    if (ieq(n, "unhideSSN")) return {true, cmds.set_ssn_hidden(H(0), false) ? 1 : 0};
    if (ieq(n, "holdSSN")) return {true, cmds.set_ssn_held(H(0), true) ? 1 : 0};
    if (ieq(n, "unholdSSN")) return {true, cmds.set_ssn_held(H(0), false) ? 1 : 0};
    if (ieq(n, "disableSSN")) return {true, cmds.set_ssn_disabled(H(0), true) ? 1 : 0};
    if (ieq(n, "enableSSN")) return {true, cmds.set_ssn_disabled(H(0), false) ? 1 : 0};

    // ---- environment ---- (world::WeatherState carries the handler cites)
    // All three return 1 [orig: WacCmd_Quake @0x4ED4CE; Env_TriggerLightningFlashA
    // @0x4ED50A; Env_TriggerLightningFlashB @0x4ED51A].
    if (ieq(n, "quake")) { cmds.quake(A(0)); return {true, 1}; }
    if (ieq(n, "flash")) { cmds.lightning_flash(); return {true, 1}; }
    if (ieq(n, "farflash")) { cmds.lightning_far_flash(); return {true, 1}; }

    // ---- player text / debug console ----
    // text/ptext feed the player message channel [orig: WAC text @ 0x4EDB50
    // -> Chat_AddMessageChannel1 @ 0x4985D0], while consol/pconsol feed the
    // distinct on-screen debug channel [orig: @ 0x4EDBE0 ->
    // Chat_AddDebugMessage]. Keep them separate so game hosts can present
    // mission text without leaking authored debug output into the HUD.
    // Every one returns 1 [orig: Chat_AddSystemMessage @0x4EDB64;
    // Chat_AddFormattedIntMessage @0x4EDBC0; Wac_ConsolDebugMessage @0x4EDBF4;
    // WacCmd_ConsolNumber @0x4EDC50].
    if (ieq(n, "text") || ieq(n, "ptext")) {
        w.out.effects.push({"text", 0, 0, 0, 0, S(0)});
        return {true, 1};
    }
    if (ieq(n, "consol") || ieq(n, "pconsol")) {
        w.out.effects.push({"debug_text", 0, 0, 0, 0, S(0)});
        return {true, 1};
    }
    if (ieq(n, "text#")) {
        w.out.effects.push({"text", A(1), 0, 0, 0, S(0)});
        return {true, 1};
    }
    if (ieq(n, "consol#")) {
        w.out.effects.push({"debug_text", A(1), 0, 0, 0, S(0)});
        return {true, 1};
    }

    // A dedicated, mission-owned voice channel with synchronous asset resolution
    // and physical playback completion from the host.
    // [orig: @0x4ED610, @0x4F78D0, @0x4F79B0]
    if (ieq(n, "wave") || ieq(n, "pwave")) return {true, w.script.voice.wave(w, S(0))};
    if (ieq(n, "SSNwave")) return {true, w.script.voice.ssn_wave(w, H(0), S(1), A(2), false)};
    if (ieq(n, "SSNradio")) return {true, w.script.voice.ssn_wave(w, H(0), S(1), 0, true)};
    // No branch above owns the row: report the miss so the caller records
    // the dispatch gap (the same kind WacVm::dispatch records locally).
    return {};
}

} // namespace opennova::wac
