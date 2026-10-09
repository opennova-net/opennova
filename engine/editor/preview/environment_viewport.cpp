#include <editor/preview/environment_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/environment_document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/tod_clock.h>
#include <formats/mission/bms.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/precipitation.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The size its device draws at where no canvas sizes it (a headless Shell's).
constexpr ViewportState kHeadlessSize{ 1024, 768 };
// A new camera: a person's eye a metre behind the point it orbits, looking a little up at the horizon.
constexpr float kEyeDistance = 1.0f;
constexpr float kEyePitch = -0.08f;
constexpr double kFixed24PerHour = 16777216.0;

std::string base_of(const std::string &path) {
	std::string name = path;
	if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
	return name;
}

std::string stem_of(const std::string &path) {
	std::string name = base_of(path);
	if (const size_t dot = name.find_last_of('.'); dot != std::string::npos) name.erase(dot);
	return name;
}

std::string hex_of(const env::Rgb &rgb) {
	const auto byte = [](float c) { return std::clamp(int(c * 255.0f + 0.5f), 0, 255); };
	char text[8];
	std::snprintf(text, sizeof(text), "#%02x%02x%02x", byte(rgb.r), byte(rgb.g), byte(rgb.b));
	return text;
}

double percent_of(int32_t q16) {
	return double(q16) * 100.0 / 65536.0;
}

JsonValue command_json(const EnvironmentWeatherCommand &command) {
	JsonValue out = JsonValue::make_object();
	out.set("percent", json_number(command.percent));
	out.set("seconds", json_number(command.seconds));
	return out;
}

bool read_command(const JsonValue &json, const char *name, EnvironmentWeatherCommand &out, std::string &error) {
	const std::string what = std::string("options.") + name;
	if (!json.is_object()) {
		error = what + " is {percent, seconds}: a script's " + name + "(percent, seconds).";
		return false;
	}
	EnvironmentWeatherCommand command = out;
	for (const io::JsonMember &member : json.object) {
		int64_t whole = 0;
		if (member.key == "percent") {
			if (!io::json_whole_in(member.value, 0, 100, whole)) {
				error = what + ".percent is a whole percent from 0 to 100.";
				return false;
			}
			command.percent = int(whole);
		} else if (member.key == "seconds") {
			if (!io::json_whole_in(member.value, 0, kEnvironmentCommandSecondsMost, whole)) {
				error = what + ".seconds is the whole seconds it takes to get there, 0 to " +
				        std::to_string(kEnvironmentCommandSecondsMost) + ".";
				return false;
			}
			command.seconds = int(whole);
		} else {
			error = "Unknown member \"" + member.key + "\" of " + what + " (it takes percent, seconds).";
			return false;
		}
	}
	out = command;
	return true;
}

// A SetViewport's options over `held` (each member optional); `uses` the missions that run on the
// environment (a mission named must be one), `named_rain` and `named_overcast` whether a command was named.
bool read_options(const JsonValue &json, const EnvironmentUses &uses, EnvironmentViewportOptions &held,
		bool &named_rain, bool &named_overcast, std::string &error) {
	named_rain = named_overcast = false;
	if (!json.is_object()) {
		error = "options is an object {time, day_seconds, mission, header, rain, overcast, show}.";
		return false;
	}
	EnvironmentViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "time") {
			if (value.is_null()) {
				options.time = -1.0;
				continue;
			}
			if (!value.is_number() || !(value.number >= 0.0) || !(value.number < 24.0)) {
				error = "options.time is the hour the clock runs from, 0 to below 24 (null: the game's start).";
				return false;
			}
			options.time = value.number;
		} else if (key == "day_seconds") {
			int64_t whole = 0;
			if (!io::json_whole_in(value, 0, 24 * 60 * 60, whole)) {
				error = "options.day_seconds is a day's length in seconds, 0 (the game's) to 86400.";
				return false;
			}
			options.day_seconds = int(whole);
		} else if (key == "mission") {
			if (!value.is_string()) {
				error = "options.mission is the path of a mission that runs on the environment (\"\" the first).";
				return false;
			}
			if (!value.string.empty()) {
				const auto found = std::find_if(uses.missions.begin(), uses.missions.end(),
						[&](const EnvironmentMissionUse &use) { return use.mission == value.string; });
				if (found == uses.missions.end()) {
					std::string listed;
					for (size_t i = 0; i < uses.missions.size(); ++i) listed += (i ? ", " : "") + uses.missions[i].mission;
					error = "No mission that runs on the environment is " + value.string +
					        (listed.empty() ? std::string(" (none does).") : " (" + listed + ").");
					return false;
				}
			}
			options.mission = value.string;
		} else if (key == "header") {
			if (!value.is_bool()) {
				error = "options.header is true or false: the mission header's fog and water over the environment.";
				return false;
			}
			options.header = value.boolean;
		} else if (key == "rain") {
			if (!read_command(value, "rain", options.rain, error)) return false;
			named_rain = true;
		} else if (key == "overcast") {
			if (!read_command(value, "overcast", options.overcast, error)) return false;
			named_overcast = true;
		} else if (key == "show") {
			if (!value.is_object()) {
				error = "options.show is {terrain, water}.";
				return false;
			}
			for (const io::JsonMember &shown : value.object) {
				if ((shown.key != "terrain" && shown.key != "water") || !shown.value.is_bool()) {
					error = "options.show takes terrain and water, each true or false.";
					return false;
				}
				(shown.key == "terrain" ? options.terrain : options.water) = shown.value.boolean;
			}
		} else {
			error = "Unknown environment option \"" + key +
			        "\" (it takes time, day_seconds, mission, header, rain, overcast, show).";
			return false;
		}
	}
	held = options;
	return true;
}

