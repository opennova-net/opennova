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
// The draw's hidden sections and origin: the sector builder's for a psec /
// cesp render tag (palm_sections), else the section and piece masks and a
// tower clone's husk pivot.
uint32_t item_hidden_sections(const Entity &entity);
Vec3 item_section_render_position(const World &world, const Entity &entity);
// CTerrainMap_BuildSectorTransformMatrices's sections and origin over the
// palm state, the callback a palm fragment collides and scars through.
uint32_t palm_sector_hidden_sections(const Entity &entity);
Vec3 palm_sector_position(const World &world, const Entity &entity);
// A tower section clone's husk pivot, else the entity position.
Vec3 section_clone_position(const World &world, const Entity &entity);
} // namespace opennova::world
