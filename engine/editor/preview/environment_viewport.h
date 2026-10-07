#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/environment_uses.h>
#include <editor/session/view/view_revisions.h>
#include <formats/env/env.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

// What an environment viewport shows, and why not (the deep-integration plan's DI-19b): the kind's reason.
enum class EnvironmentViewStatus : uint8_t {
	NoProject, // no project is open
	NoEnvironment, // no environment is open at its path
	Unwritable, // the document cannot be written, so the game would read no such file (detail: why)
	Ready,
};
// "no_project", "no_environment", "unwritable", "ready": its token on the wire.
const char *environment_view_status_token(EnvironmentViewStatus status);

// A weather command as a mission's script gives it: a percent and the seconds it takes to get there
// [orig: WacCmd_Rain @ 0x4edf60; WacCmd_Overcast @ 0x4ee040].
struct EnvironmentWeatherCommand {
	int percent = 0;
	int seconds = 0;
	bool operator==(const EnvironmentWeatherCommand &o) const { return percent == o.percent && seconds == o.seconds; }
	bool operator!=(const EnvironmentWeatherCommand &o) const { return !(*this == o); }
};

// How an environment viewport plays and draws (its options): the time of day the clock starts from
// (hours 0..24; below 0 the clock the game starts it on: the mission's header, else the file's curtime),
// the day's length the clock runs at (0 the game's: the mission's header, else the file's tod_rate; else a
// day in that many seconds, the editor's aid), the using mission whose terrain, tiles and clock it is drawn
// with ("" the first), whether that mission's header sets its overrides over the environment (fog and
// water, as the game loads the two), the last rain and overcast a script command set, and the layers drawn
// (the terrain, the water).
struct EnvironmentViewportOptions {
	double time = -1.0;
	int day_seconds = 0;
	std::string mission;
	bool header = false;
	EnvironmentWeatherCommand rain;
	EnvironmentWeatherCommand overcast;
	bool terrain = true;
	bool water = true;
	bool operator==(const EnvironmentViewportOptions &o) const {
		return time == o.time && day_seconds == o.day_seconds && mission == o.mission && header == o.header &&
				rain == o.rain && overcast == o.overcast && terrain == o.terrain && water == o.water;
	}
	bool operator!=(const EnvironmentViewportOptions &o) const { return !(*this == o); }
};

// The toolbar's day lengths (seconds; 0 the game's).
inline constexpr int kEnvironmentDaySeconds[] = { 0, 24 * 60, 4 * 60, 60, 20 };
// The longest a command's seconds go (an hour), and how far behind its clock the weather catches up in
// one follow (ten seconds of game ticks; a longer stall drops the rest, as a seek does).
inline constexpr int kEnvironmentCommandSecondsMost = 3600;
inline constexpr int32_t kEnvironmentCatchUpTicks = 620;
// The eye's height over the ground (metres), a person's standing there.
inline constexpr float kEnvironmentEyeHeight = 1.8f;

// The options on the wire (the envelope's `options`, a SetViewport's): {time (null: the start), day_seconds,
// mission, header, rain {percent, seconds}, overcast {percent, seconds}, show {terrain, water}}.
io::JsonValue environment_options_to_json(const EnvironmentViewportOptions &options);
// The change a SetViewport makes to set the options to `options` (a weather member only where it differs
// from `held`'s: a command is issued each time a SetViewport names it), and its camera.
std::string environment_options_change(const EnvironmentViewportOptions &options, const EnvironmentViewportOptions &held);
std::string environment_camera_change(const OrbitCamera &camera);
// A time of day as "HH:MM" (hours 0..24).
std::string environment_clock_words(double hours);

// Where the clock's start and its rate come from: the option, the using mission's header, the file
// (curtime, tod_rate), or the engine's default (Environment_InitDefaults' advance of 75).
enum class EnvironmentClockFrom : uint8_t { Option, Mission, Environment, Default };
const char *environment_clock_from_token(EnvironmentClockFrom from);

