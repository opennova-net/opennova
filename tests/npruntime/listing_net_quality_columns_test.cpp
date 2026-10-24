// The NovaWorld Host list's MI1..MI3 (D-NET-NEW-L9-50): the host CNetQuality's send-window
// averages, read at every server-info update. A mission start's reset leaves them 0; the main
// frame's first sample, the frame-rate counter still 0 (its first 2 s window not closed), stores
// the bandwidth term 255 into one slot of five, so MI1 reads 51; five samples at 16 fps or more
// read 1. Ping and loss stay 0 with no eligible slot. The LN S1 / S1d / S2r2 retail listen hosts
// publish MI1 "0" at registration, "51" in the update after the load and "1" in the first
// periodic update (LN-s6's S3 record), MI2 and MI3 never leaving "0".
// [orig: Lobby_UpdateServerInfo @0x4FE8C0 - g_NetQualityHostAvgBandwidth read @0x4FEB3C ("MI1"),
//  g_NetQualityHostAvgPing @0x4FEB71 ("MI2"), g_NetQualityHostAvgLoss @0x4FEBA6 ("MI3");
//  CNetQuality_UpdateMetrics @0x4C52C0 - the bandwidth term @0x4C531B..0x4C5350, the averages
//  @0x4C557F..0x4C5585; CNetStats_Reset @0x4C2FF0 (Game_StartMission's call @0x5243AA)]

#include <net/napi/session.h>
#include <net/novaworld/lobby_vars.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h>
#include <runtime/inmatch/server_net_quality.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>

using namespace opennova;

namespace {

int failures = 0;

void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++failures;
}

// The Host list's value of `name` over the context's columns ("" when the list lacks it).
std::string host_list_value(const inmatch::NapiNPServerCtx &ctx, const char *name) {
	HostRegistration columns;
	inmatch::read_session_listing_columns(columns, ctx, hud::GameTextLookup{});
	for (const ClientVar &v : make_host_var_list(columns, HostLobbyText{}, /*full=*/true))
		if (v.name == name) return v.value;
	return {};
}

void run_the_columns_follow_the_send_window() {
	// A HostOnly authority in session over a live world, no player slot eligible. Heap-held
	// (MSVC's 1 MB test-main stack).
	auto world = std::make_unique<world::World>();
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::set_connection_mode(*ctx, inmatch::ConnectionMode::HostOnly);
	ctx->is_in_session = 1;
	world->rules.mp_session = true;
	world->registry.configure_pool(0, 8);
	ctx->world = world.get();
	const auto sample = [&](int32_t fps) {
		ctx->stats_avg_fps = fps;
		ctx->net_quality_sample_countdown = 0;
		inmatch::Server_SampleHostNetQuality(*ctx);
	};

	expect(host_list_value(*ctx, "MI1") == "0" && host_list_value(*ctx, "MI2") == "0" &&
	               host_list_value(*ctx, "MI3") == "0",
	       "a reset window publishes MI1..MI3 \"0\" (the registration's)");
	sample(0);
	expect(host_list_value(*ctx, "MI1") == "51",
	       "the first sample, the frame-rate counter still 0: 255 in one slot of five, MI1 \"51\"");
	expect(host_list_value(*ctx, "MI2") == "0" && host_list_value(*ctx, "MI3") == "0",
	       "no eligible slot: MI2 and MI3 stay \"0\"");
	for (int i = 0; i < 5; ++i) sample(62);
	expect(host_list_value(*ctx, "MI1") == "1",
	       "five samples at 16 fps or more: the floor 1 in every slot, MI1 \"1\"");
	HostRegistration columns;
	inmatch::read_session_listing_columns(columns, *ctx, hud::GameTextLookup{});
	expect(columns.mi1 == ctx->host_quality_window.avg_bandwidth &&
	               columns.mi2 == ctx->host_quality_window.avg_ping &&
	               columns.mi3 == ctx->host_quality_window.avg_loss,
	       "MI1..MI3 are the send window's three averages");
}

} // namespace

int main() {
	run_the_columns_follow_the_send_window();
	if (failures == 0) std::printf("npruntime_listing_net_quality_columns: all passed\n");
	return failures == 0 ? 0 : 1;
}