JsonValue keyframe_colours(const env::TodState &tod, bool night) {
	JsonValue out = JsonValue::make_object();
	out.set("light", json_string(hex_of(night ? tod.moon : tod.sun)));
	out.set("sun", json_string(hex_of(tod.sun)));
	out.set("moon", json_string(hex_of(tod.moon)));
	out.set("ground", json_string(hex_of(tod.ground)));
	out.set("sky", json_string(hex_of(tod.sky)));
	out.set("fog", json_string(hex_of(tod.fog)));
	out.set("skyfog", json_string(hex_of(tod.skyfog)));
	return out;
}

} // namespace

const char *environment_view_status_token(EnvironmentViewStatus status) {
	switch (status) {
	case EnvironmentViewStatus::NoProject: return "no_project";
	case EnvironmentViewStatus::NoEnvironment: return "no_environment";
	case EnvironmentViewStatus::Unwritable: return "unwritable";
	case EnvironmentViewStatus::Ready: return "ready";
	}
	return "no_project";
}

const char *environment_clock_from_token(EnvironmentClockFrom from) {
	switch (from) {
	case EnvironmentClockFrom::Option: return "option";
	case EnvironmentClockFrom::Mission: return "mission";
	case EnvironmentClockFrom::Environment: return "environment";
	case EnvironmentClockFrom::Default: break;
	}
	return "default";
}

std::string environment_clock_words(double hours) {
	// The nearest minute, as the mission format's header_time_to_hhmm rounds one (a truncating 16.16 keyframe
	// time reads as the minute it was written at), the day wrapped.
	const double wrapped = std::fmod(std::max(hours, 0.0), 24.0);
	const int minutes = int(std::lround(wrapped * 60.0)) % (24 * 60);
	char text[8];
	std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
	return text;
}

io::JsonValue environment_options_to_json(const EnvironmentViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("time", options.time >= 0.0 ? json_number(options.time) : JsonValue::make_null());
	out.set("day_seconds", json_number(options.day_seconds));
	out.set("mission", json_string(options.mission));
	out.set("header", JsonValue::make_bool(options.header));
	out.set("rain", command_json(options.rain));
	out.set("overcast", command_json(options.overcast));
	JsonValue show = JsonValue::make_object();
	show.set("terrain", JsonValue::make_bool(options.terrain));
	show.set("water", JsonValue::make_bool(options.water));
	out.set("show", std::move(show));
	return out;
}

std::string environment_options_change(const EnvironmentViewportOptions &options, const EnvironmentViewportOptions &held) {
	JsonValue out = environment_options_to_json(options);
	// A command is issued each time a change names it: only the one that moved.
	JsonValue taken = JsonValue::make_object();
	for (io::JsonMember &member : out.object) {
		if (member.key == "rain" && options.rain == held.rain) continue;
		if (member.key == "overcast" && options.overcast == held.overcast) continue;
		taken.set(member.key, std::move(member.value));
	}
	return viewport_change(ViewportKind::Environment, "options", std::move(taken));
}

std::string environment_camera_change(const OrbitCamera &camera) {
	// What a SetViewport takes: the camera's own members in the mission's terms (mission_camera.h).
	JsonValue view = mission_camera_to_json(camera);
	JsonValue taken = JsonValue::make_object();
	for (const char *member : { "target", "yaw", "pitch", "distance" })
		if (const JsonValue *value = view.get(member)) taken.set(member, *value);
	return viewport_change(ViewportKind::Environment, "camera", std::move(taken));
}

// --- EnvironmentViewport ---------------------------------------------------------------------------