// An environment's viewport (DI-19b; ViewportKind::Environment, the Document tab's main view; CONTEXT.md
// "Environment viewport"): the environment at its path as the game would read it were it saved now (the
// open document's bytes through the engine's reader, env::load_mission_env), drawn by the Shell's device
// as the game draws a mission's sky over the terrain of a mission that runs on it (session/environment_uses:
// the first, or the option's), with no terrain where none does. Its clock is the game's mission clock
// [orig: g_EnvCurTimeFixed24, advanced each tick by g_EnvTodAdvancePerTick, Environment_SetTodAdvanceRate
// @ 0x57d170] on the preview clock's game ticks: from the start (the option's hour, the mission header's
// start time, else the file's curtime) at the game's rate (the mission's day length, a 0 one standing it,
// else the file's tod_rate, else the engine's default) or a day in the option's seconds. Its weather is the
// game's weather home (world::WeatherState) seeded as a mission starts it [orig: Environment_SnapStateToTargets
// @ 0x57d1e0 via env::weather_seed_from_config; Environment_MissionStartInit @ 0x57f1e0] and stepped by the
// game's own tick a game tick at a time (tick_sim: the clock, the rain and overcast springs), with the rain and
// overcast a script sets [orig: WacCmd_Rain @ 0x4edf60; WacCmd_Overcast @ 0x4ee040]; its device mirrors that
// home's clock and channels into the runtime's own (MissionEnvironment, Weather), whose colour legs, sky dome,
// sun and moon, water and drops draw the picture. Its camera stands on the ground at the terrain's middle,
// looking out; it orbits, pans and dollies (preview/orbit_canvas); a point of the picture names nothing.
class EnvironmentViewport final : public ViewportModel {
public:
	explicit EnvironmentViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	EnvironmentViewStatus view_status() const { return reason_; }
	const EnvironmentViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	// The environment as the game reads it (its file's bytes, the open document standing in, over the drawn
	// mission's terrain's .trn: env::load_mission_env), the logical name it is read by, and whether the load
	// has an overcast table (overcast.def, the .trn's keyframes; none: has_overcast false).
	const env::Config &config() const { return config_; }
	const std::string &file_name() const { return file_name_; }
	bool has_overcast() const { return has_overcast_; }
	// The missions that run on it, and the one drawn (null: none).
	const EnvironmentUses &uses() const { return uses_; }
	const EnvironmentMissionUse *mission() const;
	// The header the picture is drawn with: the mission's terrain, tile set and overrides (the overrides
	// only with the header option), and the mission's own name (the game reads <mission>.til beside it).
	const MissionSceneHeader &header() const { return header_; }
	const std::string &mission_name() const { return mission_name_; }
	// The clock: its start (8.24 hours), its advance a tick (0: it stands), where each comes from, and the
	// time it shows now (8.24 hours; hours as a number).
	uint32_t start_fixed24() const { return start_fixed24_; }
	uint32_t advance_per_tick() const { return advance_; }
	EnvironmentClockFrom start_from() const { return start_from_; }
	EnvironmentClockFrom rate_from() const { return rate_from_; }
	uint32_t time_fixed24() const { return weather_.tod_fixed24; }
	double hours() const;
	// The weather home (the sim legs' state: the clock, the rain and overcast springs, the precipitation
	// kind), how many game ticks it has run in all, and how many times its clock was set anew (a seek: the
	// device takes the colours at once).
	const world::WeatherState &weather() const { return weather_; }
	uint64_t weather_ticks() const { return weather_ticks_; }
	uint64_t clock_sets() const { return clock_sets_; }
	// The names its device asked the project's files for and did not find.
	const std::vector<std::string> &missing() const { return missing_; }
	// The camera standing on the ground at the terrain's middle, looking out at the horizon (the start's
	// heading kept), on a picture `width` x `height`.
	OrbitCamera framed(int width, int height) const;

