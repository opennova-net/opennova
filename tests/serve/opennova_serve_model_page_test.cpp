// opennova-serve's S2C 0x68 cursor wrap (net parity lane L8, V-L3-s2's addendum): a dedicated
// host's mission start freezes the model-def list its loads built, with no renderer, and every
// frame hands its count to the session as the 0x68 cursor's wrap (a slot's cursor advances by 50
// and returns to 0 at the count, server_tick_maintenance's). The loose mount here carries a
// weapon.def of 30 weapons, each naming a first- and a third-person model, and the 60 model
// files: the list holds 60 nodes (no Avatars.def, ammo tracers, items, celestial models,
// hudfx.def or foliage on this mount), so a slot's cursors run 50, 0, 50, ... where the count
// of 0 ours handed over read 0 every quartet.
// [orig: Game_StartMission @0x524360 (the call @0x525A6E) -> Render_InitMissionTextures
//  @0x587120 (the call @0x58728C) -> CEffectWorld_RebuildAllModelBuffers @0x5B3A80;
//  ThreediGp_LoadModel @0x5B6160; Server_SendLoadedModelPageRequest @0x511360 (+50 @0x51137E, the wrap
//  @0x511391..0x511393)]
#include "server.h"
#include "serve_test_support.h"

#include "common/test_paths.h"

#include <runtime/inmatch/loaded_model_list.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
	const fs::path dir = serve_test::fresh_dir("model_page");
	const fs::path work = serve_test::fresh_dir("model_page_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()));
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "GameName \"Model Page\"\r\nMaxPlayers 4\r\nMission servetst.bms\r\n";
	}
	// 30 weapons, each with its own first- and third-person model, every one a copy of a
	// minted crate.
	const fs::path crate = test_paths_fixture("threedi/synth/crate.3di");
	{
		std::ofstream weapons(dir / "weapon.def", std::ios::binary);
		for (int i = 0; i < 30; ++i) {
			char name[16];
			std::snprintf(name, sizeof(name), "t%02d", i);
			weapons << "weapon \"WPN_T" << i << "\"\r\n\tcategory 1\r\n\tgfx1 " << name
			        << "_1st\r\n\tgfx3 " << name << "_3rd\r\nend\r\n\r\n";
			for (const char *view : {"_1st.3di", "_3rd.3di"}) {
				std::error_code ec;
				fs::copy_file(crate, dir / (std::string(name) + view), ec);
				CHECK(!ec);
			}
		}
	}

	serve::ServeOptions options;
	std::string error;
	CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/host",
					(dir / "test.host").string(), "--loose-root", "--lan-port", "0"},
			options, error) == 0);
	auto server = std::make_unique<serve::Server>(options);
	const bool started = server->start(error);
	if (!started) std::printf("start: %s\n", error.c_str());
	CHECK(started);
	if (!started) return 1;

	// The weapon table read the 30 rows, and the mission start's page counts their 60 models.
	CHECK(server->kernel().weapon_defs_ok && server->kernel().weapon_defs.count == 30);
	CHECK(server->frame(1.0 / 62.5));
	CHECK(server->role().state.host_owner.ctx.loaded_model_list_count == 60u);

	server->stop();
	if (failures == 0) std::printf("opennova_serve_model_page: all passed\n");
	return failures == 0 ? 0 : 1;
}