EnvironmentViewport::EnvironmentViewport(std::string path) :
		ViewportModel(ViewportKind::Environment, std::move(path), kHeadlessSize) {
	camera_.yaw = 0.0f;
	camera_.pitch = kEyePitch;
	camera_.distance = kEyeDistance;
	camera_.target = PreviewVec3{ 0.0f, kEnvironmentEyeHeight, 0.0f };
	camera_.near_plane = 0.5f;
	camera_.far_plane = 8000.0f;
}

std::unique_ptr<ViewportModel> EnvironmentViewport::make(const std::string &path) {
	return std::make_unique<EnvironmentViewport>(path);
}

ViewportStatus EnvironmentViewport::status() const {
	switch (reason_) {
	case EnvironmentViewStatus::Ready: return ViewportStatus::Ready;
	case EnvironmentViewStatus::Unwritable: return ViewportStatus::Failed;
	case EnvironmentViewStatus::NoProject:
	case EnvironmentViewStatus::NoEnvironment: break;
	}
	return ViewportStatus::Empty;
}

std::string EnvironmentViewport::message() const {
	switch (reason_) {
	case EnvironmentViewStatus::NoProject: return "Open a project to see its environments.";
	case EnvironmentViewStatus::NoEnvironment: return "Open an environment to see its sky.";
	case EnvironmentViewStatus::Unwritable: return "The environment cannot be written, so the game would read no such file: " + detail_;
	case EnvironmentViewStatus::Ready: break;
	}
	return std::string();
}

std::string EnvironmentViewport::caption() const {
	if (reason_ != EnvironmentViewStatus::Ready) return std::string();
	std::string out = " at " + environment_clock_words(hours());
	if (const EnvironmentMissionUse *use = mission()) out += ", over " + base_of(use->mission);
	return out;
}

const EnvironmentMissionUse *EnvironmentViewport::mission() const {
	for (const EnvironmentMissionUse &use : uses_.missions)
		if (use.mission == mission_path_) return &use;
	return nullptr;
}

double EnvironmentViewport::hours() const {
	return double(weather_.tod_fixed24) / kFixed24PerHour;
}

ViewportAction EnvironmentViewport::stop_(EnvironmentViewStatus reason, const std::string &detail) {
	reason_ = reason;
	detail_ = detail;
	shown_none();
	return picture_.stop();
}

bool EnvironmentViewport::read_(const SessionView &view, const DocumentBase &document, std::string &why) {
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const bool document_moved = document.identity() != read_identity_ || document.load_generation() != read_load_ ||
	                            document.revision() != read_revision_;
	const bool files_moved = generation != read_generation_ && read_stamps_.moved(files);
	const bool terrain_moved = header_.terrain != read_terrain_;
	if (!document_moved && !files_moved && !terrain_moved && read_generation_ != UINT64_MAX) {
		why = unwritable_;
		return unwritable_.empty();
	}
	read_identity_ = document.identity();
	read_load_ = document.load_generation();
	read_revision_ = document.revision();
	read_generation_ = generation;
	read_terrain_ = header_.terrain;
	read_stamps_.clear();
	// The game reads the file Save would write: a document that cannot be written leaves none to read.
	const SerializeResult written = document.serialize();
	unwritable_.clear();
	if (!written.ok()) {
		unwritable_ = written.issues.front().message;
		why = unwritable_;
		return false;
	}
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path()) : nullptr;
	file_name_ = entry ? entry->logical_name : base_of(path());
	// As the drawn mission's load reads it (env::load_mission_env): its terrain's .trn, then overcast.def (the
	// overcast table, by the runtime's own name), then this file over them.
	const auto read_text = [&](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		const bool found = files.read(name, bytes);
		read_stamps_.note(name, files.stamp(name));
		text.assign(bytes.begin(), bytes.end());
		return found;
	};
	std::string terrain_text, overcast_text, text;
	env::MissionEnvTexts texts;
	if (!header_.terrain.empty() && read_text(header_.terrain + ".trn", terrain_text)) texts.terrain = &terrain_text;
	if (read_text(env::kOvercastFile, overcast_text)) texts.overcast = &overcast_text;
	if (read_text(file_name_, text)) texts.environment = &text;
	env::MissionEnv loaded;
	env::load_mission_env(texts, loaded);
	config_ = std::move(loaded.config);
	overcast_ = std::move(loaded.overcast);
	has_overcast_ = texts.overcast != nullptr || !overcast_.keyframes.empty();
	// What the home was seeded from moved: seeded again at the next step.
	seeded_ = false;
	return true;
}

