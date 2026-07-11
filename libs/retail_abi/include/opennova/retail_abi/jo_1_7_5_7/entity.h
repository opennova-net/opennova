#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <opennova/retail_abi/address32.h>
#include <opennova/retail_abi/fixed_types.h>

namespace opennova::retail::jo_1_7_5_7 {

struct WeaponDef;
struct PlayerEntity;

struct EntityPool {
    Address32<std::byte> data;
    std::uint32_t element_size;
    std::uint32_t used;
    std::uint32_t capacity;
};

enum class EntityPoolKind : std::uint32_t {
    organic = 0,
    vehicle_or_item = 1,
    static_or_building = 2,
    pool_3 = 3,
    effects = 4,
};

// Exact-size sparse overlay of the recovered organic/player record. Every
// named field is witnessed; uncharacterized spans remain explicit. This is
// retail process memory, not the semantic opennova::world::Entity model.
struct PlayerEntity {
    std::uint8_t unknown_0000[0x4];
    FixedVec3 position;
    BamAngles3 orientation;
    std::uint8_t unknown_001c[0x8];
    std::uint32_t flags;
    // May identify an entity in any pool, so its target layout is deliberately
    // not narrowed to PlayerEntity.
    Address32<std::byte> ground_entity;
    std::uint8_t unknown_002c[0x2];
    std::uint16_t ssn;
    std::uint8_t unknown_0030[0x3C];
    FixedVec3 camera_offset;

    std::uint32_t owner_connection_id;
    std::uint32_t dcb_id;
    std::uint8_t unknown_0080[0x74];

    char name[16];
    std::uint8_t unknown_0104[0x18];
    std::uint16_t command_group;
    std::int16_t health;
    std::int16_t armor;
    std::uint8_t unknown_0122[0x3A];
    std::uint16_t net_id;
    std::uint8_t unknown_015e[0x4];
    std::int16_t team;
    std::uint8_t unknown_0164[0x130];

    std::uint8_t player_class;
    std::uint8_t unknown_0295[0x3];
    Address32<WeaponDef> weapon_def;
    std::uint8_t unknown_029c[0x14];
    std::uint8_t equipped_adm_index;
    std::uint8_t unknown_02b1[0xC3];
    std::uint8_t animation_slot;
    std::uint8_t unknown_0375[0x13];
};

static_assert(sizeof(EntityPool) == 0x10);
static_assert(alignof(EntityPool) == alignof(std::uint32_t));
static_assert(offsetof(EntityPool, data) == 0x0);
static_assert(offsetof(EntityPool, element_size) == 0x4);
static_assert(offsetof(EntityPool, used) == 0x8);
static_assert(offsetof(EntityPool, capacity) == 0xC);
static_assert(std::is_standard_layout_v<EntityPool>);
static_assert(std::is_trivially_copyable_v<EntityPool>);

static_assert(sizeof(EntityPoolKind) == sizeof(std::uint32_t));

static_assert(sizeof(PlayerEntity) == 0x388);
static_assert(alignof(PlayerEntity) == alignof(std::uint32_t));
static_assert(offsetof(PlayerEntity, unknown_0000) == 0x000);
static_assert(offsetof(PlayerEntity, position) == 0x004);
static_assert(offsetof(PlayerEntity, orientation) == 0x010);
static_assert(offsetof(PlayerEntity, unknown_001c) == 0x01C);
static_assert(offsetof(PlayerEntity, flags) == 0x024);
static_assert(offsetof(PlayerEntity, ground_entity) == 0x028);
static_assert(offsetof(PlayerEntity, unknown_002c) == 0x02C);
static_assert(offsetof(PlayerEntity, ssn) == 0x02E);
static_assert(offsetof(PlayerEntity, unknown_0030) == 0x030);
static_assert(offsetof(PlayerEntity, camera_offset) == 0x06C);
static_assert(offsetof(PlayerEntity, owner_connection_id) == 0x078);
static_assert(offsetof(PlayerEntity, dcb_id) == 0x07C);
static_assert(offsetof(PlayerEntity, unknown_0080) == 0x080);
static_assert(offsetof(PlayerEntity, name) == 0x0F4);
static_assert(offsetof(PlayerEntity, unknown_0104) == 0x104);
static_assert(offsetof(PlayerEntity, command_group) == 0x11C);
static_assert(offsetof(PlayerEntity, health) == 0x11E);
static_assert(offsetof(PlayerEntity, armor) == 0x120);
static_assert(offsetof(PlayerEntity, unknown_0122) == 0x122);
static_assert(offsetof(PlayerEntity, net_id) == 0x15C);
static_assert(offsetof(PlayerEntity, unknown_015e) == 0x15E);
static_assert(offsetof(PlayerEntity, team) == 0x162);
static_assert(offsetof(PlayerEntity, unknown_0164) == 0x164);
static_assert(offsetof(PlayerEntity, player_class) == 0x294);
static_assert(offsetof(PlayerEntity, unknown_0295) == 0x295);
static_assert(offsetof(PlayerEntity, weapon_def) == 0x298);
static_assert(offsetof(PlayerEntity, unknown_029c) == 0x29C);
static_assert(offsetof(PlayerEntity, equipped_adm_index) == 0x2B0);
static_assert(offsetof(PlayerEntity, unknown_02b1) == 0x2B1);
static_assert(offsetof(PlayerEntity, animation_slot) == 0x374);
static_assert(offsetof(PlayerEntity, unknown_0375) == 0x375);
static_assert(std::is_standard_layout_v<PlayerEntity>);
static_assert(std::is_trivially_copyable_v<PlayerEntity>);

}  // namespace opennova::retail::jo_1_7_5_7
