#pragma once

// The process's remote-admin console, the half both embedders share: retail's game opens it in
// every Jointops.exe process (the listen host, a joiner, single player, the menus) when game.cfg's
// remote_admin_port is nonzero, and pumps it at the end of every main frame and every menu frame;
// opennova-serve opens it the same way. The wire is net/admin/admin_server.h's, the verbs
// admin_console.h's; the sockets stay the embedder's (apps/common AdminTcpServer, the game's
// AdminTcpPump). The witness record is docs/net/novaworld-net-re.md §6.9.
// [orig: Game_InitSubsystems @0x4A72B8..0x4A72D9 (admin.cfg, the port, CAdminServer_Listen);
//  Game_ProcessMainFrame @0x5268F1 and Menu_UpdateFrame @0x552901 (CAdminServer_ProcessFrame)]

#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::inmatch {

// The admin server's log, by bare name in the working directory, truncated when the process
// starts. [orig: "admin_log.txt" @0x7C0860; CAdminServer_Construct @0x402C39, the fopen("w")
//  @0x402C84]
inline constexpr const char *kAdminLogFileName = "admin_log.txt";

// The listener's port: game.cfg's remote_admin_port as the 16-bit word it is stored as; 0 opens
// no listener. [orig: Game_InitSubsystems @0x4A72C7 (movzx of remoteAdminPort_A18),
//  @0x4A72CE..0x4A72D1 (the nonzero test), CAdminServer_Listen @0x4A72D9]
uint16_t remote_admin_listen_port(const gamecfg::GameCfg &cfg);

// One admin_log.txt line as retail's text-mode stream writes it: each "\n" reaches the file as
// CR LF. [orig: CAdminServer_Construct @0x402C84, fopen(.., "w"), a text-mode stream]
std::string admin_log_file_text(std::string_view line);

// The admin server's command half for a process whose sessions come and go: the console bound
// over the current host context, else one over a session-less context (the menus, a joiner,
// before a host's match binds), as retail's dispatcher runs over the process's globals whatever
// the scene. Rebinding drops the previous console.
// [orig: CAdminServer_DispatchCommand @0x406720 (no session test of its own);
//  CAdminServer_HandleStatus @0x402E30]
class RemoteAdminConsoles final : public AdminCommandHandler {
public:
	// The console over `ctx` (null: the session-less context) with `seams`.
	void bind(NapiNPServerCtx *ctx, AdminConsole::Seams seams);
	// Whether the console is the one over `ctx` (null: the session-less one).
	bool bound_to(const NapiNPServerCtx *ctx) const { return console_ != nullptr && bound_ == ctx; }
	AdminConsole *console() { return console_.get(); }

	bool dispatch(const AdminSession &session, std::string_view line,
			std::vector<std::string> &replies) override;
	std::string status_report() override;

private:
	NapiNPServerCtx session_less_;
	std::unique_ptr<AdminConsole> console_;
	const NapiNPServerCtx *bound_ = nullptr;
};

} // namespace opennova::inmatch
