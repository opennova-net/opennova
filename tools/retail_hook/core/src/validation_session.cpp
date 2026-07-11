#include <opennova/retail_hook/validation_session.h>

#include <opennova/retail_abi/address32.h>
#include <opennova/retail_abi/jo_1_7_5_7/entity.h>
#include <opennova/retail_abi/jo_1_7_5_7/weapon.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace opennova::retail_hook {
namespace {

namespace abi = opennova::retail::jo_1_7_5_7;
using opennova::retail::Address32;

constexpr std::uint32_t kHardMaximumPlayerCapacity = 65536;

static_assert(std::is_same_v<decltype(abi::PlayerEntity::owner_connection_id),
                             std::uint32_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::dcb_id), std::uint32_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::ssn), std::uint16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::net_id), std::uint16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::health), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::armor), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::team), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::player_class), std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::equipped_adm_index),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::animation_slot),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::WeaponDef::hip_pose.position.x), float>);
static_assert(std::is_same_v<
              decltype(abi::WeaponDef::aimed_pose.position.x),
              float>);

[[nodiscard]] bool digest_is_zero(const Sha256Digest& digest) noexcept {
    return std::all_of(digest.begin(), digest.end(), [](std::uint8_t byte) {
        return byte == 0;
    });
}

[[nodiscard]] bool checked_add(ProcessAddress base,
                               std::uint64_t offset,
                               ProcessAddress& result) noexcept {
    const auto sum = static_cast<std::uint64_t>(base) + offset;
    if (sum > std::numeric_limits<ProcessAddress>::max()) {
        return false;
    }
    result = static_cast<ProcessAddress>(sum);
    return true;
}

[[nodiscard]] bool checked_byte_count(std::uint32_t count,
                                      std::uint32_t element_size,
                                      std::size_t& result) noexcept {
    const auto bytes = static_cast<std::uint64_t>(count) * element_size;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(bytes);
    return true;
}

[[nodiscard]] bool checked_range(ProcessAddress start,
                                 std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }

    const auto last = static_cast<std::uint64_t>(start) + size - 1U;
    return last <= std::numeric_limits<ProcessAddress>::max();
}

[[nodiscard]] bool range_within_image(std::uint32_t rva,
                                      std::size_t size,
                                      std::uint32_t image_size) noexcept {
    if (image_size == 0) {
        return true;
    }
    const auto end = static_cast<std::uint64_t>(rva) + size;
    return end <= image_size;
}

[[nodiscard]] std::string player_context(std::uint32_t slot) {
    return "player slot " + std::to_string(slot);
}

template <typename T>
[[nodiscard]] CheckResult read_object(const IMemory& memory,
                                      ProcessAddress address,
                                      T& destination,
                                      const std::string& label) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "retail memory overlays must be trivially copyable");

    if (address == 0) {
        return CheckResult::failure(ValidationError::null_address,
                                    label + " resolved to a null address");
    }
    if (!checked_range(address, sizeof(T))) {
        return CheckResult::failure(ValidationError::address_overflow,
                                    label + " crosses the 32-bit address limit");
    }
    if (!memory.readable(address, sizeof(T))) {
        return CheckResult::failure(ValidationError::unreadable_memory,
                                    label + " is not readable");
    }
    if (!memory.read(address, &destination, sizeof(T))) {
        return CheckResult::failure(ValidationError::memory_read_failed,
                                    label + " changed or failed while reading");
    }
    return CheckResult::success();
}

template <typename T>
[[nodiscard]] CheckResult write_verified(IMemory& memory,
                                         ProcessAddress address,
                                         const T& value,
                                         MutationAudit& audit) {
    static_assert(std::is_trivially_copyable_v<T>);
    audit.target_address = address;
    audit.stage = MutationStage::validated;
    // IMemory::write returning false cannot prove the destination was left
    // untouched (the production adapter may fail after the copy), so record
    // that the write boundary was crossed before making the call.
    audit.stage = MutationStage::write_attempted;
    if (!memory.write(address, &value, sizeof(value))) {
        return CheckResult::failure(ValidationError::memory_write_failed,
                                    "memory write was attempted but did not report success");
    }
    audit.stage = MutationStage::written;

    T observed{};
    auto check = read_object(memory, address, observed, "mutated field");
    if (!check) {
        return check;
    }
    if (std::memcmp(&observed, &value, sizeof(value)) != 0) {
        return CheckResult::failure(
            ValidationError::write_verification_failed,
            "mutated field did not retain the requested value");
    }
    audit.stage = MutationStage::verified;
    return CheckResult::success();
}