	ViewportStatus status() const override;
	const char *reason() const override { return environment_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const FileStamps *picture_reads() const override { return &picture_.files(); }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// "frame" (the camera back on the ground at the terrain's middle), "start" (the clock back to the
	// game's start: the time option cleared, the clock sought to tick 0), "clear" (the rain and the
	// overcast to 0 at once, as a script's rain(0, 0) and overcast(0, 0)).
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The keyframes, each an item: its index, its time as its name, kind "keyframe", and whether the clock
	// is in the segment that starts at it.
	io::JsonValue items_json(const ViewportInput &input) const override;
	io::JsonValue notes_json(const ViewportInput &input) const override;

	// The keyframe segment the clock is in now [orig: Environment_FindKeyframeSegment @ 0x57dd80: the
	// last keyframe at or before the time, wrapping below the first to the last, toward the next]: indexes
	// into the file's keyframes in time order (-1: none), and how far along it is (0..1).
	struct Segment {
		int from = -1;
		int to = -1;
		double fraction = 0.0;
	};
	Segment segment() const;
	// The keyframes' times, in the file's order sorted by time (HHMM).
	std::vector<int> keyframe_times() const;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(EnvironmentViewStatus reason, const std::string &detail);
	// What the game reads of the environment and the overcast table, again where a file moved.
	bool read_(const SessionView &view, const DocumentBase &document, std::string &why);
	// The mission it is drawn over, its header and its clock, from the uses and the options; true when the
	// picture's layout (the terrain, the tiles, the overrides) moved.
	bool place_(const SessionView &view);
	// The clock's start and rate from the options, the mission and the file.
	void clock_();
	// The weather home seeded as a mission's start seeds it, at the clock's ticks.
	void seed_(const PreviewClock &clock);
	// The clock set to its base plus `ticks` ticks at the advance (a seek: the springs keep their state).
	void set_clock_(int32_t ticks);
	// The game's ticks the preview clock ran since the last follow stepped on the weather home.
	void step_(const PreviewClock &clock);
	// The ground under the camera's framing (the mission's terrain through the game's own load).
	void follow_ground_(const SessionView &view);

	EnvironmentViewStatus reason_ = EnvironmentViewStatus::NoProject;
	std::string detail_;
	EnvironmentViewportOptions options_;
	OrbitCamera camera_;
	bool framed_ = false; // the camera framed on the ground once
	bool options_moved_ = false; // an option the device draws by moved: an Update
	bool layout_moved_ = false; // an option the picture is made from moved: a Rebuild
	// The rain and overcast commands an apply named, issued on the home at the next follow.
	bool rain_pending_ = false, overcast_pending_ = false;
	PreviewFollow picture_;
	uint64_t layout_serial_ = 1; // moves with the terrain, the tiles, the overrides
	// The file as last read, over the drawn mission's terrain's .trn and overcast.def.
	env::Config config_;
	env::Config overcast_;
	bool has_overcast_ = false;
	std::string file_name_;
	uint64_t read_identity_ = 0, read_load_ = 0, read_revision_ = 0;
	uint64_t read_generation_ = UINT64_MAX;
	std::string read_terrain_; // the terrain whose .trn read ahead of it
	FileStamps read_stamps_; // the .trn, overcast.def and the environment as read
	std::string unwritable_; // why the document cannot be written ("" it can)
	// The uses, again when the project, its files, its graph or its documents move.
	EnvironmentUses uses_;
	RevisionKey uses_key_;
	bool uses_known_ = false;
	MissionSceneHeader header_;
	std::string mission_name_;
	std::string mission_path_; // the use drawn ("" none)
	// The clock: its start, the time it runs from at the preview clock's tick 0 (the start, or where it
	// stood when the rate moved), and whether it was set once.
	uint32_t start_fixed24_ = 0;
	uint32_t base_fixed24_ = 0;
	bool clocked_ = false;
	uint32_t advance_ = 0;
	EnvironmentClockFrom start_from_ = EnvironmentClockFrom::Default;
	EnvironmentClockFrom rate_from_ = EnvironmentClockFrom::Default;
	// The weather home and its stepping.
	world::WeatherState weather_;
	bool seeded_ = false;
	uint64_t seed_key_ = 0; // what the home was seeded from (the config read, the header, the clock)
	int32_t stepped_ = 0; // the preview clock's ticks the home stands at
	uint64_t seeks_ = 0; // the preview clock's seeks it followed
	uint64_t weather_ticks_ = 0;
	uint64_t clock_sets_ = 0;
	// The ground the framing stands the camera on.
	MissionGround ground_;
	std::vector<std::string> missing_;
};

} // namespace opennova::editor
