// A mission's soundscape where the camera stands (DI-36, editor/preview/mission_listen): over a real session, a project
// holding a bank of ambient sets, the rain's two loops and the thunder, an item catalog whose rows the game updates as
// env-sound emitters (markers, and a building), and a minted mission placing them with a script that rains from its
// start and flashes the lightning a few seconds in. Listening, each source registers the set its item authors for the
// hour as the runtime's AmbientMixer registers it (a night-only source quiet at noon, a set no bank holds silent, a far
// source out of range), the loudest layers take the game's eight channels (more within reach: the rest outranked), the
// rain's two loops register beside the camera at the rain the script's start made, the lightning's thunder reaches the
// Shell's clip voices as a one-shot planned at its distance, a seek back runs the script from its start again; the wire
// carries the body's `listen`, each source's `sound` and the options' `listen`; the overlay rings each source.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_listen.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/audio/ambient_channel_pool.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "common/test_paths.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

constexpr const char *kMission = "missions/listen.bms";

struct SetSpec {
	const char *name;
	uint16_t falloff;
	uint16_t min;
	uint32_t volume;
};
// The bank: a day bed heard to 100 m, a night bed to 50 m with a 10 m proximity fade, the rain's two loops to 30 m,
// the thunder, and a set the script plays.
const SetSpec kSets[] = {
	{ "LP_DAY", 100, 0, 200 },   { "LP_NIGHT", 50, 10, 180 }, { "LPNV_RAIN_L", 30, 0, 255 },
	{ "LPNV_RAIN_R", 30, 0, 255 }, { "THUNDER", 2000, 0, 255 }, { "BIRDS", 200, 0, 220 },
};

