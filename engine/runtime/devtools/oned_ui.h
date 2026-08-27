// ONED's run surface (ADR 0037 scope, ADR 0039 toolkit): the one ImGui window
// that holds the game-data directory, its recents, the game/expansion
// profile, the retail install directory, the three actions (Run OpenNova,
// Stage & Run Retail, Stop) and the status line.
//
// The UI owns the text fields (the user edits them in place); the embedder
// seeds them, reads them back on the apply/commit requests, and pushes the
// state only it has — recents, the readiness reasons, the running flag, the
// status line. Every button and field edge becomes a typed OnedRequest the
// embedder drains and executes (process spawning, native directory dialogs
// and settings persistence stay with the shell).
#pragma once

#include <devtools/imgui_pass.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::devtools {

enum class OnedStatusKind { INFO, WARN, ERROR };

enum class OnedAction {
	NONE,
	EDIT_RESOURCE_DIR,   // a keystroke in the game-data field (the implicit default is no longer implicit)
	APPLY_RESOURCE_DIR,  // the game-data field committed (Enter / focus left)
	APPLY_RETAIL_DIR,    // the retail field committed
	COMMIT_PROFILE,      // the game code or expansion field committed
	BROWSE_RESOURCE_DIR,
	BROWSE_RETAIL_DIR,
	SELECT_RECENT,       // index = the recent directory picked
	CLEAR_RECENTS,
	RUN_OPENNOVA,
	RUN_RETAIL,
	STOP,
};

struct OnedRequest {
	OnedAction action = OnedAction::NONE;
	int index = -1;
};

class OnedUi {
public:
	OnedUi();
	~OnedUi();
	OnedUi(const OnedUi &) = delete;
	OnedUi &operator=(const OnedUi &) = delete;

	ImGuiPass &pass() { return pass_; }
	const ImGuiPass &pass() const { return pass_; }

	// The four text fields: the UI owns them, the embedder seeds and reads them.
	void set_resource_dir(std::string_view value) { resource_dir_ = value; }
	const std::string &resource_dir() const { return resource_dir_; }
	void set_game_code(std::string_view value) { game_code_ = value; }
	const std::string &game_code() const { return game_code_; }
	void set_expansion(std::string_view value) { expansion_ = value; }
	const std::string &expansion() const { return expansion_; }
	void set_retail_dir(std::string_view value) { retail_dir_ = value; }
	const std::string &retail_dir() const { return retail_dir_; }

	// State only the embedder has.
	void set_recent_dirs(std::vector<std::string> dirs) { recent_dirs_ = std::move(dirs); }
	const std::vector<std::string> &recent_dirs() const { return recent_dirs_; }
	// An empty block enables the action; a non-empty one disables it and is
	// its tooltip (the readiness reason).
	void set_readiness(std::string_view opennova_block, std::string_view retail_block, bool running);
	const std::string &opennova_block() const { return opennova_block_; }
	const std::string &retail_block() const { return retail_block_; }
	bool is_running() const { return running_; }
	void set_status(std::string_view text, OnedStatusKind kind);
	const std::string &status_text() const { return status_text_; }
	OnedStatusKind status_kind() const { return status_kind_; }

	// One layout pass (see ImGuiPass::draw_frame).
	bool draw_frame(uint64_t frame_index) { return pass_.draw_frame(frame_index); }

	// The request queue: the UI pushes on every button/field edge, the
	// embedder drains it after the frame. push_request is also the
	// automation/test entry (the same edges, without a mouse).
	void push_request(OnedRequest request) { requests_.push_back(request); }
	bool take_request(OnedRequest &out);
	bool has_requests() const { return !requests_.empty(); }

private:
	class Surface;
	friend class Surface;

	ImGuiPass pass_;
	std::string resource_dir_;
	std::string game_code_;
	std::string expansion_;
	std::string retail_dir_;
	std::vector<std::string> recent_dirs_;
	std::string opennova_block_;
	std::string retail_block_;
	bool running_ = false;
	std::string status_text_;
	OnedStatusKind status_kind_ = OnedStatusKind::INFO;
	std::vector<OnedRequest> requests_;
};

}  // namespace opennova::devtools
