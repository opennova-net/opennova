// The environment viewport (the deep-integration plan's DI-19b; editor/preview/environment_viewport.h): an
// environment's sky as the game draws it, the Document tab's main view beside its records, over the terrain
// of a mission that runs on it. Its kind's row; through a session: opened, its device asked to make the
// picture; its clock the game's mission clock on the preview clock's ticks (the mission header's start and
// day length, a day length of 0 a clock that stands, the file's curtime and tod_rate with no mission, a day
// in the option's seconds), a scrub running it from an hour, the keyframe segment it is in and the colours
// the game's time-of-day compute makes there; the rain and the overcast a script sets, stepped by the game's
// own weather tick (half way over half the seconds, the drops the game draws at that rain), the overcast
// cross-fading toward black with no overcast table; the mission header's overrides; an edit of the
// environment rebuilding its picture; the wire's options, its refusals and its commands.
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/document_types.h>
#include <editor/documents/environment_document.h>
#include <editor/preview/environment_listen.h>
#include <editor/preview/environment_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/env/tod_clock.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <runtime/environment/precipitation.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

constexpr double kFixed24PerHour = 16777216.0;

int test_kind() {
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Environment)) == "environment");
	ViewportKind named = ViewportKind::kCount;
	TEST_EXPECT(viewport_kind_from_token("environment", named) && named == ViewportKind::Environment);
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Environment);
	// The Document tab's main view, read as Save would write it, two devices at most (each holds a terrain).
	TEST_EXPECT(row.role == ViewportRole::Main && row.as_saved && !row.part && row.canvas && row.devices == 2);
	TEST_EXPECT(main_viewport_kind(DocumentTypeId::Environment) == ViewportKind::Environment);
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::Environment) == ViewportKind::Environment);
	TEST_EXPECT(preview_kind_of(DocumentTypeId::Environment) == ViewportKind::kCount);
	std::printf("kind: the environment's, the Main role, as saved, two devices\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{ "opennova_editor_environment_viewport" };
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	editor_test::FakeDevices devices;
	const std::string path = "day.env";
	std::string root() const { return session.view().project.root; }
	const EnvironmentViewport *viewport() {
		return static_cast<const EnvironmentViewport *>(session.viewports().find(path, ViewportKind::Environment));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	void advance(double seconds) {
		session.advance(seconds);
		pump();
	}
	void set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		pump();
	}
	JsonValue state() {
		JsonValue args = JsonValue::make_object();
		args.set("path", opennova::io::json_string(path));
		args.set("op", opennova::io::json_string("state"));
		std::string error;
		return session.query("viewport", args, error);
	}
	JsonValue body() {
		const JsonValue answer = state();
		const JsonValue *body = answer.get("body");
		return body ? *body : JsonValue();
	}
};

const JsonValue *at(const JsonValue &value, const char *a, const char *b = nullptr) {
	const JsonValue *one = value.get(a);
	return b && one ? one->get(b) : one;
}

opennova::env::Keyframe keyframe(int time, uint32_t sun, uint32_t fog) {
	opennova::env::Keyframe out;
	out.time = time;
	out.sun = opennova::env::packed_to_rgb01(sun);
	out.moon = opennova::env::packed_to_rgb01(0x202040);
	out.fog = opennova::env::packed_to_rgb01(fog);
	out.skyfog = out.fog;
	out.sky = opennova::env::packed_to_rgb01(0x4060a0);
	out.ground = opennova::env::packed_to_rgb01(0x303030);
	return out;
}

