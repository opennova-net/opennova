#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/tick_rate.h>
#include <editor/preview/mission_scene.h>
#include <runtime/world/sound_emitter_mailbox.h>
#include <runtime/world/weather_state.h>

namespace opennova {
class ResourceIndex;
}
namespace opennova::assets {
class AssetStore;
}
namespace opennova::mission {
class MissionKernel;
}
namespace opennova::world {
class World;
struct Entity;
}

namespace opennova::editor {

class MissionDocument;
class ProjectAssetSource;

// A sound the mission's script, its weather or its items raised on the clock (what the Listen hears of them): the
// clock's tick it rose on, the set, and where the game plays it from: Thunder and Relative at a distance along a
// bearing from the listener (the weather's lightning, world::weather_thunder_sounds; the script's `sound`,
// WacCmd_Sound @ 0x4ed590 -> Sound_PlayTriggerSetScaled @ 0x527b90), Positional at a mission point (the script's
// `soundsettossn` and `soundtotarget` [orig: WacCmd_SoundSetToSsn @ 0x4f1dd0; WacCmd_SoundToTarget @ 0x4f7f60 ->
// Entity_PlaySound3D_FullVolume @ 0x528e20], and an item's time-of-day shot, items.def's dawnshot, dayshot, duskshot
// and nightshot replayed at its sound point at random intervals [orig: Entity_SpawnRegionalEffect @ 0x408290]).
// Dialog: a Play dialog action of the mission's events, its dialog's number (DI-32: the game queues the dialog on
// its one dialog channel [orig: EventAction_Dispatch @ 0x454461 -> Dialog_PlayByIndex @ 0x527ae0]; the Listen plays
// its lines). Voice: a script's voice wave on the scripted voice channel (SSNwave, SSNradio; wave and pwave where a
// local player stands, which the Listen has none of [orig: Wac_PlayScriptedVoiceWave @ 0x4ed610, its local player
// test; WacCmd_SsnWave @ 0x4f78d0; WacCmd_SsnRadio @ 0x4f79b0]), its file.
struct MissionScriptSound {
	enum class Kind : uint8_t { Thunder, Relative, Positional, Dialog, Voice };
	Kind kind = Kind::Thunder;
	int32_t tick = 0;
	std::string set;
	int32_t distance_q16 = 0; // Thunder, Relative
	int32_t bearing = 0;      // Thunder, Relative: a 0..255 turn from the listener's facing
	double at[3] = { 0.0, 0.0, 0.0 }; // Positional: the mission point, metres
	int shot = -1; // Positional: an item's time-of-day shot, its region (0 dawn .. 3 night); -1 the script's
	int32_t dialog = 0; // Dialog: its number (dlg%03i)
	std::string wave;   // Voice: the .wav it plays
};
// An ambient loop an item's update registered on the clock: a placed genx item's hum, its time-of-day region's set at
// its sound point on its cohort's last visit of the batch (world::update_item_ambient_sound [orig:
// Entity_UpdateWaterPhysicsAndEffects @0x4A93C5..0x4A93D6 -> Entity_UpdateEnvSoundEmitter @0x4A8080]), on the clock's
// tick it rose on.
struct MissionScriptEmitter {
	int32_t tick = 0;
	world::SoundEmitterEvent event;
};

// "thunder", "relative", "positional", "dialog", "voice".
const char *mission_script_sound_kind_token(MissionScriptSound::Kind kind);

// A seat the mission's start took (S27, the maintainer's call): a person its script's first execution seated before
// the first frame (`ssn2ssn` arming the person's AI slot +0x90 with a carrier, then `ssnuse` boarding it [orig:
// WacCmd_SsnToSsn @ 0x4F7330; WacScript_TryMountEntityToVehicle @ 0x4F70F0]; the PreMission pass seats none,
// docs/world/world-wac-ai-re.md 41.1 "No mount at the start"), as its first update poses it: the seat's pose the
// carrier's seat bone gives (world::VehicleSystem::pose_mounted_occupant, the body heading, pitch and roll its seat
// block writes [orig: Entity_UpdateInfantryAI @0x4BED72..0x4BEDDA -> Entity_GetBoneTransformAndOrientation
// @0x4B0C50]) and the clip its seat block requests (world::AiSystem::seat_animation_state). The rider and the carrier
// by the records the start promoted them from (world::Entity::spawn_origin: a pool and a place there, the file's order,
// MissionEntityMark's pool and index); a carrier the start spawned itself (an addeweap child) is no record. The units a
// record's: metres (x east, y north, z up) and degrees (the yaw a record's yaw, the pitch and roll its pitch and roll).
struct MissionStartSeat {
	int rider = -1; // its place among the organics
	MissionPool carrier_pool = MissionPool::Item;
	int carrier = -1; // its place in its pool; -1: no record
	double x = 0.0, y = 0.0, z = 0.0;
	double pitch = 0.0, yaw = 0.0, roll = 0.0;
	int state = -1; // the seat's clip (g_AnimStateNameTable's index; world::infantry_anim_key names its row)
	bool operator==(const MissionStartSeat &o) const;
	bool operator!=(const MissionStartSeat &o) const { return !(*this == o); }
};

// A mission's start and its script as the game runs them, for the mission view's Listen (ADR 0046 DI-36) and the seats
// its start takes (S27; the mission view follows it while its scene holds a person or it listens): the mission
// booted by the engine's own kernel over the project's files (mission::MissionKernel, the one mission boot, ADR 0042
// d3) to its start as a headless host starts it: the weather seeded from the mission's .env under its header's
// overrides through the one derivation (env::weather_seed_from_config), then the start's boundary
// (MissionKernel::complete_mission_start: the script's first execution, the environment's initializer and the 255-tick
// settle) [orig: Game_StartMission @ 0x525cb8 -> WacScript_InitAndLoad @ 0x4f91f0; Environment_MissionStartInit
// @ 0x57f1e0]. Then on the preview clock, a game tick at a time, the script alone as the game runs it while a player
// plays (the WAC on its own divider, every 62nd tick, its admission open [orig: WacScript_AdvanceTick @ 0x4f81a0; the
// gate @ 0x51d8bd reads a human]), the mission's events after it in the script pass's order (BmsEventSystem: the
// timed and variable conditions fire as the game's do, a Play dialog queuing its dialog, DI-32 [orig:
// EventTrigger_UpdateAllEntries via Server_TickUpdate; the pre-mission pass at the start, Game_StartMission
// @ 0x525b86]) and the weather tick after them (MissionKernel::tick_weather: the rain's and the
// overcast's springs, the lightning sequencers and their thunder) [orig: Environment_UpdateWeatherTick @ 0x57e9b0, after
// the entity update @ 0x52674b]; and between them the entity update's two cohort walks of the statics and the markers
// (world::tick_item_event_pool: each item's class think on its age clock, whose env-sound class replays its
// time-of-day shots [orig: Entity_UpdateAllEntities @ 0x4c2244..0x4c2398 -> Entity_SpawnRegionalEffect @ 0x408290]).
// The boot takes the seat table (the items' seats from their models, as the game's load builds them), so a start that
// seats a person seats it as the game's does (start_seats). Nothing else of the world runs: no entity moves, so a condition on one reads its start (the picture has no
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
	// name (its script's), `records` (the entities' shape: mission_start_records), or a file the boot read (a script,
	// the .env, a table) by its stamp; another edit of the document's entities (a move) does not boot it again. True
	// when it booted.
	bool follow(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
			const MissionSceneHeader &header, const std::string &basename, uint64_t records = 0);
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
	// The items' ambient loops registered since the last take, oldest first (a step back of the clock drops them).
	std::vector<MissionScriptEmitter> take_emitters();
	// Every set an item of the mission loops (its time-of-day loops, each once): what the Listen describes for them.
	const std::vector<std::string> &item_loop_sets() const { return loop_sets_; }
	// The seats the start took (S27), in pool 0's order: what the mission view seats its people on.
	const std::vector<MissionStartSeat> &start_seats() const { return start_seats_; }
	// `seat` (one of start_seats) posed again by the game's seat pose over its carrier placed where `carrier` (its
	// record) stands now, as a move of the record places it [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6]: its
	// point, its angles and its clip (sit_24's follows the hull's roll). False (`out` the seat as taken): no such seat,
	// or no start.
	bool seat_on(const MissionStartSeat &seat, const MissionEntityMark &carrier, MissionStartSeat &out) const;
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
	// The seats the start took (start_seats).
	void take_seats_(mission::MissionKernel &kernel);
	struct SeatBodies;
	// A seat's pose and clip over `carrier` (its point, angles and state into `out`).
	static void pose_seat_(world::World &w, const SeatBodies &bodies, const world::Entity &carrier, MissionStartSeat &out);

