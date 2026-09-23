#include <runtime/wac/wac_layered_load.h>

#include <formats/mus/mus.h>
#include <formats/rtxt/rtxt.h>
#include <formats/wac/bytecode.h>
#include <formats/wac/program.h>
#include <runtime/wac/compiler.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/particle/effect_catalog_names.h>
#include <formats/particle/parser.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::wac {

void load_script_sound_sets(const mission::BootFileSource &files,
        const std::string &mission_basename, audio::SoundSetIndex &sounds) {
    sounds.clear();
    if (!files.valid()) return;
    std::vector<std::string> chain = audio::global_bank_chain(files.expansion_name);
    if (!mission_basename.empty()) chain.insert(chain.begin(), mission_basename + ".lwf");
    int bank_index = 0;
    for (const auto &name : chain) {
        std::vector<uint8_t> bytes;
        lwf::File bank;
        std::string error;
        if (files.read_file(name, bytes) && lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank, error))
            sounds.add_bank(bank_index++, bank);
    }
}

void load_script_effect_catalog(const mission::BootFileSource &files,
        particle::EffectCatalogNames &effects, particle::EffectSceneConfig *scene) {
    effects.clear();
    if (scene != nullptr) scene->documents.clear();
    if (!files.valid() || !files.list_files) return;
    // The effect world's mounted PTL + regional table load order.
    // [orig: CEffectSystem_Init @0x5F6070]
    const std::string regional = files.has_file("fgn2.bin") ? ".ptg" : ".ptu";
    for (const std::string &extension : {std::string(".ptl"), regional}) {
        for (const std::string &name : files.list_files(extension)) {
            std::vector<uint8_t> bytes;
            particle::ParticleFile document;
            particle::ParseError parse_error;
            if (files.read_file(name, bytes) && particle::load_particles_from_buffer(
                    reinterpret_cast<const char *>(bytes.data()), bytes.size(), document, parse_error)) {
                effects.add_document(document);
                if (scene != nullptr) scene->documents.push_back({name, std::move(document)});
            }
        }
    }
}

