#include <runtime/world/minefield.h>

#include <base/io/fixed.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace opennova::world {
namespace {
int32_t fixed(float v) { return static_cast<int32_t>(v * io::kFp16One); }
FixedVec3 position(const Entity &e) {
    return {fixed(e.position.x), fixed(e.position.y), fixed(e.position.z)};
}
int32_t add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t distance(FixedVec3 a, FixedVec3 b) {
    const auto delta = [](int32_t x, int32_t y) {
        return static_cast<double>(static_cast<int32_t>(
                static_cast<uint32_t>(x) - static_cast<uint32_t>(y)));
    };
    const double x = delta(a.x, b.x), y = delta(a.y, b.y), z = delta(a.z, b.z);
    return static_cast<int32_t>(std::min(std::sqrt(x*x + y*y + z*z),
                                       static_cast<double>(INT32_MAX)));
}

// [orig: Weapon_FireProcess @0x53F5B0]
void fire(World &world, Entity &field, FixedVec3 p, uint8_t ammo_index) {
	world.round_sim.fire_source(world, &field, p,
			field.veh.yaw_seeded ? field.veh.yaw_bam : bam_heading_from_mission_yaw_deg(field.yaw),
			bam_from_degrees_wrapped(field.pitch), ammo_index);
}

} // namespace

// [orig: Entity_InitHardpoints @ 0x4417D0]
void MinefieldSystem::initialize(World &world, Entity &entity, bool think_enabled,
        bool render_enabled, bool has_graphic, uint32_t small_ammo, uint32_t large_ammo,
        const std::string &small_marker, const std::string &large_marker,
        const std::vector<MinefieldPoint> &points) {
    MinefieldState &state = entity.minefield;
    if (state.initialized) return;
    state.initialized = true;
    state.think = think_enabled;
    state.render = render_enabled;
    state.small_marker = small_marker;
    state.large_marker = large_marker;
    const CollisionMatrix matrix = entity_placement_matrix(entity);
    std::copy(std::begin(matrix.m), std::end(matrix.m), state.placement);
    if (!think_enabled) return; // render selection does not install an init callback
    entity.engine_flags |= kEntityFlagMatrixBuilt;
    if (!has_graphic) return;
    state.age = (entity.handle.packed & 0x7F) + 7;
    state.ammo_small = small_ammo;
    state.ammo_large = large_ammo;
    const FixedVec3 origin = position(entity);
    int n = 0;
    for (const MinefieldPoint &point : points) {
        if (point.type < 1 || point.type > 4) continue;
        if (n == MinefieldState::kSlots) break;
        const int32_t xyz[] = {point.position.x, point.position.y, point.position.z};
        const int32_t base[] = {origin.x, origin.y, origin.z};
        state.types[n] = point.type;
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t delta = static_cast<int32_t>(
                    static_cast<uint32_t>(xyz[axis]) - static_cast<uint32_t>(base[axis]));
            state.offsets[n][axis] = static_cast<int16_t>(retail_signed_i16(delta >> 8));
        }
        state.rotations[n++] = static_cast<uint8_t>(world.next_prng16_b());
    }
    entity.engine_flags |= 0x02000000u;
}

FixedVec3 MinefieldSystem::point(const Entity &field, int slot) const {
    const MinefieldState &s = field.minefield;
    return {add(s.placement[3], static_cast<int32_t>(s.offsets[slot][0]) * 256),
            add(s.placement[7], static_cast<int32_t>(s.offsets[slot][1]) * 256),
            add(s.placement[11], static_cast<int32_t>(s.offsets[slot][2]) * 256)};
}

