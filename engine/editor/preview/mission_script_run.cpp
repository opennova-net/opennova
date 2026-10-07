#include <editor/preview/mission_script_run.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <base/io/tick_rate.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_scene.h>
#include <formats/env/env.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/destruction.h>

namespace opennova::editor {

namespace {

// The header's fields the start reads (its environment and its overrides, its clock), and the mission's name.
std::string key_of(const MissionSceneHeader &header, const std::string &basename) {
	std::string key = basename + "|" + header.environment + "|" + std::to_string(header.start_time) + "|" +
			std::to_string(header.minutes_per_day) + "|" + std::to_string(header.attrib_flags) + "|" +
			std::to_string(header.water_override) + "|" + std::to_string(header.fog_override) + "|" +
			std::to_string(header.water_murk);
	for (int i = 0; i < 3; ++i)
		key += "|" + std::to_string(header.fog_color[i]) + "," + std::to_string(header.water_color[i]);
	return key;
}

// The dialogs and the script voices the tick's effects raised (the Listen's, DI-32): a Play dialog's queue
// [orig: EventAction_Dispatch @ 0x454461, "dialog"] and a voice channel's wave [orig: Wac_PlayScriptedVoiceWave
// @ 0x4ed610, WacCmd_SsnWave @ 0x4f78d0, WacCmd_SsnRadio @ 0x4f79b0: ScriptVoiceChannel::start's "dialog_wav"].
void take_voices(const world::WorldOutbox &out, int32_t tick, std::vector<MissionScriptSound> &sounds) {
	for (const world::Effect &effect : out.effects.entries()) {
		MissionScriptSound heard;
		heard.tick = tick;
		if (effect.kind == "dialog") {
			heard.kind = MissionScriptSound::Kind::Dialog;
			heard.dialog = effect.a;
		} else if (effect.kind == "dialog_wav" && !effect.str.empty()) {
			heard.kind = MissionScriptSound::Kind::Voice;
			heard.wave = effect.str;
		} else {
			continue;
		}
		sounds.push_back(std::move(heard));
	}
}

// The presentation the script's world raised that no one here takes, let go as a host with no presenter lets it go
// (WorldOutbox::discard_presentation), with the wire's queues a host's tick would send.
void let_go(world::WorldOutbox &out) {
	out.discard_presentation();
	out.entity_events.clear();
	out.hud_relays.clear();
	out.powerup_grants.clear();
	out.powerup_weapon_grants.clear();
	out.script_remote_commands.clear();
}

} // namespace

const char *mission_script_sound_kind_token(MissionScriptSound::Kind kind) {
	switch (kind) {
	case MissionScriptSound::Kind::Relative: return "relative";
	case MissionScriptSound::Kind::Positional: return "positional";
	case MissionScriptSound::Kind::Dialog: return "dialog";
	case MissionScriptSound::Kind::Voice: return "voice";
	case MissionScriptSound::Kind::Thunder: break;
	}
	return "thunder";
}

MissionScriptRun::MissionScriptRun() = default;
MissionScriptRun::~MissionScriptRun() = default;

const world::WeatherState *MissionScriptRun::weather() const {
	return kernel_ ? &kernel_->world.weather : nullptr;
}

uint32_t MissionScriptRun::script_runs() const {
	return kernel_ ? kernel_->wac.runs() : 0;
}

std::vector<MissionScriptSound> MissionScriptRun::take_sounds() {
	std::vector<MissionScriptSound> out;
	out.swap(sounds_);
	return out;
}

void MissionScriptRun::close() {
	kernel_.reset();
	reads_.reset();
	key_.clear();
	error_.clear();
	sounds_.clear();
	start_sounds_.clear();
	tick_ = 0;
	failed_ = false;
	scripted_ = false;
}

bool MissionScriptRun::follow(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
		const MissionSceneHeader &header, const std::string &basename) {
	if (!files) {
		close();
		return false;
	}
	// What the last boot read, as it stands now.
	bool moved = key_of(header, basename) != key_;
	if (!moved && reads_)
		for (const Read &read : *reads_)
			if (files->stamp(read.name) != read.stamp) {
				moved = true;
				break;
			}
	if (!moved && (kernel_ || failed_)) return false;
	boot_(files, mission, header, basename);
	return true;
}

void MissionScriptRun::boot_(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
		const MissionSceneHeader &header, const std::string &basename) {
	const auto start = std::chrono::steady_clock::now();
	kernel_.reset();
	sounds_.clear();
	start_sounds_.clear();
	tick_ = 0;
	error_.clear();
	failed_ = false;
	scripted_ = false;
	key_ = key_of(header, basename);
	++boots_;
	// The project's files as the game looks them up (an open document standing in for its file), each name the boot
	// asks for noted with its stamp then: a later change of one boots the start again.
	auto reads = std::make_shared<std::vector<Read>>();
	reads_ = reads;
	mission::BootFileSource source;
	source.has_file = [files, reads](const std::string &name) {
		const uint64_t stamp = files->stamp(name);
		reads->push_back({ name, stamp });
		return stamp != 0;
	};
	source.read_file = [files, reads](const std::string &name, std::vector<uint8_t> &out) {
		reads->push_back({ name, files->stamp(name) });
		return files->read(name, out);
	};
	bms::File composed;
	if (!mission.compose(composed)) {
		error_ = "The mission does not compose, so its start does not run.";
		failed_ = true;
		return;
	}
	// The mission's .env under the header's overrides, as the game loads it with the terrain [orig:
	// Game_LoadTerrainDuringConnect @ 0x520710 -> Terrain_LoadEnvironmentConfig @ 0x52073b]; a mission naming none, or
	// one the project lacks, starts on the engine's defaults (env::load_mission_env).
	env::Config config;
	std::vector<uint8_t> env_bytes;
	const bool env_read = !header.environment.empty() && source.read_file(header.environment + ".env", env_bytes);
	const std::string env_text(env_bytes.begin(), env_bytes.end());
	env::load_mission_env(env_read ? &env_text : nullptr, config);
	env::apply_bms_overrides(config, env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
			header.fog_override, header.fog_color, header.water_color, header.water_murk));
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->open_document(std::move(composed), basename, source);
	// The start as a headless host boots it, beside the picture's own choices: no player of its own (the listener
	// stands in for one's ears), no collision, seat or terrain tables (nothing moves), the boundary held for the
	// weather's seed.
	mission::KernelBootOptions options;
	options.playable = false;
	options.collision = false;
	options.seat_specs = false;
	options.terrain = false;
	options.defer_mission_start = true;
	std::string error;
	if (!kernel->boot(options, error)) {
		error_ = "The mission's start does not run: " + error;
		failed_ = true;
		return;
	}
	// The items' time-of-day shots bound by name over the banks the boot's catalog read, as the game's simulation binds
	// them once its catalogs stand (Simulation::apply_sound_state_to_world) [orig: ItemDef_ResolveAllResources
	// @ 0x49e5f0].
	if (const def::DefItemsFile *items = kernel->items_table()) mission::resolve_item_event_sounds(kernel->world, *items);
	// The seed from the .env and the header's clock, then the start's boundary: the script's first execution, the
	// initializer and the 255-tick settle [orig: Environment_SnapStateToTargets @ 0x57d1e0; Game_StartMission
	// @ 0x525cb8 -> Environment_MissionStartInit @ 0x57f878].
	kernel->world.weather.seed(env::weather_seed_from_config(config, kernel->mission.header));
	kernel->complete_mission_start();
	// What the settle raised is the start's own, never heard: the mission has not begun. The dialogs the pre-mission
	// pass queued play once it has [docs/audio/lwf-dbf-sound-re.md: a PreMission PlayWavList is registered at the
	// start and plays back serially]: heard at tick 0.
	take_voices(kernel->world.out, 0, start_sounds_);
	sounds_ = start_sounds_;
	kernel->world.out.weather_sounds.clear();
	kernel->world.out.slot_sounds.clear();
	let_go(kernel->world.out);
	scripted_ = kernel->wac_loaded;
	kernel_ = std::move(kernel);
	boot_us_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void MissionScriptRun::step_() {
	mission::MissionKernel &kernel = *kernel_;
	world::WorldOutbox &out = kernel.world.out;
	const int32_t tick = tick_ + 1;
	// The script's pass, as the game's tick runs it while a player plays: the WAC on its divider, its admission open.
	world::TickContext context;
	context.world = &kernel.world;
	context.logic_tick = kernel.world.logic_tick;
	context.is_authority = true;
	context.phase = world::TickPhase::Gameplay;
	context.script_admitted = true;
	kernel.wac.tick(kernel.world, context);
	// The mission's events after it, in the script pass's order (the WAC, then the events): a Play dialog queues its
	// dialog [orig: EventAction_Dispatch @ 0x4542e0, case 7 @ 0x454461].
	kernel.events.tick(kernel.world, context);
	// The entity update's cohort walks of the statics and the markers alone: each item's class think, the env-sound
	// class's time-of-day shots among them [orig: Entity_UpdateAllEntities @ 0x4c2244..0x4c2398].
	kernel.world.weather.tod_fixed24 = clock_fixed24_;
	world::tick_item_event_pool(kernel.world, 2);
	world::tick_item_event_pool(kernel.world, 3);
	++kernel.world.logic_tick;
	// Then the weather's tick, the clock held at the picture's hour.
	kernel.world.weather.tod_advance_per_tick = 0;
	kernel.tick_weather();
	for (const world::WeatherSoundEvent &sound : out.weather_sounds) {
		MissionScriptSound heard;
		heard.kind = MissionScriptSound::Kind::Thunder;
		heard.tick = tick;
		heard.set = "THUNDER";
		heard.distance_q16 = sound.distance_q16;
		heard.bearing = sound.bearing;
		sounds_.push_back(std::move(heard));
	}
	out.weather_sounds.clear();
	for (const world::ScriptSoundEvent &sound : out.script_sounds) {
		if (sound.kind != world::ScriptSoundEvent::Kind::ListenerRelative) continue;
		MissionScriptSound heard;
		heard.kind = MissionScriptSound::Kind::Relative;
		heard.tick = tick;
		heard.set = sound.name;
		heard.distance_q16 = sound.distance_q16;
		heard.bearing = sound.bearing;
		sounds_.push_back(std::move(heard));
	}
	for (const world::SoundSlotEvent &sound : out.slot_sounds) {
		if (sound.set_name[0] == '\0') continue;
		MissionScriptSound heard;
		heard.kind = MissionScriptSound::Kind::Positional;
		heard.tick = tick;
		heard.set = sound.set_name;
		if (sound.slot >= audio::kSlotShotDawn && sound.slot < audio::kSlotShotDawn + 4)
			heard.shot = int(sound.slot) - int(audio::kSlotShotDawn);
		for (int axis = 0; axis < 3; ++axis) heard.at[axis] = double(sound.pos[axis]) / 65536.0;
		sounds_.push_back(std::move(heard));
	}
	out.slot_sounds.clear();
	take_voices(out, tick, sounds_);
	let_go(out);
	tick_ = tick;
}

void MissionScriptRun::set_hours(double hours) {
	const double day = std::fmod(std::max(hours, 0.0), 24.0);
	clock_fixed24_ = uint32_t(std::llround(day * double(1u << 24))) % (24u << 24);
}

bool MissionScriptRun::run_to(int32_t tick) {
	if (!kernel_) return true;
	tick = std::max(tick, 0);
	if (tick < tick_) {
		// Back: the start again (its sealed point, the script's state with it).
		kernel_->restore_baseline();
		let_go(kernel_->world.out);
		kernel_->world.out.weather_sounds.clear();
		kernel_->world.out.slot_sounds.clear();
		sounds_ = start_sounds_;
		tick_ = 0;
	}
	const int32_t run = std::min(tick - tick_, kMissionScriptCatchUpTicks);
	for (int32_t i = 0; i < run; ++i) step_();
	// Of a run longer than a second, only what its last second raised is heard (the rest would sound at once).
	const int32_t heard_from = tick_ - io::kTicksPerSecondInt;
	sounds_.erase(std::remove_if(sounds_.begin(), sounds_.end(),
						  [heard_from](const MissionScriptSound &sound) { return sound.tick <= heard_from; }),
			sounds_.end());
	return tick_ == tick;
}

} // namespace opennova::editor