bool EnvironmentViewport::place_(const SessionView &view) {
	const RevisionKey key =
			revision_key(view.revisions, { ViewConcern::Project, ViewConcern::Files, ViewConcern::Graph, ViewConcern::Documents });
	if (!uses_known_ || key != uses_key_) {
		uses_ = environment_uses(view, path());
		uses_key_ = key;
		uses_known_ = true;
	}
	// The option's mission where it still runs on it, else the first.
	const EnvironmentMissionUse *use = nullptr;
	for (const EnvironmentMissionUse &each : uses_.missions)
		if (!options_.mission.empty() && each.mission == options_.mission) use = &each;
	if (!use && !uses_.missions.empty()) use = &uses_.missions.front();
	MissionSceneHeader header;
	header.environment = stem_of(path());
	std::string name;
	if (use) {
		header.terrain = use->terrain;
		header.tile_set = use->tile_set;
		header.start_time = use->start_time;
		header.minutes_per_day = use->minutes_per_day;
		if (options_.header) {
			header.attrib_flags = use->attrib_flags;
			header.water_override = use->water_override;
			header.fog_override = use->fog_override;
			header.water_murk = use->water_murk;
			for (int i = 0; i < 3; ++i) {
				header.fog_color[i] = use->fog_color[i];
				header.water_color[i] = use->water_color[i];
			}
		}
		// The mission's own name, whose <mission>.til the ground reads: cut at its first dot as the game's extension
		// swap cuts it (mission::mission_base_name) [orig: Path_ReplaceOrAppendExtension @ 0x53c780, the scan
		// @ 0x53c7c4].
		name = mission::mission_base_name(use->mission);
	}
	const std::string drawn = use ? use->mission : std::string();
	const bool moved = header != header_ || name != mission_name_ || drawn != mission_path_;
	header_ = header;
	mission_name_ = name;
	mission_path_ = drawn;
	if (moved) {
		++layout_serial_;
		seeded_ = false;
		framed_ = false;
	}
	return moved;
}

void EnvironmentViewport::clock_() {
	const EnvironmentMissionUse *use = mission();
	const bool from_mission = use && use->read;
	// The start: the option's hour; else the clock the game starts the mission on, the header's Q8.8 start
	// hour [orig: Game_StartMission @ 0x5253ca..0x5253d5]; else the file's curtime, 16.16 hours << 8
	// [orig: TimeOfDay_ParseProperty @ 0x57d0d0] (a preview with no mission clock keeps it).
	if (options_.time >= 0.0) {
		start_fixed24_ = world::WeatherState::tod_fixed24_from_minutes(options_.time * 60.0);
		start_from_ = EnvironmentClockFrom::Option;
	} else if (from_mission) {
		start_fixed24_ = uint32_t(env::tod_start_fixed24(use->start_time));
		start_from_ = EnvironmentClockFrom::Mission;
	} else {
		start_fixed24_ = env::tod_file_clock(config_).start_fixed24;
		start_from_ = EnvironmentClockFrom::Environment;
	}
	// The rate: a day in the option's seconds (the editor's aid); else the mission's day length, 0 a clock
	// that stands [orig: Environment_SetTodAdvanceRate @ 0x57d170]; else the file's tod_rate [orig:
	// TimeOfDay_ParseProperty @ 0x57d0fe..0x57d118]; else the engine's default [orig: Environment_InitDefaults
	// @ 0x57c22f].
	if (options_.day_seconds > 0) {
		advance_ = uint32_t(env::kTodDayFixed24 / (options_.day_seconds * world::WeatherState::kWacTicksPerSecond));
		rate_from_ = EnvironmentClockFrom::Option;
	} else if (from_mission) {
		advance_ = uint32_t(env::tod_advance_per_tick(use->minutes_per_day));
		rate_from_ = EnvironmentClockFrom::Mission;
	} else {
		// The file's own clock (formats/env tod_file_clock): its tod_rate's advance, else the engine's default.
		advance_ = env::tod_file_clock(config_).advance_per_tick;
		rate_from_ = config_.tod_rate_set ? EnvironmentClockFrom::Environment : EnvironmentClockFrom::Default;
	}
}

void EnvironmentViewport::seed_(const PreviewClock &clock) {
	const int32_t ticks = clock.ticks();
	// As a mission's start seeds the weather: the environment (the header's overrides over it where the
	// option says so) through the one derivation every embedder runs, then the start's initializer.
	env::Config config = config_;
	const EnvironmentMissionUse *use = mission();
	if (options_.header && use) env::apply_bms_overrides(config, use->overrides);
	bms::Header header{};
	world::WeatherSeed seed = env::weather_seed_from_config(config, header);
	seed.tod_fixed24 = start_fixed24_;
	seed.tod_advance_per_tick = advance_;
	weather_.seed(seed);
	// The start's initializer and its settle, its 255 whole ticks before the mission runs
	// (world::WeatherState::settle_mission_start), the sim legs of each.
	world::WeatherTickEvents settle;
	weather_.settle_mission_start([&] { weather_.tick_sim(nullptr, settle); });
	// The script's weather as the options last set it, there at once.
	weather_.command_rain(options_.rain.percent, 0);
	weather_.command_overcast(options_.overcast.percent, 0);
	env::EnvScalarChannels &channels = weather_.core.scalar_channels;
	channels.rain_pct_fp = channels.rain_pct_target_fp;
	channels.overcast_fp = channels.overcast_target_fp;
	weather_.overcast_for_tod_q16 = channels.overcast_fp;
	rain_pending_ = overcast_pending_ = false;
	seeded_ = true;
	stepped_ = ticks;
	seeks_ = clock.tick_seeks();
	set_clock_(ticks);
}

