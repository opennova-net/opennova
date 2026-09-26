#include "devtools/dev_tools.h"

#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/sub_viewport.hpp>

#include <base/io/log_ring.h>
#include <base/io/strutil.h> // iequals: binding an ACTION row to its slot by suffix

#if OPENNOVA_DEVTOOLS
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/ai_window.h>
#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/game_status_snapshot.h>
#include <runtime/inmatch/session.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/entity_detail_snapshot.h>
#include <runtime/devtools/entity_directory_snapshot.h>
#include <runtime/devtools/environment_snapshot.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/imgui_abi.h>
#include <runtime/devtools/overlay_canvas.h>
#include <runtime/devtools/physics_request.h>
#include <runtime/devtools/physics_snapshot.h>
#include <runtime/devtools/physics_window.h>
#include <runtime/devtools/rays_request.h>
#include <runtime/devtools/rays_snapshot.h>
#include <runtime/devtools/rays_window.h>
#include <runtime/devtools/stats_window.h>
#include <runtime/devtools/weapon_request.h>
#include <runtime/devtools/weapon_window.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::def;
#endif

namespace godot {

void DevTools::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &DevTools::is_available);
	ClassDB::bind_method(D_METHOD("set_platform_windows_allowed", "allowed"),
			&DevTools::set_platform_windows_allowed);
	ClassDB::bind_method(D_METHOD("are_platform_windows_allowed"),
			&DevTools::are_platform_windows_allowed);
	ClassDB::bind_method(D_METHOD("is_open"), &DevTools::is_open);
	ClassDB::bind_method(D_METHOD("set_open", "open"), &DevTools::set_open);
	ClassDB::bind_method(D_METHOD("toggle"), &DevTools::toggle);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "stats"), &DevTools::set_frame_stats);
	ClassDB::bind_method(D_METHOD("get_frame_stats"), &DevTools::get_frame_stats);
	ClassDB::bind_method(D_METHOD("set_simulation", "simulation"), &DevTools::set_simulation);
	ClassDB::bind_method(D_METHOD("set_debug_control_table", "table"),
			&DevTools::set_debug_control_table);
	ClassDB::bind_method(D_METHOD("get_debug_control_table"), &DevTools::get_debug_control_table);
	ClassDB::bind_method(D_METHOD("select_entity", "handle"), &DevTools::select_entity);
	ClassDB::bind_method(D_METHOD("selected_entity_handle"), &DevTools::selected_entity_handle);
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
	ClassDB::bind_static_method("DevTools", D_METHOD("project_mission_point", "camera", "mission_point"),
			&DevTools::project_mission_point);
	ClassDB::bind_method(D_METHOD("window_titles"), &DevTools::window_titles);
	ClassDB::bind_method(D_METHOD("set_window_open", "title", "open"), &DevTools::set_window_open);
	ClassDB::bind_method(D_METHOD("overlay_names"), &DevTools::overlay_names);
	ClassDB::bind_method(D_METHOD("set_overlay_enabled", "name", "enabled"),
			&DevTools::set_overlay_enabled);
	ClassDB::bind_method(D_METHOD("overlay_last_draw", "name"), &DevTools::overlay_last_draw);
	ClassDB::bind_method(D_METHOD("status_text"), &DevTools::status_text);
	ClassDB::bind_method(D_METHOD("game_status_text"), &DevTools::game_status_text);
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
		texts.append(opennova::to_gd(entry.text));
	}
	Dictionary out;
	out["sequences"] = sequences;
	out["levels"] = levels;
	out["texts"] = texts;
	return out;
}

// Both flavours: the table serves MCP in the release DLL too; only the
// windows that would drain into it are compiled out there. The debug flavour
// hands the tools the table's catalog once (the control board's definitions).
void DevTools::set_debug_control_table(const Ref<DebugControlTable> &p_table) {
	control_table_ = p_table;
#if OPENNOVA_DEVTOOLS
	push_control_catalog();
#endif
}

bool DevTools::is_available() const {
#if OPENNOVA_DEVTOOLS
	return tools_->pass().is_attached();
#else
	return false;
#endif
}

void DevTools::set_platform_windows_allowed(bool p_allowed) {
	platform_windows_allowed_ = p_allowed;
#if OPENNOVA_DEVTOOLS
	if (is_available()) {
		tools_->pass().set_platform_windows_enabled(
				platform_windows_allowed_ && window_allows_platform_windows());
	}
#endif
}

#if OPENNOVA_DEVTOOLS

DevTools::DevTools() : tools_(std::make_unique<opennova::devtools::GameDevTools>()) {
	tools_->set_game_viewport(this);
	// The process ring (installed at extension init, register_types.cpp).
	tools_->set_log_ring(&opennova::io::LogRing::instance());
}

DevTools::~DevTools() = default;

