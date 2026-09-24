#include <runtime/devtools/game_dev_tools.h>

#include <runtime/devtools/ai_window.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/demo_window.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/entity_detail_snapshot.h>
#include <runtime/devtools/entity_properties_window.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/physics_window.h>
#include <runtime/devtools/rays_window.h>
#include <runtime/devtools/stats_window.h>
#include <runtime/devtools/weapon_window.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace opennova::devtools {

GameDevTools::GameDevTools() {
	auto game = std::make_unique<GameWindow>(control_board_);
	game_window_ = game.get();
	game->open = true;
	pass_.register_window(std::move(game));
	auto stats = std::make_unique<StatsWindow>();
	stats_window_ = stats.get();
	stats->open = true;
	pass_.register_window(std::move(stats));
	// Closed by default; a world pick or the "Windows" menu opens them. The
	// Properties window reads the Entities window's selection, so the list
	// registers first and outlives it (the pass owns both).
	auto entities = std::make_unique<EntitiesWindow>();
	entities_window_ = entities.get();
	pass_.register_window(std::move(entities));
	auto properties = std::make_unique<EntityPropertiesWindow>(*entities_window_);
	entity_properties_window_ = properties.get();
	pass_.register_window(std::move(properties));
	auto weapon = std::make_unique<WeaponWindow>();
	weapon_window_ = weapon.get();
	pass_.register_window(std::move(weapon));
	auto environment = std::make_unique<EnvironmentWindow>();
	environment_window_ = environment.get();
	pass_.register_window(std::move(environment));
	// The AI window reads the Entities window's selection too (its deep pane
	// rides the same detail push).
	auto ai = std::make_unique<AiWindow>(*entities_window_);
	ai_window_ = ai.get();
	pass_.register_window(std::move(ai));
	auto rays = std::make_unique<RaysWindow>();
	rays_window_ = rays.get();
	pass_.register_window(std::move(rays));
	auto physics = std::make_unique<PhysicsWindow>();
	physics_window_ = physics.get();
	pass_.register_window(std::move(physics));
	pass_.register_window(std::make_unique<DemoWindow>());
}

void GameDevTools::set_game_viewport(GameViewport *viewport) {
	game_window_->set_viewport(viewport);
}

void GameDevTools::set_game_play_available(bool available) {
	game_window_->set_play_available(available);
}

void GameDevTools::set_game_spectator_state(bool available, bool active) {
	game_window_->set_spectator_state(available, active);
}

void GameDevTools::set_game_input_mode(GameInputMode mode) {
	game_window_->set_input_mode(mode);
}

void GameDevTools::reset_game_input_mode() {
	game_window_->reset_input_mode();
}

void GameDevTools::request_game_escape() {
	game_window_->request_escape();
}

bool GameDevTools::take_game_request(GameWindowRequest &request) {
	return game_window_->take_request(request);
}

void GameDevTools::set_frame_stats(FrameStatsBoard *board) {
	stats_window_->set_board(board);
}

void GameDevTools::set_control_catalog(std::vector<ControlSpec> catalog) {
	control_board_.set_catalog(std::move(catalog));
	control_board_.clear_states();
}

void GameDevTools::set_control_states(const std::vector<ControlState> &states) {
	control_board_.set_states(states);
}

bool GameDevTools::needs_control_states() const {
	return pass_.is_open() && control_board_.has_catalog();
}

void GameDevTools::wanted_control_ids(std::vector<const char *> &out) const {
	out.clear();
	if (!pass_.is_open()) return;
	for (int i = 0; i < pass_.window_count(); ++i) {
		const Window &window = pass_.window(i);
		if (window.open) window.wanted_controls(out);
	}
	// One read per row per push, whichever windows share it.
	std::sort(out.begin(), out.end(),
			[](const char *a, const char *b) { return std::strcmp(a, b) < 0; });
	out.erase(std::unique(out.begin(), out.end(),
					  [](const char *a, const char *b) { return std::strcmp(a, b) == 0; }),
			out.end());
}

void GameDevTools::set_game_status(const GameStatusSnapshot &status) {
	game_window_->set_status(status);
}

void GameDevTools::set_entity_directory(EntityDirectorySnapshot snapshot) {
	entities_window_->set_directory(std::move(snapshot));
}

