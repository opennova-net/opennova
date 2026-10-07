#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <runtime/world/weather_state.h>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::editor {

class MissionDocument;
class ProjectAssetSource;
struct MissionSceneHeader;

// A sound the mission's script, its weather or its items raised on the clock (what the Listen hears of them): the
// clock's tick it rose on, the set, and where the game plays it from: Thunder and Relative at a distance along a
// bearing from the listener (the weather's lightning, world::weather_thunder_sounds; the script's `sound`,
// WacCmd_Sound @ 0x4ed590 -> Sound_PlayTriggerSetScaled @ 0x527b90), Positional at a mission point (the script's
// `soundsettossn` and `soundtotarget` [orig: WacCmd_SoundSetToSsn @ 0x4f1dd0; WacCmd_SoundToTarget @ 0x4f7f60 ->
// Entity_PlaySound3D_FullVolume @ 0x528e20], and an item's time-of-day shot, items.def's dawnshot, dayshot, duskshot
// and nightshot replayed at its sound point at random intervals [orig: Entity_SpawnRegionalEffect @ 0x408290]).
struct MissionScriptSound {
	enum class Kind : uint8_t { Thunder, Relative, Positional };
	Kind kind = Kind::Thunder;
	int32_t tick = 0;
	std::string set;
	int32_t distance_q16 = 0; // Thunder, Relative
	int32_t bearing = 0;      // Thunder, Relative: a 0..255 turn from the listener's facing
	double at[3] = { 0.0, 0.0, 0.0 }; // Positional: the mission point, metres
	int shot = -1; // Positional: an item's time-of-day shot, its region (0 dawn .. 3 night); -1 the script's
};
// "thunder", "relative", "positional".
const char *mission_script_sound_kind_token(MissionScriptSound::Kind kind);

// A mission's start and its script as the game runs them, for the mission view's Listen (ADR 0046 DI-36): the mission
// booted by the engine's own kernel over the project's files (mission::MissionKernel, the one mission boot, ADR 0042
// d3) to its start as a headless host starts it: the weather seeded from the mission's .env under its header's
// overrides through the one derivation (env::weather_seed_from_config), then the start's boundary
// (MissionKernel::complete_mission_start: the script's first execution, the environment's initializer and the 255-tick
// settle) [orig: Game_StartMission @ 0x525cb8 -> WacScript_InitAndLoad @ 0x4f91f0; Environment_MissionStartInit
// @ 0x57f1e0]. Then on the preview clock, a game tick at a time, the script alone as the game runs it while a player
// plays (the WAC on its own divider, every 62nd tick, its admission open [orig: WacScript_AdvanceTick @ 0x4f81a0; the
// gate @ 0x51d8bd reads a human]) and the weather tick after it (MissionKernel::tick_weather: the rain's and the
// overcast's springs, the lightning sequencers and their thunder) [orig: Environment_UpdateWeatherTick @ 0x57e9b0, after
// the entity update @ 0x52674b]; and between them the entity update's two cohort walks of the statics and the markers
// (world::tick_item_event_pool: each item's class think on its age clock, whose env-sound class replays its
// time-of-day shots [orig: Entity_UpdateAllEntities @ 0x4c2244..0x4c2398 -> Entity_SpawnRegionalEffect @ 0x408290]).
// Nothing else of the world runs: no entity moves, so a condition on one reads its start (the picture has no
// simulation, DI-31's rule), and one on the player reads none (the listener stands in for the player's ears, not its
// body). The mission's clock stands at the picture's hour (set_hours), as the Listen's sources read it. A step back of the clock restores the start (MissionKernel::restore_baseline) and runs
// on to it; a jump runs every tick on the way, at most kMissionScriptCatchUpTicks a call, so a long one catches up over
// the frames, and only what its last second raised is heard.
class MissionScriptRun {
public:
	MissionScriptRun();
	~MissionScriptRun();
	MissionScriptRun(const MissionScriptRun &) = delete;
	MissionScriptRun &operator=(const MissionScriptRun &) = delete;

	// The start booted again where what it read moved: the mission's header (its environment, its clock), the mission's
	// name (its script's), or a file the boot read (a script, the .env, a table) by its stamp; an edit of the document's
	// entities does not boot it again. True when it booted.
	bool follow(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
			const MissionSceneHeader &header, const std::string &basename);
	// The script and the weather run on toward the clock's tick `tick` (a tick back: from the start again), at most
	// kMissionScriptCatchUpTicks of it; true when it stands there.
	bool run_to(int32_t tick);
	// The mission clock held at `hours` (the picture's hour): what the items' shots read.
	void set_hours(double hours);
	// Nothing held (Listen off, no mission).
	void close();

	bool booted() const { return kernel_ != nullptr; }
	// Why the start does not run ("" it does).
	const std::string &error() const { return error_; }
	// The weather as the script has it now (null before a boot).
	const world::WeatherState *weather() const;
	// The sounds raised since the last take, oldest first.
	std::vector<MissionScriptSound> take_sounds();
	// The tick it ran to; how many times it booted, the last boot's cost (microseconds); the script's executions; and
	// whether the mission compiled a script (a layer of game.wac, server.wac and <mission>.wac the project holds).
	int32_t tick() const { return tick_; }
	uint64_t boots() const { return boots_; }
	int64_t boot_us() const { return boot_us_; }
	uint32_t script_runs() const;
	bool scripted() const { return scripted_; }

private:
	struct Read {
		std::string name;
		uint64_t stamp = 0;
	};
	void boot_(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
			const MissionSceneHeader &header, const std::string &basename);
	// One game tick: the script's pass, then the weather's; what they raised taken.
	void step_();

	std::unique_ptr<mission::MissionKernel> kernel_;
	std::shared_ptr<std::vector<Read>> reads_; // the files the boot read, each with its stamp then
	std::string key_; // the header's fields the boot read, and the mission's name
	std::string error_;
	int32_t tick_ = 0;
	uint64_t boots_ = 0;
	int64_t boot_us_ = 0;
	bool scripted_ = false;
	bool failed_ = false; // the last boot failed: not tried again until what it read moves
	uint32_t clock_fixed24_ = 12u << 24; // the picture's hour on the 8.24 clock
	std::vector<MissionScriptSound> sounds_;
};

// How many game ticks one run takes at most (a minute of the game's): a longer run (a jump of the clock) goes on at
// the next call.
inline constexpr int32_t kMissionScriptCatchUpTicks = 62 * 60;

} // namespace opennova::editor