bool DevTools::attach_imgui() {
	Engine *engine = Engine::get_singleton();
	if (engine->is_editor_hint()) {
		return false;
	}
	DisplayServer *display = DisplayServer::get_singleton();
	if (display == nullptr || display->get_name() == "headless") {
		return false;
	}
	if (!engine->has_singleton("ImGuiGD")) {
		UtilityFunctions::push_warning(
				"DevTools: the imgui-godot addon is not loaded (run scripts/bootstrap_imgui_godot.sh, or scripts/bootstrap_godot.sh for a dev checkout); ImGui surfaces unavailable");
		return false;
	}
	Object *imgui = engine->get_singleton("ImGuiGD");
	const opennova::devtools::ImGuiAbi abi = opennova::devtools::imgui_abi();
	const Variant pointers = imgui->call("GetImGuiPtrs", String::utf8(abi.version), abi.io_size,
			abi.vert_size, abi.idx_size, abi.wchar_size);
	if (pointers.get_type() != Variant::PACKED_INT64_ARRAY) {
		UtilityFunctions::push_warning("DevTools: ImGuiGD.GetImGuiPtrs returned no pointer table; ImGui surfaces unavailable");
		return false;
	}
	const PackedInt64Array table = pointers;
	if (table.size() != 3 || table[0] == 0) {
		// The addon printed the version/size mismatch itself.
		UtilityFunctions::push_warning(vformat(
				"DevTools: the imgui-godot addon rejected the engine's ImGui %s (context hand-off refused); ImGui surfaces unavailable",
				abi.version));
		return false;
	}
	return tools_->pass().attach_imgui(reinterpret_cast<void *>(static_cast<intptr_t>(table[0])),
			reinterpret_cast<opennova::devtools::ImGuiAllocFn>(static_cast<intptr_t>(table[1])),
			reinterpret_cast<opennova::devtools::ImGuiFreeFn>(static_cast<intptr_t>(table[2])), nullptr);
}

bool DevTools::window_allows_platform_windows() const {
	const Window *window = get_window();
	if (window == nullptr) {
		return true;
	}
	const Window::Mode mode = window->get_mode();
	return mode != Window::MODE_FULLSCREEN && mode != Window::MODE_EXCLUSIVE_FULLSCREEN;
}

void DevTools::set_layer_visible(bool p_visible) {
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SetVisible", p_visible);
	}
}

void DevTools::sync_layer_visible() {
	const bool visible = is_available() && tools_->pass().is_open();
	if (visible != layer_visible_) {
		layer_visible_ = visible;
		set_layer_visible(visible);
	}
}

void DevTools::_ready() {
	if (!attach_imgui()) {
		set_process(false);
		return;
	}
	// Clear multi-viewport before the first NewFrame of a fullscreen launch.
	tools_->pass().set_platform_windows_enabled(
			platform_windows_allowed_ && window_allows_platform_windows());
	// Layout follows the addon's NewFrame and all game callbacks, immediately
	// before the addon's render pass at the highest process priority.
	set_process_priority(INT_MAX - 1);
	set_process_mode(PROCESS_MODE_ALWAYS);
	set_process(true);
	sync_layer_visible();
}

void DevTools::_exit_tree() {
	set_game_play_available(false);
	set_game_playing_internal(false);
	game_viewport_ = nullptr;
	rendered_game_viewport_size_ = Vector2i();
	tools_->set_game_viewport(nullptr);
	tools_->reset_game_input_mode();
	// Close the pass BEFORE dropping the Simulation: the windows' hide edges
	// queue their teardown (the Weapon window releases its hold and, with REC
	// off, its ring) and that drain needs a world to reach.
	tools_->pass().set_open(false);
	apply_weapon_requests();
	set_simulation(nullptr);
	control_table_.unref();
	tools_->set_frame_stats(nullptr);
	if (frame_stats_.is_valid()) {
		frame_stats_->sync_capture_signal();
	}
	if (layer_visible_) {
		layer_visible_ = false;
		set_layer_visible(false);
	}
	tools_->pass().detach_imgui();
	set_process(false);
}

void DevTools::_process(double p_delta) {
	if (!is_available()) {
		return;
	}
	// The display frame the status readout averages (every frame, so the
	// first push after an open reads the real interval).
	frame_ms_sum_ += p_delta * 1000.0;
	frame_ms_peak_ = std::max(frame_ms_peak_, p_delta * 1000.0);
	++frame_ms_count_;
	auto &pass = tools_->pass();
	pass.set_platform_windows_enabled(platform_windows_allowed_ && window_allows_platform_windows());
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	// The F3 row times the tools' whole cost: the overlay feed, the layout
	// pass, the request drains and the record pushes. The overlay records go
	// in first so a layer draws the tick the image shows.
	const int64_t start = Time::get_singleton()->get_ticks_usec();
	push_overlay_frame();
	const bool drew = pass.draw_frame(frame);
	apply_game_requests();
	sync_game_spectator_state();
	apply_control_requests();
	apply_weapon_requests();
	apply_rays_requests();
	apply_physics_requests();
	push_control_states();
	push_game_status();
	push_entity_detail(push_entity_directory());
	push_weapon_records();
	push_environment_snapshot();
	push_ai_debug();
	push_rays_snapshot();
	push_physics_snapshot();
	push_domain_records();
	const int64_t tools_us = Time::get_singleton()->get_ticks_usec() - start;
	if (drew && frame_stats_.is_valid() && frame_stats_->is_capture_active()) {
		frame_stats_->add(FrameStats::FRAME_DEBUG_REFRESH, tools_us);
	}
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
	sync_layer_visible();
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
		}
	}
}

void DevTools::sync_game_spectator_state() {
	Simulation *sim = simulation();
	const bool available = sim != nullptr && !sim->is_joiner() && sim->has_local_player();
	tools_->set_game_spectator_state(
			available, sim != nullptr && sim->is_local_spectator());
}