bool GameDevTools::needs_entity_directory() const {
	// The Properties and AI windows read the list's selected row, and the
	// detail card refreshes on the directory's cadence, so the directory keeps
	// flowing while any selection-following window shows (the list holds its
	// selection pending across its own close and re-applies it per push, and
	// its "entity is gone" rule clears a dead selection for all three).
	return pass_.is_open() &&
			(entities_window_->open || entity_properties_window_->open || ai_window_->open);
}

bool GameDevTools::take_control_request(ControlRequest &request) {
	return game_window_->take_control_request(request) ||
			entities_window_->take_request(request) ||
			environment_window_->take_request(request);
}

void GameDevTools::report_control_result(const ControlResult &result) {
	std::string text = result.id;
	text += result.ok ? ": ok" : ": failed";
	if (!result.message.empty()) {
		text += " (";
		text += result.message;
		text += ")";
	}
	if (result.ok && !result.detail.empty()) {
		// The status line carries a short head of a read's payload; the full
		// text belongs to the Log window.
		constexpr size_t kHead = 96;
		text += " = ";
		text += result.detail.size() > kHead ? result.detail.substr(0, kHead) + "..." : result.detail;
	}
	pass_.post_status(std::move(text), result.ok ? StatusLevel::Info : StatusLevel::Error);
}

void GameDevTools::select_entity(uint16_t handle) {
	entities_window_->select_handle(handle);
	if (handle != world::EntityHandle::kInvalid) {
		// The pick is "show me this": the card comes up beside the row.
		entity_properties_window_->open = true;
		entity_properties_window_->request_focus();
	}
}

void GameDevTools::clear_entity_selection() {
	entities_window_->clear_selection();
	entity_properties_window_->clear();
	ai_window_->clear_detail();
}

uint16_t GameDevTools::selected_entity_handle() const {
	return entities_window_->selected_handle();
}

void GameDevTools::set_entity_detail(EntityDetailSnapshot detail) {
	// Both selection-following panes accept the same card (each drops a card
	// that no longer names the selection).
	ai_window_->set_detail(detail);
	entity_properties_window_->set_detail(std::move(detail));
}

bool GameDevTools::needs_entity_detail() const {
	return pass_.is_open() &&
			(entity_properties_window_->open || ai_window_->open) &&
			entities_window_->selected_handle() != world::EntityHandle::kInvalid;
}

void GameDevTools::set_weapon_definition(WeaponDefinitionSnapshot definition) {
	weapon_window_->set_definition(std::move(definition));
}

void GameDevTools::set_weapon_live(WeaponLiveSnapshot live) {
	weapon_window_->set_live(std::move(live));
}

bool GameDevTools::needs_weapon_records() const {
	return pass_.is_open() && weapon_window_->open;
}

bool GameDevTools::take_weapon_request(WeaponRequest &request) {
	return weapon_window_->take_request(request);
}

void GameDevTools::set_environment_snapshot(const EnvironmentSnapshot &snapshot) {
	environment_window_->set_snapshot(snapshot);
}

bool GameDevTools::needs_environment_snapshot() const {
	return pass_.is_open() && environment_window_->open;
}

void GameDevTools::set_ai_debug(AiDebugSnapshot snapshot) {
	ai_window_->set_snapshot(std::move(snapshot));
}

bool GameDevTools::needs_ai_debug() const {
	return pass_.is_open() && ai_window_->open;
}

void GameDevTools::set_rays_snapshot(const RaysSnapshot &snapshot) {
	rays_window_->set_snapshot(snapshot);
}

bool GameDevTools::needs_rays_snapshot() const {
	return pass_.is_open() && rays_window_->open;
}

bool GameDevTools::take_rays_request(RaysRequest &request) {
	return rays_window_->take_request(request);
}

void GameDevTools::set_physics_snapshot(const PhysicsSnapshot &snapshot) {
	physics_window_->set_snapshot(snapshot);
}

bool GameDevTools::needs_physics_snapshot() const {
	return pass_.is_open() && physics_window_->open;
}

bool GameDevTools::take_physics_request(PhysicsRequest &request) {
	return physics_window_->take_request(request);
}

}  // namespace opennova::devtools
