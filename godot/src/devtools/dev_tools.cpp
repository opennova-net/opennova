#include "devtools/dev_tools.h"

#include "simulation/simulation.h"

#include <godot_cpp/classes/sub_viewport.hpp>

#include <base/io/log_ring.h>

#if OPENNOVA_DEVTOOLS
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/time.hpp>
#include <runtime/devtools/debug_request.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/entity_directory_snapshot.h>
#include <runtime/devtools/environment_request.h>
#include <runtime/devtools/environment_snapshot.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/stats_window.h>

#include <algorithm>
#include <cstring>
#include <utility>
#endif

namespace godot {

void DevTools::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_open"), &DevTools::is_open);
	ClassDB::bind_method(D_METHOD("set_open", "open"), &DevTools::set_open);
	ClassDB::bind_method(D_METHOD("toggle"), &DevTools::toggle);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "stats"), &DevTools::set_frame_stats);
	ClassDB::bind_method(D_METHOD("get_frame_stats"), &DevTools::get_frame_stats);
	ClassDB::bind_method(D_METHOD("set_simulation", "simulation"), &DevTools::set_simulation);
	ClassDB::bind_method(D_METHOD("set_game_viewport", "viewport"), &DevTools::set_game_viewport);
	ClassDB::bind_method(D_METHOD("set_game_play_available", "available"), &DevTools::set_game_play_available);
	ClassDB::bind_method(D_METHOD("is_game_play_available"), &DevTools::is_game_play_available);
	ClassDB::bind_method(D_METHOD("set_game_playing", "playing"), &DevTools::set_game_playing);
	ClassDB::bind_method(D_METHOD("is_game_playing"), &DevTools::is_game_playing);
	ClassDB::bind_method(D_METHOD("handle_tools_toggle"), &DevTools::handle_tools_toggle);
	ClassDB::bind_method(D_METHOD("handle_game_escape"), &DevTools::handle_game_escape);
	ClassDB::bind_method(D_METHOD("get_rendered_game_viewport_size"), &DevTools::get_rendered_game_viewport_size);
	ClassDB::bind_method(D_METHOD("feed_stats_window", "frames", "sums", "peaks", "sample_frames"),
			&DevTools::feed_stats_window);
	ClassDB::bind_method(D_METHOD("stats_reading_frames"), &DevTools::stats_reading_frames);
	ClassDB::bind_method(D_METHOD("stats_row_ids"), &DevTools::stats_row_ids);
	ClassDB::bind_method(D_METHOD("stats_row_average", "row_id"), &DevTools::stats_row_average);
	ClassDB::bind_method(D_METHOD("stats_row_peak", "row_id"), &DevTools::stats_row_peak);
	ClassDB::bind_method(D_METHOD("stats_row_info", "row_id"), &DevTools::stats_row_info);
	ClassDB::bind_method(D_METHOD("reset_layout"), &DevTools::reset_layout);
	ClassDB::bind_static_method("DevTools", D_METHOD("engine_log_after", "cursor"),
			&DevTools::engine_log_after);
	ADD_SIGNAL(MethodInfo("open_changed", PropertyInfo(Variant::BOOL, "open")));
	ADD_SIGNAL(MethodInfo("game_input_mode_changed", PropertyInfo(Variant::BOOL, "playing")));
}

// Both flavours: the engine log ring records regardless of OPENNOVA_DEVTOOLS
// (it is io infrastructure, not an ImGui window).
Dictionary DevTools::engine_log_after(int64_t p_cursor) {
	const std::vector<opennova::io::LogRingEntry> entries =
			opennova::io::LogRing::instance().entries_after(
					p_cursor > 0 ? static_cast<uint64_t>(p_cursor) : 0);
	PackedInt64Array sequences;
	PackedStringArray levels;
	PackedStringArray texts;
	for (const opennova::io::LogRingEntry &entry : entries) {
		sequences.append(static_cast<int64_t>(entry.sequence));
		levels.append(String(opennova::io::log_level_name(entry.level)));
		texts.append(String::utf8(entry.text.c_str()));
	}
	Dictionary out;
	out["sequences"] = sequences;
	out["levels"] = levels;
	out["texts"] = texts;
	return out;
}