bool DevTools::draw(int p_requested_width, int p_requested_height) {
	if (game_viewport_ == nullptr) {
		return false;
	}
	const Vector2i requested(std::max(1, p_requested_width), std::max(1, p_requested_height));
	if (game_viewport_->get_size() != requested) {
		game_viewport_->set_size(requested);
	}
	rendered_game_viewport_size_ = requested;
	Engine *engine = Engine::get_singleton();
	if (!engine->has_singleton("ImGuiGD")) {
		return false;
	}
	// The addon draws the image at the cursor at the viewport's size, then an
	// invisible button over it: the window's last item is the image rect.
	engine->get_singleton("ImGuiGD")->call("SubViewport", game_viewport_);
	return true;
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

void DevTools::set_simulation(const Ref<Simulation> &p_simulation) {
	const ObjectID id = p_simulation.is_valid() ? ObjectID(p_simulation->get_instance_id()) : ObjectID();
	if (simulation_id_ == id) {
		return;
	}
	// The outgoing world takes nothing of the Weapon window's with it: the
	// hold latch and the trace ring are released on the Simulation being
	// dropped, whatever the window's own state.
	if (Simulation *outgoing = simulation(); outgoing != nullptr && outgoing != p_simulation.ptr()) {
		outgoing->debug_weapon_set_fire_held(false);
		outgoing->debug_weapon_arm_trace(false);
	}
	simulation_id_ = id;
	last_entity_push_ms_ = -1;
	last_detail_handle_ = -1;
	weapon_trace_primed_ = false;
	weapon_def_dirty_ = true;
	weapon_def_name_.clear();
	weapon_records_live_ = false;
	last_environment_push_ms_ = -1;
	last_ai_push_ms_ = -1;
	last_rays_push_ms_ = -1;
	rays_recording_ = false; // a fresh world starts with the capture off
	last_physics_push_ms_ = -1;
	contacts_recording_ = false; // likewise the contact capture
	// A new world's first tick re-pushes every overlay record.
	overlay_tick_ = static_cast<uint64_t>(-1);
	overlay_wants_ = 0;
	last_hitbox_push_ms_ = -1;
	if (overlay_live_) {
		tools_->clear_overlay_records();
	}
	// A packed handle names a slot, not an entity: the selection never crosses
	// from one world to the next.
	tools_->clear_entity_selection();
	if (p_simulation.is_null()) {
		// The unload edge: invalid records clear the pushed state so a window
		// left open never shows a dead world's rows or card.
		tools_->set_entity_directory(opennova::devtools::EntityDirectorySnapshot{});
		tools_->set_entity_detail(opennova::devtools::EntityDetailSnapshot{});
		tools_->set_environment_snapshot(opennova::devtools::EnvironmentSnapshot{});
		tools_->set_ai_debug(opennova::devtools::AiDebugSnapshot{});
		tools_->set_rays_snapshot(opennova::devtools::RaysSnapshot{});
		tools_->set_physics_snapshot(opennova::devtools::PhysicsSnapshot{});
		clear_domain_records();
	}
	sync_game_spectator_state();
}

void DevTools::select_entity(int p_handle) {
	if (p_handle < 0 || p_handle >= static_cast<int>(opennova::world::EntityHandle::kInvalid)) {
		tools_->clear_entity_selection();
		return;
	}
	tools_->select_entity(static_cast<uint16_t>(p_handle));
	// The next frame pushes the directory (and then the detail card) at once
	// rather than waiting out the cadence.
	last_entity_push_ms_ = -1;
}

int DevTools::selected_entity_handle() const {
	const uint16_t handle = tools_->selected_entity_handle();
	return handle == opennova::world::EntityHandle::kInvalid ? -1 : static_cast<int>(handle);
}

bool DevTools::push_due(int64_t &r_last_ms, double p_seconds) {
	const int64_t now_ms = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
	if (r_last_ms >= 0 && now_ms - r_last_ms < static_cast<int64_t>(p_seconds * 1000.0)) {
		return false;
	}
	r_last_ms = now_ms;
	return true;
}

namespace {

// The table's row kinds, in the control board's order.
static_assert(static_cast<int>(opennova::devtools::ControlKind::Check) == DebugControlRow::CHECK,
		"ControlKind::Check");
static_assert(static_cast<int>(opennova::devtools::ControlKind::Slider) == DebugControlRow::SLIDER,
		"ControlKind::Slider");
static_assert(static_cast<int>(opennova::devtools::ControlKind::Enum) == DebugControlRow::ENUM,
		"ControlKind::Enum");
static_assert(static_cast<int>(opennova::devtools::ControlKind::Action) == DebugControlRow::ACTION,
		"ControlKind::Action");
// The status readout mirrors the session's role and state by value.
static_assert(static_cast<int>(opennova::devtools::StatusRole::ListenServer) ==
				static_cast<int>(opennova::inmatch::RoleKind::ListenHost),
		"StatusRole::ListenServer");
static_assert(static_cast<int>(opennova::devtools::StatusRole::DedicatedServer) ==
				static_cast<int>(opennova::inmatch::RoleKind::DedicatedHost),
		"StatusRole::DedicatedServer");
static_assert(static_cast<int>(opennova::devtools::StatusState::Failed) ==
				static_cast<int>(opennova::inmatch::State::Failed),
		"StatusState::Failed");

// A row value as the board's typed argument (Check = Bool, Slider = Float,
// Enum = Int); false for a missing value.
bool control_value_from_variant(const Variant &p_value, opennova::devtools::ControlArg &r_arg) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			r_arg = opennova::devtools::ControlArg::boolean(p_value);
			return true;
		case Variant::INT:
			r_arg = opennova::devtools::ControlArg::integer(p_value);
			return true;
		case Variant::FLOAT:
			r_arg = opennova::devtools::ControlArg::number(p_value);
			return true;
		default:
			return false;
	}
}

} // namespace