void EnvironmentViewport::set_clock_(int32_t ticks) {
	// The start plus the ticks since at the advance, day-wrapped as the game's clock wraps.
	weather_.tod_advance_per_tick = advance_;
	weather_.tod_fixed24 = uint32_t(env::tod_advance(int32_t(base_fixed24_), std::max(ticks, 0), int32_t(advance_)));
	weather_.compute_night_phase();
	++clock_sets_;
}

void EnvironmentViewport::step_(const PreviewClock &clock) {
	const int32_t ticks = clock.ticks();
	const bool sought = clock.tick_seeks() != seeks_ || ticks < stepped_;
	seeks_ = clock.tick_seeks();
	if (sought) {
		// A seek moves the clock, never the weather: its springs keep their state.
		stepped_ = ticks;
		set_clock_(ticks);
		return;
	}
	const int32_t behind = ticks - stepped_;
	const int32_t run = std::min(behind, kEnvironmentCatchUpTicks);
	world::WeatherTickEvents events;
	for (int32_t i = 0; i < run; ++i) {
		weather_.tod_advance_per_tick = advance_;
		weather_.tick_sim(nullptr, events);
		++weather_ticks_;
	}
	stepped_ = ticks;
	// Caught up past what one follow runs: the clock set where the ticks put it.
	if (behind > run) set_clock_(ticks);
}

void EnvironmentViewport::follow_ground_(const SessionView &view) {
	if (header_.terrain.empty() || !view.findings.assets) return;
	ground_.follow(view.findings.assets, view.findings.assets->generation(), header_, mission_name_);
}

OrbitCamera EnvironmentViewport::framed(int, int) const {
	OrbitCamera camera = camera_;
	double point[3] = { 0.0, 0.0, 0.0 };
	double ground = 0.0;
	if (const terrain::TerrainHeightField *field = ground_.height_field()) {
		// The terrain's middle: its sector grid's [orig: the 512-unit sectors from the .trn's origin,
		// runtime/terrain_query/coords.h], the mission's y the world's -z.
		const terrain::SectorLayout &layout = field->layout;
		const double x = (double(layout.origin_x) + double(std::max(layout.sector_count, 1)) * 0.5) * terrain::COORDS_SECTOR_SIZE;
		const double z = (double(layout.origin_y) + double(std::max(layout.sector_rows, 1)) * 0.5) * terrain::COORDS_SECTOR_SIZE;
		point[0] = x;
		point[1] = -z;
		const float height = terrain::height_field_height_world_bilinear(*field, float(x), float(z));
		if (std::isfinite(height)) ground = height;
	}
	if (ground_.water()) ground = std::max(ground, ground_.water_height());
	else if (const EnvironmentMissionUse *use = mission(); use && header_.terrain.empty())
		ground = std::max(ground, double(use->water_height));
	point[2] = ground + kEnvironmentEyeHeight;
	camera.target = mission_to_preview(point);
	camera.pitch = kEyePitch;
	camera.distance = kEyeDistance;
	return camera;
}