int write_mission(const std::string &root, const char *name, int attrib, int fog, int start, int minutes) {
	opennova::bms::File file;
	opennova::mission::make_default(file);
	std::string why;
	TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "island", why) &&
	            opennova::mission::set_header_string(file, "environment", "day", why) &&
	            opennova::mission::set_header_int(file, "attrib_flags", attrib, why) &&
	            opennova::mission::set_header_int(file, "fog_override", fog, why) &&
	            opennova::mission::set_header_int(file, "start_time", start, why) &&
	            opennova::mission::set_header_int(file, "minutes_per_day", minutes, why));
	std::vector<uint8_t> bytes;
	TEST_EXPECT(opennova::bms::write(file, bytes, why));
	TEST_EXPECT(editor_test::write_bytes(root + "/" + name, bytes));
	return 0;
}

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.dir.file("project"), "Environment"));
	editor_test::create_missing_files(rig.session);
	const std::string root = rig.root();
	// A day of three keyframes, its own clock at 15:00 and a day of 120 minutes.
	opennova::env::Config config;
	config.curtime = 1500;
	config.tod_rate = 120;
	config.tod_rate_set = true;
	config.fog_level = 900.0f;
	config.sky_height = 175.0f;
	config.keyframes = { keyframe(600, 0xc08040, 0x8090a0), keyframe(1200, 0xfff0e0, 0xa0b0c0),
		                 keyframe(1800, 0xc06020, 0x605060) };
	std::ostringstream text;
	std::string error;
	TEST_EXPECT(opennova::env::save_env(text, config, error));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", text.str()));
	TEST_EXPECT(editor_test::write_text(root + "/island.trn",
	                                    "polytrn_colormap map.tga\r\npolytrn_detailmap detail.tga\r\npolytrn_polydata island.cpt\r\n"
	                                    "polytrn_sectorcount 1\r\npolytrn_sectors 0\r\nwater_height 25\r\n"));
	// Two missions on it: one fogging closer and starting at 06:00 on a 30-minute day (the 60-minute floor),
	// one starting at 18:00 on a day of 0 (its clock stands).
	TEST_EXPECT(write_mission(root, "dawn.bms", 0x2, 600, 6 << 8, 30) == 0);
	TEST_EXPECT(write_mission(root, "dusk.bms", 0, 0, 18 << 8, 0) == 0);
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();

	// Opened: its records beside its sky, the device asked to make the picture.
	editor_test::handle_to_end(rig.session, request::open_document(rig.path));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const EnvironmentViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && viewport->uses().missions.size() == 2);
	if (!viewport) return 1;
	editor_test::FakeDevice *device = rig.devices.held(rig.path, ViewportKind::Environment);
	TEST_EXPECT(device && device->since(0) == std::vector<ViewportAction>{ ViewportAction::Rebuild });
	rig.pump();
	TEST_EXPECT(device && device->last() == ViewportAction::Keep);

	// Over dawn.bms: its terrain, its clock (06:00, the 60-minute floor of its 30-minute day).
	rig.set(R"({"kind": "environment", "options": {"mission": "dawn.bms"}, "clock": {"playing": true}})");
	const uint32_t dawn_advance = uint32_t(opennova::env::tod_advance_per_tick(60));
	TEST_EXPECT(viewport->mission() && viewport->mission()->mission == "dawn.bms");
	TEST_EXPECT(viewport->header().terrain == "island" && viewport->mission_name() == "dawn");
	TEST_EXPECT(viewport->start_from() == EnvironmentClockFrom::Mission && viewport->start_fixed24() == 6u << 24);
	TEST_EXPECT(viewport->rate_from() == EnvironmentClockFrom::Mission && viewport->advance_per_tick() == dawn_advance);
	TEST_EXPECT(viewport->time_fixed24() == 6u << 24);
	// A second of the preview clock: 62 game ticks of the game's clock and weather tick.
	const uint64_t ticks = viewport->weather_ticks();
	rig.advance(1.0);
	const int32_t run = int32_t(viewport->weather_ticks() - ticks);
	TEST_EXPECT(run == 62 || run == 63);
	TEST_EXPECT(viewport->time_fixed24() == (6u << 24) + uint32_t(run) * dawn_advance);
	JsonValue body = rig.body();
	TEST_EXPECT(at(body, "clock", "start") && at(body, "clock", "start")->string == "06:00" &&
	            at(body, "clock", "rate_from")->string == "mission");
	TEST_EXPECT(at(body, "mission", "path") && at(body, "mission", "path")->string == "dawn.bms");

	// Over dusk.bms: 18:00, its day length of 0 a clock that stands, as the game runs it.
	rig.set(R"({"kind": "environment", "options": {"mission": "dusk.bms"}})");
	TEST_EXPECT(viewport->start_fixed24() == 18u << 24 && viewport->advance_per_tick() == 0);
	rig.advance(1.0);
	// 18:00 is still day: the sun gives way to the moon at 18:45.
	TEST_EXPECT(viewport->time_fixed24() == 18u << 24 && !viewport->weather().is_night_phase());
	body = rig.body();
	TEST_EXPECT(at(body, "clock", "stands") && at(body, "clock", "stands")->boolean);
	bool stands_note = false;
	const JsonValue state = rig.state();
	if (const JsonValue *notes = state.get("notes"))
		for (const JsonValue &note : notes->array) stands_note |= note.get_string("code", "") == "environment.clock_stands";
	TEST_EXPECT(stands_note);
	// A day in 60 seconds (the editor's aid) runs it on from where it stands.
	rig.set(R"({"kind": "environment", "options": {"day_seconds": 60}})");
	const uint32_t fast = uint32_t(opennova::env::kTodDayFixed24 / (60 * 62));
	TEST_EXPECT(viewport->advance_per_tick() == fast && viewport->rate_from() == EnvironmentClockFrom::Option);
	rig.advance(0.5);
	TEST_EXPECT(viewport->time_fixed24() > 18u << 24 && viewport->time_fixed24() <= (18u << 24) + 32u * fast);
	rig.set(R"({"kind": "environment", "options": {"day_seconds": 0}})");

	// A scrub: the clock runs from 12:30, in the segment from the 12:00 keyframe toward the 18:00 one.
	rig.set(R"({"kind": "environment", "options": {"time": 12.5}, "clock": {"playing": false}})");
	TEST_EXPECT(viewport->start_from() == EnvironmentClockFrom::Option);
	TEST_EXPECT(std::abs(viewport->hours() - 12.5) < 1e-6 && !viewport->weather().is_night_phase());
	const EnvironmentViewport::Segment segment = viewport->segment();
	TEST_EXPECT(segment.from == 1 && segment.to == 2 && std::abs(segment.fraction - 1.0 / 12.0) < 1e-3);
	body = rig.body();
	TEST_EXPECT(at(body, "tod", "from") && at(body, "tod", "from")->string == "12:00" &&
	            at(body, "tod", "to")->string == "18:00" && at(body, "clock", "time")->string == "12:30");
	// The colours the game's compute makes at 12:30: the 12:00 keyframe's sun a twelfth toward 18:00's (red 255 to
	// 192: (5461 * -63 + (255 << 16) + 0x8000) >> 16 = 250).
	const JsonValue *colours = at(body, "tod", "colours");
	TEST_EXPECT(colours && colours->get_string("light", "") == colours->get_string("sun", "") &&
	            colours->get_string("sun", "") != "#fff0e0" && colours->get_string("sun", "").rfind("#fa", 0) == 0);
	// The keyframes are the items, the one the clock is in marked.
	const JsonValue now = rig.state();
	const JsonValue *listed = now.get("items");
	TEST_EXPECT(listed && listed->array.size() == 3 && listed->array[1].get_string("name", "") == "12:00" &&
	            listed->array[1].get_bool("current", false));

	// The rain a script sets: rain(100, 2), stepped by the game's spring (a 32nd of the way a tick, no more
	// than the command's step: 529 a tick over 124 ticks), past half way after a second of game ticks; the
	// drops the game draws at that rain.
	rig.set(R"({"kind": "environment", "clock": {"playing": true}, "options": {"rain": {"percent": 100, "seconds": 2}}})");
	const opennova::env::EnvScalarChannels &channels = viewport->weather().core.scalar_channels;
	TEST_EXPECT(channels.rain_pct_target_fp == 0x10000 && channels.rain_pct_fp < 0x1000 && channels.rain_step_fp == 529);
	rig.advance(1.0);
	TEST_EXPECT(channels.rain_pct_fp > 0x7000 && channels.rain_pct_fp < 0x9000 && viewport->weather().raining());
	body = rig.body();
	TEST_EXPECT(at(body, "weather", "rain") && at(body, "weather", "rain")->get_bool("falling", false) &&
	            at(body, "weather", "rain")->get_number("drops", 0) ==
	                    double(opennova::env::PrecipitationField::active_count(channels.rain_pct_fp)));
	// There once the spring's tail runs out, held under the start's clamp (0xFFFF, Environment_MissionStartInit's):
	// every drop drawn.
	for (int second = 0; second < 6; ++second) rig.advance(1.0);
	TEST_EXPECT(channels.rain_pct_fp == 0xFFFF &&
	            rig.body().get("weather")->get("rain")->get_number("drops", 0) == double(opennova::env::PrecipitationField::kSlots));
	// The overcast: overcast(100, 0); with no overcast table every keyframed colour fades toward black (env #41),
	// the blend's snap past 63356 taking it whole.
	rig.set(R"({"kind": "environment", "options": {"overcast": {"percent": 100, "seconds": 0}}})");
	rig.advance(0.1);
	TEST_EXPECT(channels.overcast_fp > 0 && channels.overcast_fp < 0xFFFF);
	for (int second = 0; second < 6; ++second) rig.advance(1.0);
	TEST_EXPECT(channels.overcast_fp == 0xFFFF);
	body = rig.body();
	TEST_EXPECT(at(body, "tod", "colours") && at(body, "tod", "colours")->get_string("sun", "") == "#000000" &&
	            !at(body, "tod", "overcast_table")->boolean);
	// Clear: rain(0, 0) and overcast(0, 0), the springs back down.
	editor_test::Gathered gathered;
	std::string why;
	const ViewportContext context = viewport_context(rig.session.view(), *viewport);
	TEST_EXPECT(viewport->command(context, "clear", {}, gathered, why));
	for (const EditorRequest &request : gathered.requests) rig.session.handle(request);
	for (int second = 0; second < 6; ++second) rig.advance(1.0);
	TEST_EXPECT(channels.rain_pct_target_fp == 0 && channels.overcast_target_fp == 0 && channels.overcast_fp < 64 &&
	            !viewport->weather().raining());

	// The mission header's overrides: dawn.bms fogs at 600 m.
	rig.set(R"({"kind": "environment", "options": {"mission": "dawn.bms"}})");
	const size_t before = device->taken.size();
	rig.set(R"({"kind": "environment", "options": {"header": true}})");
	TEST_EXPECT(viewport->header().attrib_flags == 0x2 && viewport->header().fog_override == 600);
	TEST_EXPECT(channels.fog_dist_target_fp == 600 << 16);
	TEST_EXPECT(device->taken.size() > before && device->last() == ViewportAction::Rebuild);
	rig.set(R"({"kind": "environment", "options": {"header": false}})");
	TEST_EXPECT(channels.fog_dist_target_fp == 900 << 16 && viewport->header().attrib_flags == 0);

	// An edit of the environment: the picture made again over the document as Save would write it.
	Document *open = rig.session.document_for(rig.path);
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	Edit fog;
	fog.address = NodeAddress{ open->rows().front()->id, node_kind(EnvironmentKind::Environment), 0 };
	fog.field = "fog_level";
	fog.value = int64_t(700);
	const size_t taken = device->taken.size();
	editor_test::handle_to_end(rig.session, request::edit_record(rig.path, fog));
	rig.pump();
	TEST_EXPECT(device->taken.size() > taken && device->last() == ViewportAction::Rebuild);
	TEST_EXPECT(viewport->config().fog_level == 700.0f && channels.fog_dist_target_fp == 700 << 16);

	// The wire: the options as set, refusals naming what each takes, the commands.
	const JsonValue wire = rig.state();
	const JsonValue *options = wire.get("options");
	TEST_EXPECT(options && options->get_string("mission", "") == "dawn.bms" && options->get("time")->number == 12.5 &&
	            !options->get_bool("header", true));
	for (const char *refused : { R"({"kind": "environment", "options": {"time": 24}})",
	                             R"({"kind": "environment", "options": {"mission": "night.bms"}})",
	                             R"({"kind": "environment", "options": {"rain": {"percent": 101}}})",
	                             R"({"kind": "environment", "options": {"sun": true}})" }) {
		rig.session.handle(request::set_viewport(rig.path, refused));
		TEST_EXPECT(rig.session.outcome().refused && !rig.session.outcome().findings.empty());
	}
	gathered.requests.clear();
	TEST_EXPECT(viewport->command(context, "start", {}, gathered, why) && gathered.requests.size() == 1);
	for (const EditorRequest &request : gathered.requests) rig.session.handle(request);
	rig.pump();
	TEST_EXPECT(viewport->start_from() == EnvironmentClockFrom::Mission && viewport->time_fixed24() == 6u << 24);
	TEST_EXPECT(!viewport->command(context, "dance", {}, gathered, why) && why.find("frame, start, clear") != std::string::npos);
	std::printf("session: the game's clock over a mission's start and day (a day of 0 standing), a scrub and its "
	            "keyframe segment, rain and overcast as a script sets them, the header's overrides, an edit, the wire\n");
	return 0;
}

