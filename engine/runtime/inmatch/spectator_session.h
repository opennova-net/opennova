#pragma once

// THE DEATH SCREEN'S SESSION LEGS: what crosses from the role's replica
// runtime (the client-local spectate state every role folds, the listen
// host's own loopback included) into the world and the local player while
// g_DeathScreenActive is up. The free-fly motor itself is world-side
// (world/spectator_motor.h); this is its per-tick input stamp, the death
// screen's action filter ahead of the body input, and the spectator actions
// with the sub-mode wrap's pose placement.
// [orig: Camera_UpdateFreeFly @0x4b2980; sub_52AD50 @0x52ad50;
//  sub_52AFF0 @0x52aff0]

#include <cstdint>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::inmatch {

class ClientRuntime;
struct InputPacket;

// The spectate state onto world.spectator, ahead of the entity update: the
// latch, the sub-mode, the SU gate and the target — the authority's own
// registry entity (its replica rows name its world's handles) or, on a
// joiner, the decoded row's pose. A death-screen rising edge the replica
// counted seats the local entity's eye. A null runtime clears the state.
// [orig: g_DeathScreenActive @0xA860EC, dword_A860F0, dword_A860F4,
//  g_ScoreboardStatusSuffixEnabled @0xA85B49 — the reads
//  @0x4b40f8 / @0x4b2994 / @0x4b29c0 / @0x4b2a35;
//  NapiNPClientMsg_0x00A @0x42ffb8..0x42ffc9 (the eye)]
void stamp_spectator_motor(mission::MissionKernel &kernel, const ClientRuntime *runtime,
		bool joiner);

// One tick's frame input onto the local player while the death screen is
// up. The fire, reload and medic rows are mode-1 rows the death screen's
// binding scan never admits; the movement rows are refused while the local
// player is dead (action flag bit 0) or the deploy screen is up (0x8000000,
// held for good by a join-time spectator in a mission with spawn zones);
// otherwise the chase sub-mode with a target takes the
// movement keys onto the orbit/zoom actions and the mouse onto the orbit, the
// first-person sub-mode swallows both, and the free sub-mode passes them to
// the body input the free-fly motor packs.
// [orig: Input_IsBindingActiveForMode @0x497ea0 (row +8 bit 2 on the death
//  screen); Input_HandleActionBinding @0x49ad6d..0x49ada0 (the dead gate),
//  @0x49add7 (the deploy-screen gate),
//  @0x49ae4c (sub_52AD50 first); sub_52AD50 @0x52ad50]
void apply_death_screen_input(mission::MissionKernel &kernel, ClientRuntime &runtime,
		bool joiner, const InputPacket &input);

// A spectator action (500 the sub-mode cycle, 501 / 502 the target step) on
// the role's replica; the cycle's wrap back to the free sub-mode then places
// the local entity on the composed view pose.
// [orig: Input_HandleActionBinding cases 500..502 @0x49bd58..0x49bd9e;
//  sub_52AFF0 @0x52b082..0x52b0ee]
void spectate_action(mission::MissionKernel &kernel, ClientRuntime &runtime, int code);

} // namespace opennova::inmatch
