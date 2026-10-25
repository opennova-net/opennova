// Section-based item callbacks and their independently moving fragments.
// [orig: palm @ 0x53C4C0; psec @ 0x53BE10]
#pragma once
#include <runtime/world/destruction.h>
namespace opennova::world {
class IItemPieceSpawner {
public:
    virtual ~IItemPieceSpawner() = default;
    // The callback supplies the retail template. The definition selects its
    // allocation pool; MissionKernel binds the model before returning it. It
    // publishes no proximity table and builds no list: the caller runs its own
    // tail (a palm fragment's own list; none for a section piece).
    virtual EntityHandle spawn_item_piece(const Entity &seed) = 0;
};
bool tower_item_event(World &world, Entity &entity, int phase, ItemHitContext hit);
void palm_item_event(World &world, Entity &entity, int phase, ItemHitContext hit);
bool tick_item_section_motion(World &world, Entity &entity);
// The psec row's mover over an entity's words: the savedLivePose stamp, then the fall by its
// velocity words (+0x98 / +0x9C / +0xA0, which the caller passes: a palm piece's motor words,
// a person body's InfantryState::vel) onto the terrain plus a quarter unit, the spin by its
// +0x270 word while airborne, and the Flags 0x20000 clear.
// [orig: Entity_UpdatePhysicsStep @ 0x53BE10]
void update_physics_step(World &world, Entity &entity, int32_t &vel_x, int32_t &vel_y,
        int32_t &slide_z);
// The draw's hidden sections and origin: the sector builder's for a psec /
// cesp render tag (palm_sections), else the section and piece masks (with the
// rotor / aux sections the chel render tag hides by rotor speed and the
// airborne flag, chel_rotor_hidden_sections) and a tower clone's husk pivot
// under the cveh / chel / tank / cpln render tag (death_section_pivot) or the
// section offset under the bldg render tag (bldg_section_pivot).
uint32_t chel_rotor_hidden_sections(const Entity &entity);
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
