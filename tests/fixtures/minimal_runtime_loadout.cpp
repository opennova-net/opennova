// Boots the committed minimal mission through the production mission kernel and
// proves its single-player BMS kit wins over the engine's WPN_M4AUTO fallback.
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr const char *kMissionName = "mnml.bms";
constexpr const char *kExpectedWeapon = "WPN_AK47AUTO";

int failures = 0;

#define CHECK(cond, msg)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			std::fprintf(stderr, "FAIL: %s\n", (msg));    \
			++failures;                                    \
		}                                                 \
	} while (0)

} // namespace

int main() {
	opennova::mission::MissionKernel kernel;
	std::string error;
	if (!kernel.open(GAME_ASSETS_DIR, kMissionName, error)) {
		std::fprintf(stderr, "FAIL: minimal mission opens: %s\n", error.c_str());
		return 1;
	}

	opennova::mission::KernelBootOptions options;
	options.wac = false;
	options.collision = false;
	options.seat_specs = false;
	if (!kernel.boot(options, error)) {
		std::fprintf(stderr, "FAIL: minimal mission boots: %s\n", error.c_str());
		return 1;
	}

	CHECK(kernel.local.has_local_player(), "minimal mission spawns the local player");
	CHECK(kernel.local.loadout.spawn_kit_set, "the offline BMS kit is promoted");
	CHECK(!kernel.local.loadout.spawn_kit.empty(), "the promoted offline kit is not empty");
	if (!kernel.local.loadout.spawn_kit.empty())
		CHECK(kernel.local.loadout.spawn_kit.front().name == kExpectedWeapon,
		      "the promoted first weapon is WPN_AK47AUTO");

	CHECK(kernel.local.inventory_valid, "the promoted kit builds a weapon inventory");
	const opennova::world::WeaponInventorySlot *equipped =
			kernel.local.inventory.slot(kernel.local.inventory.equipped_combo);
	CHECK(equipped != nullptr && equipped->adm_index >= 0,
	      "the spawn-default inventory slot is equipped");

	const opennova::world::WeaponTableEntry *equipped_def = nullptr;
	if (equipped != nullptr && equipped->adm_index >= 0 && equipped->adm_index < 256)
		equipped_def = kernel.world.tables.weapons.by_index(
				static_cast<uint8_t>(equipped->adm_index));
	CHECK(equipped_def != nullptr, "the equipped slot resolves through weapon.def");
	if (equipped_def != nullptr)
		CHECK(equipped_def->name == kExpectedWeapon,
		      "the equipped weapon identity is WPN_AK47AUTO");

	const opennova::world::Entity *player = kernel.local.player();
	CHECK(player != nullptr, "the spawned player entity exists");
	if (player != nullptr && equipped != nullptr && equipped->adm_index >= 0)
		CHECK(player->equipped_adm_index == static_cast<uint8_t>(equipped->adm_index),
		      "the player entity carries the AK inventory selection");

	if (failures == 0)
		std::printf("OK: minimal single-player runtime equips WPN_AK47AUTO\n");
	return failures == 0 ? 0 : 1;
}
