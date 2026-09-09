// Raw terrain gradients and their two organic motor consumers.
// [orig: Terrain_GetHeightGradient @0x606330; org1 @0x4BA896;
// org2 @0x4B79DC]
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

namespace {
constexpr int32_t q16(int n) { return n * 65536; }

struct Field {
    std::vector<uint16_t> heights = std::vector<uint16_t>(1024 * 1024, 5000);
    std::array<int, 256> sectors{};
    terrain::TerrainHeightField field;

    Field() {
        sectors.fill(1);
        field.heightmap = heights.data();
        field.dim = 1024;
        field.layout.sector_grid = sectors.data();
    }
    uint16_t &at(int x, int y) { return heights[y * 1024 + x]; }
    void gradient(int x, int y, int dx, int dy) {
        at(x - 1, y) = 5000;
        at(x + 1, y) = static_cast<uint16_t>(5000 + dx);
        at(x, y - 1) = 5000;
        at(x, y + 1) = static_cast<uint16_t>(5000 + dy);
    }
    terrain::TerrainHeightGradient sample(int32_t x, int32_t y) const {
        return terrain::height_field_gradient_fixed(field, x, y);
    }
};

void test_raw_gradient() {
    Field f;
    f.gradient(10, 20, 789, -110);
    auto g = f.sample(q16(10) + 65535, -q16(20) - 65535);
    CHECK(g.dx == 789 && g.dy == -110); // integer floor, mission Y is negated
    f.at(11, 20) = 65535;
    f.at(9, 20) = 0;
    g = f.sample(q16(10), -q16(20));
    CHECK(g.dx == 65535); // samples are unsigned words, differences are signed

    f.sectors.fill(0);
    g = f.sample(q16(10), -q16(20));
    CHECK(g.dx == 0 && g.dy == 0);
    g = terrain::height_field_gradient_fixed({}, 0, 0);
    CHECK(g.dx == 0 && g.dy == 0);

    f.sectors.fill(1);
    f.gradient(511, 511, -789, 110);
    g = f.sample(-1, 1);
    CHECK(g.dx == -789 && g.dy == 110); // NEG/SAR, not truncation toward zero
}

void test_quadrants_and_seams() {
    Field f;
    const int xs[] = {10, 10, 522, 522};
    const int ys[] = {20, 532, 20, 532};
    for (int id = 1; id <= 4; ++id) {
        f.gradient(xs[id - 1], ys[id - 1], id * 100, -id * 50);
        f.sectors.fill(id);
        const auto g = f.sample(q16(10), -q16(20));
        CHECK(g.dx == id * 100 && g.dy == -id * 50);
    }
    // Bottom-right quadrant, both far-edge taps: locked axes wrap to 512;
    // unlocked axes cross the atlas edge to 0. Opposite taps stay at 1022.
    f.sectors.fill(4);
    f.at(0, 1023) = 640;
    f.at(512, 1023) = 200;
    f.at(1022, 1023) = 100;
    f.at(1023, 0) = 500;
    f.at(1023, 512) = 240;
    f.at(1023, 1022) = 20;
    for (int bits = 0; bits < 4; ++bits) {
        f.field.locks.set(3, (bits & 1) != 0, (bits & 2) != 0);
        const auto g = f.sample(q16(511), -q16(511));
        CHECK(g.dx == ((bits & 1) ? 100 : 540));
        CHECK(g.dy == ((bits & 2) ? 220 : 480));
    }
    f.field.locks = {};
    f.field.locks.set(0, true, true);
    auto g = f.sample(q16(511), -q16(511));
    CHECK(g.dx == 540 && g.dy == 480); // only the selected quadrant's locks apply
}

void test_sector_edges() {
    Field f;
    f.field.layout.origin_x = 2;
    f.field.layout.origin_y = -3;
    f.field.layout.sector_count = 1;
    f.field.layout.sector_rows = 1;
    f.sectors.fill(0);
    f.sectors[0] = 1;
    f.sectors[15] = 2;
    f.sectors[15 * 16] = 3;
    f.sectors[255] = 4;
    f.gradient(10, 20, 100, 0);
    f.gradient(10, 532, 200, 0);
    f.gradient(522, 20, 300, 0);
    f.gradient(522, 532, 400, 0);
    const auto sample_cell = [&](int x, int y) {
        return f.sample(q16((x + 2) * 512 + 10), -q16((y - 3) * 512 + 20)).dx;
    };
    CHECK(sample_cell(-1, -1) == 100);
    CHECK(sample_cell(16, -1) == 200);
    CHECK(sample_cell(-1, 16) == 300);
    CHECK(sample_cell(16, 16) == 400); // clamp to padded cell 15, not authored extent
    f.field.wrap_x = true;
    CHECK(sample_cell(-1, -1) == 200);
    CHECK(sample_cell(16, -1) == 100);
    f.field.wrap_z = true;
    CHECK(sample_cell(-1, -1) == 400);
    CHECK(sample_cell(16, 16) == 100);
    f.field.wrap_x = false;
    CHECK(sample_cell(-1, -1) == 300);
}

struct RootSource final : world::IRootMotionSource {
    bool has_clip(int, int) const override { return true; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int, int32_t &phase, world::RootMotionFrame &out) override {
        ++phase;
        out = {};
        out.dx = 1111;
        out.dy = 2222;
        out.dz = 3333;
        return true;
    }
};

struct Motor {
    Field field;
    RootSource root;
    std::unique_ptr<world::World> w = std::make_unique<world::World>();
    world::EntityHandle handle;

