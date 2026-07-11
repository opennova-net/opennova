#include <opennova/retail_hook/buffer_memory.h>
#include <opennova/retail_hook/parity_adapter.h>
#include <opennova/retail_hook/validation_session.h>
#include <opennova/retail_abi/jo_1_7_5_7/layout.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace hook = opennova::retail_hook;
namespace abi = opennova::retail::jo_1_7_5_7;
using opennova::retail::Address32;

class UncertainWriteMemory final : public hook::IMemory {
public:
    enum class FailureMode {
        return_false_after_write,
        fail_verification_read,
    };

    UncertainWriteMemory(hook::ProcessAddress base,
                         std::vector<std::byte> bytes,
                         FailureMode mode)
        : backing_(base, std::move(bytes)), mode_(mode) {}

    bool readable(hook::ProcessAddress address,
                  std::size_t size) const noexcept override {
        return backing_.readable(address, size);
    }

    bool read(hook::ProcessAddress address,
              void* destination,
              std::size_t size) const noexcept override {
        if (fail_verification_read_ && address == last_write_address_ &&
            size == last_write_size_) {
            fail_verification_read_ = false;
            return false;
        }
        return backing_.read(address, destination, size);
    }

    bool write(hook::ProcessAddress address,
               const void* source,
               std::size_t size) noexcept override {
        if (!backing_.write(address, source, size)) return false;
        last_write_address_ = address;
        last_write_size_ = size;
        if (mode_ == FailureMode::return_false_after_write) return false;
        fail_verification_read_ = true;
        return true;
    }

    const hook::BufferMemory& backing() const noexcept { return backing_; }

private:
    hook::BufferMemory backing_;
    FailureMode mode_;
    mutable bool fail_verification_read_{};
    hook::ProcessAddress last_write_address_{};
    std::size_t last_write_size_{};
};

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

