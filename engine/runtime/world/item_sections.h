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
// tower clone's husk pivot under the cveh / chel / tank / cpln render tag
// (death_section_pivot) or the section offset under the bldg render tag
// (bldg_section_pivot).
uint32_t item_hidden_sections(const Entity &entity);
Vec3 item_section_render_position(const World &world, const Entity &entity);
// CTerrainMap_BuildSectorTransformMatrices's sections and origin over the
// palm state, the callback a palm fragment collides and scars through.
uint32_t palm_sector_hidden_sections(const Entity &entity);
Vec3 palm_sector_position(const World &world, const Entity &entity);
// The graphic-model COBJ origin the bldg render row translates an entity's
// draw by (negated, in the entity's own frame): the section its byte +0x2C1
// (death_anim_state's second byte) names, null for section 0, an index past
// the model's sections or another render tag.
const std::array<int32_t, 3> *bldg_section_origin_q16(const World &world, const Entity &entity);
// A tower section clone's husk pivot under the cveh / chel / tank / cpln
// render tag, the graphic model's section offset (+0x2C1's section) under the
// bldg render tag, else the entity position. The draw's alone: collision
// never pivots.
Vec3 section_clone_position(const World &world, const Entity &entity);
} // namespace opennova::world