#if OPENNOVA_DEVTOOLS

DevTools::DevTools() : tools_(std::make_unique<opennova::devtools::GameDevTools>()) {
	tools_->set_game_viewport(this);
}

DevTools::~DevTools() = default;

opennova::devtools::ImGuiPass *DevTools::engine_pass() {
	return &tools_->pass();
}

void DevTools::_exit_tree() {
	set_game_play_available(false);
	set_game_playing_internal(false);
	set_simulation(nullptr);
	game_viewport_ = nullptr;
	rendered_game_viewport_size_ = Vector2i();
	tools_->set_game_viewport(nullptr);
	tools_->reset_game_input_mode();
	tools_->pass().set_open(false);
	tools_->set_frame_stats(nullptr);
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
	ImGuiPassNode::_exit_tree();
}

void DevTools::after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) {
	(void)p_frame_index;
	if (p_drew && frame_stats_.is_valid() && frame_stats_->is_capture_active()) {
		frame_stats_->add(FrameStats::FRAME_DEBUG_REFRESH, p_layout_us);
	}
	apply_game_requests();
	sync_game_spectator_state();
	apply_debug_requests();
	apply_environment_requests();
	push_entity_directory();
	push_environment_snapshot();
	if (open_ && !tools_->pass().is_open()) {
		// Closed from inside (Escape, the menu).
		set_game_playing_internal(false);
		tools_->reset_game_input_mode();
		open_ = false;
		emit_signal("open_changed", false);
	}
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
}

bool DevTools::is_open() const {
	return tools_->pass().is_open();
}

void DevTools::set_open(bool p_open) {
	if (p_open == tools_->pass().is_open()) {
		return;
	}
	// Every workspace lifetime begins and ends in Interact. This also clears
	// queued requests so an Escape from the previous lifetime cannot leak.
	set_game_playing_internal(false);
	tools_->reset_game_input_mode();
	tools_->pass().set_open(p_open);
	open_ = p_open;
	sync_layer_visible();
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
	emit_signal("open_changed", p_open);
}

void DevTools::set_game_viewport(SubViewport *p_viewport) {
	game_viewport_ = p_viewport;
	rendered_game_viewport_size_ = Vector2i();
}

void DevTools::set_game_play_available(bool p_available) {
	if (game_play_available_ == p_available) {
		return;
	}
	game_play_available_ = p_available;
	tools_->set_game_play_available(p_available);
	apply_game_requests();
}

bool DevTools::is_game_play_available() const {
	return game_play_available_;
}

void DevTools::set_game_playing_internal(bool p_playing) {
	if (game_playing_ == p_playing) {
		tools_->set_game_input_mode(p_playing
				? opennova::devtools::GameInputMode::Play
				: opennova::devtools::GameInputMode::Interact);
		return;
	}
	game_playing_ = p_playing;
	tools_->set_game_input_mode(p_playing
			? opennova::devtools::GameInputMode::Play
			: opennova::devtools::GameInputMode::Interact);
	emit_signal("game_input_mode_changed", p_playing);
}

void DevTools::set_game_playing(bool p_playing) {
	if (p_playing && (!is_open() || !game_play_available_)) {
		return;
	}
	set_game_playing_internal(p_playing);
}

bool DevTools::is_game_playing() const {
	return game_playing_;
}

bool DevTools::handle_tools_toggle() {
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (last_tools_toggle_frame_ == frame) {
		return true;
	}
	last_tools_toggle_frame_ = frame;
	toggle();
	return true;
}

bool DevTools::handle_game_escape() {
	if (!is_open()) {
		return false;
	}
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (last_game_escape_frame_ == frame) {
		return true;
	}
	last_game_escape_frame_ = frame;
	tools_->request_game_escape();
	apply_game_requests();
	return true;
}

Vector2i DevTools::get_rendered_game_viewport_size() const {
	return rendered_game_viewport_size_;
}

