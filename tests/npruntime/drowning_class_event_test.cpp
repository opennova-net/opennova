// The drowning's class callback (D-NET-NEW-L3-2): after the attacker clear, the
// drown selection and Health -1, retail's breath sample calls the body's
// entity+0x1C8 callback with event 0. The plyr callback's health tail requests
// the staged drown clip on the primary channel (+0x2BC, the deferred +0x2B8
// cleared); event 0 lies outside {1, 3, 4, 5}, so the kill-cause bits 8..11 of
// entity+0x2C clear (a head-zone bit latched by an earlier hit does not survive
// into the drowning's death), and the think cadence re-arms.
// [orig: Server_UpdatePlayerBreathTimers -- +0x178 = 0 @0x50D800,
//  Entity_ComputeAnimSlotIndex @0x50D80A, Health -1 @0x50D819, the +0x1C8 call
//  (body, 0, 0) @0x50D824..0x50D838; Entity_HandleDamageAndTriggerZones -- the
//  health tail @0x407AC5..0x407B01 (+0x2BC @0x407AFB, +0x2B8 @0x407B01), the
//  cause clear @0x407B3B..0x407B4F, the re-arm @0x407B5E]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_idle_timers.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

void test_the_drowning_runs_the_class_callback() {
	inmatch::NapiNPServerCtx ctx;
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	world.rules.mp_session = true;
	world.rules.logic_authority = true;
	world.registry.configure_pool(0, 16);
	ctx.world = &world;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	w::PlayerSpawn spawn;
	spawn.team = 1;
	const w::EntityHandle body_h = w::spawn_remote_player(world, spawn);
	inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
			inmatch::kFirstJoinerDcb, 1, &transport, ns::TransportMode::Client, body_h, true);
	c.slot_state = inmatch::PlayerSlotState::Joined;
	ctx.np_protocol.connection_list.push_back(std::move(c));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.back();

	w::Entity *body = world.registry.get(body_h);
	w::AiEntity *motor = world.ai.for_handle(body_h);
	CHECK(body != nullptr && motor != nullptr);
	if (body == nullptr || motor == nullptr) return;
	// Under the water plane, one sample short of the limit (4 * breathtime).
	world.env.water_z = 1 << 16;
	body->position.z = 0.0f;
	body->eye_offset_z = 0;
	conn.link.underwater_breath_samples =
			static_cast<uint32_t>(world.script.wac_values.breathtime) * 4u;
	body->cause_flags = 0x800u | 0x2000u; // an earlier head-zone hit; the hold latch
	body->spawn_phase = 17;
	motor->inf.anim_pending = 7;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);

	const int32_t drown = w::compute_death_anim_state(0, 0, w::death_cause::kDrown);
	CHECK(body->health == -1);
	CHECK(body->death_anim_state == drown);
	CHECK(!body->last_attacker.valid());
	CHECK((body->cause_flags & 0xF00u) == 0u);  // event 0 clears bits 8..11
	CHECK((body->cause_flags & 0x2000u) != 0u); // and nothing else
	CHECK(motor->inf.anim_state == drown);      // the primary channel's request
	CHECK(motor->inf.anim_pending == 0);
	CHECK(body->spawn_phase == 64);
}

} // namespace

int main() {
	test_the_drowning_runs_the_class_callback();
	if (failures == 0) std::printf("drowning_class_event: all passed\n");
	return failures == 0 ? 0 : 1;
}
