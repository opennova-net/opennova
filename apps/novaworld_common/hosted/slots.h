#pragma once

// A slot: one hosted server's position on a machine (CONTEXT.md "Slot"). Slot k
// of a machine is the game's UDP port 32768 + k (open to everyone), the remote
// admin's TCP port 32800 + k (open to the service alone), the container's user
// 10001 + k and the working directory /srv/slots/slot-k (0700, that user's). A
// shared machine has up to kMaxSlots of them (ONNET_FLEET_SHARED_SLOTS), an own
// machine one, slot 0: the ports and the user a machine of its own always had
// (the serve image's user is 10001). Service policy, shared by the NovaWorld
// service (placement.h, the rendered game.cfg) and opennova-hostd (apps/hostd:
// the containers, the per-UID egress counters); the machine's user-data
// reserves both port ranges out of Linux's ephemeral range and fences every
// slot's user off the instance metadata service. Not a port.

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

inline constexpr int kMaxSlots = 32;
inline constexpr uint16_t kSlotGamePortBase = 32768;
inline constexpr uint16_t kSlotAdminPortBase = 32800;
inline constexpr uint32_t kSlotUidBase = 10001;
inline constexpr const char *kSlotsRoot = "/srv/slots";

constexpr bool slot_valid(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

constexpr uint16_t slot_game_port(int slot) {
	return static_cast<uint16_t>(kSlotGamePortBase + slot);
}
constexpr uint16_t slot_admin_port(int slot) {
	return static_cast<uint16_t>(kSlotAdminPortBase + slot);
}
constexpr uint32_t slot_uid(int slot) {
	return kSlotUidBase + static_cast<uint32_t>(slot);
}

// The slot whose user `uid` is, or -1.
constexpr int slot_of_uid(uint32_t uid) {
	return uid >= kSlotUidBase && uid < kSlotUidBase + static_cast<uint32_t>(kMaxSlots)
			? static_cast<int>(uid - kSlotUidBase)
			: -1;
}

// "slot-<k>", the working directory's name under the slots' root.
inline std::string slot_dir_name(int slot) {
	return "slot-" + std::to_string(slot);
}
// "/srv/slots/slot-<k>".
inline std::string slot_dir(int slot) {
	return std::string(kSlotsRoot) + "/" + slot_dir_name(slot);
}

// What opennova-serve leaves its machine's agent in the slot's working
// directory (apps/serve/run_reports.h): the tick health it rewrites once a
// minute (--stats-file) and the error a non-zero exit ended on (--exit-file).
// The service names both on every hosted start (hosted_settings.h
// render_serve_args); the agent reads them there.
inline constexpr const char *kTickStatsFileName = "tick-stats.json";
inline constexpr const char *kExitFileName = "exit.txt";

// The two port ranges never meet: every game port lies below the first admin port.
static_assert(slot_game_port(kMaxSlots - 1) < kSlotAdminPortBase,
		"the slots' game and admin ports overlap");
static_assert(slot_of_uid(slot_uid(kMaxSlots - 1)) == kMaxSlots - 1 &&
				slot_of_uid(kSlotUidBase - 1) == -1,
		"slot_of_uid inverts slot_uid");

} // namespace opennova::novaworld_server::hosted
