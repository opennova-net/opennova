#include <devtools/game_dev_tools.h>

#include <devtools/demo_window.h>
#include <devtools/stats_window.h>

namespace opennova::devtools {

GameDevTools::GameDevTools() : pass_(ImGuiPassOptions{}) {
	auto stats = std::make_unique<StatsWindow>();
	stats_window_ = stats.get();
	stats->open = true;
	pass_.register_window(std::move(stats));
	pass_.register_window(std::make_unique<DemoWindow>());
}

void GameDevTools::set_frame_stats(FrameStatsBoard *board) {
	frame_stats_ = board;
	stats_window_->set_board(board);
}

}  // namespace opennova::devtools