// The table's catalog as the control board's definitions: pushed once when
// the table is lent (its rows never change after setup).
void DevTools::push_control_catalog() {
	std::vector<opennova::devtools::ControlSpec> catalog;
	if (control_table_.is_valid()) {
		const TypedArray<DebugControlRow> rows = control_table_->list_controls();
		catalog.reserve(static_cast<size_t>(rows.size()));
		for (int64_t i = 0; i < rows.size(); ++i) {
			const Ref<DebugControlRow> row = rows[i];
			if (row.is_null()) continue;
			opennova::devtools::ControlSpec spec;
			spec.id = opennova::to_std(String(row->get_id()));
			spec.label = opennova::to_std(row->get_label());
			spec.tooltip = opennova::to_std(row->get_tooltip());
			spec.kind = static_cast<opennova::devtools::ControlKind>(row->get_kind());
			spec.minimum = row->get_minimum();
			spec.maximum = row->get_maximum();
			spec.step = row->get_step();
			const PackedStringArray choices = row->get_choices();
			for (int64_t c = 0; c < choices.size(); ++c) {
				spec.choices.push_back(opennova::to_std(choices[c]));
			}
			catalog.push_back(std::move(spec));
		}
	}
	tools_->set_control_catalog(std::move(catalog));
	last_control_state_push_ms_ = -1;
}

// The live states of the rows the visible windows read, on the board's
// cadence (and at once after a command, so a toggle confirms promptly).
void DevTools::push_control_states() {
	if (control_table_.is_null() || !tools_->needs_control_states()) {
		last_control_state_push_ms_ = -1;
		return;
	}
	std::vector<const char *> ids;
	tools_->wanted_control_ids(ids);
	// A window that just opened reads its rows this frame, not a cadence later.
	const bool ids_changed = !std::equal(ids.begin(), ids.end(), control_ids_.begin(), control_ids_.end(),
			[](const char *a, const char *b) { return std::strcmp(a, b) == 0; });
	if (ids_changed) {
		last_control_state_push_ms_ = -1;
		control_ids_ = ids;
	}
	if (!push_due(last_control_state_push_ms_, opennova::devtools::GameDevTools::kControlStateSeconds)) {
		return;
	}
	std::vector<opennova::devtools::ControlState> states;
	states.reserve(ids.size());
	for (const char *id : ids) {
		const Ref<DebugControlState> state = control_table_->get_state(StringName(id), true);
		opennova::devtools::ControlState out;
		out.id = id;
		if (state.is_valid()) {
			out.writable = state->is_writable();
			out.has_value = control_value_from_variant(state->get_value(), out.value);
			out.reason = opennova::to_std(state->get_reason());
		}
		states.push_back(std::move(out));
	}
	tools_->set_control_states(states);
}

// The Game window's readout: the session under the world, its logic clock
// and the display frame, on the board's cadence while the tools are open.
void DevTools::push_game_status() {
	if (!tools_->needs_game_status()) {
		last_status_push_ms_ = -1;
		return;
	}
	if (!push_due(last_status_push_ms_, opennova::devtools::GameDevTools::kControlStateSeconds)) {
		return;
	}
	opennova::devtools::GameStatusSnapshot status;
	status.fps = Engine::get_singleton()->get_frames_per_second();
	status.frame_ms = frame_ms_count_ > 0 ? frame_ms_sum_ / static_cast<double>(frame_ms_count_) : 0.0;
	status.frame_ms_peak = frame_ms_peak_;
	frame_ms_sum_ = 0.0;
	frame_ms_peak_ = 0.0;
	frame_ms_count_ = 0;
	if (Simulation *sim = simulation(); sim != nullptr) {
		status.world = true;
		status.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());
		status.role = static_cast<opennova::devtools::StatusRole>(sim->session_role());
		status.state = static_cast<opennova::devtools::StatusState>(sim->session_state());
		status.transport_locked = sim->is_transport_locked();
		status.peers = sim->get_host_peer_count();
		status.playing = status.state == opennova::devtools::StatusState::Running;
	}
	tools_->set_game_status(status);
}

namespace {

// One window argument as the Variant the table marshals against the row's
// schema (the same kinds an MCP caller sends).
Variant control_arg_to_variant(const opennova::devtools::ControlArg &p_arg) {
	using Kind = opennova::devtools::ControlArg::Kind;
	switch (p_arg.kind) {
		case Kind::Int:
			return Variant(p_arg.i);
		case Kind::Float:
			return Variant(p_arg.f);
		case Kind::Bool:
			return Variant(p_arg.b);
		case Kind::Text:
			return Variant(opennova::to_gd(p_arg.text));
		case Kind::Vec3:
			return Variant(Vector3(p_arg.v[0], p_arg.v[1], p_arg.v[2]));
	}
	return Variant();
}

} // namespace