ViewportAction EnvironmentViewport::follow_(const ViewportInput &input, PreviewClock &clock) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) return stop_(EnvironmentViewStatus::NoProject, std::string());
	const DocumentBase *document = input.document;
	if (!document || !dynamic_cast<const EnvironmentDocument *>(document))
		return stop_(EnvironmentViewStatus::NoEnvironment, std::string());
	// The mission drawn first: its terrain's .trn reads ahead of the file (read_).
	place_(view);
	std::string why;
	if (!read_(view, *document, why)) {
		const ViewportAction action = stop_(EnvironmentViewStatus::Unwritable, why);
		reason_ = EnvironmentViewStatus::Unwritable;
		detail_ = why;
		shown(*document);
		return action;
	}
	reason_ = EnvironmentViewStatus::Ready;
	detail_.clear();
	// The mission header's overrides toggled: the home seeded again over them.
	if (layout_moved_) seeded_ = false;
	layout_moved_ = false;
	// The clock's start and rate. A start that moved (a scrub, another mission, the file's curtime) runs
	// from it anew at tick 0, the clock sought as a clip newly chosen starts at its first tick; another rate
	// runs on from where the clock stands, now its base at tick 0.
	const uint32_t start = start_fixed24_, advance = advance_;
	const EnvironmentClockFrom start_from = start_from_;
	clock_();
	const bool first = !clocked_;
	clocked_ = true;
	if (first || start != start_fixed24_ || start_from != start_from_) {
		base_fixed24_ = start_fixed24_;
		clock.seek_ticks(0);
	} else if (advance != advance_) {
		base_fixed24_ = weather_.tod_fixed24;
		clock.seek_ticks(0);
	}
	if (!seeded_) seed_(clock);
	else step_(clock);
	// The commands an apply named, as a script issues them now.
	if (rain_pending_) weather_.command_rain(options_.rain.percent, options_.rain.seconds);
	if (overcast_pending_) weather_.command_overcast(options_.overcast.percent, options_.overcast.seconds);
	rain_pending_ = overcast_pending_ = false;
	// The camera on the ground once the ground is read (the one change its follow derives).
	follow_ground_(view);
	if (!framed_ && (header_.terrain.empty() || ground_.height_field() || !ground_.error().empty())) {
		framed_ = true;
		const ViewportState picture = size();
		camera_ = framed(picture.width, picture.height);
		state_moved();
	}
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const PreviewFollow::Key key{ 0, layout_serial_ };
	const bool moved = input.change != ChangeClass::None;
	switch (picture_.follow(key, moved, files, generation)) {
	case PreviewFollow::Found::Same:
		shown(*document);
		if (options_moved_) {
			options_moved_ = false;
			return ViewportAction::Update;
		}
		return ViewportAction::Keep;
	case PreviewFollow::Found::Files:
		options_moved_ = false;
		missing_.clear();
		return picture_.built(FileStamps());
	case PreviewFollow::Found::Anew: break;
	}
	picture_.show(key, generation);
	shown(*document);
	options_moved_ = false;
	missing_.clear();
	return picture_.built(FileStamps());
}

bool EnvironmentViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool EnvironmentViewport::check_(const io::JsonValue &json, std::string &error) const {
	EnvironmentViewportOptions options = options_;
	bool rain = false, overcast = false;
	if (const JsonValue *member = json.get("options");
			member && !read_options(*member, uses_, options, rain, overcast, error))
		return false;
	OrbitCamera camera = camera_;
	if (const JsonValue *member = json.get("camera"); member && !mission_camera_from_json(*member, camera, error))
		return false;
	return true;
}

void EnvironmentViewport::apply_(const io::JsonValue &json, PreviewClock &clock) {
	std::string error;
	if (const JsonValue *member = json.get("options")) {
		EnvironmentViewportOptions options = options_;
		bool rain = false, overcast = false;
		if (read_options(*member, uses_, options, rain, overcast, error)) {
			// A scrub runs the clock from its hour at tick 0 (unless the same change sets the clock).
			if (options.time != options_.time && options.time >= 0.0 && !json.get("clock")) clock.seek_ticks(0);
			if (options.mission != options_.mission || options.header != options_.header) layout_moved_ = true;
			if (options.terrain != options_.terrain || options.water != options_.water) options_moved_ = true;
			rain_pending_ = rain_pending_ || rain;
			overcast_pending_ = overcast_pending_ || overcast;
			options_ = options;
		}
	}
	if (const JsonValue *member = json.get("camera")) mission_camera_from_json(*member, camera_, error);
}

bool EnvironmentViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	bool moved = false;
	for (const std::string &name : report.missing)
		if (std::find(missing_.begin(), missing_.end(), name) == missing_.end()) {
			missing_.push_back(name);
			moved = true;
		}
	return moved;
}

std::vector<int> EnvironmentViewport::keyframe_times() const {
	std::vector<int> times;
	for (const env::Keyframe &keyframe : config_.keyframes) times.push_back(keyframe.time);
	std::stable_sort(times.begin(), times.end());
	return times;
}

EnvironmentViewport::Segment EnvironmentViewport::segment() const {
	Segment out;
	// The game's search in 16.16 hours (formats/env find_keyframe_segment), the fraction elapsed over the
	// segment's length (0 for none).
	std::vector<int> times = keyframe_times();
	for (int &time : times) time = env::hhmm_to_hours_fp(float(time));
	const env::KeyframeSegment at = env::find_keyframe_segment(times, int(weather_.tod_fixed24 >> 8));
	out.from = at.lo;
	out.to = at.hi;
	out.fraction = at.length_fp > 0 ? double(at.elapsed_fp) / double(at.length_fp) : 0.0;
	return out;
}

