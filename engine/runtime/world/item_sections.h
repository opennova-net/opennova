// Section-based item callbacks and their independently moving fragments.
// [orig: palm @ 0x53C4C0; psec @ 0x53BE10]
#pragma once
#include <runtime/world/destruction.h>
namespace opennova::world {
class IItemPieceSpawner {
public:
    virtual ~IItemPieceSpawner() = default;
    // The callback supplies the retail template. The definition selects its
    // allocation pool; MissionKernel binds the model before returning it.
    virtual EntityHandle spawn_item_piece(const Entity &seed) = 0;
};
bool tower_item_event(World &world, Entity &entity, int phase, ItemHitContext hit);
void palm_item_event(World &world, Entity &entity, int phase, ItemHitContext hit);
bool tick_item_section_motion(World &world, Entity &entity);
uint32_t item_hidden_sections(const Entity &entity);
Vec3 item_section_render_position(const World &world, const Entity &entity);
} // namespace opennova::world
