#include <opennova/retail_hook/buffer_memory.h>
#include <opennova/retail_hook/validation_session.h>
#include <opennova/retail_abi/jo_1_7_5_7/layout.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hook = opennova::retail_hook;
namespace abi = opennova::retail::jo_1_7_5_7;
using opennova::retail::Address32;

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

template <typename T>
static void put(hook::BufferMemory& memory, hook::ProcessAddress address, const T& value) {
    CHECK(memory.write(address, &value, sizeof(value)));
}

template <typename T>
static T get(const hook::BufferMemory& memory, hook::ProcessAddress address) {
    T value{};
    CHECK(memory.read(address, &value, sizeof(value)));
    return value;
}

int main() {
    constexpr hook::ProcessAddress kImageBase = 0x00400000;
    constexpr std::size_t kImageSize = 0x00780000;
    constexpr hook::ProcessAddress kPlayerData = kImageBase + 0x00010000;
    constexpr hook::ProcessAddress kWeapon = kImageBase + 0x00020000;

    // The local fixture adapter observes the same checked-memory seam as the
    // injected-process adapter, including zero-length and overflow edges.
    {
        hook::BufferMemory small{0x1000, 16};
        std::uint32_t value = 0x12345678;
        CHECK(small.readable(0x1000, sizeof(value)));
        CHECK(!small.readable(0x0FFF, sizeof(value)));
        CHECK(!small.readable(0x100F, sizeof(value)));
        CHECK(small.write(0x1004, &value, sizeof(value)));
        CHECK(get<std::uint32_t>(small, 0x1004) == value);
    }

    hook::BufferMemory memory{kImageBase, kImageSize};
    const hook::BuildProfile& profile = hook::jo_1_7_5_7_profile();
    const hook::ProcessAddress pool_address =
        kImageBase + profile.symbols.entity_pools;
    const hook::ProcessAddress local_global =
        kImageBase + profile.symbols.local_player_entity;

    abi::EntityPool pool{};
    pool.data = Address32<std::byte>{kPlayerData};
    pool.element_size = sizeof(abi::PlayerEntity);
    pool.used = 2;
    pool.capacity = 4;
    put(memory, pool_address, pool);

    abi::PlayerEntity local{};
    local.position = {0x00100000, -0x00080000, 0x00050000};
    local.orientation = {0x40000000, -0x10000000, 0};
    local.flags = 0x104;
    local.ground_entity = Address32<std::byte>{kImageBase + 0x00030000};
    local.ssn = 0x1234;
    local.camera_offset = {0x1000, 0x2000, 0x3000};
    local.owner_connection_id = 42;
    local.dcb_id = 7;
    std::memcpy(local.name, "LocalProbe", sizeof("LocalProbe"));
    local.health = 90;
    local.armor = 25;
    local.command_group = 3;
    local.net_id = 0x5678;
    local.team = 1;
    local.player_class = 8;
    local.weapon_def = Address32<abi::WeaponDef>{kWeapon};
    local.equipped_adm_index = 9;
    local.animation_slot = 3;
    put(memory, kPlayerData, local);

    abi::PlayerEntity remote{};
    remote.position = {-0x00010000, 0x00020000, 0x00030000};
    remote.orientation = {-0x40000000, 0x08000000, 0};
    remote.owner_connection_id = 84;
    std::memcpy(remote.name, "RemoteProbe", sizeof("RemoteProbe"));
    remote.health = 55;
    remote.team = 2;
    remote.player_class = 6;
    remote.equipped_adm_index = 21;
    put(memory, kPlayerData + sizeof(abi::PlayerEntity), remote);

    abi::WeaponDef weapon{};
    std::memcpy(weapon.name, "WPN_M4AUTO", sizeof("WPN_M4AUTO"));
    weapon.hip_pose.position = {9.07f * 256.0f, 20.74f * 256.0f, -183.0f * 256.0f};
    weapon.hip_pose.rotation = {0x016C16C0, -0x005B05B0, 0};
    weapon.aimed_pose.position = {-44.98f * 256.0f, 44.05f * 256.0f, -162.0f * 256.0f};
    weapon.aimed_pose.rotation = {0, 0, 0};
    weapon.render_fov = 80.0f;
    put(memory, kWeapon, weapon);

    const Address32<abi::PlayerEntity> local_pointer{kPlayerData};
    put(memory, local_global, local_pointer);

    hook::ExecutableIdentity identity = profile.identity;
    identity.image_size = static_cast<std::uint32_t>(kImageSize);
    hook::OpenResult opened = hook::ValidationSession::open(
        memory, kImageBase, identity, profile);
    CHECK(opened);
    CHECK(opened.session != nullptr);

    hook::ValidationSnapshot snapshot = opened.session->sample();
    CHECK(snapshot);
    CHECK(snapshot.player_pool_used == 2);
    CHECK(snapshot.player_pool_capacity == 4);
    CHECK(snapshot.players.size() == 2);
    if (snapshot.players.size() == 2) {
        const hook::PlayerObservation& observed_local = snapshot.players[0];
        CHECK(observed_local.is_local);
        CHECK(observed_local.address == kPlayerData);
        CHECK(observed_local.name == "LocalProbe");
        CHECK(observed_local.ground_entity_address == kImageBase + 0x00030000);
        CHECK(observed_local.owner_connection_id == 42);
        CHECK(observed_local.dcb_id == 7);
        CHECK(observed_local.ssn == 0x1234);
        CHECK(observed_local.net_id == 0x5678);
        CHECK(observed_local.command_group == 3);
        CHECK(observed_local.position_x_raw == 0x00100000);
        CHECK(observed_local.camera_offset_z_raw == 0x3000);
        CHECK(observed_local.yaw_raw == 0x40000000);
        CHECK(observed_local.flags == 0x104);
        CHECK(observed_local.health == 90);
        CHECK(observed_local.armor == 25);
        CHECK(observed_local.weapon_check);
        CHECK(observed_local.weapon.has_value());
        if (observed_local.weapon.has_value()) {
            const hook::WeaponObservation& observed_weapon = *observed_local.weapon;
            CHECK(observed_weapon.address == kWeapon);
            CHECK(observed_weapon.name == "WPN_M4AUTO");
            CHECK(observed_weapon.position_x == weapon.hip_pose.position.x);
            CHECK(observed_weapon.position_z == weapon.hip_pose.position.z);
            CHECK(observed_weapon.rotation_yaw_raw == weapon.hip_pose.rotation.yaw);
            CHECK(observed_weapon.alternate_position_x == weapon.aimed_pose.position.x);
            CHECK(observed_weapon.render_fov == 80.0f);
        }
        CHECK(!snapshot.players[1].is_local);
        CHECK(snapshot.players[1].team == 2);
    }

    // Writes are denied by the session interface unless opted into explicitly.
    hook::Mutation health_change = hook::SetPlayerHealth{0, 42, 90, 75};
    hook::CheckResult denied = opened.session->apply(health_change);
    CHECK(denied.error == hook::ValidationError::writes_disabled);
    CHECK(get<abi::PlayerEntity>(memory, kPlayerData).health == 90);

    // An opted-in fixture mutation checks identity and old value, writes one
    // named field, then verifies it through the memory seam.
    hook::OpenResult writable = hook::ValidationSession::open(
        memory, kImageBase, identity, profile, hook::SessionOptions{true});
    CHECK(writable);
    CHECK(writable.session->apply(health_change));
    CHECK(get<abi::PlayerEntity>(memory, kPlayerData).health == 75);
    hook::CheckResult stale = writable.session->apply(health_change);
    CHECK(stale.error == hook::ValidationError::mutation_precondition_failed);

    // Unsupported builds never reach typed memory.
    hook::ExecutableIdentity wrong_identity = identity;
    wrong_identity.sha256[0] ^= 0xFF;
    hook::OpenResult rejected = hook::ValidationSession::open(
        memory, kImageBase, wrong_identity, profile);
    CHECK(!rejected);
    CHECK(rejected.check.error == hook::ValidationError::unsupported_executable);

    // A stride disagreement is a validation result, not a best-effort cast.
    pool.element_size = sizeof(abi::PlayerEntity) + 1;
    put(memory, pool_address, pool);
    snapshot = opened.session->sample();
    CHECK(!snapshot);
    CHECK(snapshot.check.error == hook::ValidationError::element_size_mismatch);
    pool.element_size = sizeof(abi::PlayerEntity);
    put(memory, pool_address, pool);

    // A bad non-local weapon pointer preserves the useful entity snapshot and
    // carries a per-player diagnostic instead of discarding every observation.
    remote.weapon_def = Address32<abi::WeaponDef>{0xFFFFFF00};
    put(memory, kPlayerData + sizeof(abi::PlayerEntity), remote);
    snapshot = opened.session->sample();
    CHECK(snapshot);
    CHECK(snapshot.players.size() == 2);
    if (snapshot.players.size() == 2) {
        CHECK(!snapshot.players[1].weapon_check);
        CHECK(!snapshot.players[1].weapon.has_value());
    }

    pool.used = 5;
    put(memory, pool_address, pool);
    snapshot = opened.session->sample();
    CHECK(snapshot.check.error == hook::ValidationError::corrupt_pool_counts);

    std::printf("retail_hook_validation: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
