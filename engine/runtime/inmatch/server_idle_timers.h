#pragma once

#include <cstdint>

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h> // IEntityIdleTimers

namespace opennova::inmatch {

// The drown limit: four samples per `breathtime` second, the WAC named value
// wac_var_breathtime (20 from WacScript_FreeAll @0x4F6381; the named-value table
// row @0x82EFEC lets a script write it), scaled in 32 bits and compared
// signed by both readers.
// [orig: Server_UpdatePlayerBreathTimers `lea edi,[edx*4]` @0x50D7F2;
//  GameEvent_PlayerDeath `lea ecx,[eax*4]` @0x5172FB]
int32_t breath_sample_limit(const world::World &world);

// One every-32 sample of every in-match player's underwater breath
// (playerSlot+460). An eye strictly below the water plane counts a sample:
// three WATER_GAG warnings lead up to the limit, and the first sample past it
// drowns the player through the ordinary no-killer death transaction. A
// surfacing after more than four samples plays the breath or the gasp
// composite and clears the count; so does the dead flag.
// [orig: Server_UpdatePlayerBreathTimers @0x50D770, called from
//  Server_TickUpdate @0x51D8D7 behind the every-32 gate @0x51D8C4]
void Server_UpdatePlayerBreathTimers(NapiNPServerCtx &ctx, world::World &world);

// The session's player slots behind the World's every-32 idle legs
// (world::ServerIdleLegs): Server_TickUpdate installs one around its
// script pass.
// [orig: Server_TickUpdate @0x51D7E0 (the Server_UpdatePlayerBreathTimers call
//  @0x51D8D7)]
class ServerIdleTimers final : public world::IEntityIdleTimers {
public:
	explicit ServerIdleTimers(NapiNPServerCtx &ctx) : ctx_(ctx) {}
	void update_entity_idle_timers(world::World &world) override {
		Server_UpdatePlayerBreathTimers(ctx_, world);
	}

private:
	NapiNPServerCtx &ctx_;
};

} // namespace opennova::inmatch
