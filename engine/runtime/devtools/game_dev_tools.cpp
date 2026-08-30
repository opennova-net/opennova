#include <runtime/devtools/game_dev_tools.h>

#include <runtime/devtools/demo_window.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/stats_window.h>

#include <utility>

namespace opennova::devtools {

namespace {

ImGuiPassOptions game_pass_options() {
	ImGuiPassOptions options;
	// The Game window owns the Play/Interact-aware Escape policy.
	options.escape_closes = false;
	return options;
}

}  // namespace

GameDevTools::GameDevTools() : pass_(game_pass_options()) {
	auto game = std::make_unique<GameWindow>();
	game_window_ = game.get();
	game->open = true;
	pass_.register_window(std::move(game));
	auto stats = std::make_unique<StatsWindow>();
	stats_window_ = stats.get();
	stats->open = true;
	pass_.register_window(std::move(stats));
	// Closed by default; opens from the "Windows" menu.
	auto entities = std::make_unique<EntitiesWindow>();
	entities_window_ = entities.get();
	pass_.register_window(std::move(entities));
	auto environment = std::make_unique<EnvironmentWindow>();
	environment_window_ = environment.get();
	pass_.register_window(std::move(environment));
	pass_.register_window(std::make_unique<DemoWindow>());
}

void GameDevTools::set_game_viewport(GameViewport *viewport) {
	game_window_->set_viewport(viewport);
}

void GameDevTools::set_game_play_available(bool available) {
	game_window_->set_play_available(available);
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

void GameDevTools::set_entity_directory(EntityDirectorySnapshot snapshot) {
	entities_window_->set_directory(std::move(snapshot));
}

bool GameDevTools::needs_entity_directory() const {
	return pass_.is_open() && entities_window_->open;
}

bool GameDevTools::take_debug_request(DebugRequest &request) {
	return entities_window_->take_request(request);
}

void GameDevTools::set_environment_snapshot(const EnvironmentSnapshot &snapshot) {
	environment_window_->set_snapshot(snapshot);
}

bool GameDevTools::needs_environment_snapshot() const {
	return pass_.is_open() && environment_window_->open;
}

bool GameDevTools::take_environment_request(EnvironmentRequest &request) {
	return environment_window_->take_request(request);
}

}  // namespace opennova::devtools