// Drain the F3 windows' control requests into the ONE debug-control table
// MCP's game_debug drives too (ADR 0043 d12): the row's schema validates the
// arguments and its session-role gate refuses a joiner, once for both
// surfaces. F3 is the local operator, so it carries the per-call
// confirmation. Every verdict is reported back to the tools (the status line,
// the Log window), so a refused or failed command is never silent.
void DevTools::apply_control_requests() {
	opennova::devtools::ControlRequest request;
	bool drained = false;
	while (tools_->take_control_request(request)) {
		opennova::devtools::ControlResult result;
		result.id = request.id;
		if (control_table_.is_null()) {
			result.message = "no debug-control table";
			tools_->report_control_result(result);
			continue;
		}
		Array args;
		for (const opennova::devtools::ControlArg &arg : request.args) {
			args.push_back(control_arg_to_variant(arg));
		}
		const Ref<DebugInvokeResult> outcome =
				control_table_->invoke(StringName(request.id), args, true);
		const Error error = outcome->get_error();
		result.ok = error == OK;
		if (!result.ok) {
			String message = UtilityFunctions::error_string(error);
			const Ref<DebugControlState> state = outcome->get_state();
			if (state.is_valid() && !state->get_reason().is_empty()) {
				message += ": " + state->get_reason();
			}
			result.message = opennova::to_std(message);
		} else if (outcome->get_result().get_type() != Variant::NIL) {
			// A read's payload in full (the Log window keeps it); capped so a
			// huge dictionary cannot flood the ring.
			constexpr int64_t kDetailCap = 4096;
			const String text = outcome->get_result().stringify();
			result.detail = opennova::to_std(text.length() > kDetailCap ? text.substr(0, kDetailCap) : text);
		}
		tools_->report_control_result(result);
		drained = drained || result.ok;
	}
	if (drained) {
		// The records pushed this same frame show the mutation, not the
		// reading from up to a cadence ago.
		last_entity_push_ms_ = -1;
		last_environment_push_ms_ = -1;
		last_ai_push_ms_ = -1;
		last_control_state_push_ms_ = -1;
	}
}

// Push the entity-directory record while the Entities window shows, on its
// 0.5 s cadence: the ENGINE join (world::inspect::entity_directory) through
// the Simulation's native accessor — no TypedArray/Variant round-trip
// (ADR 0042 d6).
bool DevTools::push_entity_directory() {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr || !tools_->needs_entity_directory()) {
		last_entity_push_ms_ = -1;
		return false;
	}
	if (!push_due(last_entity_push_ms_, opennova::devtools::EntitiesWindow::kRefreshSeconds)) {
		return false;
	}
	opennova::devtools::EntityDirectorySnapshot snapshot;
	snapshot.rows = simulation_->native_entity_directory();
	snapshot.valid = true;
	snapshot.logic_tick = static_cast<uint64_t>(simulation_->get_logic_tick());
	// The session-role fact the debug-control table reads too (ADR 0042 d5):
	// the joiner is the one non-authoritative role.
	snapshot.authority = simulation_->session_role() != Simulation::ROLE_JOINER;
	snapshot.session_live = simulation_->is_host_listening();
	tools_->set_entity_directory(std::move(snapshot));
	return true;
}

// Push the selected row's detail record (the ENGINE card,
// world::inspect::build_entity_card, through the Simulation's native accessor)
// while the window shows a selection: on every directory push, and at once
// when the selection moved since the last detail push, so a row click never
// shows a stale or empty pane for a cadence.
void DevTools::push_entity_detail(bool p_directory_pushed) {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr || !tools_->needs_entity_detail()) {
		last_detail_handle_ = -1;
		return;
	}
	const int handle = static_cast<int>(tools_->selected_entity_handle());
	if (!p_directory_pushed && handle == last_detail_handle_) {
		return;
	}
	last_detail_handle_ = handle;
	opennova::devtools::EntityDetailSnapshot detail;
	detail.card = simulation_->native_entity_card(handle);
	detail.logic_tick = static_cast<uint64_t>(simulation_->get_logic_tick());
	tools_->set_entity_detail(std::move(detail));
}

// Drain the Weapon window's typed edits and triggers into the engine's own
// weapon seams (ADR 0042 d6). Requests queued with no world behind them drain
// and drop. Nothing here writes a file: the window's edits are live only.
void DevTools::apply_weapon_requests() {
	using Request = opennova::devtools::WeaponRequest;
	// The window passes its enums as plain ints so simulation.h carries no
	// devtools include; pin the pairing here, where both are visible.
	static_assert(static_cast<int>(Request::TextField::Anim) == 0, "TextField::Anim");
	static_assert(static_cast<int>(Request::TextField::SoundSet) == 1, "TextField::SoundSet");
	static_assert(static_cast<int>(Request::TextField::SoundSetEnd) == 2, "TextField::SoundSetEnd");
	static_assert(static_cast<int>(Request::TextField::Particle) == 3, "TextField::Particle");
	static_assert(static_cast<int>(Request::TextField::ParticleUserPoint) == 4,
			"TextField::ParticleUserPoint");
	static_assert(static_cast<int>(Request::Trigger::Fire) == 0, "Trigger::Fire");
	static_assert(static_cast<int>(Request::Trigger::Reload) == 1, "Trigger::Reload");
	static_assert(static_cast<int>(Request::Trigger::ScopeToggle) == 2, "Trigger::ScopeToggle");
	static_assert(static_cast<int>(Request::Trigger::NextWeapon) == 3, "Trigger::NextWeapon");
	static_assert(static_cast<int>(Request::Trigger::PrevWeapon) == 4, "Trigger::PrevWeapon");

	Request request;
	Simulation *simulation_ = simulation();
	while (tools_->take_weapon_request(request)) {
		if (simulation_ == nullptr) {
			continue;
		}
		switch (request.kind) {
			case Request::Kind::SetActionDelays:
				(void)simulation_->debug_weapon_set_action_delays(request.action_id,
						request.delay_start, request.delay_end, request.rebake);
				weapon_def_dirty_ = true;
				break;
			case Request::Kind::SetActionText:
				(void)simulation_->debug_weapon_set_action_text(request.action_id,
						static_cast<int>(request.field), String::utf8(request.text));
				weapon_def_dirty_ = true;
				break;
			case Request::Kind::TriggerAction:
				(void)simulation_->debug_weapon_trigger(static_cast<int>(request.trigger));
				break;
			case Request::Kind::SetFireHeld:
				simulation_->debug_weapon_set_fire_held(request.held);
				break;
			case Request::Kind::ArmTrace:
				simulation_->debug_weapon_arm_trace(request.armed);
				// Disarming releases the ring, so the cursor restarts with the
				// next arm; a hold cannot outlive the recording that shows it.
				if (!request.armed) {
					simulation_->debug_weapon_set_fire_held(false);
					weapon_trace_primed_ = false;
				}
				break;
			case Request::Kind::ClearTrace:
				simulation_->debug_weapon_clear_trace();
				weapon_trace_primed_ = false;
				break;
		}
	}
}

