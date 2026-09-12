#include <runtime/wac/wac_layered_load.h>

#include <formats/wac/program.h>
#include <runtime/wac/compiler.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/particle/effect_catalog_names.h>
#include <formats/particle/parser.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/bank_chain.h>

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
		const std::string &mission_basename, world::EntityRegistry *registry,
		bool strict_diagnostics, std::string &error, particle::EffectCatalogNames *effect_catalog,
        const audio::SoundSetIndex *sound_catalog) {
	error.clear();
	if (!files.valid()) return WacLayeredLoadStatus::kAbsent;
	// The original layering, absent files skipped in order
	// [orig: WacScript_InitAndLoad @0x4f91f0].
	std::vector<std::string> sources;
    CompileEnv env;
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
	env.registry = registry;
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
    if (sources.empty()) return WacLayeredLoadStatus::kAbsent;
	Program program = compile_program(sources, env);
	// ok() is false only when a diagnostic carries error=true, so the strict
	// arm's "every diagnostic is fatal" test subsumes it.
	const bool blocked =
			strict_diagnostics ? !program.diagnostics.empty() : !program.ok();
	if (blocked) {
		error = "WAC for " + mission_basename + " failed to compile cleanly (" +
				std::to_string(program.error_count()) + " error(s), " +
				std::to_string(program.diagnostics.size()) + " diagnostic(s))";
		for (const Diagnostic &diagnostic : program.diagnostics) {
			error += ": line " + std::to_string(diagnostic.line) + ", column " +
					std::to_string(diagnostic.col) + ": " + diagnostic.message;
			break;
		}
		return WacLayeredLoadStatus::kBlocked;
	}
	system.set_program(std::move(program));
	return WacLayeredLoadStatus::kLoaded;
}

} // namespace opennova::wac