std::unique_ptr<CanvasHalf> EnvironmentViewport::make_canvas() const {
	OrbitCanvasHooks hooks;
	hooks.camera = [](const ViewportModel &viewport) -> const OrbitCamera & {
		return static_cast<const EnvironmentViewport &>(viewport).camera();
	};
	hooks.framed = [](const ViewportModel &viewport, int width, int height) {
		return static_cast<const EnvironmentViewport &>(viewport).framed(width, height);
	};
	hooks.change = environment_camera_change;
	return std::make_unique<OrbitCanvas>(hooks);
}

ViewportHit EnvironmentViewport::hit(const ViewportContext &context, float, float) const {
	ViewportHit out;
	out.current = current(context.input);
	return out;
}

bool EnvironmentViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "An environment's picture has no handles: its keywords are edited in its records.";
	return false;
}

bool EnvironmentViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in an environment's picture: its camera and its clock are set with set_viewport.";
	return false;
}

bool EnvironmentViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &out, std::string &error) const {
	if (name == "frame") {
		out.request(request::set_viewport(path(), environment_camera_change(framed(context.width, context.height))));
		return true;
	}
	if (name == "start") {
		JsonValue change = JsonValue::make_object();
		JsonValue options = JsonValue::make_object();
		options.set("time", JsonValue::make_null());
		change.set("kind", json_string(viewport_kind_token(ViewportKind::Environment)));
		change.set("options", std::move(options));
		JsonValue ticks = JsonValue::make_object();
		ticks.set("ticks", json_number(0));
		change.set("clock", std::move(ticks));
		out.request(request::set_viewport(path(), io::json_write(change)));
		return true;
	}
	if (name == "clear") {
		EnvironmentViewportOptions cleared = options_;
		cleared.rain = EnvironmentWeatherCommand{};
		cleared.overcast = EnvironmentWeatherCommand{};
		JsonValue options = JsonValue::make_object();
		options.set("rain", command_json(cleared.rain));
		options.set("overcast", command_json(cleared.overcast));
		out.request(request::set_viewport(path(), viewport_change(ViewportKind::Environment, "options", std::move(options))));
		return true;
	}
	error = "An environment's picture has no command \"" + name + "\" (frame, start, clear).";
	return false;
}

io::JsonValue EnvironmentViewport::options_json() const {
	return environment_options_to_json(options_);
}

io::JsonValue EnvironmentViewport::camera_json() const {
	return mission_camera_to_json(camera_);
}