// Push the Weapon window's records while it shows: the definition only when
// something moved it (an applied request, a different weapon, the clip rings
// resolving), the live state EVERY frame — the trace pane is a scope on a
// 62.5 Hz signal, so the Entities window's 0.5 s cadence would alias it away.
// The trace itself is drained incrementally from the pump's ring.
void DevTools::push_weapon_records() {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr) {
		// The unload edge: an invalid definition clears a window left open,
		// and the trace cursor restarts with the next world.
		if (weapon_records_live_) {
			tools_->set_weapon_definition(opennova::devtools::WeaponDefinitionSnapshot{});
			weapon_records_live_ = false;
		}
		weapon_trace_primed_ = false;
		weapon_def_dirty_ = true;
		weapon_def_name_.clear();
		return;
	}
	// Hidden: no record is built, but the trace cursor is kept so the reopen
	// drains exactly what the ring recorded meanwhile (REC keeps it armed).
	if (!tools_->needs_weapon_records()) return;
	const opennova::world::LocalPlayerWeapon *weapon =
			simulation_->native_local_player_weapon();
	if (weapon == nullptr) {
		if (weapon_records_live_) {
			tools_->set_weapon_definition(opennova::devtools::WeaponDefinitionSnapshot{});
			weapon_records_live_ = false;
		}
		weapon_trace_primed_ = false;
		weapon_def_dirty_ = true;
		weapon_def_name_.clear();
		return;
	}
	weapon_records_live_ = true;

	// --- the definition, on change ---
	const size_t rings = weapon->clip_rings.size();
	if (weapon_def_dirty_ || weapon_def_name_ != weapon->def_name || weapon_def_rings_ != rings) {
		weapon_def_dirty_ = false;
		weapon_def_name_ = weapon->def_name;
		weapon_def_rings_ = rings;
		opennova::devtools::WeaponDefinitionSnapshot def;
		def.valid = true;
		def.serial = ++weapon_def_serial_;
		def.weapon_name = weapon->def_name;
		def.adm_index = simulation_->native_equipped_weapon_adm_index();
		def.clip_capacity = weapon->def.clip_capacity;
		def.auto_fire = weapon->def.auto_fire;
		def.burst3 = weapon->def.burst3;
		def.clip_keys = simulation_->native_equipped_weapon_clip_keys();
		const DefWeaponDef *row = simulation_->native_equipped_weapon_row();
		for (int id = 0; id < opennova::world::weapon_action::kCount; ++id) {
			const opennova::world::WeaponFsmAction &baked = weapon->def.actions[id];
			opennova::devtools::WeaponActionRow &out = def.actions[id];
			out.delay_start = baked.delay_start;
			out.delay_end = baked.delay_end;
			out.has_anim = baked.has_anim;
			out.anim_key = baked.anim_key;
			out.soundset = baked.soundset;
			out.soundsetend = baked.soundsetend;
			out.particle = baked.particle;
			out.particle_userpoint = baked.particle_userpoint;
			// The authored row behind the slot, matched the way the bake binds it.
			if (row != nullptr) {
				const char *suffix = opennova::world::kWeaponActionSuffixes[id];
				for (size_t i = 0; i < row->actions_count; ++i) {
					const DefWeaponAction &authored = row->actions[i];
					if (!opennova::strutil::iequals(authored.name, suffix)) continue;
					out.authored = true;
					out.authored_name = authored.name;
					out.function = authored.function;
					out.authored_delay_start = authored.delaystart;
					out.authored_delay_end = authored.delayend;
					break;
				}
			}
			// The clip this row resolves to, in ticks — what an `auto` delay
			// bakes from. Read WITHOUT advancing the ring: the bake's own reads
			// are consuming, and a push must not rotate the variant order.
			if (baked.anim_key[0] != '\0') {
				std::string key = baked.anim_key;
				for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				for (const auto &ring : weapon->clip_rings) {
					if (ring.first != key || ring.second.lengths.empty()) continue;
					const size_t head = static_cast<size_t>(ring.second.head) %
							ring.second.lengths.size();
					out.clip_ticks = opennova::world::weapon_anim_ticks_from_ms(
							static_cast<int32_t>(ring.second.lengths[head] * 1000.0f));
					break;
				}
			}
		}
		tools_->set_weapon_definition(std::move(def));
	}

	// --- the live state, every frame ---
	opennova::devtools::WeaponLiveSnapshot live;
	live.valid = true;
	live.logic_tick = static_cast<uint64_t>(simulation_->get_logic_tick());
	// The ACTIVE slot: the borrowed UseGun parent slot when one is engaged,
	// which is what the pump runs and the trace records.
	const opennova::world::WeaponSlotState *active = simulation_->native_active_weapon_slot();
	const opennova::world::WeaponSlotState &slot = active != nullptr ? *active : weapon->slot;
	live.current = slot.current;
	live.next = slot.next;
	live.prev = slot.prev;
	live.phase = slot.phase;
	live.counter = slot.counter;
	live.clip = slot.clip;
	live.reserve = slot.reserve;
	live.heat = opennova::world::weapon_slot_accumulated_heat(
			weapon->def, slot, static_cast<int32_t>(live.logic_tick));
	// The REAL input gates decide; the strings only NAME which leg refused,
	// so the window can say so instead of greying a button silently.
	live.fire_block = simulation_->native_weapon_input_block();
	if (!opennova::world::weapon_fsm_reload_allowed(weapon->def, slot)) {
		if (weapon->def.clip_capacity <= 0) {
			live.reload_block = "the def authors no clipsize, so there is no magazine to reload";
		} else if (slot.clip == weapon->def.clip_capacity) {
			live.reload_block = "the magazine is already full";
		} else {
			live.reload_block = "the reserve is empty";
		}
	}
	if (!opennova::world::weapon_fsm_scope_toggle_allowed(weapon->def, slot)) {
		if (slot.current == opennova::world::weapon_action::kReload ||
				slot.current == opennova::world::weapon_action::kSwitchFrom) {
			live.scope_block = "a reload or holster is running";
		} else {
			live.scope_block = "the def is not Scoped or Sighted (flags & 3)";
		}
	}
	live.fire_held = simulation_->debug_weapon_fire_held();
	live.trace_armed = weapon->trace_armed;

	// The trace delta: only samples the window has not seen, walked back from
	// the ring's write head. A newest tick below the cursor is a restarted
	// logic clock (a round restart), so the cursor re-primes and the window
	// takes the whole ring again.
	const uint32_t newest = opennova::world::weapon_trace_samples_since(
			*weapon, last_weapon_trace_tick_, !weapon_trace_primed_, live.trace);
	if (weapon_trace_primed_ && newest != 0 && newest < last_weapon_trace_tick_) {
		live.trace.clear();
		opennova::world::weapon_trace_samples_since(*weapon, 0, true, live.trace);
	}
	if (newest != 0) {
		last_weapon_trace_tick_ = newest;
		weapon_trace_primed_ = true;
	}
	tools_->set_weapon_live(std::move(live));
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
	if (!push_due(last_environment_push_ms_, opennova::devtools::EnvironmentWindow::kRefreshSeconds)) {
		return;
	}
	opennova::devtools::EnvironmentSnapshot snapshot;
	simulation_->native_environment_snapshot(snapshot);
	tools_->set_environment_snapshot(snapshot);
}