static bool near(double actual, double expected,
                 double tolerance = 0.00001) {
    return std::abs(actual - expected) <= tolerance;
}

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

    std::array<abi::EntityPool, 5> pools{};
    const std::array<std::uint32_t, 5> strides{
        904, 1360, 812, 988, 988};
    const std::array<std::uint32_t, 5> capacities{
        256, 1200, 1200, 768, 128};
    for (std::size_t index = 0; index < pools.size(); ++index) {
        pools[index].data = Address32<std::byte>{
            static_cast<std::uint32_t>(
                kPlayerData + index * 0x00100000)};
        pools[index].element_size = strides[index];
        pools[index].capacity = capacities[index];
        put(
            memory,
            pool_address +
                static_cast<hook::ProcessAddress>(
                    index * sizeof(abi::EntityPool)),
            pools[index]);
    }
    pools[0].used = 3;
    put(memory, pool_address, pools[0]);
    abi::EntityPool& pool = pools[0];

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

    abi::PlayerEntity ai{};
    ai.position = {0x00040000, 0x00050000, 0x00060000};
    std::memcpy(ai.name, "ASH_G3D", sizeof("ASH_G3D"));
    ai.health = 100;
    ai.team = 2;
    put(memory, kPlayerData + 2 * sizeof(abi::PlayerEntity), ai);

    abi::WeaponDef weapon{};
    std::memcpy(weapon.name, "WPN_M4AUTO", sizeof("WPN_M4AUTO"));
    weapon.special_hold = 17;
    weapon.attack_anim = 23;
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
    CHECK(snapshot.player_pool_used == 3);
    CHECK(snapshot.player_pool_capacity == 256);
    CHECK(snapshot.pool_descriptors.size() == 5);
    if (snapshot.pool_descriptors.size() == 5) {
        CHECK(snapshot.pool_descriptors[0].element_size == 904);
        CHECK(snapshot.pool_descriptors[1].capacity == 1200);
        CHECK(snapshot.pool_descriptors[3].element_size == 988);
        CHECK(snapshot.pool_descriptors[4].capacity == 128);
    }
    CHECK(snapshot.players.size() == 3);
    if (snapshot.players.size() == 3) {
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
            CHECK(observed_weapon.special_hold == weapon.special_hold);
            CHECK(observed_weapon.attack_anim == weapon.attack_anim);
            CHECK(observed_weapon.position_x == weapon.hip_pose.position.x);
            CHECK(observed_weapon.position_z == weapon.hip_pose.position.z);
            CHECK(observed_weapon.rotation_yaw_raw == weapon.hip_pose.rotation.yaw);
            CHECK(observed_weapon.alternate_position_x == weapon.aimed_pose.position.x);
            CHECK(observed_weapon.render_fov == 80.0f);
        }
        CHECK(!snapshot.players[1].is_local);
        CHECK(snapshot.players[1].team == 2);
        CHECK(snapshot.players[2].owner_connection_id == 0);
        CHECK(snapshot.players[2].name == "ASH_G3D");
    }

    const opennova::parity::ProducerIdentity retail_identity{
        opennova::parity::SourceKind::retail,
        opennova::parity::RunRole::host,
        "retail-test",
    };
    const opennova::parity::FrameSnapshot parity_snapshot =
        hook::to_parity_snapshot(
            snapshot,
            retail_identity,
            opennova::parity::StateLane::authoritative,
            10,
            20,
            30);
    CHECK(parity_snapshot.identity.source ==
          opennova::parity::SourceKind::retail);
    CHECK(parity_snapshot.identity.stream_id == "retail-test");
    CHECK(parity_snapshot.frame_index == 10);
    CHECK(parity_snapshot.pools.size() == 5);
    CHECK(parity_snapshot.pools[0].base_address == kPlayerData);
    CHECK(parity_snapshot.entities.size() == 3);
    CHECK(parity_snapshot.entities[0].identity.name == "LocalProbe");
    CHECK(parity_snapshot.entities[0].armor == 25);
    CHECK(parity_snapshot.entities[0].kind ==
          opennova::parity::EntityKind::player);
    CHECK(parity_snapshot.entities[2].kind ==
          opennova::parity::EntityKind::organic);
    CHECK(parity_snapshot.player.has_value());
    CHECK(parity_snapshot.weapon.has_value());
    if (parity_snapshot.player.has_value()) {
        CHECK(parity_snapshot.player->armor == 25);
    }
    if (parity_snapshot.weapon.has_value()) {
        CHECK(parity_snapshot.weapon->special_hold == weapon.special_hold);
        CHECK(parity_snapshot.weapon->attack_anim == weapon.attack_anim);
        CHECK(near(parity_snapshot.weapon->primary.x, 9.07));
        CHECK(near(parity_snapshot.weapon->primary.y, 20.74));
        CHECK(near(parity_snapshot.weapon->primary.z, -183.0));
        CHECK(near(parity_snapshot.weapon->alternate.x, -44.98));
        CHECK(near(parity_snapshot.weapon->alternate.y, 44.05));
        CHECK(near(parity_snapshot.weapon->alternate.z, -162.0));
    }

    hook::CaptureCheckpointTracker checkpoint_tracker;
    opennova::parity::FrameSnapshot pre_mission{};
    pre_mission.identity = retail_identity;
    pre_mission.lane = opennova::parity::StateLane::authoritative;
    pre_mission.frame_index = 1;
    CHECK(!checkpoint_tracker.accepted_frame(pre_mission).has_value());
    pre_mission.frame_index = 2;
    pre_mission.player = opennova::parity::PlayerState{};
    pre_mission.player->present = true;
    CHECK(!checkpoint_tracker.accepted_frame(pre_mission).has_value());
    pre_mission.frame_index = 3;
    pre_mission.weapon = opennova::parity::WeaponState{};
    pre_mission.weapon->present = true;
    const auto capture_start =
        checkpoint_tracker.accepted_frame(pre_mission);
    CHECK(capture_start.has_value());
    if (capture_start.has_value()) {
        CHECK(capture_start->name == "capture-start");
        CHECK(capture_start->occurrence == 0);
        CHECK(capture_start->frame_index == 3);
    }
    pre_mission.frame_index = 4;
    pre_mission.player.reset();
    pre_mission.weapon.reset();
    CHECK(!checkpoint_tracker.accepted_frame(pre_mission).has_value());
    const auto capture_end = checkpoint_tracker.finish();
    CHECK(capture_end.has_value());
    if (capture_end.has_value()) {
        CHECK(capture_end->name == "capture-end");
        CHECK(capture_end->occurrence == 0);
        CHECK(capture_end->frame_index == 3);
    }

    // Writes are denied by the session interface unless opted into explicitly.
    hook::Mutation health_change = hook::SetPlayerHealth{0, 42, 90, 75};
    hook::MutationResult denied = opened.session->apply(health_change);
    CHECK(denied.check.error == hook::ValidationError::writes_disabled);
    CHECK(denied.audit.kind == hook::MutationKind::player_health);
    CHECK(denied.audit.stage == hook::MutationStage::rejected);
    CHECK(denied.audit.target_address == 0);
    CHECK(get<abi::PlayerEntity>(memory, kPlayerData).health == 90);

    // An opted-in fixture mutation checks identity and old value, writes one
    // named field, then verifies it through the memory seam.
    hook::OpenResult writable = hook::ValidationSession::open(
        memory, kImageBase, identity, profile, hook::SessionOptions{true});
    CHECK(writable);
    hook::MutationResult changed = writable.session->apply(health_change);
    CHECK(changed);
    CHECK(changed.audit.kind == hook::MutationKind::player_health);
    CHECK(changed.audit.stage == hook::MutationStage::verified);
    CHECK(changed.audit.target_address ==
          kPlayerData + offsetof(abi::PlayerEntity, health));
    const auto& audited_health =
        std::get<hook::SetPlayerHealth>(changed.audit.request);
    CHECK(audited_health.expected_value == 90);
    CHECK(audited_health.value == 75);
    const opennova::parity::MutationAudit parity_audit =
        hook::to_parity_mutation_audit(
            changed, retail_identity, 40, 50);
    CHECK(parity_audit.identity.stream_id == "retail-test");
    CHECK(parity_audit.simulation_tick == 40);
    CHECK(parity_audit.timestamp_ns == 50);
    CHECK(parity_audit.operation == "set_player_health");
    CHECK(parity_audit.result ==
          opennova::parity::MutationResult::applied);
    CHECK(std::get<std::int64_t>(parity_audit.before) == 90);
    CHECK(std::get<std::int64_t>(parity_audit.after) == 75);
    CHECK(get<abi::PlayerEntity>(memory, kPlayerData).health == 75);
    hook::MutationResult stale = writable.session->apply(health_change);
    CHECK(stale.check.error == hook::ValidationError::mutation_precondition_failed);
    CHECK(stale.audit.kind == hook::MutationKind::player_health);
    CHECK(stale.audit.stage == hook::MutationStage::rejected);

    hook::MutationResult team_changed = writable.session->apply(
        hook::Mutation{hook::SetPlayerTeam{0, 42, 1, 3}});
    CHECK(team_changed);
    CHECK(team_changed.audit.kind == hook::MutationKind::player_team);
    CHECK(team_changed.audit.stage == hook::MutationStage::verified);
    CHECK(team_changed.audit.target_address ==
          kPlayerData + offsetof(abi::PlayerEntity, team));
    CHECK(get<abi::PlayerEntity>(memory, kPlayerData).team == 3);

    hook::MutationResult adm_changed = writable.session->apply(
        hook::Mutation{hook::SetEquippedAdmIndex{0, 42, 9, 12}});
    CHECK(adm_changed);
    CHECK(adm_changed.audit.kind ==
          hook::MutationKind::equipped_adm_index);
    CHECK(adm_changed.audit.stage == hook::MutationStage::verified);
    CHECK(adm_changed.audit.target_address ==
          kPlayerData + offsetof(abi::PlayerEntity, equipped_adm_index));
    CHECK(get<abi::PlayerEntity>(
              memory, kPlayerData).equipped_adm_index == 12);

    // Active-weapon pose writes are pinned to both the player identity and the
    // currently equipped definition pointer before touching the witnessed pose.
    hook::SetActiveWeaponPose hip_change{};
    hip_change.player_slot = 0;
    hip_change.expected_owner_connection_id = 42;
    hip_change.expected_weapon_address = kWeapon;
    hip_change.pose = hook::ActiveWeaponPose::hip;
    hip_change.expected_value = hook::WeaponPoseValue{
        weapon.hip_pose.position.x,
        weapon.hip_pose.position.y,
        weapon.hip_pose.position.z,
        weapon.hip_pose.rotation.yaw,
        weapon.hip_pose.rotation.pitch,
        weapon.hip_pose.rotation.roll,
    };
    hip_change.value = hook::WeaponPoseValue{
        100.0f, 200.0f, -300.0f, 0x01020304, -0x01020304, 0x11111111,
    };
    hook::MutationResult hip_changed =
        writable.session->apply(hook::Mutation{hip_change});
    CHECK(hip_changed);
    CHECK(hip_changed.audit.kind == hook::MutationKind::active_weapon_pose);
    CHECK(hip_changed.audit.stage == hook::MutationStage::verified);
    CHECK(hip_changed.audit.active_weapon_address == kWeapon);
    CHECK(hip_changed.audit.target_address ==
          kWeapon + offsetof(abi::WeaponDef, hip_pose));
    const abi::WeaponDef changed_weapon =
        get<abi::WeaponDef>(memory, kWeapon);
    CHECK(changed_weapon.hip_pose.position.x == 100.0f);
    CHECK(changed_weapon.hip_pose.rotation.yaw == 0x01020304);

    hip_change.expected_weapon_address = kWeapon + 4;
    hook::MutationResult wrong_weapon =
        writable.session->apply(hook::Mutation{hip_change});
    CHECK(wrong_weapon.check.error ==
          hook::ValidationError::mutation_precondition_failed);
    CHECK(wrong_weapon.audit.stage == hook::MutationStage::rejected);

    hook::SetActiveWeaponFov fov_change{};
    fov_change.player_slot = 0;
    fov_change.expected_owner_connection_id = 42;
    fov_change.expected_weapon_address = kWeapon;
    fov_change.expected_value = 80.0f;
    fov_change.value = 72.5f;
    hook::MutationResult fov_changed =
        writable.session->apply(hook::Mutation{fov_change});
    CHECK(fov_changed);
    CHECK(fov_changed.audit.kind ==
          hook::MutationKind::active_weapon_render_fov);
    CHECK(fov_changed.audit.stage == hook::MutationStage::verified);
    CHECK(fov_changed.audit.active_weapon_address == kWeapon);
    CHECK(fov_changed.audit.target_address ==
          kWeapon + offsetof(abi::WeaponDef, render_fov));
    CHECK(get<abi::WeaponDef>(memory, kWeapon).render_fov == 72.5f);

    fov_change.expected_value = 80.0f;
    fov_change.value = 65.0f;
    hook::MutationResult stale_fov =
        writable.session->apply(hook::Mutation{fov_change});
    CHECK(stale_fov.check.error ==
          hook::ValidationError::mutation_precondition_failed);
    CHECK(get<abi::WeaponDef>(memory, kWeapon).render_fov == 72.5f);

    // Once the memory adapter is called, a false return does not prove that
    // the target was untouched. Preserve that uncertainty in both the typed
    // result and portable trace instead of reporting a safe rejection.
    UncertainWriteMemory attempted_memory(
        kImageBase,
        memory.bytes(),
        UncertainWriteMemory::FailureMode::return_false_after_write);
    hook::OpenResult attempted_session = hook::ValidationSession::open(
        attempted_memory,
        kImageBase,
        identity,
        profile,
        hook::SessionOptions{true});
    CHECK(attempted_session);
    hook::MutationResult attempted = attempted_session.session->apply(
        hook::Mutation{hook::SetPlayerHealth{0, 42, 75, 70}});
    CHECK(attempted.check.error == hook::ValidationError::memory_write_failed);
    CHECK(attempted.audit.stage == hook::MutationStage::write_attempted);
    CHECK(get<abi::PlayerEntity>(
              attempted_memory.backing(), kPlayerData).health == 70);
    const opennova::parity::MutationAudit attempted_audit =
        hook::to_parity_mutation_audit(
            attempted, retail_identity, 60, 70);
    CHECK(attempted_audit.result ==
          opennova::parity::MutationResult::unverified);

    UncertainWriteMemory verification_memory(
        kImageBase,
        memory.bytes(),
        UncertainWriteMemory::FailureMode::fail_verification_read);
    hook::OpenResult verification_session = hook::ValidationSession::open(
        verification_memory,
        kImageBase,
        identity,
        profile,
        hook::SessionOptions{true});
    CHECK(verification_session);
    hook::MutationResult unverified = verification_session.session->apply(
        hook::Mutation{hook::SetPlayerHealth{0, 42, 75, 65}});
    CHECK(unverified.check.error == hook::ValidationError::memory_read_failed);
    CHECK(unverified.audit.stage == hook::MutationStage::written);
    CHECK(get<abi::PlayerEntity>(
              verification_memory.backing(), kPlayerData).health == 65);
    const opennova::parity::MutationAudit unverified_audit =
        hook::to_parity_mutation_audit(
            unverified, retail_identity, 80, 90);
    CHECK(unverified_audit.result ==
          opennova::parity::MutationResult::unverified);

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

    pools[3].element_size += 4;
    put(
        memory,
        pool_address + 3 * sizeof(abi::EntityPool),
        pools[3]);
    snapshot = opened.session->sample();
    CHECK(!snapshot);
    CHECK(snapshot.check.error ==
          hook::ValidationError::element_size_mismatch);
    pools[3].element_size = strides[3];
    put(
        memory,
        pool_address + 3 * sizeof(abi::EntityPool),
        pools[3]);

    // A bad non-local weapon pointer preserves the useful entity snapshot and
    // carries a per-player diagnostic instead of discarding every observation.
    remote.weapon_def = Address32<abi::WeaponDef>{0xFFFFFF00};
    put(memory, kPlayerData + sizeof(abi::PlayerEntity), remote);
    snapshot = opened.session->sample();
    CHECK(snapshot);
    CHECK(snapshot.players.size() == 3);
    if (snapshot.players.size() == 3) {
        CHECK(!snapshot.players[1].weapon_check);
        CHECK(!snapshot.players[1].weapon.has_value());
    }

    pool.used = 257;
    put(memory, pool_address, pool);
    snapshot = opened.session->sample();
    CHECK(snapshot.check.error == hook::ValidationError::corrupt_pool_counts);

    std::printf("retail_hook_validation: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
