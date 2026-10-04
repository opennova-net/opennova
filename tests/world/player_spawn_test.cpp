// The host's own-player keystone (net-re §5.2b/§5.38): spawn_player builds a pool-0
// player-infantry entity, PlayerBodyInput ports the 8-way input mapping
// [orig: Player_PackInputStateToEntity @0x4df450], and the infantry motor's local-player
// branch drives + mirrors the pose to the registry Entity (the S2C 0x0A source).
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void submit_player_input(AiEntity &ae, const PlayerInput &in) {
    apply_player_body_input(ae, pack_player_body_input(player_input_flags(in, false), in));
}

// Test root source: directional states expose distinct local root axes, so the player motor must
// select the real 8-way state rather than rotate a forward clip by a port-only offset.
struct DirectionalClip : IRootMotionSource {
    int32_t step;
    explicit DirectionalClip(int32_t s) : step(s) {}
    bool has_clip(int, int) const override { return true; }
    int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; } // looping doubles
    bool advance(int, int state_id, int32_t &phase, RootMotionFrame &out) override {
        phase += 1;
        out = RootMotionFrame{};
        const int dir = dir_for_state(state_id);
        switch (dir) {
            case 0: out.dx = step; break;                       // forward
            case 1: out.dx = step; out.dy = -step; break;        // forwardright
            case 2: out.dy = -step; break;                       // right
            case 3: out.dx = -step; out.dy = -step; break;       // backright
            case 4: out.dx = -step; break;                       // back
            case 5: out.dx = -step; out.dy = step; break;        // backleft
            case 6: out.dy = step; break;                        // left
            case 7: out.dx = step; out.dy = step; break;         // forwardleft
            default: break;
        }
        return true;
    }
    static int dir_for_state(int state_id) {
        if (state_id >= anim_state::kWalkForward && state_id < anim_state::kWalkForward + 8)
            return state_id - anim_state::kWalkForward;
        if (state_id >= anim_state::kWalkCrouchForward && state_id < anim_state::kWalkCrouchForward + 8)
            return state_id - anim_state::kWalkCrouchForward;
        if (state_id >= anim_state::kWalkProneForward && state_id < anim_state::kWalkProneForward + 8)
            return state_id - anim_state::kWalkProneForward;
        // The forward run gaits the promotion reaches from pure-forward walk
        // [orig: run_2/run_3 @0x4b729d; ANIMNUM 9/10 are forward-only clips].
        if (state_id == anim_state::kRun2 || state_id == anim_state::kRun3)
            return 0;
        return -1;
    }
};