// Push the AI debug record while the AI window shows, on its 0.5 s cadence:
// the ENGINE join (world::inspect::ai_debug_report) through the Simulation's
// native accessor (ADR 0042 d6).
void DevTools::push_ai_debug() {
	Simulation *simulation_ = simulation();
	if (simulation_ == nullptr || !tools_->needs_ai_debug()) {
		last_ai_push_ms_ = -1;
		return;
	}
	if (!push_due(last_ai_push_ms_, opennova::devtools::AiWindow::kRefreshSeconds)) {
		return;
	}
	opennova::devtools::AiDebugSnapshot snapshot;
	snapshot.valid = simulation_->native_ai_debug(snapshot.report);
	snapshot.logic_tick = static_cast<uint64_t>(simulation_->get_logic_tick());
	tools_->set_ai_debug(std::move(snapshot));
}

// Drain the Rays window's typed requests: the filter/TTL/clear land on the
// Simulation's ray-debug seam.
void DevTools::apply_rays_requests() {
	opennova::devtools::RaysRequest request;
	Simulation *simulation_ = simulation();
	while (tools_->take_rays_request(request)) {
		using Kind = opennova::devtools::RaysRequest::Kind;
		if (simulation_ == nullptr) {
			continue;
		}
		switch (request.kind) {
			case Kind::SetCategoryMask:
				simulation_->set_ray_debug_filter(request.a, -1);
				break;
			case Kind::SetTtlTicks:
				simulation_->set_ray_debug_filter(-1, request.a);
				break;
			case Kind::Clear:
				simulation_->clear_ray_debug();
				break;
		}
		overlay_filters_dirty_ = true;
	}
}

// Push the ray-capture record while the Rays window shows, on its 0.25 s
// cadence: counts + filter state through Simulation::native_rays_snapshot —
// no Variant round-trip (ADR 0042 d6). Recording follows the window, so the
// capture costs nothing while it is hidden.
void DevTools::push_rays_snapshot() {
	Simulation *simulation_ = simulation();
	const bool shown = simulation_ != nullptr && tools_->needs_rays_snapshot();
	if (simulation_ != nullptr && shown != rays_recording_) {
		simulation_->set_ray_debug_recording(shown);
		rays_recording_ = shown;
	}
	if (!shown) {
		last_rays_push_ms_ = -1;
		return;
	}
	if (!push_due(last_rays_push_ms_, opennova::devtools::RaysWindow::kRefreshSeconds)) {
		return;
	}
	opennova::devtools::RaysSnapshot snapshot;
	simulation_->native_rays_snapshot(snapshot);
	tools_->set_rays_snapshot(snapshot);
}