[[nodiscard]] bool pose_matches(const abi::WeaponPose& actual,
                                const WeaponPoseValue& expected) noexcept {
    return actual.position.x == expected.position_x &&
           actual.position.y == expected.position_y &&
           actual.position.z == expected.position_z &&
           actual.rotation.yaw == expected.rotation_yaw_raw &&
           actual.rotation.pitch == expected.rotation_pitch_raw &&
           actual.rotation.roll == expected.rotation_roll_raw;
}

[[nodiscard]] abi::WeaponPose to_abi_pose(const WeaponPoseValue& value) noexcept {
    return abi::WeaponPose{
        {value.position_x, value.position_y, value.position_z},
        {value.rotation_yaw_raw,
         value.rotation_pitch_raw,
         value.rotation_roll_raw},
    };
}

[[nodiscard]] std::string bounded_name(const abi::PlayerEntity& entity) {
    const auto* characters = reinterpret_cast<const char*>(&entity.name);
    std::size_t length = 0;
    while (length < sizeof(entity.name) && characters[length] != '\0') {
        ++length;
    }
    return std::string(characters, length);
}

[[nodiscard]] std::string bounded_name(const abi::WeaponDef& weapon) {
    const auto* characters = reinterpret_cast<const char*>(&weapon.name);
    std::size_t length = 0;
    while (length < sizeof(weapon.name) && characters[length] != '\0') {
        ++length;
    }
    return std::string(characters, length);
}

struct LoadedPlayerPool {
    abi::EntityPool descriptor{};
    ProcessAddress data{};
    std::size_t used_bytes{};
};

using PoolDescriptors =
    std::array<abi::EntityPool, abi::kEntityPoolCount>;

}  // namespace

struct ValidationSession::Impl {
    IMemory* memory{};
    ProcessAddress image_base{};
    BuildProfile profile{};
    SessionOptions options{};
    ProcessAddress pool_descriptor_table{};
    ProcessAddress local_player_global{};