    Motor(int dx, int dy, bool player = false, int32_t heading = 0) {
        field.gradient(100, 100, dx, dy);
        w->registry.configure_pool(0, 4);
        world::Entity body;
        body.kind = world::EntityKind::Organic;
        body.health = 100;
        body.health_max = 100;
        body.has_item_def = true;
        body.item_type = 3;
        if (player) body.engine_flags |= world::kEntityFlagPlayer;
        handle = w->registry.spawn(0, body);
        const int index = w->ai.attach(handle);
        auto &e = *w->ai.at(index);
        e.inf.active = true;
        e.inf.adm_id = 1;
        e.inf.is_local_player = player;
        e.inf.anim_state = world::anim_state::kWalkForward;
        e.inf.anim_prev = e.inf.anim_state;
        e.inf.body_heading = heading;
        e.inf.target_heading = heading;
        for (int leg = 0; leg < 2; ++leg) {
            e.inf.leg_yaw[leg] = heading;
            e.inf.leg_target[leg] = heading;
        }
        e.heading = io::bam_add(heading, 0x20000000); // independent NPC look
        e.pos[0] = q16(100);
        e.pos[1] = -q16(100);
        e.pos[2] = q16(60);
        e.health = 100;
        w->tables.terrain = &field.field;
        w->ai.terrain = &field.field;
        w->ai.root_motion = &root;
        w->ai.is_authority = true;
    }
    world::AiEntity &ai() { return *w->ai.for_handle(handle); }
    world::Entity &entity() { return *w->registry.get(handle); }
    void tick(uint32_t key = 1) {
        w->logic_tick = key;
        w->ai.tick_infantry(ai(), *w, key);
    }
};

void test_motor_gradient_and_rotation() {
    {
        Motor m(767, 0);
        m.entity().flags |= 0x10000;
        m.entity().engine_flags |= 0x10000;
        m.ai().inf.path_state = 2;
        m.tick();
        CHECK(m.ai().pos[0] == q16(100) + 1111);
        CHECK(m.ai().pos[1] == -q16(100) + 2222);
        CHECK(((m.entity().flags | m.entity().engine_flags) & 0x10000) == 0);
        CHECK(m.ai().inf.path_state == 2); // flat ground does not cancel a cached detour
    }
    {
        Motor m(768, 0);
        m.tick();
        CHECK(m.ai().pos[0] == q16(100) - 4915);
        CHECK(m.ai().pos[1] == -q16(100));
        CHECK(m.ai().pos[2] == q16(60) + 3333); // keep the clip's vertical lane
        CHECK(m.ai().inf.vel[2] == -167);
        CHECK(m.ai().inf.path_state == 1);
        CHECK((m.entity().engine_flags & 0x10000) != 0);
    }
    {
        Motor m(-512, -768);
        m.ai().inf.vel[2] = -900;
        m.w->ai.is_authority = false;
        m.tick();
        CHECK(m.ai().pos[0] == q16(100) + 3276); // exact negative half rounds upward
        CHECK(m.ai().pos[1] == -q16(100) - 4915);
        CHECK(m.ai().inf.vel[2] == -900);
        CHECK(m.ai().inf.path_state == 1); // motor response also runs on clients
    }
    for (bool player : {false, true}) {
        Motor m(512, 768, player, 0x40000000);
        m.tick();
        CHECK(m.ai().pos[0] == q16(100) - (player ? 3277 : 4915));
        CHECK(m.ai().pos[1] == -q16(100) - (player ? -4915 : 3277));
        CHECK(m.ai().inf.path_state == (player ? 0 : 1));
        CHECK(m.ai().inf.vel[2] == (player ? -375 : -167));
    }
    {
        Motor m(462, 614); // both components are below 768, magnitude exceeds it
        m.tick();
        CHECK((m.entity().engine_flags & 0x10000) != 0);
    }
    {
        Motor m(461, 614);
        m.tick();
        CHECK((m.entity().engine_flags & 0x10000) == 0);
    }
}

void test_suppression_and_carriers() {
    for (const uint32_t blocked : {0x8000u, 0x100000u, 0x800000u}) {
        Motor m(768, 0);
        m.entity().flags |= blocked | 0x10000;
        m.entity().engine_flags |= 0x10000;
        m.tick();
        CHECK(((m.entity().flags | m.entity().engine_flags) & 0x10000) == 0);
        CHECK(m.ai().pos[0] == q16(100) + (blocked == 0x100000u ? 0 : 1111));
    }
    // The 0x2000 half of the mask is the motors' own product, written into the
    // registry Flags pair: org1 on its fall edge [orig: @0x4BF8C8..0x4BF8CF],
    // org2 on the jump [orig: @0x4B7EDB..0x4B7EEF]. Neither is stamped here.
    {
        Motor m(768, 0);
        m.tick(2); // grounded sample on the even tick: slide + clamp, then the edge
        CHECK((m.entity().engine_flags & 0x10000) != 0);
        CHECK((m.entity().flags & 0x2000) != 0);
        CHECK((m.entity().engine_flags & 0x2000) != 0);
        CHECK(m.ai().inf.airborne);
        m.ai().inf.vel[2] = 0x1600; // an upward impulse the clamp would otherwise eat
        const int32_t x = m.ai().pos[0];
        m.tick(4);
        CHECK(((m.entity().flags | m.entity().engine_flags) & 0x10000) == 0);
        CHECK(m.ai().pos[0] == x + 1111);         // the clip root is kept, no slide pair
        CHECK(m.ai().inf.vel[2] == 0x1600 - 416); // gravity only, no -167 clamp
    }
    {
        Motor m(0, 0, true); // the jump gate refuses steep ground (0x10000)
        m.ai().pos[2] = 5000 * 256;
        m.ai().inf.jump_requested = true;
        m.tick(1);
        CHECK(m.ai().inf.airborne);
        CHECK((m.entity().flags & 0x2000) != 0);
        CHECK((m.entity().engine_flags & 0x2000) != 0);
        CHECK(m.ai().inf.vel[2] == 0x1600);
        m.field.gradient(100, 100, 768, 0); // the arc now sits over a steep cell
        const int32_t x = m.ai().pos[0];
        m.tick(2);
        CHECK(((m.entity().flags | m.entity().engine_flags) & 0x10000) == 0);
        CHECK(m.ai().inf.vel[2] == 0x1600 - 208); // no -167 clamp
        CHECK(m.ai().pos[0] == x + 819);          // (63 * 833) >> 6 momentum, no slide pair
    }
    for (bool player : {false, true}) {
        Motor m(768, 0, player);
        world::Entity platform;
        platform.kind = world::EntityKind::Item;
        const auto carrier = m.w->registry.spawn(0, platform);
        m.entity().ground_target = carrier;
        m.entity().flags |= 0x10000;
        m.entity().engine_flags |= 0x10000;
        m.tick();
        CHECK(m.ai().pos[0] == q16(100) + 1111);
        CHECK(((m.entity().flags | m.entity().engine_flags) & 0x10000) ==
              (player ? 0u : 0x10000u));
        CHECK(m.ai().inf.path_state == 0);
    }
    Motor m(768, 0);
    m.w->tables.terrain = nullptr;
    m.w->ai.terrain = nullptr;
    m.entity().engine_flags |= 0x10000;
    m.tick();
    CHECK((m.entity().engine_flags & 0x10000) == 0);
    CHECK(m.ai().pos[0] == q16(100) + 1111);
}


void test_player_jump_uses_current_slope() {
    for (bool steep : {false, true}) {
        Motor m(steep ? 768 : 0, 0, true);
        m.ai().pos[2] = 5000 * 256; // stand on the raw16=5000 terrain
        m.entity().engine_flags |= 0x10000; // previous tick's slide
        m.ai().inf.jump_requested = true;
        m.tick();
        CHECK(m.ai().inf.jump_cooldown == (steep ? 0 : 32));
        CHECK(m.ai().inf.airborne == !steep);
        CHECK((((m.entity().flags | m.entity().engine_flags) & 0x2000) != 0) == !steep);
        CHECK(((m.entity().engine_flags & 0x10000) != 0) == steep);
    }
}

void test_slope_arms_route_detour_before_think() {
    Motor m(768, 0);
    auto &ai = m.w->ai;
    ai.nav.channels.resize(2);
    ai.nav.channels[1].count = 1;
    ai.nav.channels[1].entries[0] = 0;
    world::NavEntry node{};
    node.f[0] = q16(1);
    node.f[1] = q16(120);
    node.f[2] = -q16(100);
    node.f[3] = q16(20);
    ai.nav.nodes.push_back(node);
    m.ai().slot.f[35] = 1;
    m.ai().slot.f[37] = 1;
    m.tick(16);
    CHECK(m.ai().inf.move_mode != 0);
    CHECK(m.ai().inf.path_state == 2);
    CHECK(m.ai().inf.detour_target[0] != 0);
}
} // namespace

int main() {
    test_raw_gradient();
    test_quadrants_and_seams();
    test_sector_edges();
    test_motor_gradient_and_rotation();
    test_suppression_and_carriers();
    test_slope_arms_route_detour_before_think();
    test_player_jump_uses_current_slope();
    if (failures == 0) std::puts("infantry terrain: OK");
    return failures == 0 ? 0 : 1;
}