void DevTools::apply_game_requests() {
	opennova::devtools::GameWindowRequest request;
	while (tools_->take_game_request(request)) {
		switch (request) {
			case opennova::devtools::GameWindowRequest::EnterPlay:
				set_game_playing(true);
				break;
			case opennova::devtools::GameWindowRequest::EnterInteract:
				set_game_playing_internal(false);
				break;
			case opennova::devtools::GameWindowRequest::CloseTools:
				set_open(false);
				return;
			case opennova::devtools::GameWindowRequest::EnableSpectator:
			case opennova::devtools::GameWindowRequest::DisableSpectator: {
				Simulation *sim = simulation();
				if (sim != nullptr && !sim->is_joiner()) {
					(void)sim->set_local_spectator(
							request == opennova::devtools::GameWindowRequest::EnableSpectator);
				}
				break;
			}
		}
	}
}

void DevTools::sync_game_spectator_state() {
	Simulation *sim = simulation();
	const bool available = sim != nullptr && !sim->is_joiner() && sim->has_local_player();
	tools_->set_game_spectator_state(
			available, sim != nullptr && sim->is_local_spectator());
}

void DevTools::draw(int p_requested_width, int p_requested_height) {
	if (game_viewport_ == nullptr) {
		return;
	}
	const Vector2i requested(std::max(1, p_requested_width), std::max(1, p_requested_height));
	if (game_viewport_->get_size() != requested) {
		game_viewport_->set_size(requested);
	}
	rendered_game_viewport_size_ = requested;
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", game_viewport_);
	}
}

void DevTools::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
	tools_->set_frame_stats(p_stats.is_valid() ? &p_stats->board() : nullptr);
	if (p_stats.is_valid()) {
		p_stats->sync_capture_signal();
	}
}

Simulation *DevTools::simulation() const {
	return simulation_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(simulation_id_))
			: nullptr;
}

void DevTools::set_simulation(Simulation *p_simulation) {
	const ObjectID id = p_simulation != nullptr ? ObjectID(p_simulation->get_instance_id()) : ObjectID();
	if (simulation_id_ == id) {
		return;
	}
	simulation_id_ = id;
	last_entity_push_ms_ = -1;
	last_environment_push_ms_ = -1;
	if (p_simulation == nullptr) {
		// The unload edge: an invalid snapshot clears the pushed record so a
		// window left open never shows a dead world's rows.
		tools_->set_entity_directory(opennova::devtools::EntityDirectorySnapshot{});
		tools_->set_environment_snapshot(opennova::devtools::EnvironmentSnapshot{});
	}
	sync_game_spectator_state();
}

// Drain the F3 windows' typed mutation requests into the SAME engine-backed
// debug delegates the MCP control plane uses (ADR 0042 d6). Requests queued
// with no world behind them drain and drop.
void DevTools::apply_debug_requests() {
	opennova::devtools::DebugRequest request;
	Simulation *simulation_ = simulation();
	while (tools_->take_debug_request(request)) {
		if (simulation_ == nullptr) {
			continue;
		}
		// The window's requests carry the engine handle; they reach the engine
		// mutators (EntityCommands, ADR 0042 d5) by that handle, no index detour.
		opennova::world::EntityCommands *commands = simulation_->entity_commands();
		switch (request.kind) {
			case opennova::devtools::DebugRequest::Kind::SetEntityHealth:
				if (commands != nullptr) {
					(void)commands->set_entity_health(request.target, request.health);
				}
				break;
			case opennova::devtools::DebugRequest::Kind::SetEntityPosition:
				if (commands != nullptr) {
					(void)commands->set_entity_position(request.target,
							opennova::world::Vec3{request.pos[0], request.pos[1], request.pos[2]});
				}
				break;
			case opennova::devtools::DebugRequest::Kind::TeleportLocalPlayer:
				simulation_->debug_teleport_local_player(
						Vector3(request.pos[0], request.pos[1], request.pos[2]),
						request.yaw, request.pitch);
				break;
		}
	}
}

