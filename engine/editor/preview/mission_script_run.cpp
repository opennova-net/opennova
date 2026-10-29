#include <editor/preview/mission_script_run.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

#include <base/io/fixed.h>
#include <base/io/hash.h>
#include <base/io/tick_rate.h>
#include <base/resource_index/resource_index.h>
#include <base/title/title.h>
#include <base/vfs/file_source.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_scene.h>
#include <formats/env/env.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/assets/asset_store.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

namespace {

// A file source of two calls (the boot's reads, each noted with its stamp then).
class CalledFiles final : public FileSource {
public:
	std::function<bool(const std::string &, std::vector<uint8_t> &)> reads;
	std::function<uint64_t(const std::string &)> stamps;
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return reads(name, out); }
	uint64_t stamp(const std::string &name) const override { return stamps(name); }
};

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

std::vector<MissionScriptEmitter> MissionScriptRun::take_emitters() {
	std::vector<MissionScriptEmitter> out;
	out.swap(emitters_);
	return out;
}

std::vector<MissionScriptSound> MissionScriptRun::take_sounds() {
	std::vector<MissionScriptSound> out;
	out.swap(sounds_);
	return out;
}

bool MissionStartSeat::operator==(const MissionStartSeat &o) const {
	return rider == o.rider && carrier_pool == o.carrier_pool && carrier == o.carrier && x == o.x && y == o.y &&
			z == o.z && pitch == o.pitch && yaw == o.yaw && roll == o.roll && state == o.state;
}

struct MissionScriptRun::SeatBodies {
	world::Entity rider, carrier;
	world::InfantryState body; // the rider's channels (what its seat's clip reads: its .adm)
	world::Seat seat;
};

uint64_t mission_start_records(const MissionScene &scene) {
	uint64_t key = io::kFnv1a64Offset;
	for (const MissionEntityMark &mark : scene.entities()) {
		key = io::fnv1a64_value(key, uint8_t(mark.pool));
		key = io::fnv1a64_value(key, mark.item);
		key = io::fnv1a64_value(key, int32_t(mark.ssn));
		// Whether the spawn takes the record: its attributes' session flags and its team [orig:
		// Entity_SpawnFromBMSRecord @ 0x40ea94..0x40eac2, the team @ 0x40eb1c].
		key = io::fnv1a64_value(key, mark.attributes);
		key = io::fnv1a64_value(key, int32_t(mark.team));
	}
	return key;
}

void MissionScriptRun::close() {
	kernel_.reset();
	models_.reset();
	index_.reset();
	reads_.reset();
	key_.clear();
	error_.clear();
	sounds_.clear();
	start_sounds_.clear();
	start_seats_.clear();
	seat_bodies_.clear();
	tick_ = 0;
	failed_ = false;
	scripted_ = false;
}

bool MissionScriptRun::follow(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
		const MissionSceneHeader &header, const std::string &basename, uint64_t records) {
	if (!files) {
		close();
		return false;
	}
	// What the last boot read, as it stands now.
	bool moved = key_of(header, basename) + "|" + std::to_string(records) != key_;
	if (!moved && reads_)
		for (const Read &read : *reads_)
			if (files->stamp(read.name) != read.stamp) {
				moved = true;
				break;
			}
	if (!moved && (kernel_ || failed_)) return false;
	boot_(files, mission, header, basename);
	key_ += "|" + std::to_string(records);
	return true;
}