io::JsonValue EnvironmentViewport::body_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_object();
	out.set("environment", json_string(file_name_));
	// The mission it is drawn over: its terrain, and what its header sets over the environment.
	if (const EnvironmentMissionUse *use = mission()) {
		JsonValue drawn = JsonValue::make_object();
		drawn.set("path", json_string(use->mission));
		drawn.set("title", json_string(use->title));
		drawn.set("terrain", json_string(use->terrain));
		drawn.set("terrain_file", json_string(use->terrain_file));
		drawn.set("tile_set", json_string(use->tile_set));
		JsonValue overrides = JsonValue::make_array();
		for (const EnvironmentOverride &each : environment_overrides(*use)) overrides.push(json_string(each.words));
		drawn.set("overrides", std::move(overrides));
		drawn.set("header_applied", JsonValue::make_bool(options_.header));
		out.set("mission", std::move(drawn));
	} else {
		out.set("mission", JsonValue::make_null());
	}
	out.set("missions", json_number(double(uses_.missions.size())));
	// The clock: the time now, where it started and from what, and its rate.
	JsonValue clock = JsonValue::make_object();
	clock.set("time", json_string(environment_clock_words(hours())));
	clock.set("hours", json_number(hours()));
	clock.set("fixed24", json_number(double(weather_.tod_fixed24)));
	clock.set("start", json_string(environment_clock_words(double(start_fixed24_) / kFixed24PerHour)));
	clock.set("start_from", json_string(environment_clock_from_token(start_from_)));
	clock.set("advance_per_tick", json_number(double(advance_)));
	clock.set("rate_from", json_string(environment_clock_from_token(rate_from_)));
	clock.set("stands", JsonValue::make_bool(advance_ == 0));
	// A day's length at that advance, in real seconds (0: it stands).
	clock.set("day_seconds", json_number(advance_ == 0 ? 0.0 : double(env::kTodDayFixed24) / double(advance_) / io::kTickHz));
	clock.set("night", JsonValue::make_bool(weather_.is_night_phase()));
	clock.set("ticks", json_number(double(weather_ticks_)));
	out.set("clock", std::move(clock));
	// The keyframe segment the clock is in, and the colours the game's time-of-day compute makes of it, the
	// overcast table cross-faded in by the overcast the tick reads (toward black with none: env #41)
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40].
	JsonValue tod = JsonValue::make_object();
	const Segment at = segment();
	const std::vector<int> times = keyframe_times();
	tod.set("keyframes", json_number(double(times.size())));
	if (at.from >= 0) {
		tod.set("from", json_string(environment_clock_words(double(env::hhmm_to_hours_fp(float(times[size_t(at.from)]))) / 65536.0)));
		tod.set("to", json_string(environment_clock_words(double(env::hhmm_to_hours_fp(float(times[size_t(at.to)]))) / 65536.0)));
		tod.set("fraction", json_number(at.fraction));
		const float time = float(weather_.tod_hhmm());
		const env::TodState state =
				env::tod_colors(config_, has_overcast_ ? &overcast_ : nullptr, time, weather_.overcast_for_tod_q16);
		tod.set("colours", keyframe_colours(state, weather_.is_night_phase()));
	}
	tod.set("overcast_table", JsonValue::make_bool(has_overcast_ && !overcast_.keyframes.empty()));
	out.set("tod", std::move(tod));
	// The weather as a script left it: each channel's target and where its spring stands, and the drops
	// the game draws at that rain [orig: the drop gate @ 0x5dee48; the active count @ 0x5dec92..0x5deca6].
	const env::EnvScalarChannels &channels = weather_.core.scalar_channels;
	JsonValue weather = JsonValue::make_object();
	JsonValue rain = JsonValue::make_object();
	rain.set("percent", json_number(percent_of(channels.rain_pct_fp)));
	rain.set("target", json_number(percent_of(channels.rain_pct_target_fp)));
	rain.set("falling", JsonValue::make_bool(weather_.raining()));
	rain.set("drops", json_number(weather_.raining() ? env::PrecipitationField::active_count(channels.rain_pct_fp) : 0));
	weather.set("rain", std::move(rain));
	JsonValue overcast = JsonValue::make_object();
	overcast.set("percent", json_number(percent_of(channels.overcast_fp)));
	overcast.set("target", json_number(percent_of(channels.overcast_target_fp)));
	weather.set("overcast", std::move(overcast));
	weather.set("fog", json_number(double(channels.fog_dist_fp) / 65536.0));
	out.set("weather", std::move(weather));
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : missing_) missing.push(json_string(name));
	out.set("missing", std::move(missing));
	return out;
}

io::JsonValue EnvironmentViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	const std::vector<int> times = keyframe_times();
	const Segment at = segment();
	for (size_t i = 0; i < times.size(); ++i) {
		JsonValue item = JsonValue::make_object();
		item.set("index", json_number(double(i)));
		item.set("name", json_string(environment_clock_words(double(env::hhmm_to_hours_fp(float(times[i]))) / 65536.0)));
		item.set("kind", json_string("keyframe"));
		item.set("hours", json_number(double(env::hhmm_to_hours_fp(float(times[i]))) / 65536.0));
		item.set("current", JsonValue::make_bool(int(i) == at.from));
		out.push(std::move(item));
	}
	return out;
}

io::JsonValue EnvironmentViewport::notes_json(const ViewportInput &) const {
	JsonValue notes = JsonValue::make_array();
	if (reason_ != EnvironmentViewStatus::Ready) return notes;
	const auto note = [&](const char *code, const std::string &message) {
		JsonValue row = JsonValue::make_object();
		row.set("code", json_string(code));
		row.set("message", json_string(message));
		notes.push(std::move(row));
	};
	const EnvironmentMissionUse *use = mission();
	if (!use)
		note("environment.no_mission", uses_.reading ? "Reading the project's references..."
		                                              : "No mission runs on it: its sky over no ground (a mission's header "
		                                                "names its environment).");
	else if (use->terrain_file.empty() && !use->terrain.empty())
		note("environment.no_terrain", base_of(use->mission) + " names the terrain " + use->terrain +
		                                       ", which the project does not have: the sky over no ground.");
	if (use && !options_.header && !environment_overrides(*use).empty())
		note("environment.header_overrides", base_of(use->mission) +
		                                             "'s header sets its own fog or water over this environment: "
		                                             "Mission header shows them.");
	if (advance_ == 0)
		note("environment.clock_stands", "The clock stands: " +
		                                         (use ? base_of(use->mission) + "'s day length is 0"
		                                              : std::string("its day length is 0")) +
		                                         ", as the game runs it. A day length runs it.");
	for (const std::string &name : missing_) {
		JsonValue row = JsonValue::make_object();
		row.set("code", json_string("file.missing"));
		row.set("name", json_string(name));
		row.set("message", json_string("The project has no " + name + ": import it to see it."));
		notes.push(std::move(row));
	}
	return notes;
}

} // namespace opennova::editor