    [[nodiscard]] CheckResult load_pool_descriptors(
        PoolDescriptors& descriptors,
        std::vector<PoolDescriptorObservation>* observations = nullptr) const {
        auto check = read_object(
            *memory,
            pool_descriptor_table,
            descriptors,
            "entity pool descriptor table");
        if (!check) {
            return check;
        }

        if (observations != nullptr) {
            observations->clear();
            observations->reserve(descriptors.size());
        }
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const abi::EntityPool& descriptor = descriptors[index];
            const abi::EntityPoolShape expected =
                abi::kEntityPoolShapes[index];
            if (descriptor.element_size != expected.element_size) {
                std::ostringstream detail;
                detail << "pool " << index << " element size is "
                       << descriptor.element_size << ", expected "
                       << expected.element_size;
                return CheckResult::failure(
                    ValidationError::element_size_mismatch,
                    detail.str());
            }
            if (descriptor.capacity != expected.capacity) {
                std::ostringstream detail;
                detail << "pool " << index << " capacity is "
                       << descriptor.capacity << ", expected "
                       << expected.capacity;
                return CheckResult::failure(
                    ValidationError::implausible_pool_count,
                    detail.str());
            }
            if (descriptor.used > descriptor.capacity) {
                return CheckResult::failure(
                    ValidationError::corrupt_pool_counts,
                    "pool " + std::to_string(index) +
                        " used count exceeds its capacity");
            }
            if (descriptor.data.is_null()) {
                return CheckResult::failure(
                    ValidationError::null_address,
                    "pool " + std::to_string(index) +
                        " has a null data pointer");
            }
            std::size_t capacity_bytes{};
            if (!checked_byte_count(
                    descriptor.capacity,
                    descriptor.element_size,
                    capacity_bytes) ||
                !checked_range(
                    descriptor.data.value(), capacity_bytes)) {
                return CheckResult::failure(
                    ValidationError::address_overflow,
                    "pool " + std::to_string(index) +
                        " capacity range overflows");
            }
            if (observations != nullptr) {
                ProcessAddress descriptor_address{};
                if (!checked_add(
                        pool_descriptor_table,
                        index * sizeof(abi::EntityPool),
                        descriptor_address)) {
                    return CheckResult::failure(
                        ValidationError::address_overflow,
                        "pool descriptor address overflows");
                }
                observations->push_back(PoolDescriptorObservation{
                    static_cast<std::uint32_t>(index),
                    descriptor_address,
                    descriptor.data.value(),
                    descriptor.element_size,
                    descriptor.used,
                    descriptor.capacity,
                });
            }
        }
        return CheckResult::success();
    }

    [[nodiscard]] CheckResult validate_player_pool(
        const abi::EntityPool& descriptor,
        LoadedPlayerPool& loaded) const {
        loaded.descriptor = descriptor;

        if (loaded.descriptor.element_size != sizeof(abi::PlayerEntity)) {
            std::ostringstream detail;
            detail << "player pool element size is "
                   << loaded.descriptor.element_size << ", expected "
                   << sizeof(abi::PlayerEntity);
            return CheckResult::failure(ValidationError::element_size_mismatch,
                                        detail.str());
        }
        if (loaded.descriptor.used > loaded.descriptor.capacity) {
            return CheckResult::failure(
                ValidationError::corrupt_pool_counts,
                "player pool used count exceeds its capacity");
        }
        if (loaded.descriptor.used > profile.maximum_player_capacity ||
            loaded.descriptor.capacity > profile.maximum_player_capacity) {
            return CheckResult::failure(
                ValidationError::implausible_pool_count,
                "player pool count exceeds the profile's plausibility limit");
        }

        if (loaded.descriptor.capacity == 0) {
            loaded.data = 0;
            loaded.used_bytes = 0;
            return CheckResult::success();
        }
        if (loaded.descriptor.data.is_null()) {
            return CheckResult::failure(ValidationError::null_address,
                                        "allocated player pool has null data");
        }

        loaded.data = loaded.descriptor.data.value();
        std::size_t capacity_bytes{};
        if (!checked_byte_count(loaded.descriptor.capacity,
                                loaded.descriptor.element_size,
                                capacity_bytes) ||
            !checked_range(loaded.data, capacity_bytes)) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        "player pool capacity range overflows");
        }
        if (loaded.descriptor.used == 0) {
            loaded.used_bytes = 0;
            return CheckResult::success();
        }

        if (!checked_byte_count(loaded.descriptor.used,
                                loaded.descriptor.element_size,
                                loaded.used_bytes) ||
            !checked_range(loaded.data, loaded.used_bytes)) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        "player pool byte range overflows");
        }
        if (!memory->readable(loaded.data, loaded.used_bytes)) {
            return CheckResult::failure(ValidationError::unreadable_memory,
                                        "player pool data range is not readable");
        }

        return CheckResult::success();
    }

    [[nodiscard]] CheckResult load_player_pool(
        LoadedPlayerPool& loaded) const {
        PoolDescriptors descriptors{};
        auto check = load_pool_descriptors(descriptors);
        if (!check) {
            return check;
        }
        return validate_player_pool(
            descriptors[profile.player_pool_index], loaded);
    }

    [[nodiscard]] CheckResult player_address(const LoadedPlayerPool& loaded,
                                             std::uint32_t slot,
                                             ProcessAddress& result) const {
        if (slot >= loaded.descriptor.used) {
            return CheckResult::failure(ValidationError::invalid_player_slot,
                                        player_context(slot) + " is outside the pool");
        }
        const auto offset = static_cast<std::uint64_t>(slot) *
                            loaded.descriptor.element_size;
        if (!checked_add(loaded.data, offset, result) ||
            !checked_range(result, sizeof(abi::PlayerEntity))) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        player_context(slot) + " address overflows");
        }
        return CheckResult::success();
    }
};