	// The project's files as the start's models read them (the seat table's, the items' graphic models): an index over
	// the boot's reads and the kernel's asset store over it (MissionKernel::set_assets), which the kernel's own store,
	// over a mount the start never opens, does not serve. Declared ahead of the kernel, which points at them.
	std::unique_ptr<ResourceIndex> index_;
	std::unique_ptr<assets::AssetStore> models_;
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
	std::vector<MissionScriptEmitter> emitters_;
	std::vector<std::string> loop_sets_;
	std::vector<MissionStartSeat> start_seats_;
	// Each start seat's rider and carrier as the start left them, and its seat (seat_on poses copies).
	std::vector<std::shared_ptr<const SeatBodies>> seat_bodies_;
	// What the start raised that the game plays once the mission runs: the dialogs the pre-mission pass queued (heard
	// at tick 0, kept for a step back to the start).
	std::vector<MissionScriptSound> start_sounds_;
};

// The shape of a mission's entities its start reads (S27): each record's pool, item and SSN, and what decides whether
// the spawn takes it (its attributes, its team), in the scene's order (an added or removed record, another item, SSN,
// attribute or team, boots the start again; a move does not).
uint64_t mission_start_records(const MissionScene &scene);

// How many game ticks one run takes at most (a minute of the game's): a longer run (a jump of the clock) goes on at
// the next call.
inline constexpr int32_t kMissionScriptCatchUpTicks = io::kTicksPerSecondInt * 60;

} // namespace opennova::editor
