#pragma once

// The game.wac / server.wac / <mission>.wac layered load: read the layers in
// the retail order through the embedder's mounted-file source, compile them as
// ONE program (V#/G# scope spans the layers), and install it on the WacSystem
// [orig: WacScript_InitAndLoad @0x4f91f0]. Promoted from the retail-mission rig's
// install_wac step (ADR 0042 d3).

#include <runtime/mission/runtime_boot.h> // mission::BootFileSource
#include <runtime/wac/wac_system.h>

#include <string>

namespace opennova::world {
class World;
}

namespace opennova::particle { class EffectCatalogNames; struct EffectSceneConfig; }
namespace opennova::audio { class SoundSetIndex; }

namespace opennova::wac {

enum class WacLayeredLoadStatus {
	kAbsent,  // no layer exists: an empty program is installed for the BMS-only mission
	kLoaded,  // compiled and installed on the WacSystem
};

// `world` lends its registry, so symbolic group names resolve to the mission's
// groups (mission promotion has populated it by the time the boot's
// install_wac step runs), and the values a GLOOP operand reads before the load
// resets them (CompileEnv::load_dword). Retail never refuses a script: it
// keeps the first error and runs what compiled [orig: WacScript_InitAndLoad
// @0x4F91F0 ignores the compiles' returns @0x4F94A8 / @0x4F950E / @0x4F9597
// and executes @0x4F976B]. The FX and SOUNDSET literals bind against the
// embedder's catalogs (MissionKernel builds both once per boot, see
// load_script_effect_catalog / load_script_sound_sets); a caller without them
// gets temporaries read from the same files.
WacLayeredLoadStatus wac_layered_load(WacSystem &system,
		const mission::BootFileSource &files,
		const std::string &mission_basename, world::World *world,
        particle::EffectCatalogNames *effect_catalog = nullptr,
        const audio::SoundSetIndex *sound_catalog = nullptr,
        const std::shared_ptr<opennova::mus::MusGlobals> &music_globals = {});

// The global bank chain the audio host searches (audio::global_bank_chain); no
// WAV decode needed.
void load_script_sound_sets(const mission::BootFileSource &files, audio::SoundSetIndex &sounds);

// The effect NAMES the mounted .ptl files plus the regional .ptu/.ptg table
// define, in the effect world's load order (first registration wins)
// [orig: CEffectSystem_Init @0x5F6070], then the names the mission start
// pools before the WAC compile (mission_effect_interns.h), so the compile's
// FX handles are retail's. Native hosts can also retain the parsed documents
// to run callback-owned effects without a renderer.
void load_script_effect_catalog(const mission::BootFileSource &files,
        particle::EffectCatalogNames &effects, particle::EffectSceneConfig *scene = nullptr);

} // namespace opennova::wac