// [orig: Entity_LandmineThink @ 0x441A40]
void MinefieldSystem::think(World &world, Entity &field) {
    constexpr int count = MinefieldState::kSlots;
    if ((field.section_mask & 0x3FFFu) == 0x3FFFu) {
        world.registry.despawn(field.handle); // Entity_Destroy @ 0x43E810; no kill transaction
        return;
    }
    field.minefield.age = world.rules.logic_authority ? 3 : 1;
    std::array<FixedVec3, count> positions;
    std::array<bool, count> contacts{};
    for (int i = 0; i < count; ++i) positions[i] = point(field, i);
    const FixedVec3 center = position(field);
    const int32_t radius = fixed(field.bound_radius);
    auto actor = [&](const MinefieldActor &body) {
        if (body.item_id == 0 || (body.flags & 1) ||
            distance(body.position, center) > radius) return;
        FixedVec3 foot = body.position;
        const uint32_t stance = body.move_order & 0x300;
        foot.z = add(foot.z, stance == 0 ? -0x10000 : stance == 0x100 ? 0 : -0x8000);
        for (int i = 0; i < count; ++i)
            if (!(field.section_mask & (1u << i)) && !contacts[i] &&
                distance(foot, positions[i]) < 49152) contacts[i] = true;
    };
    std::vector<MinefieldActor> actors = remote_actors;
    world.registry.for_each_in_pool(0, [&](const Entity &e) {
        actors.push_back({e.handle.packed, e.item_id,
            (e.engine_flags | e.flags), static_cast<uint32_t>(e.net_move_input) |
                (static_cast<uint32_t>(e.net_stance_bits) << 8), position(e)});
    });
    std::stable_sort(actors.begin(), actors.end(), [](const auto &a, const auto &b) {
        return a.handle < b.handle;
    });
    for (const auto &body : actors) actor(body);
    world.registry.for_each_in_pool(1, [&](const Entity &vehicle) {
        if (!vehicle.item_id || !vehicle.has_item_def ||
            vehicle.item_type != 1 || ((vehicle.engine_flags | vehicle.flags) & 1)) return;
        const int32_t vehicle_radius = fixed(vehicle.bound_radius);
        FixedVec3 probe = position(vehicle);
        if (distance(probe, center) > add(radius, vehicle_radius)) return;
        probe.z = add(probe.z, 4096);
        for (int i = 0; i < count; ++i) {
            if ((field.section_mask & (1u << i)) || contacts[i]) continue;
            FixedVec3 contact = probe;
            const bool indoors = (vehicle.engine_flags & kEntityFlagIndoors) != 0;
            int32_t ground = probe.z;
            if (world.collision)
                ground = world.collision->minefield_ground(world, vehicle.handle, probe, indoors);
            else if (!indoors)
                ground = world.tables.terrain ? fixed(terrain::height_field_height_world_bilinear(
                        *world.tables.terrain, probe.x * io::kInvFp16One,
                        -probe.y * io::kInvFp16One)) : 0;
            contact.z = add(ground, 0x8000);
            if (distance(contact, positions[i]) - vehicle_radius / 2 < 49152)
                contacts[i] = true;
        }
    });
    for (int i = 0; i < count; ++i) {
        if (!contacts[i]) continue;
        const uint8_t type = field.minefield.types[i];
        if (type == 1 || type == 2)
            fire(world, field, positions[i], static_cast<uint8_t>(field.minefield.ammo_small));
        else if (type == 3 || type == 4)
            fire(world, field, positions[i], static_cast<uint8_t>(field.minefield.ammo_large));
        field.section_mask |= 1u << i;
    }
}

// [orig: Entity_RenderBoneAttachments @ 0x441660]
void MinefieldSystem::compile_draws(const World &world, std::vector<MinefieldDraw> &out) const {
    out.clear();
    world.registry.for_each([&](const Entity &field) {
        const MinefieldState &state = field.minefield;
        if (field.hidden || !state.render) return;
        for (int i = 0; i < MinefieldState::kSlots; ++i) {
            if (field.section_mask & (1u << i)) continue;
            const uint8_t type = state.types[i] & 0x7F;
            if (type != 1 && type != 3) continue;
            const std::string &model = type == 1 ? state.small_marker : state.large_marker;
            if (model.empty()) continue;
            MinefieldDraw draw;
            draw.owner = field.handle;
            draw.registry_spawn_id = field.registry_spawn_id;
            draw.bms_id = field.bms_id;
            draw.spawn_origin = field.spawn_origin;
            draw.slot = static_cast<uint8_t>(i);
            draw.model = model;
            std::copy(std::begin(state.placement), std::end(state.placement), draw.transform.m);
            const FixedVec3 p = point(field, i);
            draw.transform.m[3] = p.x; draw.transform.m[7] = p.y; draw.transform.m[11] = p.z;
            const uint32_t rotation = static_cast<uint32_t>(
                    static_cast<int32_t>(static_cast<int8_t>(state.rotations[i])));
            const int32_t zero[3]{};
            const CollisionMatrix turn = collision_matrix_from_euler(
                    rotation << 26, rotation << 20, 0, zero);
            // Math_BuildFixedPointRotationMatrixYXZ @ 0x615400 postmultiplies;
            // Matrix_Multiply3x4_FixedPoint @ 0x613940 rounds once per dot product.
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    int64_t sum = 0x200000;
                    for (int k = 0; k < 3; ++k)
                        sum += static_cast<int64_t>(state.placement[4*r+k]) * turn.m[4*k+c];
                    draw.transform.m[4*r+c] = static_cast<int32_t>(sum >> 22);
                }
            }
            out.push_back(std::move(draw));
        }
    });
}
} // namespace opennova::world