// Push the entity-directory record while the Entities window shows, on its
// 0.5 s cadence: the ENGINE join (world::inspect::entity_directory) through
// the Simulation's native accessor — no TypedArray/Variant round-trip
// (ADR 0042 d6).
void DevTools::push_entity_directory() {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr || !tools_->needs_entity_directory()) {
		last_entity_push_ms_ = -1;
		return;
	}
	const int64_t now_ms = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
	const int64_t cadence_ms = static_cast<int64_t>(
			opennova::devtools::EntitiesWindow::kRefreshSeconds * 1000.0);
	if (last_entity_push_ms_ >= 0 && now_ms - last_entity_push_ms_ < cadence_ms) {
		return;
	}
	last_entity_push_ms_ = now_ms;
	opennova::devtools::EntityDirectorySnapshot snapshot;
	snapshot.rows = simulation_->native_entity_directory();
	snapshot.valid = true;
	snapshot.logic_tick = static_cast<uint64_t>(simulation_->get_logic_tick());
	tools_->set_entity_directory(std::move(snapshot));
}

// Drain the Environment window's typed weather commands into the ONE
// command layer (world::EntityCommands, ADR 0042 d5) — the same handlers the
// WAC VM and the MCP rows reach.
void DevTools::apply_environment_requests() {
	opennova::devtools::EnvironmentRequest request;
	Simulation *simulation_ = simulation();
	while (tools_->take_environment_request(request)) {
		if (simulation_ == nullptr || simulation_->is_joiner()) {
			continue;
		}
		opennova::world::EntityCommands *commands = simulation_->entity_commands();
		if (commands == nullptr) {
			continue;
		}
		using Kind = opennova::devtools::EnvironmentRequest::Kind;
		switch (request.kind) {
			case Kind::Rain: commands->set_rain(request.a, request.b); break;
			case Kind::Snow: commands->set_snow(request.a, request.b); break;
			case Kind::Overcast: commands->set_overcast(request.a, request.b); break;
			case Kind::FogDistance: commands->set_fog_distance(request.a); break;
			case Kind::MoveFog: commands->move_fog(request.a, request.b); break;
			case Kind::SkySpeed: commands->set_sky_speed(request.a); break;
			case Kind::SkyHeight: commands->set_sky_height(request.a); break;
			case Kind::Quake: commands->quake(request.a); break;
			case Kind::TimeOfDayMinutes: commands->set_time_of_day_minutes(request.a); break;
			case Kind::FogType: commands->set_fog_type(request.a); break;
			case Kind::SunFade: commands->sun_fade(request.a, request.b); break;
			case Kind::ColorFade: commands->set_color_fade(request.a); break;
			case Kind::Flash: commands->lightning_flash(); break;
			case Kind::FarFlash: commands->lightning_far_flash(); break;
			case Kind::WindScale: commands->set_wind_scale(request.a); break;
			case Kind::BlockColor:
				commands->set_weather_color(
						static_cast<opennova::world::WeatherColorTarget>(request.a),
						static_cast<uint32_t>(request.b));
				break;
			case Kind::LightningColor:
				commands->set_lightning_color(static_cast<uint32_t>(request.b));
				break;
		}
	}
}

// Push the environment record while the Environment window shows, on its
// 0.25 s cadence: the ENGINE join (Simulation::native_environment_snapshot)
// over the weather home — no Variant round-trip (ADR 0042 d6).
void DevTools::push_environment_snapshot() {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr || !tools_->needs_environment_snapshot()) {
		last_environment_push_ms_ = -1;
		return;
	}
	const int64_t now_ms = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
	const int64_t cadence_ms = static_cast<int64_t>(
			opennova::devtools::EnvironmentWindow::kRefreshSeconds * 1000.0);
	if (last_environment_push_ms_ >= 0 && now_ms - last_environment_push_ms_ < cadence_ms) {
		return;
	}
	last_environment_push_ms_ = now_ms;
	opennova::devtools::EnvironmentSnapshot snapshot;
	simulation_->native_environment_snapshot(snapshot);
	tools_->set_environment_snapshot(snapshot);
}

