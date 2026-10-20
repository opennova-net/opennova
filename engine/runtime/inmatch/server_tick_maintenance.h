#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::world {
class World;
}

// The host tick's maintenance legs (split off server_tick.cpp): the per-player
// slot walk, the periodic-second sweeps and status feeds, the integrity and
// control requests, the RTT clock and the punts they stage. Server_TickUpdate
// calls each at its retail place; the definitions carry the witnesses.
namespace opennova::inmatch {

// The per-slot whole-second countdowns of the periodic-second block.
void tick_respawn_holds(NapiNPServerCtx &ctx, const world::World &world);

// The per-player maintenance walk at the tick head, ahead of the receive pump.
void update_all_active_player_slots(NapiNPServerCtx &ctx, world::World &world);

// The 1 Hz violation sweep (the flag carry limit, the friendly-kill and
// suicide punts).
void check_player_violations(NapiNPServerCtx &ctx, world::World &world);

// The 1 Hz round-robin S2C 0x2F flag-state refresh.
void refresh_next_flag_state(NapiNPServerCtx &ctx, world::World &world);

// The 1 Hz requester-targeted S2C 0x81 score refresh.
void emit_requester_score_refreshes(NapiNPServerCtx &ctx, world::World &world);

// The per-slot RTT request clock.
void emit_periodic_rtt(NapiNPServerCtx &ctx, const world::World &world);

// The stock host's per-peer maintenance requests (0x16, the integrity family,
// 0x79, the control quartet) and the age and silence punts.
void emit_periodic_session_maintenance(NapiNPServerCtx &ctx, world::World &world,
		bool periodic_second);

// The play-tick walk after the script pass.
void advance_play_ticks(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
