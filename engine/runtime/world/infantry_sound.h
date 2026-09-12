#pragma once
#include <cstdint>
namespace opennova::world {
class World;
// stance_bits retain MoveOrder bits 8/9: prone=1, crouch=2.
void emit_stance_change_sound(World &world, uint16_t source, const int32_t pos[3],
    uint8_t &previous, uint8_t stance_bits, uint32_t flags, bool parent_has_definition);
}