void MissionScriptRun::boot_(const std::shared_ptr<const ProjectAssetSource> &files, const MissionDocument &mission,
		const MissionSceneHeader &header, const std::string &basename) {
	const auto start = std::chrono::steady_clock::now();
	kernel_.reset();
	models_.reset();
	index_.reset();
	sounds_.clear();
	start_sounds_.clear();
	start_seats_.clear();
	seat_bodies_.clear();
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
	// The mission's environment under the header's overrides, as the game loads it with the terrain
	// (env::load_mission_env_config): its .trn, overcast.def, then its .env over them; a mission naming none, or one
	// the project lacks, starts on the earlier passes over the engine's defaults.
	const env::EnvTextReader read_env = [&source](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!source.read_file(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
	env::MissionEnv mission_env;
	env::load_mission_env_config(read_env, header.terrain, header.environment,
			env::bms_env_overrides_from_header(header.attrib_flags, header.water_override, header.fog_override,
					header.fog_color, header.water_color, header.water_murk),
			mission_env);
	// The models the start reads (the seat table's, each item's graphic model: whether a carrier offers a seat) through
	// the same noted reads [orig: Game_ReloadEntityModelsAndCallbacks @0x522830, the model loads].
	auto read_models = std::make_shared<CalledFiles>();
	read_models->reads = source.read_file;
	read_models->stamps = [files, reads](const std::string &name) {
		const uint64_t stamp = files->stamp(name);
		reads->push_back({ name, stamp });
		return stamp;
	};
	auto index = std::make_unique<ResourceIndex>();
	index->mount_source(read_models);
	auto models = std::make_unique<assets::AssetStore>(index.get());
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->set_assets(models.get());
	kernel->open_document(std::move(composed), basename, source);
	// The start as a headless host boots it, beside the picture's own choices: no player of its own (the listener
	// stands in for one's ears), no collision or terrain tables (nothing moves), the seat table (a start's script may
	// seat a person, S27), the boundary held for the weather's seed.
	mission::KernelBootOptions options;
	// The rules its start runs: the default title's; the preview names no project's game.
	options.title_rules = &world::title_rules(default_title().code);
	options.playable = false;
	options.collision = false;
	options.seat_specs = true;
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
	kernel->world.weather.seed(env::weather_seed_from_config(mission_env.config, kernel->mission.header));
	kernel->complete_mission_start();
	take_seats_(*kernel);
	// The sets the items loop, each once.
	loop_sets_.clear();
	for (const auto &[item, traits] : kernel->world.tables.item_death_traits.rows)
		for (const std::string &set : traits.regional_loops)
			if (!set.empty() && std::find(loop_sets_.begin(), loop_sets_.end(), set) == loop_sets_.end())
				loop_sets_.push_back(set);
	// What the settle raised is the start's own, never heard: the mission has not begun. The dialogs the pre-mission
	// pass queued play once it has [docs/audio/lwf-dbf-sound-re.md: a PreMission PlayWavList is registered at the
	// start and plays back serially]: heard at tick 0.
	take_voices(kernel->world.out, 0, start_sounds_);
	sounds_ = start_sounds_;
	kernel->world.out.weather_sounds.clear();
	kernel->world.out.slot_sounds.clear();
	kernel->world.out.sound_emitters.clear();
	emitters_.clear();
	let_go(kernel->world.out);
	scripted_ = kernel->wac_loaded;
	index_ = std::move(index);
	models_ = std::move(models);
	kernel_ = std::move(kernel);
	boot_us_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void MissionScriptRun::take_seats_(mission::MissionKernel &kernel) {
	world::World &w = kernel.world;
	// A record's pool by the kind its spawn origin packs (promote's spawn_origin_pack over world::EntityKind).
	const auto pool_of = [](int kind, MissionPool &out) {
		switch (world::EntityKind(kind)) {
		case world::EntityKind::Organic: out = MissionPool::Organic; return true;
		case world::EntityKind::Building: out = MissionPool::Building; return true;
		case world::EntityKind::Marker: out = MissionPool::Marker; return true;
		case world::EntityKind::Item: out = MissionPool::Item; return true;
		}
		return false;
	};
	// Each person the start seated, its record's, posed on a copy as its first update poses it (the start's own words
	// stand: the game poses the rider on its update, after the start) [orig: Entity_UpdateInfantryAI
	// @0x4BED72..0x4BEDDA].
	w.registry.for_each_in_pool(0, [&](const world::Entity &row) {
		const world::Entity *carrier = row.mounted ? w.registry.get(row.mount_target) : nullptr;
		const world::AiEntity *body = carrier ? w.ai.for_handle(row.handle) : nullptr;
		MissionPool rider_pool = MissionPool::Item;
		world::Seat seat;
		if (body == nullptr || row.spawn_origin == world::kSpawnOriginNone ||
				!pool_of(world::spawn_origin_kind(row.spawn_origin), rider_pool) || rider_pool != MissionPool::Organic ||
				!w.vehicles.rider_seat(row, *carrier, seat))
			return;
		auto bodies = std::make_shared<SeatBodies>();
		bodies->rider = row;
		bodies->carrier = *carrier;
		bodies->body = body->inf;
		bodies->seat = seat;
		MissionStartSeat taken;
		taken.rider = int(world::spawn_origin_index(row.spawn_origin));
		if (carrier->spawn_origin != world::kSpawnOriginNone &&
				pool_of(world::spawn_origin_kind(carrier->spawn_origin), taken.carrier_pool))
			taken.carrier = int(world::spawn_origin_index(carrier->spawn_origin));
		pose_seat_(w, *bodies, *carrier, taken);
		start_seats_.push_back(taken);
		seat_bodies_.push_back(std::move(bodies));
	});
}

void MissionScriptRun::pose_seat_(world::World &w, const SeatBodies &bodies, const world::Entity &carrier,
		MissionStartSeat &out) {
	const auto metres = [](const world::PositionWord &word) { return double(word.q16) / io::kFp16OneD; };
	world::Entity posed = bodies.rider;
	const world::MountedPose pose = w.vehicles.pose_mounted_occupant(posed, carrier, bodies.seat);
	out.x = metres(pose.position.x);
	out.y = metres(pose.position.y);
	out.z = metres(pose.position.z);
	out.yaw = world::mission_yaw_deg_from_bam_heading(pose.heading);
	out.pitch = world::degrees_from_angle_bam(pose.pitch);
	out.roll = world::degrees_from_angle_bam(pose.roll);
	out.state = w.ai.seat_animation_state(bodies.body, carrier, bodies.seat);
}

bool MissionScriptRun::seat_on(const MissionStartSeat &seat, const MissionEntityMark &carrier, MissionStartSeat &out) const {
	out = seat;
	if (!kernel_) return false;
	for (size_t i = 0; i < start_seats_.size() && i < seat_bodies_.size(); ++i) {
		if (start_seats_[i] != seat) continue;
		// The carrier's words where the record stands now, as its spawn takes them (its position's three dwords, its
		// Yaw word from 90 less the yaw, its pitch and roll) [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6].
		world::Entity placed = seat_bodies_[i]->carrier;
		const auto word = [](double metres) { return int32_t(std::lround(metres * io::kFp16OneD)); };
		placed.position.set_q16({ word(carrier.x), word(carrier.y), word(carrier.z) });
		placed.heading = world::placed_heading_bam(carrier.yaw);
		placed.pitch_bam = world::spawn_angle_bam(carrier.pitch);
		placed.roll_bam = world::spawn_angle_bam(carrier.roll);
		if (placed.veh.yaw_seeded) {
			placed.veh.yaw_bam = placed.heading;
			placed.veh.air_pitch_bam = placed.pitch_bam;
			placed.veh.air_roll_bam = placed.roll_bam;
		}
		pose_seat_(kernel_->world, *seat_bodies_[i], placed, out);
		return true;
	}
	return false;
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
		heard.set = world::kThunderSoundSet;
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
		for (int axis = 0; axis < 3; ++axis) heard.at[axis] = double(sound.pos[axis]) / io::kFp16OneD;
		sounds_.push_back(std::move(heard));
	}
	out.slot_sounds.clear();
	// The items' ambient loops their update registered this tick (a genx item's hum, on its cohort's visit).
	for (world::SoundEmitterEvent &event : out.sound_emitters.drain()) emitters_.push_back({ tick, std::move(event) });
	take_voices(out, tick, sounds_);
	let_go(out);
	tick_ = tick;
}

void MissionScriptRun::set_hours(double hours) {
	// The hour on the 8.24 clock, as the game's dev scrub sets it (world::WeatherState::tod_fixed24_from_minutes).
	clock_fixed24_ = world::WeatherState::tod_fixed24_from_minutes(hours * 60.0);
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
		kernel_->world.out.sound_emitters.clear();
		sounds_ = start_sounds_;
		emitters_.clear();
		tick_ = 0;
	}
	const int32_t run = std::min(tick - tick_, kMissionScriptCatchUpTicks);
	// The run is the frame's batch: its last tick is the one an item's update registers its loop on, as the game's frame
	// marks the last of its batch [orig: dword_24E0E80, read @0x4A93CC].
	for (int32_t i = 0; i < run; ++i) {
		kernel_->world.rules.last_tick_of_batch = i + 1 == run;
		step_();
	}
	// Of a run longer than a second, only what its last second raised is heard (the rest would sound at once).
	const int32_t heard_from = tick_ - io::kTicksPerSecondInt;
	sounds_.erase(std::remove_if(sounds_.begin(), sounds_.end(),
						  [heard_from](const MissionScriptSound &sound) { return sound.tick <= heard_from; }),
			sounds_.end());
	return tick_ == tick;
}

} // namespace opennova::editor
