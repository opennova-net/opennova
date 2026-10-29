// The parity runner's slot staging on our host (inmatch::stage_slot_position):
// the authority writes the three Position words of the body a player slot holds
// and, as the onHook bridge's slot pose does, mission yaw 0 and pitch 0 on the body
// and its brain; an empty slot, a bodiless player and a joiner refuse. On the
// authority the staged body then holds against its player's uplinks from the old
// spot only past the chase's 2-unit drop, as on a retail host.
// [orig: Server_BanPlayerSlot @0x508304..0x508332 (the slot walk);
//  Server_PositionPlayerForSpawn @0x50CFBE / @0x50CFCA / @0x50CFD0 (the Position
//  words); Entity_UpdateInfantryPlayerBody @0x4B431F..0x4B4386 (the drop)]

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/slot_staging.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

#include "conn_fixture.h"

#include <cstdint>
#include <cstdio>
#include <memory>

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace rep = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct Rig {
    std::unique_ptr<w::World> world = std::make_unique<w::World>();
    inmatch::NapiNPServerCtx ctx;
    w::EntityHandle body;

    Rig() {
        world->registry.configure_pool(0, 4);
        w::Entity seed;
        seed.kind = w::EntityKind::Organic;
        seed.health = 100;
        seed.alive = true;
        seed.flags |= w::kEntityFlagPlayer;
        seed.position = {10.0f, 20.0f, 0.0f};
        body = world->registry.spawn(0, seed);
        w::AiEntity *motor = world->ai.at(world->ai.attach(body));
        motor->inf.active = true;
        motor->health = 100;
        ctx.world = world.get();
        ctx.is_authority = 1;
        inmatch::NapiNPConnection conn = conn_fixture::make_conn(
                1, 1, nullptr, opennova::replication::TransportMode::Client, body, true);
        conn.reply.player_slot = 1;
        ctx.np_protocol.connection_list.push_back(conn);
    }
    w::Entity &ent() { return *world->registry.get(body); }
};

void test_a_slot_body_takes_the_three_position_words_and_yaw_0() {
    Rig r;
    r.ent().heading = w::placed_heading_bam(33);
    r.ent().pitch_bam = 0x01000000;
    r.world->ai.for_handle(r.body)->heading = w::placed_heading_bam(33);
    r.world->ai.for_handle(r.body)->pitch = 0x01000000;
    const inmatch::SlotStagingResult staged =
            inmatch::stage_slot_position(r.ctx, 1, 35.0, -16.0, 16.9);
    CHECK(staged.status == inmatch::SlotStagingStatus::Staged);
    CHECK(staged.handle == r.body.packed);
    // 16.9 truncates to 0x10E666 (16.9 * 65536 = 1107558.4).
    CHECK(r.ent().position.x.q16 == 35 * 65536);
    CHECK(r.ent().position.y.q16 == -16 * 65536);
    CHECK(r.ent().position.z.q16 == 1107558);
    CHECK(staged.position_q16[2] == 1107558);
    // Mission yaw 0 and pitch 0, on the body and its brain (x6's staged retail body:
    // heading byte 64, the yaw-0 heading's top byte).
    const int32_t yaw0 = w::bam_heading_from_mission_yaw_deg(0.0);
    CHECK(static_cast<uint32_t>(yaw0) >> 24 == 64u);
    CHECK(r.ent().heading == yaw0);
    CHECK(r.ent().pitch_bam == 0);
    CHECK(r.world->ai.for_handle(r.body)->heading == yaw0);
    CHECK(r.world->ai.for_handle(r.body)->pitch == 0);
    CHECK(staged.heading_bam == yaw0 && staged.pitch_bam == 0);
}

void test_an_empty_slot_a_bodiless_player_and_a_joiner_refuse() {
    Rig r;
    CHECK(inmatch::stage_slot_position(r.ctx, 2, 1.0, 1.0, 1.0).status ==
          inmatch::SlotStagingStatus::NoPlayer);
    r.ctx.np_protocol.connection_list[0].link.owned_entity = w::EntityHandle{};
    CHECK(inmatch::stage_slot_position(r.ctx, 1, 1.0, 1.0, 1.0).status ==
          inmatch::SlotStagingStatus::NoBody);
    Rig j;
    j.ctx.is_authority = 0;
    CHECK(inmatch::stage_slot_position(j.ctx, 1, 1.0, 1.0, 1.0).status ==
          inmatch::SlotStagingStatus::NotAuthority);
    CHECK(j.ent().position.x.q16 == 10 * 65536);
    Rig o;
    CHECK(inmatch::stage_slot_position(o.ctx, 1, 40000.0, 0.0, 0.0).status ==
          inmatch::SlotStagingStatus::OutOfRange);
}

// Staged 3 units from where its player still reports itself, the body holds: the
// uplink's gap is over 2 units, which the authority drops. Staged 1 unit away, the
// next uplink pulls it back by a bucket step.
void test_a_staged_body_holds_against_its_players_uplink_only_past_two_units() {
    for (const double offset : {3.0, 1.0}) {
        Rig r;
        r.world->ai.is_authority = true;
        CHECK(inmatch::stage_slot_position(r.ctx, 1, 10.0 + offset, 20.0, 0.0).status ==
              inmatch::SlotStagingStatus::Staged);
        rep::PlayerIntent intent;
        intent.entity_handle = r.body.packed;
        intent.pos_x = 10 * 65536; // the player's own report, still at the old spot
        intent.pos_y = 20 * 65536;
        intent.pos_z = 0;
        CHECK(rep::apply_player_intent(*r.world, intent, true));
        w::AiEntity &body_ai = *r.world->ai.for_handle(r.body);
        r.world->ai.update_organic(body_ai, *r.world, 1,
                w::body_move_row(*r.world, body_ai, r.world->registry.get(r.body)));
        const int32_t staged_x = static_cast<int32_t>((10.0 + offset) * 65536.0);
        if (offset > 2.0) {
            CHECK(r.ent().position.x.q16 == staged_x);
        } else {
            // One step of ((10 >> 1) - 65536) / 10 = -6553 toward the report.
            CHECK(r.ent().position.x.q16 == staged_x + (5 - 65536) / 10);
        }
    }
}

} // namespace

int main() {
    test_a_slot_body_takes_the_three_position_words_and_yaw_0();
    test_an_empty_slot_a_bodiless_player_and_a_joiner_refuse();
    test_a_staged_body_holds_against_its_players_uplink_only_past_two_units();
    if (failures == 0) std::printf("npruntime_slot_staging: all passed\n");
    return failures == 0 ? 0 : 1;
}
