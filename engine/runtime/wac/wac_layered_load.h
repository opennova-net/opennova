#pragma once

// The game.wac / server.wac / <mission>.wac layered load: read the layers in
// the retail order through the embedder's mounted-file source, compile them as
// ONE program (V#/G# scope spans the layers), and install it on the WacSystem
// [orig: WacScript_InitAndLoad @0x4f91f0]. Promoted from the retail-mission rig's
// install_wac step (ADR 0042 d3); the dedicated host (apps/nw_server) is the
// strict-mode consumer, through KernelBootOptions::wac_strict_diagnostics.

#include <runtime/mission/runtime_boot.h> // mission::BootFileSource
#include <runtime/wac/wac_system.h>

#include <string>

namespace opennova::world {
class EntityRegistry;
}

namespace opennova::particle { class EffectScene; }
namespace opennova::audio { class SoundSetIndex; }

namespace opennova::wac {

enum class WacLayeredLoadStatus {
	kAbsent,  // no layer exists: the valid BMS-only mission (no error)
	kLoaded,  // compiled and installed on the WacSystem
	kBlocked, // diagnostics blocked the load; `error` says why, scripts stay off
};

// `registry` lets symbolic group/area names resolve to interned ids; mission
// promotion has populated it by the time the boot's install_wac step runs.
// Lenient (`strict_diagnostics` false — the game's policy, and the rig's):
// only a program that failed to compile (`!ok()`) is blocked; the shared
// compiler labels recoverable/legacy syntax issues as warnings so retail
// scripts keep running. Strict (the dedicated golden host's policy): EVERY
// diagnostic is fatal — running a partial script is a known wire-parity
// failure.
WacLayeredLoadStatus wac_layered_load(WacSystem &system,
		const mission::BootFileSource &files,
		const std::string &mission_basename, world::EntityRegistry *registry,
		bool strict_diagnostics, std::string &error,
        particle::EffectScene *effect_catalog = nullptr,
        const audio::SoundSetIndex *sound_catalog = nullptr);

// Same mounted mission/global chain as the audio host; no WAV decode needed.
void load_script_sound_sets(const mission::BootFileSource &files,
        const std::string &mission_basename, audio::SoundSetIndex &sounds);

} // namespace opennova::wac