// With no mission on it: the file's own clock (curtime, tod_rate) and the sky over no ground.
int test_no_mission() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.dir.file("project"), "Environment"));
	editor_test::create_missing_files(rig.session);
	opennova::env::Config config;
	config.curtime = 1530;
	config.tod_rate = 30; // the parse's 60-minute floor
	config.tod_rate_set = true;
	config.keyframes = { keyframe(1200, 0xffffff, 0xc0c0c0) };
	std::ostringstream text;
	std::string error;
	TEST_EXPECT(opennova::env::save_env(text, config, error));
	TEST_EXPECT(editor_test::write_text(rig.root() + "/day.env", text.str()));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.path));
	rig.pump();
	const EnvironmentViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && !viewport->mission());
	if (!viewport) return 1;
	TEST_EXPECT(viewport->start_from() == EnvironmentClockFrom::Environment &&
	            viewport->start_fixed24() == (15u << 24) + (1u << 23));
	TEST_EXPECT(viewport->rate_from() == EnvironmentClockFrom::Environment &&
	            viewport->advance_per_tick() == uint32_t(opennova::env::tod_advance_per_tick(60)));
	TEST_EXPECT(viewport->header().terrain.empty());
	bool no_mission = false;
	const JsonValue state = rig.state();
	if (const JsonValue *notes = state.get("notes"))
		for (const JsonValue &note : notes->array) no_mission |= note.get_string("code", "") == "environment.no_mission";
	TEST_EXPECT(no_mission);
	std::printf("no mission: the file's curtime and tod_rate (its floor), the sky over no ground\n");
	return 0;
}