const BuildProfile& jo_1_7_5_7_profile() {
    static const BuildProfile profile{
        "Joint Operations 1.7.5.7 (onHook patched)",
        ExecutableIdentity{
            Sha256Digest{
                0x9a, 0x10, 0x35, 0x44, 0x0a, 0x53, 0xaf, 0x20,
                0x57, 0xce, 0x09, 0x95, 0xac, 0x42, 0xdc, 0xed,
                0x84, 0x0d, 0x3b, 0x9f, 0xd5, 0x3c, 0x04, 0xdc,
                0x86, 0x04, 0x1a, 0x96, 0x2b, 0x84, 0xfe, 0x57,
            },
            0,
        },
        0x00400000,
        SymbolRvas{0x006892e0, 0x00775fc8},
        0,
        4096,
    };
    return profile;
}

const char* validation_error_name(ValidationError error) noexcept {
    switch (error) {
        case ValidationError::none:
            return "none";
        case ValidationError::invalid_profile:
            return "invalid_profile";
        case ValidationError::unsupported_executable:
            return "unsupported_executable";
        case ValidationError::address_overflow:
            return "address_overflow";
        case ValidationError::null_address:
            return "null_address";
        case ValidationError::unreadable_memory:
            return "unreadable_memory";
        case ValidationError::memory_read_failed:
            return "memory_read_failed";
        case ValidationError::invalid_probe:
            return "invalid_probe";
        case ValidationError::element_size_mismatch:
            return "element_size_mismatch";
        case ValidationError::corrupt_pool_counts:
            return "corrupt_pool_counts";
        case ValidationError::implausible_pool_count:
            return "implausible_pool_count";
        case ValidationError::writes_disabled:
            return "writes_disabled";
        case ValidationError::invalid_player_slot:
            return "invalid_player_slot";
        case ValidationError::mutation_precondition_failed:
            return "mutation_precondition_failed";
        case ValidationError::memory_write_failed:
            return "memory_write_failed";
        case ValidationError::write_verification_failed:
            return "write_verification_failed";
    }
    return "unknown";
}

bool CheckResult::ok() const noexcept {
    return error == ValidationError::none;
}

CheckResult::operator bool() const noexcept {
    return ok();
}

CheckResult CheckResult::success() {
    return {};
}

CheckResult CheckResult::failure(ValidationError error, std::string detail) {
    return CheckResult{error, std::move(detail)};
}

bool MutationResult::ok() const noexcept {
    return check.ok() && audit.stage == MutationStage::verified;
}

MutationResult::operator bool() const noexcept {
    return ok();
}

bool ValidationSnapshot::ok() const noexcept {
    return check.ok();
}

ValidationSnapshot::operator bool() const noexcept {
    return ok();
}

bool OpenResult::ok() const noexcept {
    return check.ok() && session != nullptr;
}

OpenResult::operator bool() const noexcept {
    return ok();
}

ValidationSession::ValidationSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ValidationSession::~ValidationSession() = default;
ValidationSession::ValidationSession(ValidationSession&&) noexcept = default;
ValidationSession& ValidationSession::operator=(ValidationSession&&) noexcept = default;