WacLayeredLoadStatus wac_layered_load(WacSystem &system,
		const mission::BootFileSource &files,
		const std::string &mission_basename, world::World *world,
		bool strict_diagnostics, std::string &error, particle::EffectCatalogNames *effect_catalog,
        const audio::SoundSetIndex *sound_catalog,
        const std::shared_ptr<opennova::mus::MusGlobals> &music_globals) {
	error.clear();
    CompileEnv env;
    env.music_globals = music_globals;
	if (!files.valid()) {
        // Even without source files retail installs a terminator, executes the
        // VM entry/exit, and increments the mutable clock at startup.
        // [orig: WacScript_InitAndLoad @0x4F91F0 -> @0x4F976B/@0x4F9770]
        system.set_program(compile_program({}, env));
        return WacLayeredLoadStatus::kAbsent;
    }
	// The original layering, absent files skipped in order
	// [orig: WacScript_InitAndLoad @0x4f91f0].
	std::vector<std::string> sources;
    env.load_source = [&files](const std::string &name, std::string &source) {
        std::vector<uint8_t> bytes;
        if (!files.has_file(name) || !files.read_file(name, bytes)) return false;
        source.assign(bytes.begin(), bytes.end());
        return true;
    };
	for (const std::string &name : {std::string("game.wac"),
				 std::string("server.wac"), mission_basename + ".wac"}) {
		if (name == ".wac") continue; // no mission basename authored
		std::vector<uint8_t> bytes;
		if (!files.has_file(name) || !files.read_file(name, bytes)) continue;
		sources.emplace_back(bytes.begin(), bytes.end());
        env.source_names.push_back(name);
	}
	env.registry = world != nullptr ? &world->registry : nullptr;
	// The GLOOP operand reads the dword behind a variable, event or engine
	// word as the load finds it: the installed program's VM and the world,
	// or the music context. [orig: Script_Compile @0x4F368A..0x4F3693]
	env.load_dword = [&system, world, music_globals](uint32_t ref) -> uint32_t {
		if (operand_kind(ref) == OperandKind::MusicVar)
			return music_globals ? uint32_t(mus::mus_globals_read(*music_globals, operand_index(ref))) : 0u;
		return world != nullptr ? uint32_t(system.vm().current_value(*world, ref)) : 0u;
	};
    // WAC binds ammo names during compilation, before the initial execution.
    // Read the same mounted ammo.def/table builder as MissionKernel: this
    // temporary table supplies indices, while World owns live ballistics.
    // [orig: WacScript_ResolveParameter @0x4F2920 -> AmmoDef_LookupByName]
    world::AmmoTable ammo;
    std::vector<uint8_t> ammo_bytes;
    if (files.has_file("ammo.def") && files.read_file("ammo.def", ammo_bytes)) {
        def::DefAmmoFile parsed = {};
        if (def::def_parse_ammo_memory(ammo_bytes.data(), ammo_bytes.size(), &parsed) == 0) {
            ammo = world::build_ammo_table(parsed);
            def::def_free_ammo(&parsed);
        }
    }
    env.ammo = &ammo;
    audio::SoundSetIndex temporary_sounds;
    if (!sound_catalog) load_script_sound_sets(files, mission_basename, temporary_sounds);
    env.sounds = sound_catalog ? sound_catalog : &temporary_sounds;
    particle::EffectCatalogNames temporary_effects;
    if (!effect_catalog) load_script_effect_catalog(files, temporary_effects);
    env.effects = effect_catalog ? effect_catalog : &temporary_effects;
	// TextToken keys resolve here, at the compile: the expansion's override
	// table first, then the mission's text table (<mission>.bin, else
	// medmssn.bin), then gametext.bin. With no mission table loaded, or no
	// entry, every key answers the one shared "".
	// [orig: MissionText_GetStringByKeyOrGameText @0x51ECD0 ->
	// TextResource_FindEntryByKey @0x75D450 (the override test @0x75D461);
	// the tables: TextResource_LoadMissionTextBin @0x51ED90, Expansion_LoadAssets
	// @0x4A4730 (the TextResource_LoadOverrideTable call @0x4A49DE)]
	rtxt::File mission_text, game_text, override_text;
	const auto load_text = [&files](const std::string &name, rtxt::File &table) {
		std::vector<uint8_t> bytes;
		std::string parse_error;
		return files.has_file(name) && files.read_file(name, bytes) &&
				rtxt::parse(bytes.data(), bytes.size(), table, parse_error);
	};
	std::vector<uint8_t> mission_text_bytes;
	std::string text_error;
	const bool has_mission_text =
			mission::resolve_mission_text(files, mission_basename, mission_text_bytes) !=
					mission::MissionTextSource::kNone &&
			rtxt::parse(mission_text_bytes.data(), mission_text_bytes.size(), mission_text, text_error);
	const bool has_game_text = load_text("gametext.bin", game_text);
	const bool has_override = !files.expansion_name.empty() &&
			load_text("expansion\\" + files.expansion_name + "\\" + files.expansion_name + ".bin",
					override_text);
	env.text_token = [&](const std::string &key) -> std::optional<std::string> {
		if (!has_mission_text) return std::nullopt;
		if (has_override && override_text.has(key)) return override_text.get(key);
		if (mission_text.has(key)) return mission_text.get(key);
		if (has_game_text && game_text.has(key)) return game_text.get(key);
		return std::nullopt;
	};
	Program program = compile_program(sources, env);
	// Only strict mode refuses, and only a catalog miss (Diagnostic::error).
	if (strict_diagnostics && !program.ok()) {
		error = "WAC for " + mission_basename + " misses the mounted catalogs (" +
				std::to_string(program.error_count()) + " error(s), " +
				std::to_string(program.diagnostics.size()) + " diagnostic(s))";
		for (const Diagnostic &diagnostic : program.diagnostics) {
			if (!diagnostic.error) continue;
			const std::string file = diagnostic.source < program.source_names.size()
					? program.source_names[diagnostic.source] : std::string();
			error += ": " + file + " (" + std::to_string(diagnostic.line) + ") " + diagnostic.message;
			break;
		}
		return WacLayeredLoadStatus::kBlocked;
	}
	system.set_program(std::move(program));
	return sources.empty() ? WacLayeredLoadStatus::kAbsent : WacLayeredLoadStatus::kLoaded;
}

} // namespace opennova::wac