int main() {
    // --- shared BAM helpers: yaw wraps as a 32-bit binary angle; pitch/camera clamps live above.
    {
        CHECK(bam_heading_from_mission_yaw_deg(90.0) == 0);
        CHECK(bam_heading_from_mission_yaw_deg(450.0) == 0);
        CHECK(bam_heading_from_mission_yaw_deg(-270.0) == 0);
        CHECK(static_cast<uint32_t>(bam_heading_from_mission_yaw_deg(0.0)) == 0x40000000u);
        CHECK(static_cast<uint32_t>(bam_heading_from_mission_yaw_deg(180.0)) == 0xC0000000u);
        CHECK(static_cast<uint32_t>(bam_heading_from_mission_yaw_deg(270.0)) == 0x80000000u);
        const int32_t wrapped = bam_heading_from_mission_yaw_deg(720.0);
        CHECK(std::fabs(normalize_mission_yaw_deg(mission_yaw_deg_from_bam_heading(wrapped)) - 0.0) < 0.01);
        CHECK(std::fabs(mission_yaw_deg_from_bam_heading(static_cast<int32_t>(0x40000000u)) - 0.0) < 0.001);
        CHECK(std::fabs(mission_yaw_deg_from_bam_heading(static_cast<int32_t>(0xC0000000u)) - 180.0) < 0.001);
        CHECK(std::fabs(mission_yaw_deg_from_bam_heading(static_cast<int32_t>(0x80000000u)) - 270.0) < 0.001);
        CHECK(bam_from_degrees_wrapped(360.0) == 0);
        CHECK(bam_from_degrees_wrapped(-360.0) == 0);
    }

    // --- §5.2b spawn: a gated pool-0 player-infantry, motor mounted, local_player published.
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);

        PlayerSpawn ps;
        ps.position = {10.0f, 20.0f, 5.0f};
        ps.yaw = 0;
        ps.team = 1;
        ps.net_id = 0xFFF0;
        const EntityHandle h = spawn_player(w, ps);

        CHECK(h.valid());
        CHECK(h.pool() == 0);
        CHECK(w.cached.local_player == h);

        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(e->item_id == kPlayerInfantryTypeId);
        CHECK((e->engine_flags & 0x100u) != 0u);
        CHECK((e->flags & 0x100u) != 0u);
        CHECK((e->flags & 2u) == 0u); // §5.2b gate cleared
        CHECK(e->health > 0);
        CHECK(e->kind == EntityKind::Organic);

        CHECK(ai.count() == 1);
        const AiEntity *ae = ai.at(0);
        CHECK(ae->inf.active);
        CHECK(ae->inf.is_local_player);
    }

    // --- D-NET-144: spawns seed FULL health from the items.def Player hp when the traits sweep
    //     resolved it (late joiners spawn after the sweep), stamping health_max so the §5.10
    //     field-17 tier denominator reads full (tier 2 / golden 0x28).
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        w.tables.player.item_hp = 150; // the class-8 Player items.def hp, stamped by resolve_item_traits
        w.tables.player.item_type = 3;
        w.tables.player.item_attrib = 0x200u;
        w.tables.player.armor_impact = 7;
        w.tables.player.armor_kz = 9;
        w.tables.player.damage_reduc_pp = 0.1f;
        w.tables.player.damage_reduc_max = 0.25f;
        const EntityHandle h = spawn_remote_player(w, PlayerSpawn{});
        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(e->has_item_def);
        CHECK(e->health == 150);
        CHECK(e->health_max == 150);
        CHECK(e->item_type == 3 && e->item_attrib == 0x200u);
        CHECK(e->armor_impact == 7 && e->armor_kz == 9);
        CHECK(e->damage_reduc_pp == 0.1f && e->damage_reduc_max == 0.25f);
        CHECK((e->engine_flags & 0x100u) != 0u);
        CHECK((e->flags & 0x100u) != 0u);
        const AiEntity *ae = ai.at(0);
        CHECK(ae->health == 150);
        CHECK(ae->inf.max_health == 150);
    }
    // A solo-mode spawn is BERSERK (AiSlot[1] 0x200), everyone's enemy; a team-mode one is not.
    // [orig: Entity_SpawnFromAnimSlotProperty @0x43C53C..0x43C54F]
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        PlayerSpawn solo;
        solo.berserk = true;
        spawn_remote_player(w, solo);
        spawn_remote_player(w, PlayerSpawn{});
        CHECK((ai.at(0)->slot.f[AiSlot::kBehaviorFlags] & 0x200) != 0);
        CHECK((ai.at(1)->slot.f[AiSlot::kBehaviorFlags] & 0x200) == 0);
    }
    // Public template carriers stay int32_t, but the retail Player ItemDef fields are
    // signed words. Oversized API inputs wrap/sign-extend at the spawn stamp rather
    // than leaking wider values into Entity or the int16 infantry mirror.
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        w.tables.player.has_item_def = true;
        w.tables.player.item_hp = 65535;       // low word 0xFFFF -> -1
        w.tables.player.armor_impact = 65546;  // low word 0x000A -> 10
        w.tables.player.armor_kz = 65535;      // low word 0xFFFF -> -1
        const EntityHandle h = spawn_remote_player(w, PlayerSpawn{});
        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(e->health == -1 && e->health_max == -1);
        CHECK(e->armor_impact == 10 && e->armor_kz == -1);
        const AiEntity *ae = ai.at(0);
        CHECK(ae->health == -1 && ae->inf.max_health == -1);
    }
    {
        World w; // item-less world: the spawn-seed fallback still spawns AT FULL (100/100)
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        w.tables.player.has_item_def = false;
        const EntityHandle h = spawn_player(w, PlayerSpawn{});
        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(!e->has_item_def);
        CHECK(e->health == 100);
        CHECK(e->health_max == 100);
    }

    // --- retail listen-server player allocation starts after .bms-resident pool-0 organics.
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);

        PlayerSpawn ps;
        ps.min_entity_slot = 4;
        ps.net_id = 0xFFF0;
        const EntityHandle host = spawn_player(w, ps);
        CHECK(host == EntityHandle::make(0, 4));
        CHECK(w.cached.local_player == host);

        PlayerSpawn peer;
        peer.min_entity_slot = 4;
        peer.net_id = 0xFFF1;
        const EntityHandle joiner = spawn_remote_player(w, peer);
        CHECK(joiner == EntityHandle::make(0, 5));
        CHECK(w.cached.local_player == host);
    }

    // --- input -> 8-way player body mapping (the witnessed mapping + opposing-key cancel).
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        spawn_player(w, PlayerSpawn{});
        AiEntity &ae = *ai.at(0);

        PlayerInput in;
        in.forward = true;
        in.look_heading = 0x10000000;
        submit_player_input(ae, in);
        CHECK(ae.inf.player_moving);
        CHECK(ae.inf.player_move_dir_index == 0);   // index 0 = forward
        CHECK(ae.inf.move_mode == 0);
        CHECK(ae.inf.target_dist == 0);
        CHECK(ae.inf.target_heading == 0x10000000); // look → facing

        in = PlayerInput{};
        in.left = true;
        submit_player_input(ae, in);
        CHECK(ae.inf.player_move_dir_index == 2);   // index 2 = left

        in = PlayerInput{};
        in.forward = true;
        in.right = true;
        submit_player_input(ae, in);
        CHECK(ae.inf.player_move_dir_index == 7);   // index 7 = forward + right

        in = PlayerInput{}; // opposing keys cancel → idle
        in.forward = true;
        in.back = true;
        submit_player_input(ae, in);
        CHECK(!ae.inf.player_moving);
        CHECK(ae.inf.player_move_dir_index == 0);
        CHECK(ae.inf.move_mode == 0);
        CHECK(ae.inf.target_dist == 0);

        in = PlayerInput{}; // F+L+R collapses to forward in the IDA switch
        in.forward = true;
        in.left = true;
        in.right = true;
        submit_player_input(ae, in);
        CHECK(ae.inf.player_moving);
        CHECK(ae.inf.player_move_dir_index == 0);

        in = PlayerInput{}; // F+B+L collapses to left
        in.forward = true;
        in.back = true;
        in.left = true;
        submit_player_input(ae, in);
        CHECK(ae.inf.player_moving);
        CHECK(ae.inf.player_move_dir_index == 2);
    }

    // --- player body input path: raw input packs separately from NPC route orders.
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        spawn_player(w, PlayerSpawn{});
        AiEntity &ae = *ai.at(0);

        struct Case {
            bool f, b, l, r;
            bool moving;
            int index;
        } cases[] = {
            {false, false, false, false, false, 0},
            {true,  false, false, false, true,  0},
            {false, true,  false, false, true,  4},
            {true,  true,  false, false, false, 0},
            {false, false, true,  false, true,  2},
            {true,  false, true,  false, true,  1},
            {false, true,  true,  false, true,  3},
            {true,  true,  true,  false, true,  2},
            {false, false, false, true,  true,  6},
            {true,  false, false, true,  true,  7},
            {false, true,  false, true,  true,  5},
            {true,  true,  false, true,  true,  6},
            {false, false, true,  true,  false, 0},
            {true,  false, true,  true,  true,  0},
            {false, true,  true,  true,  true,  4},
            {true,  true,  true,  true,  false, 0},
        };
        for (const Case &c : cases) {
            PlayerInput in;
            in.forward = c.f;
            in.back = c.b;
            in.left = c.l;
            in.right = c.r;
            in.look_heading = 0x10000000;
            const PlayerBodyInput body = pack_player_body_input(player_input_flags(in, false), in);
            CHECK(body.moving == c.moving);
            CHECK(body.move_dir_index == c.index);
            apply_player_body_input(ae, body);
            CHECK(ae.inf.player_moving == c.moving);
            CHECK(ae.inf.player_move_dir_index == c.index);
            CHECK(ae.inf.move_mode == 0);
            CHECK(ae.inf.target_dist == 0);
            CHECK(ae.inf.target_heading == 0x10000000);
        }
    }

    // --- motor drive: forward input advances pos along facing AND mirrors to the Entity.
    {
        World w;
        AiSystem &ai = w.ai;
        DirectionalClip clip(0x8000); // 0.5 u per tick along the selected state
        ai.root_motion = &clip;
        w.registry.configure_pool(0, 8);

        PlayerSpawn ps;
        ps.position = {0.0f, 0.0f, 0.0f};
        ps.yaw = 0;
        const EntityHandle h = spawn_player(w, ps);
        AiEntity &ae = *ai.at(0);

        PlayerInput in;
        in.forward = true;
        in.look_heading = ae.heading; // walk along the spawn facing

        const int32_t x0 = ae.pos[0], y0 = ae.pos[1];
        for (int i = 0; i < 30; ++i) {
            submit_player_input(ae, in); // re-assert each frame (as the controller would)
            TickContext ctx;
            ctx.world = &w;
            ctx.logic_tick = static_cast<uint32_t>(i);
            ctx.is_authority = true;
            w.update_all_entities(ctx);
        }

        CHECK(ae.pos[0] != x0 || ae.pos[1] != y0); // moved
        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[0])) - e->position.x) < 0.01f);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[1])) - e->position.y) < 0.01f);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[2])) - e->position.z) < 0.01f);
    }

    // --- player movement uses real 8-way clip states. Facing stays fixed; left input selects
    //     walk_left (state 7), then the retail body transition blends its lateral root in.
    //     Root rotation uses the body heading captured at the motor head, before
    //     this tick's leg-midpoint update turns the planted feet.
    //     [orig: Entity_UpdateInfantryPlayerBody body midpoint @0x4B4AA9..0x4B4ABB;
    //     entity+0x8C load @0x4B41E4; Q22 root rotation @0x4B41F0..0x4B4255]
    {
        World w;
        AiSystem &ai = w.ai;
        DirectionalClip clip(0x8000);
        ai.root_motion = &clip;
        w.registry.configure_pool(0, 8);
        spawn_player(w, PlayerSpawn{});
        AiEntity &ae = *ai.at(0);

        PlayerInput in;
        in.left = true;
        in.look_heading = 0; // facing +X; planted feet rotate walk_left's +Y root
        submit_player_input(ae, in);

        TickContext ctx;
        ctx.world = &w;
        ctx.logic_tick = 0;
        ctx.is_authority = true;
        w.update_all_entities(ctx);

        CHECK(ae.inf.anim_state == anim_state::kWalkForward + 6);
        CHECK(ae.inf.body_heading == 0x30000000);
        CHECK(ae.pos[0] == 0);
        CHECK(ae.pos[1] == 0);
        CHECK(ae.inf.body_clip_state() == anim_state::kIdle);
        CHECK(ae.inf.anim_blend_weight == 1.0f);

        ctx.logic_tick = 1;
        submit_player_input(ae, in);
        w.update_all_entities(ctx);
        CHECK(ae.inf.body_heading == 0x2D000000);
        CHECK(ae.pos[0] == -2017);
        CHECK(ae.pos[1] == 835);
        CHECK(ae.inf.anim_blend_weight == (1.0f / 15.0f));
    }

    // --- grounding mirror runs for NON-player motor entities too (the AI-in-the-ground fix):
    //     the motor mirrors every inf.active entity's pose into the registry Entity that
    //     snapshot_of serializes, so the wire carries the current pose, not the stale spawn Z.
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.item_id = 0x14B9;
        seed.net_id = 7;
        seed.position = {1.0f, 2.0f, 3.0f}; // authored spawn pos
        seed.alive = true;
        const EntityHandle h = w.registry.spawn(0, seed);
        AiEntity &ae = *ai.at(ai.attach(h));
        ae.net_id = 7;
        ae.inf.active = true; // a plain AI organic — NOT is_local_player
        ae.pos[0] = to_fixed(11.0f);
        ae.pos[1] = to_fixed(22.0f);
        ae.pos[2] = to_fixed(33.0f); // motor-advanced pose, distinct from the authored spawn
        TickContext ctx;
        ctx.world = &w;
        ctx.logic_tick = 0;
        ctx.is_authority = true;
        w.update_all_entities(ctx);
        const Entity *e = w.registry.get(h);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[0])) - e->position.x) < 0.01f);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[2])) - e->position.z) < 0.01f);
        CHECK(std::fabs(e->position.z - 33.0f) < 0.01f); // mirrored, not the authored 3.0
    }

    // --- player look: pitch applies and the look yaw is INSTANT (no body-turn smoothing).
    {
        World w;
        AiSystem &ai = w.ai;
        w.registry.configure_pool(0, 8);
        spawn_player(w, PlayerSpawn{}); // spawn yaw 0 -> heading ~0x40000000
        AiEntity &ae = *ai.at(0);
        PlayerInput in;
        in.look_heading = 0x20000000; // a different facing
        in.look_pitch = 0x08000000;
        submit_player_input(ae, in);
        TickContext ctx;
        ctx.world = &w;
        ctx.logic_tick = 0;
        ctx.is_authority = true;
        w.update_all_entities(ctx);
        CHECK(ae.heading == 0x20000000); // instant look yaw, not quarter-stepped toward target
        CHECK(ae.pitch == 0x08000000);   // look pitch applied (slope lean overridden)
    }

    // --- the input-flag word between packs [orig: Input_ProcessFrame @0x49d541
    //     `g_InputFlags &= ~g_InputFlagsPrev` ahead of the handlers;
    //     Player_PackInputStateToEntity @0x4df904/@0x4df909 save-and-clear]. A bit the
    //     last pack did not report is sticky until the next pack; a bit it did report
    //     survives only on a frame it is still held; packing every frame reduces to the
    //     frame's own keys.
    {
        PlayerInput none;
        PlayerInput jump;
        jump.jump = true;
        PlayerInput lean;
        lean.lean_right = true;
        // The handler bits [orig: Input_HandleActionBinding_0 @0x4e0c6e..0x4e10f1].
        PlayerInput all;
        all.forward = all.back = all.left = all.right = true;
        all.look_up = all.look_down = all.turn_left = all.turn_right = true;
        all.jump = all.lean_left = all.lean_right = all.free_look = true;
        CHECK(player_input_flags(all, false) == 0xF37Eu);
        CHECK(player_input_flags(all, true) == 0xF31Eu); // AbsorbPitch refuses 0x20/0x40

        PlayerInputFlags f;
        // Window 1 (a 12-frame holdoff): jump on frames 2..4, lean on frame 7.
        for (int frame = 1; frame <= 12; ++frame) {
            const PlayerInput &held = frame >= 2 && frame <= 4 ? jump : (frame == 7 ? lean : none);
            f.fold(player_input_flags(held, false));
        }
        CHECK(f.flags == (kInputFlagJump | kInputFlagLeanRight));
        PlayerBodyInput packed = pack_player_body_input(f.flags, none);
        CHECK(packed.jump && packed.lean_right && !packed.lean_left && !packed.moving);
        f.clear_after_pack();
        CHECK(f.flags == 0u && f.prev == (kInputFlagJump | kInputFlagLeanRight));
        // Window 2: nothing held -> the boundary pack reports neither tap.
        for (int frame = 1; frame <= 12; ++frame) f.fold(player_input_flags(none, false));
        packed = pack_player_body_input(f.flags, none);
        CHECK(!packed.jump && !packed.lean_right);
        f.clear_after_pack();
        // Window 3: jump held throughout -> reported.
        for (int frame = 1; frame <= 12; ++frame) f.fold(player_input_flags(jump, false));
        CHECK(pack_player_body_input(f.flags, none).jump);
        f.clear_after_pack();
        // Window 4: the reported jump stays held into frames 1..5, then releases:
        // the previous-pack mask clears it every frame, so it is not re-reported.
        for (int frame = 1; frame <= 12; ++frame)
            f.fold(player_input_flags(frame <= 5 ? jump : none, false));
        CHECK(!pack_player_body_input(f.flags, none).jump);
        f.clear_after_pack();
        // Packing every frame: the word is exactly the frame's keys.
        const PlayerInput *frames[] = {&jump, &none, &lean, &jump, &jump, &none};
        for (const PlayerInput *held : frames) {
            f.fold(player_input_flags(*held, false));
            CHECK(f.flags == player_input_flags(*held, false));
            f.clear_after_pack();
        }
    }

    std::printf("player_spawn: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