OpenResult ValidationSession::open(IMemory& memory,
                                   ProcessAddress image_base,
                                   const ExecutableIdentity& executable,
                                   const BuildProfile& profile,
                                   SessionOptions options) {
    const auto fail = [](ValidationError error, std::string detail) {
        return OpenResult{CheckResult::failure(error, std::move(detail)), nullptr};
    };

    if (profile.name.empty() ||
        profile.preferred_image_base == 0 ||
        profile.symbols.entity_pools == 0 ||
        profile.symbols.local_player_entity == 0 ||
        profile.maximum_player_capacity == 0 ||
        profile.maximum_player_capacity > kHardMaximumPlayerCapacity ||
        profile.player_pool_index >= abi::kEntityPoolCount ||
        digest_is_zero(profile.identity.sha256)) {
        return fail(ValidationError::invalid_profile,
                    "build profile is incomplete or outside safety limits");
    }
    if (image_base == 0) {
        return fail(ValidationError::null_address,
                    "executable image base is null");
    }
    if (digest_is_zero(executable.sha256) ||
        executable.sha256 != profile.identity.sha256 ||
        (profile.identity.image_size != 0 &&
         executable.image_size != profile.identity.image_size)) {
        return fail(ValidationError::unsupported_executable,
                    "executable identity does not match the selected profile");
    }

    if (!range_within_image(profile.symbols.entity_pools,
                            sizeof(PoolDescriptors),
                            executable.image_size) ||
        !range_within_image(profile.symbols.local_player_entity,
                            sizeof(Address32<abi::PlayerEntity>),
                            executable.image_size)) {
        return fail(ValidationError::invalid_profile,
                    "profile symbols fall outside the executable image");
    }

    ProcessAddress pool_descriptor_table{};
    ProcessAddress local_player_global{};
    if (!checked_add(
            image_base,
            profile.symbols.entity_pools,
            pool_descriptor_table) ||
        !checked_add(image_base,
                     profile.symbols.local_player_entity,
                     local_player_global)) {
        return fail(ValidationError::address_overflow,
                    "relocating profile symbols overflowed 32-bit addresses");
    }
    if (!memory.readable(
            pool_descriptor_table, sizeof(PoolDescriptors)) ||
        !memory.readable(local_player_global,
                         sizeof(Address32<abi::PlayerEntity>))) {
        return fail(ValidationError::unreadable_memory,
                    "one or more profile globals are not readable");
    }

    auto impl = std::make_unique<Impl>();
    impl->memory = &memory;
    impl->image_base = image_base;
    impl->profile = profile;
    impl->options = options;
    impl->pool_descriptor_table = pool_descriptor_table;
    impl->local_player_global = local_player_global;

    auto session = std::unique_ptr<ValidationSession>(
        new ValidationSession(std::move(impl)));
    return OpenResult{CheckResult::success(), std::move(session)};
}

ValidationSnapshot ValidationSession::sample(ProbeSet probes) const {
    ValidationSnapshot snapshot{};
    snapshot.profile_name = impl_->profile.name;

    constexpr auto known_probes = static_cast<std::uint32_t>(ProbeSet::all);
    const auto requested = static_cast<std::uint32_t>(probes);
    if ((requested & ~known_probes) != 0) {
        snapshot.check = CheckResult::failure(ValidationError::invalid_probe,
                                              "unknown probe bits were requested");
        return snapshot;
    }
    if ((probes & ProbeSet::players) == ProbeSet::none) {
        snapshot.check = CheckResult::success();
        return snapshot;
    }

    PoolDescriptors descriptors{};
    snapshot.check = impl_->load_pool_descriptors(
        descriptors, &snapshot.pool_descriptors);
    if (!snapshot.check) {
        return snapshot;
    }
    LoadedPlayerPool loaded{};
    snapshot.check = impl_->validate_player_pool(
        descriptors[impl_->profile.player_pool_index], loaded);
    if (!snapshot.check) {
        return snapshot;
    }
    snapshot.player_pool_used = loaded.descriptor.used;
    snapshot.player_pool_capacity = loaded.descriptor.capacity;

    Address32<abi::PlayerEntity> local_player{};
    snapshot.check = read_object(*impl_->memory,
                                 impl_->local_player_global,
                                 local_player,
                                 "local player global");
    if (!snapshot.check) {
        return snapshot;
    }

    if (loaded.descriptor.used == 0) {
        snapshot.check = CheckResult::success();
        return snapshot;
    }

    std::vector<abi::PlayerEntity> entities(loaded.descriptor.used);
    if (!impl_->memory->read(loaded.data, entities.data(), loaded.used_bytes)) {
        snapshot.check = CheckResult::failure(
            ValidationError::memory_read_failed,
            "player pool changed or failed while reading");
        return snapshot;
    }

    snapshot.players.reserve(entities.size());
    CheckResult local_weapon_check = CheckResult::success();
    for (std::uint32_t slot = 0; slot < loaded.descriptor.used; ++slot) {
        const auto& entity = entities[slot];
        ProcessAddress entity_address{};
        snapshot.check = impl_->player_address(loaded, slot, entity_address);
        if (!snapshot.check) {
            snapshot.players.clear();
            return snapshot;
        }

        PlayerObservation observation{};
        observation.slot = slot;
        observation.address = entity_address;
        observation.ground_entity_address = entity.ground_entity.value();
        observation.owner_connection_id = entity.owner_connection_id;
        observation.dcb_id = entity.dcb_id;
        observation.ssn = entity.ssn;
        observation.net_id = entity.net_id;
        observation.command_group = entity.command_group;
        observation.name = bounded_name(entity);
        observation.position_x_raw = entity.position.x;
        observation.position_y_raw = entity.position.y;
        observation.position_z_raw = entity.position.z;
        observation.camera_offset_x_raw = entity.camera_offset.x;
        observation.camera_offset_y_raw = entity.camera_offset.y;
        observation.camera_offset_z_raw = entity.camera_offset.z;
        observation.yaw_raw = entity.orientation.yaw;
        observation.pitch_raw = entity.orientation.pitch;
        observation.roll_raw = entity.orientation.roll;
        observation.flags = entity.flags;
        observation.health = entity.health;
        observation.armor = entity.armor;
        observation.team = entity.team;
        observation.player_class = entity.player_class;
        observation.equipped_adm_index = entity.equipped_adm_index;
        observation.animation_slot = entity.animation_slot;
        observation.is_local =
            !local_player.is_null() && local_player.value() == entity_address;

        if (!entity.weapon_def.is_null()) {
            abi::WeaponDef weapon{};
            observation.weapon_check = read_object(
                *impl_->memory,
                entity.weapon_def.value(),
                weapon,
                player_context(slot) + " weapon definition");
            if (observation.weapon_check) {
                observation.weapon = WeaponObservation{
                    entity.weapon_def.value(),
                    bounded_name(weapon),
                    weapon.special_hold,
                    weapon.attack_anim,
                    weapon.hip_pose.position.x,
                    weapon.hip_pose.position.y,
                    weapon.hip_pose.position.z,
                    weapon.hip_pose.rotation.yaw,
                    weapon.hip_pose.rotation.pitch,
                    weapon.hip_pose.rotation.roll,
                    weapon.aimed_pose.position.x,
                    weapon.aimed_pose.position.y,
                    weapon.aimed_pose.position.z,
                    weapon.aimed_pose.rotation.yaw,
                    weapon.aimed_pose.rotation.pitch,
                    weapon.aimed_pose.rotation.roll,
                    weapon.render_fov,
                };
            } else if (observation.is_local) {
                local_weapon_check = observation.weapon_check;
            }
        }

        snapshot.players.push_back(std::move(observation));
    }

    snapshot.check = std::move(local_weapon_check);
    return snapshot;
}