// S23 C: Listen hears the environment's weather at the camera. With it on and the rain falling, the rain's two loops
// (LPNV_RAIN_L and LPNV_RAIN_R beside the listener) take two channels of the project's bank; the lightning's thunder,
// a tick's sequencer A, plays the THUNDER set at a metre; with it off, nothing listens.
int test_listen() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.dir.file("project"), "Listen"));
	editor_test::create_missing_files(rig.session);
	const std::string root = rig.root();
	opennova::env::Config config;
	config.curtime = 1200;
	config.keyframes = { keyframe(600, 0xc08040, 0x8090a0), keyframe(1800, 0xc06020, 0x605060) };
	std::ostringstream text;
	std::string error;
	TEST_EXPECT(opennova::env::save_env(text, config, error));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", text.str()));
	// The bank: the rain's two loops heard to 30 m, the thunder to 2 km; a wave each.
	opennova::lwf::File bank;
	const char *sets[] = { "LPNV_RAIN_L", "LPNV_RAIN_R", "THUNDER" };
	for (size_t i = 0; i < std::size(sets); ++i) {
		opennova::lwf::Single single;
		single.name = sets[i];
		single.path = opennova::strutil::to_lower(sets[i]) + ".wav";
		bank.singles.push_back(single);
		opennova::lwf::Multi set;
		set.name = sets[i];
		set.pitch_base = opennova::lwf::kAuthoredSetPitchBase;
		set.target_id = 5000;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		opennova::lwf::Playlist layer;
		layer.falloff_radius = i < 2 ? 30 : 2000;
		layer.flags = opennova::lwf::kFlagInternal | opennova::lwf::kFlagExternal;
		layer.sndparm_indices.push_back(uint32_t(i));
		bank.playlists.push_back(layer);
		opennova::lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = opennova::lwf::kPitchUnityQ16;
		member.volume = 255;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> bytes;
	TEST_EXPECT(opennova::lwf::encode_lwf(bank, bytes, error));
	TEST_EXPECT(editor_test::write_bytes(root + "/sounds/game.lwf", bytes));
	const std::vector<uint8_t> tone = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/lwf/tone.wav");
	for (const char *set : sets)
		TEST_EXPECT(editor_test::write_bytes(root + "/sounds/" + opennova::strutil::to_lower(set) + ".wav", tone));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.path));
	rig.pump();
	const EnvironmentViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready);
	if (!viewport) return 1;
	JsonValue body = rig.body();
	TEST_EXPECT(at(body, "listen") && at(body, "listen")->is_null() && !viewport->listen().open());
	// Listen on, the rain at once: a second on, the two loops beside the listener on two channels.
	rig.set(R"({"kind": "environment", "options": {"listen": {"on": true, "volume": 0.5}, "rain": {"percent": 100, "seconds": 0}}, "clock": {"playing": true}})");
	rig.advance(1.0);
	TEST_EXPECT(viewport->listen().open() && viewport->weather().raining());
	size_t loops = 0;
	for (const MissionSoundChannel &channel : viewport->listen().channels())
		if (channel.candidate >= 0) {
			++loops;
			TEST_EXPECT(std::string(channel.source) == "rain" && channel.volume > 0 && !channel.path.empty());
		}
	TEST_EXPECT(loops == 2);
	body = rig.body();
	TEST_EXPECT(at(body, "listen", "rain") && at(body, "listen", "rain")->get("loops")->number == 2.0 &&
	            at(body, "listen", "rain")->get("sets_found")->number == 2.0);
	TEST_EXPECT(at(body, "listen", "volume") && at(body, "listen", "volume")->number == 0.5);
	// The thunder of a tick's lightning (sequencer A, a metre off), planned from the project's bank.
	EnvironmentListen listen;
	TEST_EXPECT(listen.refresh(rig.session.view()));
	opennova::world::WeatherTickEvents lightning;
	lightning.thunder_a = true;
	listen.thunder(12, lightning);
	opennova::audio::SoundSelector selector;
	uint64_t seq = 0;
	const std::vector<ClipSoundFired> fired = listen.fire_sounds(rig.session.view().project.scan.get(), selector, seq, 1.0f);
	TEST_EXPECT(fired.size() == 1 && fired[0].set == "THUNDER" && fired[0].state == "played" && fired[0].tick == 12 &&
	            fired[0].action == "thunder" && !fired[0].voices.empty() && fired[0].voices[0].path == "sounds/thunder.wav");
	TEST_EXPECT(listen.fire_sounds(nullptr, selector, seq, 1.0f).empty());
	// Off: closed, nothing listens.
	rig.set(R"({"kind": "environment", "options": {"listen": {"on": false}}})");
	rig.advance(0.1);
	TEST_EXPECT(!viewport->listen().open() && rig.body().get("listen")->is_null());
	std::printf("listen: the rain's two loops at the camera, the lightning's thunder\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_kind();
	failures += test_session();
	failures += test_no_mission();
	failures += test_listen();
	if (failures == 0) std::printf("editor_environment_viewport: all passed\n");
	return failures == 0 ? 0 : 1;
}
