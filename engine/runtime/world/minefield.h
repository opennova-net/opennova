#pragma once

#include <runtime/world/collision.h>
#include <string>
#include <vector>

namespace opennova::world {
class World;

// Resolved userpoints in authored order, already transformed and terrain-snapped.
struct MinefieldPoint {
    uint8_t type = 0;
    FixedVec3 position;
};

// Decoded pool-0 bodies; the joiner omits its native local player from this feed.
struct MinefieldActor {
    uint16_t handle = 0xFFFF;
    int32_t item_id = 0;
    uint32_t flags = 0;
    uint32_t move_order = 0;
    FixedVec3 position;
};

struct MinefieldDraw {
    EntityHandle owner;
    uint64_t registry_spawn_id = 0;
    int32_t bms_id = 0;
    uint32_t spawn_origin = kSpawnOriginNone;
    uint8_t slot = 0;
    std::string model;
    CollisionMatrix transform;
};

class MinefieldSystem {
public:
    void initialize(World &world, Entity &entity, bool think, bool render,
                    bool has_graphic, uint32_t small_ammo, uint32_t large_ammo,
                    const std::string &small_marker, const std::string &large_marker,
                    const std::vector<MinefieldPoint> &points);
    void think(World &world, Entity &field);
    FixedVec3 point(const Entity &field, int slot) const;
    void compile_draws(const World &world, std::vector<MinefieldDraw> &out) const;
    std::vector<MinefieldActor> remote_actors;
};
} // namespace opennova::world