MutationResult ValidationSession::apply(const Mutation& mutation) {
    MutationAudit audit = std::visit(
        [](const auto& requested) {
            using Request = std::decay_t<decltype(requested)>;
            MutationKind kind{};
            if constexpr (std::is_same_v<Request, SetPlayerHealth>) {
                kind = MutationKind::player_health;
            } else if constexpr (std::is_same_v<Request, SetPlayerTeam>) {
                kind = MutationKind::player_team;
            } else if constexpr (std::is_same_v<Request, SetEquippedAdmIndex>) {
                kind = MutationKind::equipped_adm_index;
            } else if constexpr (std::is_same_v<Request, SetActiveWeaponPose>) {
                kind = MutationKind::active_weapon_pose;
            } else {
                static_assert(std::is_same_v<Request, SetActiveWeaponFov>);
                kind = MutationKind::active_weapon_render_fov;
            }
            MutationAudit result{};
            result.request = requested;
            result.kind = kind;
            result.player_slot = requested.player_slot;
            result.expected_owner_connection_id =
                requested.expected_owner_connection_id;
            return result;
        },
        mutation);

    if (!impl_->options.writes_enabled) {
        return MutationResult{
            CheckResult::failure(ValidationError::writes_disabled,
                                 "session was opened with writes disabled"),
            audit,
        };
    }

    LoadedPlayerPool loaded{};
    auto check = impl_->load_player_pool(loaded);
    if (!check) {
        return MutationResult{std::move(check), audit};
    }

    check = std::visit(
        [this, &loaded, &audit](const auto& requested) -> CheckResult {
            using Request = std::decay_t<decltype(requested)>;

            ProcessAddress player_address{};
            auto result = impl_->player_address(loaded,
                                                requested.player_slot,
                                                player_address);
            if (!result) {
                return result;
            }

            abi::PlayerEntity entity{};
            result = read_object(*impl_->memory,
                                 player_address,
                                 entity,
                                 player_context(requested.player_slot));
            if (!result) {
                return result;
            }
            if (entity.owner_connection_id !=
                requested.expected_owner_connection_id) {
                return CheckResult::failure(
                    ValidationError::mutation_precondition_failed,
                    player_context(requested.player_slot) +
                        " no longer has the expected owner connection");
            }

            ProcessAddress field_address{};
            if constexpr (std::is_same_v<Request, SetPlayerHealth>) {
                if (entity.health != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " health no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, health),
                                 field_address)) {
                    return CheckResult::failure(ValidationError::address_overflow,
                                                "health field address overflows");
                }
                return write_verified(
                    *impl_->memory, field_address, requested.value, audit);
            } else if constexpr (std::is_same_v<Request, SetPlayerTeam>) {
                if (entity.team != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " team no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, team),
                                 field_address)) {
                    return CheckResult::failure(ValidationError::address_overflow,
                                                "team field address overflows");
                }
                return write_verified(
                    *impl_->memory, field_address, requested.value, audit);
            } else if constexpr (std::is_same_v<Request, SetEquippedAdmIndex>) {
                if (entity.equipped_adm_index != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " equipped ADM index no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, equipped_adm_index),
                                 field_address)) {
                    return CheckResult::failure(
                        ValidationError::address_overflow,
                        "equipped ADM index field address overflows");
                }
                return write_verified(
                    *impl_->memory, field_address, requested.value, audit);
            } else if constexpr (std::is_same_v<Request, SetActiveWeaponPose>) {
                if (entity.weapon_def.is_null()) {
                    return CheckResult::failure(
                        ValidationError::null_address,
                        player_context(requested.player_slot) +
                            " has no active weapon definition");
                }
                audit.active_weapon_address = entity.weapon_def.value();
                if (audit.active_weapon_address !=
                    requested.expected_weapon_address) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " active weapon no longer has the expected address");
                }

                abi::WeaponDef weapon{};
                result = read_object(
                    *impl_->memory,
                    audit.active_weapon_address,
                    weapon,
                    player_context(requested.player_slot) +
                        " active weapon definition");
                if (!result) {
                    return result;
                }
                const abi::WeaponPose& actual =
                    requested.pose == ActiveWeaponPose::hip
                    ? weapon.hip_pose
                    : weapon.aimed_pose;
                if (!pose_matches(actual, requested.expected_value)) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        "active weapon pose no longer has the expected value");
                }
                const std::size_t offset =
                    requested.pose == ActiveWeaponPose::hip
                    ? offsetof(abi::WeaponDef, hip_pose)
                    : offsetof(abi::WeaponDef, aimed_pose);
                if (!checked_add(
                        audit.active_weapon_address, offset, field_address)) {
                    return CheckResult::failure(
                        ValidationError::address_overflow,
                        "active weapon pose field address overflows");
                }
                const abi::WeaponPose value = to_abi_pose(requested.value);
                return write_verified(
                    *impl_->memory, field_address, value, audit);
            } else {
                static_assert(std::is_same_v<Request, SetActiveWeaponFov>);
                if (entity.weapon_def.is_null()) {
                    return CheckResult::failure(
                        ValidationError::null_address,
                        player_context(requested.player_slot) +
                            " has no active weapon definition");
                }
                audit.active_weapon_address = entity.weapon_def.value();
                if (audit.active_weapon_address !=
                    requested.expected_weapon_address) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " active weapon no longer has the expected address");
                }

                abi::WeaponDef weapon{};
                result = read_object(
                    *impl_->memory,
                    audit.active_weapon_address,
                    weapon,
                    player_context(requested.player_slot) +
                        " active weapon definition");
                if (!result) {
                    return result;
                }
                if (weapon.render_fov != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        "active weapon render FOV no longer has the expected value");
                }
                if (!checked_add(
                        audit.active_weapon_address,
                        offsetof(abi::WeaponDef, render_fov),
                        field_address)) {
                    return CheckResult::failure(
                        ValidationError::address_overflow,
                        "active weapon render FOV field address overflows");
                }
                return write_verified(
                    *impl_->memory, field_address, requested.value, audit);
            }
        },
        mutation);
    return MutationResult{std::move(check), audit};
}

}  // namespace opennova::retail_hook