void DevTools::reset_layout() {
	tools_->pass().request_layout_reset();
}

void DevTools::feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
		const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames) {
	opennova::devtools::CaptureWindow window;
	window.frames = p_frames > 0 ? static_cast<uint64_t>(p_frames) : 0;
	const int n = opennova::devtools::kSlotCount;
	for (int i = 0; i < n; ++i) {
		if (i < p_sums.size()) {
			window.sums[static_cast<size_t>(i)] = p_sums[i];
		}
		if (i < p_peaks.size()) {
			window.peaks[static_cast<size_t>(i)] = p_peaks[i];
		}
		if (i < p_sample_frames.size()) {
			window.sample_frames[static_cast<size_t>(i)] = p_sample_frames[i];
		}
	}
	tools_->stats_window().feed_external(window);
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
}

int64_t DevTools::stats_reading_frames() const {
	return static_cast<int64_t>(tools_->stats_window().reading_frames());
}

PackedStringArray DevTools::stats_row_ids() const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	PackedStringArray ids;
	for (int i = 0; i < stats.row_count(); ++i) {
		ids.push_back(String::utf8(stats.row_id(i)));
	}
	return ids;
}

namespace {

int stats_row_index(const opennova::devtools::StatsWindow &p_stats, const String &p_row_id) {
	// CharString carries no operator==; against a const char * it decays to a
	// pointer compare, so the lookup goes through strcmp.
	const CharString id = p_row_id.utf8();
	for (int i = 0; i < p_stats.row_count(); ++i) {
		if (std::strcmp(id.get_data(), p_stats.row_id(i)) == 0) {
			return i;
		}
	}
	return -1;
}

} // namespace

String DevTools::stats_row_average(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_average(row));
}

String DevTools::stats_row_peak(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_peak(row));
}

String DevTools::stats_row_info(const String &p_row_id) const {
	const opennova::devtools::StatsWindow &stats = tools_->stats_window();
	const int row = stats_row_index(stats, p_row_id);
	return row < 0 ? String() : String::utf8(stats.row_info(row));
}

#else // !OPENNOVA_DEVTOOLS — the release flavour: the class exists, nothing runs.

DevTools::DevTools() = default;

DevTools::~DevTools() = default;

opennova::devtools::ImGuiPass *DevTools::engine_pass() {
	return nullptr;
}

void DevTools::_exit_tree() {
	ImGuiPassNode::_exit_tree();
}

void DevTools::after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) {
	(void)p_frame_index;
	(void)p_drew;
	(void)p_layout_us;
}

bool DevTools::is_open() const {
	return false;
}

void DevTools::set_open(bool p_open) {
	(void)p_open;
}

void DevTools::set_game_viewport(SubViewport *p_viewport) {
	(void)p_viewport;
}

void DevTools::set_game_play_available(bool p_available) {
	(void)p_available;
}

bool DevTools::is_game_play_available() const {
	return false;
}

void DevTools::set_game_playing(bool p_playing) {
	(void)p_playing;
}

bool DevTools::is_game_playing() const {
	return false;
}

bool DevTools::handle_tools_toggle() {
	return false;
}

bool DevTools::handle_game_escape() {
	return false;
}

Vector2i DevTools::get_rendered_game_viewport_size() const {
	return Vector2i();
}

void DevTools::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
}

void DevTools::set_simulation(Simulation *p_simulation) {
	(void)p_simulation;
}

void DevTools::reset_layout() {}

void DevTools::feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
		const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames) {
	(void)p_frames;
	(void)p_sums;
	(void)p_peaks;
	(void)p_sample_frames;
}

int64_t DevTools::stats_reading_frames() const {
	return 0;
}

PackedStringArray DevTools::stats_row_ids() const {
	return PackedStringArray();
}

String DevTools::stats_row_average(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

String DevTools::stats_row_peak(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

String DevTools::stats_row_info(const String &p_row_id) const {
	(void)p_row_id;
	return String();
}

#endif

} // namespace godot