std::vector<uint8_t> bank_bytes() {
	lwf::File bank;
	for (size_t i = 0; i < std::size(kSets); ++i) {
		lwf::Single single;
		single.name = kSets[i].name;
		single.path = strutil::to_lower(kSets[i].name) + ".wav";
		bank.singles.push_back(single);
		lwf::Multi set;
		set.name = kSets[i].name;
		set.pitch_base = lwf::kAuthoredSetPitchBase;
		set.target_id = 5000;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		lwf::Playlist layer;
		layer.falloff_radius = kSets[i].falloff;
		layer.min_distance = kSets[i].min;
		layer.flags = lwf::kFlagInternal | lwf::kFlagExternal;
		layer.sndparm_indices.push_back(uint32_t(i));
		bank.playlists.push_back(layer);
		lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = lwf::kPitchUnityQ16;
		member.volume = kSets[i].volume;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

// A day-and-night bed, a night-only bed, a bed naming a set no bank holds, a crate (no emitter), a pump among the
// buildings that hums by day (an env-sound emitter in another pool), a bird tree that sings by day every second or
// two (an item's time-of-day shot, no loop).
const char *kItems =
		"begin \"Ambient bed\"\nid 100800\ntype marker\nmove_function envs\nsoundloop_1 LP_DAY\nsoundloop_2 LP_DAY\n"
		"soundloop_3 LP_DAY\nsoundloop_4 LP_NIGHT\nend\n"
		"begin \"Night frogs\"\nid 100801\ntype marker\nmove_function envs\nsoundloop_4 LP_NIGHT\nend\n"
		"begin \"Lost bed\"\nid 100802\ntype marker\nmove_function envs\nsoundloop_2 LP_NONE\nend\n"
		"begin \"Crate\"\nid 100803\ntype object\nend\n"
		"begin \"Pump\"\nid 100804\ntype building\nai_function envs\nsoundloop_1 LP_DAY\nsoundloop_2 LP_DAY\n"
		"soundloop_3 LP_DAY\nend\n"
		"begin \"Bird tree\"\nid 100805\ntype marker\nai_function envs\ndayshot BIRDS 1 1\nend\n";

// The script: rain from the start, the lightning three seconds in.
const char *kScript = "if never() then\n\train(100,1)\nendif\nif ontick(3) then\n\tflash()\nendif\n";

struct Placed {
	mission::EntityKind kind;
	int item;
	float x, y;
};
// The near beds about the origin; the pump far east; ten beds in a row far north (more than the channels).
std::vector<Placed> placed() {
	std::vector<Placed> out = {
		{ mission::EntityKind::Marker, 100800, 0.0f, 0.0f },   { mission::EntityKind::Marker, 100801, 10.0f, 0.0f },
		{ mission::EntityKind::Marker, 100802, 20.0f, 0.0f },  { mission::EntityKind::Item, 100803, 0.0f, 5.0f },
		{ mission::EntityKind::Building, 100804, 400.0f, 0.0f }, { mission::EntityKind::Marker, 100805, 5.0f, 5.0f },
	};
	for (int i = 0; i < 10; ++i) out.push_back({ mission::EntityKind::Marker, 100800, float(i) * 3.0f, 800.0f });
	return out;
}

// With `dialogs`, two events: a pre-mission one playing dialog 1, another playing dialog 2 on the first event pass.
std::vector<uint8_t> mission_bytes(bool dialogs = false) {
	bms::File mission;
	mission::make_default(mission);
	for (const Placed &each : placed()) {
		mission::EntityTransform at;
		at.x = each.x;
		at.y = each.y;
		mission::add_entity(mission, each.kind, each.item, at);
	}
	if (dialogs)
		for (const auto &[dialog, flags] : std::vector<std::pair<int, uint32_t>>{{1, uint32_t(bms::EventFlags::PreMission)}, {2, 0u}}) {
			mission::MissionEventRecord event;
			event.flags = int(flags);
			const size_t index = mission::add_event(mission, event);
			mission::MissionActionRecord action;
			action.action_type = int(bms::ActionType::PlayWavList);
			action.param1 = dialog;
			std::string error;
			mission::insert_event_action(mission, index, 0, action, error);
		}
	mission::sync_counts(mission);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bms::write(mission, bytes, error)) bytes.clear();
	return bytes;
}

struct Rig {
	editor_test::TempProjectDir dir{ "opennova_editor_mission_listen" };
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	editor_test::FakeDevices devices;
	std::string root;
	std::string path;

	bool open(bool waves = true, bool dialogs = false) {
		session.handle(request::new_project(dir.file("project"), "Listen"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		bool ok = editor_test::write_bytes(root + "/" + kMission, mission_bytes(dialogs)) &&
				editor_test::write_text(root + "/missions/listen.wac", kScript) &&
				editor_test::write_text(root + "/defs/items.def", kItems) &&
				editor_test::write_bytes(root + "/sounds/game.lwf", bank_bytes());
		const std::vector<uint8_t> tone = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/lwf/tone.wav");
		for (const SetSpec &set : kSets)
			if (waves || std::string(set.name) != "LP_NIGHT")
				ok = ok && editor_test::write_bytes(root + "/sounds/" + strutil::to_lower(set.name) + ".wav", tone);
		// The mission's dialog bank and its sounds (DI-32): dlg001 two lines, the second after half a second; dlg002 one.
		if (dialogs) {
			dbf::File bank;
			const auto line = [](const char *wave, uint8_t delay) {
				dbf::Line out;
				out.def_id_name = wave;
				out.sequence = "##";
				out.delay = delay;
				return out;
			};
			dbf::Group one, two;
			one.group_name = "dlg001";
			one.lines = { line("D1A", 0), line("D1B", 5) };
			two.group_name = "dlg002";
			two.lines = { line("D2A", 0) };
			bank.groups = { one, two };
			lwf::File sounds;
			for (const char *wave : { "D1A", "D1B", "D2A" }) {
				lwf::Single single;
				single.name = wave;
				single.path = strutil::to_lower(wave) + ".wav";
				single.value_hi = 0xD200;
				sounds.singles.push_back(single);
				ok = ok && editor_test::write_bytes(root + "/sounds/" + strutil::to_lower(wave) + ".wav", tone);
			}
			std::vector<uint8_t> dbf_bytes, lwf_bytes;
			std::string error;
			ok = ok && dbf::encode_dbf(bank, dbf_bytes, error) && lwf::encode_lwf(sounds, lwf_bytes, error) &&
					editor_test::write_bytes(root + "/missions/listen.dbf", dbf_bytes) &&
					editor_test::write_bytes(root + "/missions/listen.lwf", lwf_bytes);
		}
		if (!ok) return false;
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		path = session.document_for(kMission)->path();
		devices.sync(session);
		return true;
	}
	const MissionViewport *viewport() {
		return static_cast<const MissionViewport *>(session.viewports().follow_one(session.view(), path, ViewportKind::Mission));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		devices.sync(session);
		return session.outcome().done();
	}
	// The camera's eye about the mission point (x, y, z): looking down at it from a metre off.
	bool look_from(double x, double y, double z) {
		return set(R"({"camera": {"target": [)" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) +
				R"(], "yaw": 0, "pitch": 80, "distance": 1}})");
	}
	bool listen(bool on, double volume = 1.0, double hour = 12.0) {
		return set(R"({"options": {"time": )" + std::to_string(hour) + R"(, "listen": {"on": )" +
				std::string(on ? "true" : "false") + R"(, "volume": )" + std::to_string(volume) + "}}}");
	}
	// The clock run on to `ticks` a Shell frame at a time (the session's advance fires the one-shots, the pump follows).
	void run_to(int32_t ticks) {
		set(R"({"clock": {"playing": true, "rate": 1}})");
		while (session.viewports().clock().ticks() < ticks) {
			session.advance(2.0 / 62.5);
			devices.sync(session);
		}
	}
	const MissionDocument &document() {
		return static_cast<const MissionDocument &>(*records_of(*session.document_for(kMission)));
	}
	std::vector<NodeAddress> rows(MissionKind kind) {
		std::vector<NodeAddress> out;
		for (const Node *row : document().rows_of(kind)) out.push_back({ row->id, row->kind, 0 });
		return out;
	}
	JsonValue json() {
		const MissionViewport *model = viewport();
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
};

std::string status_of(const MissionListen &listen, NodeId row) {
	const MissionSoundSource *source = listen.source(row);
	return source ? source->status : "none";
}

size_t playing_channels(const MissionListen &listen, const char *kind = nullptr) {
	size_t count = 0;
	for (const MissionSoundChannel &channel : listen.channels())
		count += channel.candidate >= 0 && (!kind || std::string(channel.source) == kind) ? 1 : 0;
	return count;
}

} // namespace

static int test_sources_by_the_hour_and_the_reach() {
	Rig rig;
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == MissionViewStatus::Ready);
	if (!viewport) return 1;
	// Off by default: nothing held, the body says null.
	TEST_EXPECT(!viewport->options().listen.on && !viewport->listen().open());
	TEST_EXPECT(rig.json().get("body")->get("listen")->is_null());

	// On at noon, a metre over the day bed: the bed plays, the night frogs are quiet, the lost bed's set is in no bank,
	// the crate is no source, the pump among the buildings is one and out of reach.
	TEST_EXPECT(rig.look_from(0.0, 0.0, 0.0));
	TEST_EXPECT(rig.listen(true));
	rig.run_to(40);
	const MissionListen &listen = rig.viewport()->listen();
	TEST_EXPECT(listen.open());
	const std::vector<NodeAddress> markers = rig.rows(MissionKind::Marker);
	const std::vector<NodeAddress> items = rig.rows(MissionKind::Item);
	const std::vector<NodeAddress> buildings = rig.rows(MissionKind::Building);
	TEST_EXPECT(markers.size() == 14 && items.size() == 1 && buildings.size() == 1);
	TEST_EXPECT(listen.sources().size() == 15); // the fourteen markers and the pump
	TEST_EXPECT(status_of(listen, markers[0].row) == "playing");
	TEST_EXPECT(status_of(listen, markers[1].row) == "quiet");
	TEST_EXPECT(status_of(listen, markers[2].row) == "no_set");
	TEST_EXPECT(status_of(listen, markers[3].row) == "quiet"); // the bird tree: shots alone, no loop
	TEST_EXPECT(!listen.source(items[0].row));
	TEST_EXPECT(status_of(listen, buildings[0].row) == "out_of_range");
	// The walk's order: the markers first, the buildings after (resolve_envs_markers').
	TEST_EXPECT(listen.sources().front().row == markers[0].row && listen.sources().back().row == buildings[0].row);
	const MissionSoundSource *bed = listen.source(markers[0].row);
	TEST_EXPECT(bed && bed->set == "LP_DAY" && bed->bank == "game.lwf" && bed->region == 1 && bed->falloff == 100.0f);
	TEST_EXPECT(bed && bed->volume > 0 && bed->channel >= 0);
	const MissionSoundChannel *day = nullptr;
	for (const MissionSoundChannel &channel : listen.channels())
		if (channel.candidate >= 0 && channel.set == "LP_DAY") day = &channel;
	TEST_EXPECT(day && day->path == "sounds/lp_day.wav" && day->row == markers[0].row && day->bank == "game.lwf");
	TEST_EXPECT(day && day->volume == bed->volume && day->pitch_q16 == 0x10000);
	// Its hover: the hours' sets and what plays.
	const std::vector<std::string> words = listen.source_words(markers[0].row);
	TEST_EXPECT(words.size() == 6 && words[2].find("LP_DAY (game.lwf) <- now") != std::string::npos);
	TEST_EXPECT(words.back().find("Plays LP_DAY on channel") == 0);

	// At night the frogs and the bed play the night set.
	TEST_EXPECT(rig.listen(true, 1.0, 23.0));
	rig.run_to(80);
	TEST_EXPECT(status_of(listen, markers[1].row) == "playing");
	const MissionSoundSource *frogs = listen.source(markers[1].row);
	TEST_EXPECT(frogs && frogs->set == "LP_NIGHT" && frogs->region == 3 && frogs->min == 10.0f);
	TEST_EXPECT(listen.source(markers[0].row)->set == "LP_NIGHT");

	// Far from every source: nothing but the rain beside the camera.
	TEST_EXPECT(rig.look_from(0.0, 400.0, 0.0));
	rig.run_to(120);
	TEST_EXPECT(status_of(listen, markers[0].row) == "out_of_range" && playing_channels(listen, "marker") == 0);
	return 0;
}

static int test_the_channels_take_the_loudest() {
	Rig rig;
	TEST_EXPECT(rig.open());
	// Among the ten beds in a row: more within reach than the game's channels.
	TEST_EXPECT(rig.look_from(13.5, 800.0, 0.0));
	TEST_EXPECT(rig.listen(true));
	rig.run_to(40);
	const MissionListen &listen = rig.viewport()->listen();
	TEST_EXPECT(listen.channels().size() <= size_t(audio::kAmbientMixChannels));
	TEST_EXPECT(playing_channels(listen) == size_t(audio::kAmbientMixChannels));
	size_t playing = 0, outranked = 0;
	for (const MissionSoundSource &source : listen.sources()) {
		playing += std::string(source.status) == "playing" ? 1 : 0;
		outranked += std::string(source.status) == "outranked" ? 1 : 0;
	}
	// Eight channels, two of them the rain's loops: six beds play, four are outranked, the farthest.
	TEST_EXPECT(playing_channels(listen, "rain") == 2);
	TEST_EXPECT(playing == 6 && outranked == 4);
	for (const MissionSoundSource &source : listen.sources())
		if (std::string(source.status) == "outranked")
			for (const MissionSoundSource &other : listen.sources())
				if (std::string(other.status) == "playing" && other.marker >= 0) TEST_EXPECT(source.volume <= other.volume);
	// A move of the camera keeps the incumbents' channels (no wave started again).
	std::vector<uint64_t> started;
	for (const MissionSoundChannel &channel : listen.channels()) started.push_back(channel.started);
	TEST_EXPECT(rig.look_from(13.6, 800.0, 0.0));
	rig.run_to(50);
	size_t kept = 0;
	for (size_t i = 0; i < listen.channels().size() && i < started.size(); ++i)
		kept += listen.channels()[i].started == started[i] ? 1 : 0;
	TEST_EXPECT(kept == started.size());
	return 0;
}

static int test_the_scripts_weather() {
	Rig rig;
	TEST_EXPECT(rig.open());
	TEST_EXPECT(rig.look_from(0.0, 0.0, 0.0));
	TEST_EXPECT(rig.listen(true, 0.5));
	rig.run_to(10);
	const MissionListen &listen = rig.viewport()->listen();
	const MissionScriptRun &script = listen.script();
	// The start ran the script's first execution and the settle: it rains already.
	TEST_EXPECT(script.booted() && script.scripted() && script.error().empty());
	TEST_EXPECT(script.weather() && script.weather()->rain_pct_current_q16() > 0xF000);
	// The rain's two loops beside the camera: 2 m either side on the mission's x.
	const MissionSoundChannel *left = nullptr, *right = nullptr;
	for (const MissionSoundChannel &channel : listen.channels()) {
		if (channel.set == "LPNV_RAIN_L") left = &channel;
		if (channel.set == "LPNV_RAIN_R") right = &channel;
	}
	TEST_EXPECT(left && right && std::string(left->source) == "rain" && left->path == "sounds/lpnv_rain_l.wav");
	TEST_EXPECT(left && right && std::fabs(left->distance - 2.0f) < 0.05f && std::fabs(right->distance - 2.0f) < 0.05f);
	// Each at its own ear (D-SND-38): four metres apart along the mission's x.
	TEST_EXPECT(left && right && std::fabs(std::fabs(left->at.x - right->at.x) - 4.0f) < 0.01f);
	TEST_EXPECT(left && left->volume > 200);
	// The lightning three seconds in: the thunder reaches the Shell's clip voices, its volume the master's half; the
	// bird tree's day shot every second or two, at the tree [orig: Entity_SpawnRegionalEffect @ 0x408290].
	const uint64_t booted = script.boots();
	rig.run_to(62 * 6);
	TEST_EXPECT(script.script_runs() >= 4);
	bool thunder = false;
	size_t birds = 0;
	for (const ClipSoundFired &fired : listen.sounds_fired()) {
		if (fired.set == "THUNDER" && fired.state == "played") thunder = true;
		if (fired.set == "BIRDS" && fired.state == "played" && fired.action == "shot") ++birds;
	}
	TEST_EXPECT(thunder);
	TEST_EXPECT(birds >= 1);
	bool handed = false;
	for (const ClipSoundPlay &play : rig.session.clip_sounds_since(0))
		for (const WorkspaceView::Voice &voice : play.voices)
			if (voice.path == "sounds/thunder.wav") {
				handed = true;
				TEST_EXPECT(voice.volume > 0 && voice.volume <= 128);
			}
	TEST_EXPECT(handed);
	// A seek back runs the script from its start again: no boot, its tick back.
	TEST_EXPECT(rig.set(R"({"clock": {"ticks": 5, "playing": false}})"));
	rig.viewport();
	TEST_EXPECT(script.boots() == booted && script.tick() == 5);
	// The script edited: the start boots again over it.
	TEST_EXPECT(editor_test::write_text(rig.root + "/missions/listen.wac", "if never() then\n\train(0,1)\nendif\n"));
	editor_test::handle_to_end(rig.session, request::rescan());
	while (rig.session.view().activity.validation.running) rig.session.poll();
	rig.devices.sync(rig.session);
	rig.viewport();
	TEST_EXPECT(script.boots() == booted + 1 && script.weather() && script.weather()->rain_pct_current_q16() == 0);
	return 0;
}

static int test_the_wire_and_the_overlay() {
	Rig rig;
	TEST_EXPECT(rig.open(false)); // the night bed's wave left out
	TEST_EXPECT(rig.look_from(0.0, 0.0, 0.0));
	TEST_EXPECT(rig.listen(true, 0.75, 23.0));
	rig.run_to(40);
	const JsonValue envelope = rig.json();
	const JsonValue *options = envelope.get("options");
	const JsonValue *listen_options = options ? options->get("listen") : nullptr;
	TEST_EXPECT(listen_options && listen_options->get_bool("on", false) && listen_options->get_number("volume", 0) == 0.75);
	const JsonValue *body = envelope.get("body");
	const JsonValue *listen = body ? body->get("listen") : nullptr;
	TEST_EXPECT(listen && listen->is_object() && listen->get_number("budget", 0) == double(audio::kAmbientMixChannels));
	const JsonValue *sources = listen ? listen->get("sources") : nullptr;
	// At night the beds' set has no wave in the project: no channel takes it, the frogs (its only set) and the beds
	// silent for it; the lost bed's set is in no bank at all, and the pump hums by day alone.
	TEST_EXPECT(sources && sources->get_number("count", 0) == 15.0 && sources->get_number("no_wave", -1) == 12.0 &&
			sources->get_number("no_set", -1) == 1.0 && sources->get_number("quiet", -1) == 2.0);
	const JsonValue *channels = listen ? listen->get("channels") : nullptr;
	TEST_EXPECT(channels && channels->is_array());
	if (channels)
		for (const JsonValue &channel : channels->array) TEST_EXPECT(channel.get_string("source", "") == "rain");
	const JsonValue *rain = listen ? listen->get("rain") : nullptr;
	TEST_EXPECT(rain && rain->get_number("percent", 0) == 100.0 && rain->get_string("kind", "") == "rain");
	const JsonValue *script = listen ? listen->get("script") : nullptr;
	TEST_EXPECT(script && script->get_bool("booted", false) && script->get_string("error", "x").empty());
	// The game context's pair, which the project lacks: no music in a mission.
	const JsonValue *music = listen ? listen->get("music") : nullptr;
	TEST_EXPECT(music && music->get_string("bank", "") == "gamemus.sbf" && !music->get_bool("bank_found", true) &&
			!music->get_bool("heard", true));
	// The same through the sounds_playing query: the listening view, its channels.
	std::string error;
	const JsonValue playing = rig.session.query("sounds_playing", JsonValue::make_null(), error);
	const JsonValue *listening = playing.get("listening");
	TEST_EXPECT(error.empty() && listening && listening->array.size() == 1 &&
			listening->array[0].get_string("path", "") == kMission);
	const JsonValue *heard = listening && !listening->array.empty() ? listening->array[0].get("listen") : nullptr;
	TEST_EXPECT(heard && heard->get("channels") && heard->get("channels")->array.size() == channels->array.size());
	TEST_EXPECT(playing.get("clip_sounds") && playing.get("clip_sounds")->is_array() && playing.get("sound"));
	// Each source's `sound` among the items.
	size_t sounded = 0;
	if (const JsonValue *items = envelope.get("items"))
		for (const JsonValue &item : items->array)
			if (const JsonValue *sound = item.get("sound")) {
				++sounded;
				TEST_EXPECT(sound->get("slots") && sound->get("slots")->array.size() == 4);
			}
	TEST_EXPECT(sounded == 15);
	// The overlay: each source's ring of segments and its dot.
	const MissionViewport *viewport = rig.viewport();
	OverlayList shapes;
	mission_listen_shapes(viewport->listen(), viewport->camera(), 640, 480, 0, 0.0f, shapes);
	size_t lines = 0, dots = 0;
	for (const OverlayShape &shape : shapes.shapes) {
		lines += shape.kind == OverlayKind::Line ? 1 : 0;
		dots += shape.kind == OverlayKind::Marker ? 1 : 0;
	}
	TEST_EXPECT(lines > 0 && dots >= 1);
	// The options refuse what Listen does not take.
	TEST_EXPECT(!rig.set(R"({"options": {"listen": {"volume": 2}}})"));
	TEST_EXPECT(!rig.set(R"({"options": {"listen": {"loud": true}}})"));
	// Off: closed, its body null again.
	TEST_EXPECT(rig.listen(false));
	TEST_EXPECT(!rig.viewport()->listen().open() && rig.json().get("body")->get("listen")->is_null());
	return 0;
}

// The mission's dialogs heard as the game plays them (DI-32): the pre-mission event's Play dialog 1 queues its two lines
// at the start, the second once the first has ended and after its half second; the second event's dialog 2, fired on
// the first event pass, waits behind dialog 1 on the one dialog channel [orig: Dialog_UpdatePlayback @ 0x44e470]; each
// line reaches the Shell's clip voices on its tick at its wave's dialog volume (210, the Listen's half of it here), the
// body's `dialog` says the bank and its sounds; a seek back empties the channel and the start queues dialog 1 again.
static int test_the_dialogs() {
	Rig rig;
	TEST_EXPECT(rig.open(true, true));
	TEST_EXPECT(rig.look_from(0.0, 0.0, 0.0));
	TEST_EXPECT(rig.listen(true, 0.5));
	rig.run_to(62 * 6);
	const MissionListen &listen = rig.viewport()->listen();
	int32_t d1a = -1, d1b = -1, d2a = -1;
	for (const ClipSoundFired &fired : listen.sounds_fired()) {
		if (fired.action != "dialog") continue;
		TEST_EXPECT(fired.state == "played" && fired.bank == "listen.dbf" && fired.voices.size() == 1);
		if (fired.voices.size() != 1) continue;
		TEST_EXPECT(fired.voices[0].volume == 105);
		if (fired.voices[0].wave == "D1A") d1a = fired.tick;
		if (fired.voices[0].wave == "D1B") d1b = fired.tick;
		if (fired.voices[0].wave == "D2A") d2a = fired.tick;
	}
	TEST_EXPECT(d1a == 0 && d1b >= d1a + 31 && d2a >= d1b);
	bool handed = false;
	for (const ClipSoundPlay &play : rig.session.clip_sounds_since(0))
		for (const WorkspaceView::Voice &voice : play.voices) handed = handed || voice.path == "sounds/d1b.wav";
	TEST_EXPECT(handed);
	const JsonValue envelope = rig.json();
	const JsonValue *body = envelope.get("body");
	const JsonValue *heard = body ? body->get("listen") : nullptr;
	const JsonValue *dialog = heard ? heard->get("dialog") : nullptr;
	TEST_EXPECT(dialog && dialog->get_string("bank", "") == "listen.dbf" && dialog->get_bool("bank_found", false) &&
			dialog->get_string("sounds", "") == "listen.lwf");
	// Back to the start: the channel empties, and the start's dialog 1 is heard again.
	TEST_EXPECT(rig.set(R"({"clock": {"ticks": 0, "playing": false}})"));
	rig.viewport();
	rig.run_to(10);
	bool again = false;
	for (const ClipSoundFired &fired : rig.viewport()->listen().sounds_fired())
		again = again || (fired.action == "dialog" && fired.tick == 0 && fired.set == "dlg001" && !fired.voices.empty() &&
				fired.voices[0].wave == "D1A" && fired.seq > 3);
	TEST_EXPECT(again);
	return 0;
}

int main() {
	int failed = 0;
	failed |= test_sources_by_the_hour_and_the_reach();
	failed |= test_the_channels_take_the_loudest();
	failed |= test_the_scripts_weather();
	failed |= test_the_wire_and_the_overlay();
	failed |= test_the_dialogs();
	if (failed) return 1;
	std::printf("editor_mission_listen_test: OK\n");
	return 0;
}