// Drain the Physics window's typed requests: the mask/clear legs land in the
// Simulation contact-debug seam.
void DevTools::apply_physics_requests() {
	opennova::devtools::PhysicsRequest request;
	Simulation *simulation_ = simulation();
	while (tools_->take_physics_request(request)) {
		using Kind = opennova::devtools::PhysicsRequest::Kind;
		if (simulation_ == nullptr) {
			continue;
		}
		switch (request.kind) {
			case Kind::SetKindMask:
				simulation_->set_contact_debug_kind_mask(request.a);
				break;
			case Kind::Clear:
				simulation_->clear_contact_debug();
				break;
		}
		overlay_filters_dirty_ = true;
	}
}

// Push the contact-capture record while the Physics window shows, on its
// 0.25 s cadence: counts + capture state through
// Simulation::native_physics_snapshot — no Variant round-trip (ADR 0042 d6).
// The capture follows the window (the rays rule), so it costs nothing while
// it is hidden.
void DevTools::push_physics_snapshot() {
	Simulation *simulation_ = simulation();
	const bool shown = simulation_ != nullptr && tools_->needs_physics_snapshot();
	if (simulation_ != nullptr && shown != contacts_recording_) {
		simulation_->set_contact_debug_capture(shown);
		contacts_recording_ = shown;
	}
	if (!shown) {
		last_physics_push_ms_ = -1;
		return;
	}
	if (!push_due(last_physics_push_ms_, opennova::devtools::PhysicsWindow::kRefreshSeconds)) {
		return;
	}
	opennova::devtools::PhysicsSnapshot snapshot;
	simulation_->native_physics_snapshot(snapshot);
	tools_->set_physics_snapshot(snapshot);
}

void DevTools::reset_layout() {
	tools_->pass().request_layout_reset();
}

namespace {

String overlay_name(const opennova::devtools::OverlayLayer &p_layer) {
	return String::utf8(p_layer.group()) + "/" + String::utf8(p_layer.label());
}

} // namespace

PackedStringArray DevTools::window_titles() const {
	PackedStringArray out;
	const opennova::devtools::ImGuiPass &pass = tools_->pass();
	for (int i = 0; i < pass.window_count(); ++i) {
		out.push_back(String::utf8(pass.window(i).title()));
	}
	return out;
}

bool DevTools::set_window_open(const String &p_title, bool p_open) {
	opennova::devtools::ImGuiPass &pass = tools_->pass();
	const CharString title = p_title.utf8();
	for (int i = 0; i < pass.window_count(); ++i) {
		opennova::devtools::Window &window = pass.window(i);
		if (std::strcmp(window.title(), title.get_data()) != 0) continue;
		if (!window.is_closeable() && !p_open) return false;
		window.open = p_open;
		return true;
	}
	return false;
}

PackedStringArray DevTools::overlay_names() const {
	PackedStringArray out;
	const opennova::devtools::ImGuiPass &pass = tools_->pass();
	for (int i = 0; i < pass.overlay_count(); ++i) {
		out.push_back(overlay_name(pass.overlay(i)));
	}
	return out;
}

bool DevTools::set_overlay_enabled(const String &p_name, bool p_enabled) {
	opennova::devtools::ImGuiPass &pass = tools_->pass();
	for (int i = 0; i < pass.overlay_count(); ++i) {
		if (overlay_name(pass.overlay(i)) != p_name) continue;
		pass.set_overlay_enabled(pass.overlay(i), p_enabled);
		return true;
	}
	return false;
}

Vector3i DevTools::overlay_last_draw(const String &p_name) const {
	const opennova::devtools::ImGuiPass &pass = tools_->pass();
	for (int i = 0; i < pass.overlay_count(); ++i) {
		if (overlay_name(pass.overlay(i)) != p_name) continue;
		const opennova::devtools::OverlayLayerStats &stats = pass.overlay(i).last_stats();
		return Vector3i(stats.lines, stats.texts, stats.dropped);
	}
	return Vector3i(-1, -1, -1);
}

String DevTools::status_text() const {
	return String::utf8(tools_->pass().status_text());
}

String DevTools::game_status_text() const {
	return opennova::to_gd(tools_->game_window().status_text());
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

void DevTools::_ready() {
	set_process(false);
}

void DevTools::_exit_tree() {
	control_table_.unref();
	set_process(false);
}

void DevTools::_process(double p_delta) {
	(void)p_delta;
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

void DevTools::set_simulation(const Ref<Simulation> &p_simulation) {
	(void)p_simulation;
}

void DevTools::select_entity(int p_handle) {
	(void)p_handle;
}

int DevTools::selected_entity_handle() const {
	return -1;
}

void DevTools::reset_layout() {}

PackedStringArray DevTools::window_titles() const {
	return PackedStringArray();
}

bool DevTools::set_window_open(const String &p_title, bool p_open) {
	(void)p_title;
	(void)p_open;
	return false;
}

PackedStringArray DevTools::overlay_names() const {
	return PackedStringArray();
}

bool DevTools::set_overlay_enabled(const String &p_name, bool p_enabled) {
	(void)p_name;
	(void)p_enabled;
	return false;
}

Vector3i DevTools::overlay_last_draw(const String &p_name) const {
	(void)p_name;
	return Vector3i(-1, -1, -1);
}

String DevTools::status_text() const {
	return String();
}

String DevTools::game_status_text() const {
	return String();
}

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
