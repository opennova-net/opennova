#include <runtime/inmatch/remote_admin.h>

#include <formats/gamecfg/game_cfg.h>

namespace opennova::inmatch {

uint16_t remote_admin_listen_port(const gamecfg::GameCfg &cfg) {
	// [orig: Game_InitSubsystems @0x4A72C7, the movzx of the word]
	return cfg.remote_admin_port;
}

std::string admin_log_file_text(std::string_view line) {
	// [orig: CAdminServer_Construct @0x402C84, the text-mode fopen]
	std::string text;
	text.reserve(line.size() + 1);
	for (const char c : line) {
		if (c == '\n') text += '\r';
		text += c;
	}
	return text;
}

void RemoteAdminConsoles::bind(NapiNPServerCtx *ctx, AdminConsole::Seams seams) {
	console_.reset();
	bound_ = ctx;
	console_ = std::make_unique<AdminConsole>(ctx != nullptr ? *ctx : session_less_, std::move(seams));
}

// [orig: CAdminServer_DispatchCommand @0x406720]
bool RemoteAdminConsoles::dispatch(const AdminSession &session, std::string_view line,
		std::vector<std::string> &replies) {
	return console_ != nullptr ? console_->dispatch(session, line, replies) : true;
}

std::string RemoteAdminConsoles::status_report() {
	return console_ != nullptr ? console_->status_report() : std::string();
}

} // namespace opennova::inmatch
